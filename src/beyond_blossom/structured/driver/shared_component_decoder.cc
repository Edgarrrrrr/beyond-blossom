// Copyright 2026 PyMatching Contributors
// Modified for Beyond Blossom, 2026.
// Licensed under the Apache License, Version 2.0.
#include "beyond_blossom/structured/driver/shared_component_decoder.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string_view>
#include <system_error>
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif
#if defined(_OPENMP)
#include <omp.h>
#endif
#include "beyond_blossom/structured/driver/mwpm_decoding.h"
#include "beyond_blossom/structured/driver/time_partition_cuts.h"
namespace beyond_blossom::structured {
SharedDecoderConfiguration SharedDecoderConfiguration::from_environment() {
    SharedDecoderConfiguration config;
    config.seed_ordering = SeedOrdering::cycle_permutation;
    if (const char* value = std::getenv("BB_FIXED_SEED_CONSTRUCTION")) {
        const std::string_view text(value);
        if (text == "fused") config.fixed_seed_construction = FixedSeedConstruction::fused;
        else if (text == "conflict_first") config.fixed_seed_construction = FixedSeedConstruction::conflict_first;
        else throw std::invalid_argument("BB_FIXED_SEED_CONSTRUCTION must be fused or conflict_first.");
    }
    if (const char* value = std::getenv("BB_FIXED_SEED_INPUT")) {
        const std::string_view text(value);
        if (text == "scan") config.fixed_seed_input = FixedSeedInput::scan;
        else if (text == "indexed") config.fixed_seed_input = FixedSeedInput::indexed;
        else throw std::invalid_argument("BB_FIXED_SEED_INPUT must be scan or indexed.");
    }
    if (const char* value = std::getenv("BB_IMPORT_PREPARATION")) {
        const std::string_view text(value);
        if (text == "serial") config.import_preparation = ImportPreparation::serial;
        else if (text == "parallel") config.import_preparation = ImportPreparation::parallel;
        else if (text == "captained") config.import_preparation = ImportPreparation::captained;
        else if (text == "captained_serial") config.import_preparation = ImportPreparation::captained_serial;
        else if (text == "captained_reverse") config.import_preparation = ImportPreparation::captained_reverse;
        else if (text == "captained_overlap") config.import_preparation = ImportPreparation::captained_overlap;
        else throw std::invalid_argument("Invalid BB_IMPORT_PREPARATION mode.");
    }
    if (const char* value = std::getenv("BB_IMPORT_RESET_OWNER")) {
        const std::string_view text(value);
        if (text == "importers") config.import_reset_owner = ImportResetOwner::importers;
        else if (text == "captain") config.import_reset_owner = ImportResetOwner::captain;
        else throw std::invalid_argument("BB_IMPORT_RESET_OWNER must be importers or captain.");
    }
    return config;
}

struct SharedComponentDecoder::State {
    using FixedConstruction = SharedDecoderConfiguration::FixedSeedConstruction;
    using FixedSeedInput = SharedDecoderConfiguration::FixedSeedInput;
    using ImportPreparation = SharedDecoderConfiguration::ImportPreparation;
    using ImportResetOwner = SharedDecoderConfiguration::ImportResetOwner;
    const FixedConstruction fixed_construction;
    const FixedSeedInput fixed_seed_input;
    const ImportPreparation import_preparation;
    const ImportResetOwner import_reset_owner;
    enum Command : uint32_t {
        idle, component_job, partition_job, import_job, continuation_job, cell_clear_component_job, cell_clear_job, done, stop,
        partition_fill_job, preindexed_partition_job, batched_partition_fill_job, component_tail_job, owner_continuation_job,
        whole_prefix_job, whole_component_clear_job, group_prefix_job, group_partition_fill_job, import_prepare_job,
        retire_component_job, component_chain_job, group_chain_job
    };
    struct alignas(64) Worker {
        std::atomic<uint32_t> command{idle};
        std::atomic<bool> ready{false}, stopped{false};
        std::exception_ptr startup_error, teardown_error, job_error;
        size_t error_component = SIZE_MAX, tail_component = SIZE_MAX, continuation_component = SIZE_MAX;
        size_t whole_prefix_component = SIZE_MAX, whole_clear_component = SIZE_MAX;
#if BB_STRUCTURED_PROFILE_TIMERS
        import_pmu::ThreadCounters import_counters;
        std::array<import_pmu::Aggregate, 3> import_aggregates{};  // events/construction/empty
#endif
    };
    struct alignas(64) ConflictPublication {
        std::atomic<bool> ready{false};
    };
    struct alignas(64) FilledCounts { std::array<size_t, 9> counts{}; };
    struct Local {
        TimePartitionCuts time_cuts;
        std::array<size_t, 10> offsets{};
        std::array<FilledCounts, 8> filled;
        std::array<size_t, 9> valid_counts{};
        bool fixed_spans = false;
        std::array<OneHopPredecoder::PartialResult, 9> partial{};
        MatchingResult result;
        std::exception_ptr error;
        std::array<std::exception_ptr, 8> group_interior_errors;
        size_t captain = 0, group_threads = 1, active_threads = 1;
        size_t whole_import_owner_lane = 0;
        size_t conflict_owner_lane = 0;
        RegionId import_first = NO_REGION;
        std::array<PredecodeImportScratch, 8> import_scratch;
        std::array<size_t, 9> import_root_offsets{};
        std::span<AltTreeNode*> import_root_slots;
        bool prepare_import = false;
        bool captained_import = false;
        RegionId neutral_reset_end = 0;
        SharedRoute route = SharedRoute::not_run;
    };
    ComponentDecoder decoder;
    std::array<Local, 2> local;
#if BB_TAILRESET_SYNDROME_STATS
    std::array<TailResetSyndromeCounts, 2> syndromes{};
#endif
    std::array<ComponentDecoder::BoundPartitionSeedLayout, 2> seed_layouts;
    std::span<const uint64_t> scatter_events;
    bool owner_scatter_ready = false, owner_scatter_pending = false;
    bool whole_component_staged = false;
    ConflictPublication conflict_publication;
    std::array<ConflictPublication, 2> component_conflict_publications;
    using PrepareEntry = void (State::*)(const std::vector<uint64_t>&);
    PrepareEntry prepare_entry = &State::prepare_regular_shot;
    using PrefixEntry = void (State::*)();
    PrefixEntry prefix_entry = &State::run_regular_components<false>;
    PrefixEntry chain_entry = &State::run_regular_components<true>;
    using FixedImportRange = void (Mwpm::*)(const SeedView&, RegionId, size_t, size_t);
    FixedImportRange fixed_import_range = nullptr;
    using PreparingImportRange = void (Mwpm::*)(const SeedView&, RegionId, size_t, size_t, PredecodeImportScratch&);
    PreparingImportRange preparing_import_range = nullptr;
    PreparingImportRange captained_import_range = nullptr;
    FixedImportRange captained_import_no_scratch = nullptr;
    PreparingImportRange packed_captained_import_range = nullptr;
    FixedImportRange packed_captained_import_no_scratch = nullptr;
    using ContinuationEntry = void (State::*)();
    ContinuationEntry continuation_entry = &State::continue_on_captains;
    using ClearEntry = void (State::*)() noexcept;
    ClearEntry clear_entry = &State::clear_regular_components;
    bool component_owned_reset_pending = false;
    uint64_t owner_seed_generation = 0;
    std::array<Worker, 8> workers;
    std::vector<SharedThreadSnapshot> snapshots;
    size_t requested_threads, actual_threads = 0, partitions, threshold, shot_threshold, conflict_width;
    bool team_active = false, collect_matches = false;
    bool p1_fixed_index = false, owner_hard_reset = false;
#if BB_COMPONENT_OWNER_AUDIT
    std::array<std::array<uint64_t, 3>, 2> lifecycle_threads{};
#endif
    void audit_owner(size_t c, size_t phase) noexcept {
#if BB_COMPONENT_OWNER_AUDIT && defined(__linux__)
        lifecycle_threads[c][phase] = uint64_t(syscall(SYS_gettid));
#elif BB_COMPONENT_OWNER_AUDIT && defined(__APPLE__)
        pthread_threadid_np(nullptr, &lifecycle_threads[c][phase]);
#else
        (void)c; (void)phase;
#endif
    }
    bool prefix_mode = false, prefix_ready = false;
#if BB_SHARED_COMPONENT_PARALLELISM == 1
    // Captains publish this before releasing a helper command. Every helper
    // publishes done before the captain can switch to the next component.
    size_t active_component = 0;
#endif
    const bool parallel_import;
    std::exception_ptr callback_error;
#if BB_STRUCTURED_PROFILE_TIMERS
    SharedComponentProfile* timing = nullptr;
    uint64_t import_pmu_sequence = 0;
    bool import_pmu_sample = false;
#endif
    inline static thread_local State* active_team = nullptr;
    static void pause() noexcept {
#if defined(__i386__) || defined(__x86_64__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        asm volatile("yield" ::: "memory");
#else
        std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
    }
    void require_active() const {
        if (active_team != this)
            throw std::logic_error("Shared decode requires its run_with_team callback.");
    }
    struct Affinity {
#if defined(__linux__)
        cpu_set_t original{};
#elif defined(__APPLE__)
        qos_class_t original_qos = QOS_CLASS_UNSPECIFIED;
        int original_relative_priority = 0;
#endif
        bool saved = false;
        void capture() {
#if defined(__linux__)
            int error = pthread_getaffinity_np(pthread_self(), sizeof(original), &original);
            if (error)
                throw std::system_error(error, std::generic_category(), "Reading shared thread affinity");
#elif defined(__APPLE__)
            int error = pthread_get_qos_class_np(pthread_self(), &original_qos, &original_relative_priority);
            if (error)
                throw std::system_error(error, std::generic_category(), "Reading shared thread QoS");
#endif
            saved = true;
        }
        void bind(int cpu, const Affinity* inherited = nullptr) {
#if defined(__linux__)
            if (cpu < 0 && !inherited)
                return;
            cpu_set_t target;
            if (cpu < 0) {
                target = inherited->original;
            } else {
                if (cpu >= CPU_SETSIZE)
                    throw std::invalid_argument("Shared CPU exceeds CPU_SETSIZE.");
                CPU_ZERO(&target);
                CPU_SET(cpu, &target);
            }
            int error = pthread_setaffinity_np(pthread_self(), sizeof(target), &target);
            if (error)
                throw std::system_error(error, std::generic_category(), "Setting shared thread affinity");
#elif defined(__APPLE__)
            if (cpu >= 0)
                throw std::invalid_argument("Darwin shared threads use QoS; explicit CPU affinity is unavailable.");
            int error = pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
            if (error)
                throw std::system_error(error, std::generic_category(), "Setting shared thread QoS");
#else
            if (cpu >= 0)
                throw std::invalid_argument("Explicit CPU affinity requires Linux.");
#endif
        }
        static std::vector<int> current() {
            std::vector<int> result;
#if defined(__linux__)
            cpu_set_t actual;
            int error = pthread_getaffinity_np(pthread_self(), sizeof(actual), &actual);
            if (error)
                throw std::system_error(error, std::generic_category(), "Verifying shared thread affinity");
            for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
                if (CPU_ISSET(cpu, &actual))
                    result.push_back(cpu);
#endif
            return result;
        }
        void restore() {
#if defined(__linux__)
            if (saved) {
                int error = pthread_setaffinity_np(pthread_self(), sizeof(original), &original);
                if (error)
                    throw std::system_error(error, std::generic_category(), "Restoring shared thread affinity");
            }
#elif defined(__APPLE__)
            if (saved) {
                // Darwin cannot explicitly request UNSPECIFIED; pooled threads
                // with no prior requested class return to the system default.
                const auto qos = original_qos == QOS_CLASS_UNSPECIFIED ? QOS_CLASS_DEFAULT : original_qos;
                int error = pthread_set_qos_class_self_np(qos, original_relative_priority);
                if (error)
                    throw std::system_error(error, std::generic_category(), "Restoring shared thread QoS");
            }
#endif
        }
    };
    void initialize_thread(size_t id, Affinity& previous, const Affinity& caller_mask) {
        previous.capture();
        auto& snapshot = snapshots[id];
        previous.bind(snapshot.requested_cpu, id == 0 ? &caller_mask : nullptr);
#if defined(__linux__)
        snapshot.thread_id = static_cast<uint64_t>(syscall(SYS_gettid));
#elif defined(__APPLE__)
        int error = pthread_threadid_np(nullptr, &snapshot.thread_id);
        if (error)
            throw std::system_error(error, std::generic_category(), "Reading shared thread ID");
        qos_class_t qos;
        error = pthread_get_qos_class_np(pthread_self(), &qos, &snapshot.qos_relative_priority);
        if (error)
            throw std::system_error(error, std::generic_category(), "Verifying shared thread QoS");
        snapshot.qos_class = static_cast<int>(qos);
        if (qos != QOS_CLASS_USER_INITIATED || snapshot.qos_relative_priority != 0)
            throw std::runtime_error("Shared thread QoS differs from USER_INITIATED/0.");
#endif
        snapshot.affinity = Affinity::current();
        if (snapshot.requested_cpu >= 0 &&
            (snapshot.affinity.size() != 1 || snapshot.affinity[0] != snapshot.requested_cpu))
            throw std::runtime_error("Shared thread affinity differs from its requested CPU.");
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& worker = workers[id];
        worker.import_aggregates = {};
        worker.import_counters.open_from_environment();
        if (worker.import_counters.enabled()) {
            for (size_t k = 0; k < 256; ++k) {
                import_pmu::Scope empty(worker.import_counters, worker.import_aggregates[2], true);
                asm volatile("" ::: "memory");
            }
        }
#endif
    }
    void finish_thread(size_t id, Affinity& previous) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        try { workers[id].import_counters.close_on_owner(); }
        catch (...) { workers[id].teardown_error = std::current_exception(); }
#endif
        try { previous.restore(); }
        catch (...) {
            if (!workers[id].teardown_error) workers[id].teardown_error = std::current_exception();
        }
    }
    void team(const std::function<void()>& callback) {
#if !defined(_OPENMP)
        throw std::runtime_error("SharedComponentDecoder requires an OpenMP-enabled build.");
#else
        if (team_active || omp_get_level() != 0)
            throw std::logic_error("run_with_team requires an inactive decoder outside any OpenMP region.");
        Affinity caller_mask;
        caller_mask.capture();
        for (size_t id = 0; id < requested_threads; ++id) {
            auto& worker = workers[id];
            worker.command.store(idle, std::memory_order_relaxed);
            worker.ready.store(false, std::memory_order_relaxed);
            worker.stopped.store(false, std::memory_order_relaxed);
            worker.startup_error = nullptr;
            worker.teardown_error = nullptr;
            worker.job_error = nullptr;
            snapshots[id].thread_id = 0;
            snapshots[id].affinity.clear();
        }
        callback_error = nullptr;
        team_active = true;
        auto& s = *this;
        const int count_requested = static_cast<int>(requested_threads);
#pragma omp parallel num_threads(count_requested) default(none) shared(s, callback, caller_mask, count_requested)
        {
            const size_t id = static_cast<size_t>(omp_get_thread_num());
            const int count = omp_get_num_threads();
            if (id == 0)
                s.actual_threads = static_cast<size_t>(count);
            if (count != count_requested) {
                if (id == 0) {
                    try {
                        throw std::runtime_error("OpenMP runtime did not provide the requested shared team.");
                    } catch (...) {
                        s.callback_error = std::current_exception();
                    }
                }
            } else {
                Affinity previous_mask;
                try {
                    s.initialize_thread(id, previous_mask, caller_mask);
                } catch (...) {
                    s.workers[id].startup_error = std::current_exception();
                }
                if (id != 0) {
                    s.workers[id].ready.store(true, std::memory_order_release);
                    s.worker_loop(id);
                } else {
                    bool startup_ok = !s.workers[0].startup_error;
                    for (size_t helper = 1; helper < s.requested_threads; ++helper) {
                        while (!s.workers[helper].ready.load(std::memory_order_acquire))
                            pause();
                        startup_ok &= !s.workers[helper].startup_error;
                    }
                    if (startup_ok) {
                        State* previous_team = active_team;
                        active_team = &s;
                        try {
                            callback();
                        } catch (...) {
                            s.callback_error = std::current_exception();
                        }
                        // Final P1 retirement must run while its owners are still available.
                        if (s.p1_component_owners() && !s.decoder.shot_clean) {
                            try { s.retire_owned_shot(); }
                            catch (...) {
                                if (!s.callback_error) s.callback_error = std::current_exception();
                            }
                        }
                        active_team = previous_team;
                    }
                    for (size_t helper = 1; helper < s.requested_threads; ++helper)
                        s.workers[helper].command.store(stop, std::memory_order_release);
                    for (size_t helper = 1; helper < s.requested_threads; ++helper)
                        while (!s.workers[helper].stopped.load(std::memory_order_acquire))
                            pause();
                }
                s.finish_thread(id, previous_mask);
                if (id != 0)
                    s.workers[id].stopped.store(true, std::memory_order_release);
            }
        }
        team_active = false;
        try {
            caller_mask.restore();
        } catch (...) {
            if (!workers[0].teardown_error)
                workers[0].teardown_error = std::current_exception();
        }
        for (size_t id = 0; id < requested_threads; ++id)
            if (workers[id].startup_error)
                std::rethrow_exception(workers[id].startup_error);
        if (callback_error)
            std::rethrow_exception(callback_error);
        for (size_t id = 0; id < requested_threads; ++id)
            if (workers[id].teardown_error)
                std::rethrow_exception(workers[id].teardown_error);
#endif
    }
    State(ComponentDecoder&& input, std::vector<int> cpus, size_t parts, size_t limit,
          size_t shot_limit, size_t width, bool import_parallel, PrefixSchedule schedule, ContinuationPlacement placement,
          TimePartitionCuts::Geometry geometry, SharedDecoderConfiguration configuration)
        : fixed_construction(configuration.fixed_seed_construction), fixed_seed_input(configuration.fixed_seed_input),
          import_preparation(configuration.import_preparation), import_reset_owner(configuration.import_reset_owner),
          decoder(std::move(input)), snapshots(cpus.size()), requested_threads(cpus.size()),
          partitions(parts), threshold(limit), shot_threshold(shot_limit), conflict_width(width), parallel_import(import_parallel) {
        if (requested_threads != 1 && requested_threads != 2 && requested_threads != 4 && requested_threads != 8)
            throw std::invalid_argument("Shared decoder requires 1, 2, 4, or 8 threads.");
        if (partitions != 1 && partitions != 2 && partitions != 4 && partitions != 8)
            throw std::invalid_argument("Shared decoder requires 1, 2, 4, or 8 spatial partitions.");
        if (!decoder.predecode_enabled())
            throw std::invalid_argument("Shared predecode requires predecode enabled.");
        if (!decoder.shot_clean || decoder.prefix_ready) decoder.reset();
        decoder.configure_seed_ordering(configuration.seed_ordering);
        const auto count = decoder.components().size();
        if (count == 0 || count > 2)
            throw std::invalid_argument("Shared decoder requires one or two components.");
        for (size_t id = 0; id < cpus.size(); ++id) {
            if (cpus[id] < -1)
                throw std::invalid_argument("CPU IDs must be -1 or nonnegative.");
            snapshots[id].requested_cpu = cpus[id];
        }
        for (size_t c = 0; c < count; ++c) {
            auto& task = local[c];
            const auto& storage = decoder.components()[c].mwpm.flooder.graph.grid;
            // ComponentDecoder gives every local detector the same explicit
            // group label, so every component owns exactly one time geometry.
            if (!storage || storage->components.size() != 1)
                throw std::invalid_argument("Shared decoder requires one grid per component.");
            const auto& grid = *storage->components.front();
            task.time_cuts = TimePartitionCuts(grid.nx, grid.ny, grid.nt, partitions, conflict_width, geometry);
#if BB_SHARED_COMPONENT_PARALLELISM == 1
            task.group_threads = requested_threads;
            task.captain = 0;
#else
            task.group_threads = requested_threads == 1 ? 1 : requested_threads / count;
            task.captain = requested_threads == 1 ? 0 : c * task.group_threads;
#endif
            for (size_t lane = 0; lane < task.group_threads; ++lane) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                snapshots[lane].component = SharedThreadSnapshot::SHARED_COMPONENT;
#else
                snapshots[task.captain + lane].component = requested_threads == 1 && count == 2
                    ? SharedThreadSnapshot::SHARED_COMPONENT : c;
#endif
                snapshots[task.captain + lane].lane = lane;
            }
        }
#if BB_SHARED_COMPONENT_PARALLELISM == 1
        if (partitions != 1 && threshold == 0 && decoder.supports_owner_scatter())
            prepare_entry = &State::prepare_owner_shot<false>;
#endif
        switch (schedule) {
            case PrefixSchedule::component_sequential:
                break;
            case PrefixSchedule::component_owned_tail:
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                if (parallel_import || partitions == 1 || threshold != 0 || !decoder.supports_owner_scatter())
                    throw std::invalid_argument("Component-owned tails require positive CP1 partitioned input, zero component threshold, and serial whole-component import.");
                prefix_entry = &State::run_component_owned_prefix;
                chain_entry = &State::run_legacy_component_chains;
                break;
#else
                throw std::invalid_argument("Component-owned tails require a CP1 build.");
#endif
            case PrefixSchedule::component_owned_whole:
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                if (parallel_import || partitions != 1 || !decoder.supports_owner_scatter())
                    throw std::invalid_argument("Whole-component prefixes require positive CP1 P1 input and serial whole-component import.");
                prepare_entry = &State::prepare_whole_component_shot;
                prefix_entry = &State::run_whole_component_prefix;
                chain_entry = &State::run_legacy_component_chains;
                clear_entry = &State::clear_whole_components;
                break;
#else
                throw std::invalid_argument("Whole-component prefixes require a CP1 build.");
#endif
            case PrefixSchedule::component_group_owned:
                // Bind the fixed path independently of the per-shot threshold.
                // Its entry selects low-component fallback before partitioning.
                prepare_entry = partitions != 1 && decoder.supports_owner_scatter()
                    ? &State::prepare_group_owner_shot : &State::prepare_regular_shot;
                prefix_entry = &State::run_group_components<false>;
                chain_entry = &State::run_group_components<true>;
                break;
            default:
                throw std::invalid_argument("Unknown shared prefix schedule.");
        }
        switch (placement) {
            case ContinuationPlacement::component_captain:
                break;
            case ContinuationPlacement::prefix_tail_owner:
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                if (schedule != PrefixSchedule::component_owned_tail && schedule != PrefixSchedule::component_owned_whole)
                    throw std::invalid_argument("Tail-owner continuation requires a component-owned prefix.");
                continuation_entry = &State::continue_on_tail_owners;
                break;
#else
                throw std::invalid_argument("Tail-owner continuation requires a CP1 build.");
#endif
            default:
                throw std::invalid_argument("Unknown shared continuation placement.");
        }
        // A moved standalone decoder may still own a completed or prepared
        // shot. Retire that view before constructing the fixed backing.
        if (prepare_entry == &State::prepare_group_owner_shot && (!decoder.shot_clean || decoder.prefix_ready))
            decoder.reset();
        if (prepare_entry == &State::prepare_owner_shot<false> || prepare_entry == &State::prepare_group_owner_shot)
            decoder.bind_partition_buckets(partitions, conflict_width, geometry);
        // Component and TailReset P1 retain PC's fixed serial component prefixes.
        if (partitions == 1 && decoder.supports_owner_scatter() &&
            !decoder.seed_storage_generation && decoder.shot_clean)
            decoder.initialize_serial_seed_storage();

        if (prepare_entry == &State::prepare_group_owner_shot) {
            decoder.bind_fixed_seed_storage();
            if (fixed_seed_input == FixedSeedInput::indexed) decoder.bind_fixed_seed_index();
            fixed_import_range = partitions == 2 ? &Mwpm::import_fixed_predecode_range<2>
                               : partitions == 4 ? &Mwpm::import_fixed_predecode_range<4>
                                                 : &Mwpm::import_fixed_predecode_range<8>;
            preparing_import_range = partitions == 2 ? &Mwpm::import_fixed_predecode_range_preparing<2>
                                   : partitions == 4 ? &Mwpm::import_fixed_predecode_range_preparing<4>
                                                     : &Mwpm::import_fixed_predecode_range_preparing<8>;
            captained_import_range = partitions == 2 ? &Mwpm::import_fixed_predecode_range_captained<2>
                                   : partitions == 4 ? &Mwpm::import_fixed_predecode_range_captained<4>
                                                     : &Mwpm::import_fixed_predecode_range_captained<8>;
            captained_import_no_scratch = partitions == 2 ? &Mwpm::import_fixed_predecode_range_captained_no_scratch<2>
                                        : partitions == 4 ? &Mwpm::import_fixed_predecode_range_captained_no_scratch<4>
                                                          : &Mwpm::import_fixed_predecode_range_captained_no_scratch<8>;
            packed_captained_import_range = partitions == 2 ? &Mwpm::import_fixed_predecode_range_captained<2, true>
                                          : partitions == 4 ? &Mwpm::import_fixed_predecode_range_captained<4, true>
                                                            : &Mwpm::import_fixed_predecode_range_captained<8, true>;
            packed_captained_import_no_scratch = partitions == 2 ? &Mwpm::import_fixed_predecode_range_captained_no_scratch<2, true>
                                               : partitions == 4 ? &Mwpm::import_fixed_predecode_range_captained_no_scratch<4, true>
                                                                 : &Mwpm::import_fixed_predecode_range_captained_no_scratch<8, true>;
        }

    }
    void prepare_regular_shot(const std::vector<uint64_t>& events) {
        p1_fixed_index = partitions == 1 && decoder.serial_seed_storage;
        decoder.prepare_shot(events, p1_fixed_index);
    }
    void prepare_group_owner_shot(const std::vector<uint64_t>& events) {
        if (!prefix_mode || events.size() < shot_threshold) {
            prepare_regular_shot(events);
            return;
        }
        if (threshold != 0) {
            // Ownership counts precede all staging, bucket filling and
            // predecode preparation. Mixed shots keep the original entry:
            // the low component is serial while the X/Z teams can overlap.
            const size_t component_count = decoder.components().size();
            // Fewer than threshold * components events cannot put every
            // component above threshold. Division avoids product overflow.
            if (events.size() / component_count < threshold) {
                prepare_regular_shot(events);
                return;
            }
            std::array<size_t, 2> counts{};
            size_t remaining = component_count;
            for (uint64_t event : events) {
                if (event >= decoder.detector_addresses.size()) {
                    // Preserve the original validation and error priority.
                    prepare_regular_shot(events);
                    return;
                }
                const auto component = decoder.detector_addresses[event].component;
                if (component != ComponentDecoder::NO_COMPONENT && ++counts[component] == threshold && --remaining == 0)
                    break;
            }
            if (remaining != 0) {
                prepare_regular_shot(events);
                return;
            }
            // The chosen preparation still validates the entire input,
            // including any invalid or duplicate events after the early stop.
        }
        prepare_owner_shot<true>(events);
    }
    void prepare_whole_component_shot(const std::vector<uint64_t>& events) {
        // Normal decode/edges must receive filled spans before run_component.
        // Whole-shot fallback also retains the original coordinator entry.
        if (!prefix_mode || events.size() < shot_threshold) {
            prepare_regular_shot(events);
            return;
        }
        decoder.stage_component_shot(events);
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            auto& task = local[c];
            task.route = SharedRoute::serial;
            task.active_threads = 1;
            task.whole_import_owner_lane = c == 1 && requested_threads > 1 ? 1 : 0;
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& p = timing->component[c];
            p.route = task.route;
            p.active_threads = p.partitions = 1;
#if BB_STRUCTURED_PROFILE_COUNTERS
            p.seeds = decoder.components()[c].mwpm.predecoder.seed_view.active_count();
#endif
#endif
        }
        scatter_events = events;
        whole_component_staged = true;
    }
    template <bool InputSpans>
    void prepare_owner_shot(const std::vector<uint64_t>& events) {
        if (events.size() < shot_threshold) {
            prepare_regular_shot(events);
            return;
        }
        const auto count = decoder.components().size();
        if constexpr (InputSpans)
            decoder.stage_group_partitioned_shot(events, std::span(seed_layouts).first(count));
        else
            decoder.stage_bound_partitioned_shot(events, std::span(seed_layouts).first(count));
#if BB_STRUCTURED_PROFILE_TIMERS
        if constexpr (InputSpans) {
            timing->index_input_used = decoder.fixed_seed_input_indexed;
            timing->index_classification_visits = decoder.fixed_index_classification_visits;
            timing->index_entries_written = decoder.fixed_index_entries_written;
        }
#endif
        for (size_t c = 0; c < count; ++c) {
            auto& task = local[c];
            task.route = SharedRoute::partitioned;
            task.active_threads = std::min(partitions, task.group_threads);
            task.offsets = seed_layouts[c].offsets;
            task.fixed_spans = InputSpans;
            task.partial[partitions] = {};
            auto& pre = decoder.components()[c].mwpm.predecoder;
            if constexpr (!InputSpans)
                pre.prepare_partition_scratch(std::span<const size_t>(task.offsets).first(partitions + 2));
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& p = timing->component[c];
            p.route = task.route;
            p.active_threads = task.active_threads;
            p.partitions = partitions;
#if BB_STRUCTURED_PROFILE_COUNTERS
            p.seeds = pre.seed_view.active_count();
            p.conflict_seeds = InputSpans ? 0 : pre.seed_view.active_count() - task.offsets[partitions];
#endif
#endif
        }
        // All potentially throwing allocation precedes any index publication.
        scatter_events = events;
        owner_scatter_ready = owner_scatter_pending = true;
    }
    template <bool IncludeConflict = false>
    void scatter_lane(size_t lane) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->lanes[lane].owner_scatter_ns BB_TIMELINE_ARG(
            &timing->lanes[lane].owner_scatter_interval));
#endif
        uint32_t mask = 0;
        for (size_t b = lane; b < partitions; b += local[0].active_threads)
            mask |= uint32_t{1} << b;
        if constexpr (IncludeConflict)
            mask |= uint32_t{1} << partitions;
        // Fill every owned bucket in both components before any predecode
        // operation can throw. Each record precedes its active-index store.
        decoder.scatter_bound_partitioned_shot(scatter_events,
            std::span<const ComponentDecoder::BoundPartitionSeedLayout>(seed_layouts).first(decoder.components().size()), mask);
    }
    void wait_for_conflict_publication(size_t lane) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->lanes[lane].owner_conflict_wait_ns BB_TIMELINE_ARG(
            &timing->lanes[lane].owner_conflict_wait_interval));
#endif
        while (!conflict_publication.ready.load(std::memory_order_acquire))
            pause();
    }
    void scatter_batched_lane(size_t lane) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->lanes[lane].owner_scatter_ns BB_TIMELINE_ARG(
            &timing->lanes[lane].owner_scatter_interval));
#endif
        uint32_t interior_mask = 0;
        for (size_t b = lane; b < partitions; b += local[0].active_threads)
            interior_mask |= uint32_t{1} << b;
        std::array<uint32_t, 2> masks{};
        const auto count = decoder.components().size();
        for (size_t c = 0; c < count; ++c)
            masks[c] = interior_mask | (lane == local[c].conflict_owner_lane ? uint32_t{1} << partitions : 0);
        // Every owned record in both components is filled before a ready
        // publication, wait, or fallible operation on either component.
        decoder.scatter_bound_partitioned_shot(scatter_events,
            std::span<const ComponentDecoder::BoundPartitionSeedLayout>(seed_layouts).first(count),
            std::span<const uint32_t>(masks).first(count));
    }
    void wait_for_component_conflict(size_t c, size_t lane) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        ComponentDecodePhaseScope phase(p.lanes[lane].owner_conflict_wait_ns BB_TIMELINE_ARG(
            &p.lanes[lane].owner_conflict_wait_interval));
#endif
        while (!component_conflict_publications[c].ready.load(std::memory_order_acquire))
            pause();
    }
    void prepare_component(size_t c) {
        auto& task = local[c];
        auto& pre = decoder.components()[c].mwpm.predecoder;
        task.route = partitions == 1 || pre.seed_view.active_count() < threshold ? SharedRoute::serial : SharedRoute::partitioned;
        task.active_threads = task.route == SharedRoute::serial ? 1 : std::min(partitions, task.group_threads);
        if (task.route == SharedRoute::partitioned) {
            std::array<size_t, 9> counts{}, cursor{};
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                auto& p = timing->component[c];
                ComponentDecodePhaseScope count(p.bucket_count_ns BB_TIMELINE_ARG(&p.bucket_count_interval));
#endif
                for (const auto& seed : pre.seed_view.storage)
                    ++counts[task.time_cuts.bucket(seed.cell)];
                task.offsets[0] = 0;
                for (size_t b = 0; b <= partitions; ++b) {
                    task.offsets[b + 1] = task.offsets[b] + counts[b];
                    cursor[b] = task.offsets[b];
                }
            }
            // In-place counting distribution: one shot vector, O(partitions)
            // counters, no local syndrome lists or copied worker seed arrays.
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                auto& p = timing->component[c];
                ComponentDecodePhaseScope reorder(p.bucket_reorder_ns BB_TIMELINE_ARG(&p.bucket_reorder_interval));
#endif
                for (size_t b = 0; b <= partitions; ++b) {
                    while (cursor[b] < task.offsets[b + 1]) {
                        const auto target = task.time_cuts.bucket(pre.seed_view.storage[cursor[b]].cell);
                        if (target == b) {
                            ++cursor[b];
                        } else {
                            std::swap(pre.seed_view.storage[cursor[b]], pre.seed_view.storage[cursor[target]++]);
                        }
                    }
                }
            }
            pre.prepare_partition_scratch(std::span<const size_t>(task.offsets).first(partitions + 2));
            // Conflict indices are visible to every interior before dispatch.
            // Workers publish their own interiors. All stencil active reads
            // are source +/-1 time plane, so the >=2-plane guard separates
            // those reads from every other worker's index writes.
            task.partial[partitions] = {};
            pre.rebuild_cell_index_range(
                task.offsets[partitions], task.offsets[partitions + 1], task.partial[partitions].stats);
        } else if (!p1_fixed_index) {
            pre.rebuild_cell_index();
        }
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        p.route = task.route;
#if BB_STRUCTURED_PROFILE_COUNTERS
        p.seeds = pre.seed_view.active_count();
#endif
        p.active_threads = task.active_threads;
        p.partitions = task.route == SharedRoute::partitioned ? partitions : 1;
#if BB_STRUCTURED_PROFILE_COUNTERS
        p.conflict_seeds = task.route == SharedRoute::partitioned ? pre.seed_view.active_count() - task.offsets[partitions] : 0;
#endif
#endif
    }
    template <bool Preindexed = false, bool Fixed = false>
    void run_lane(size_t c, size_t lane) {
        auto& task = local[c];
        auto& pre = decoder.components()[c].mwpm.predecoder;
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->lanes[task.captain + lane].worker_ns BB_TIMELINE_ARG(
            &timing->lanes[task.captain + lane].worker_interval));
        ComponentDecodePhaseScope component_phase(
            timing->component[c].lanes[lane].worker_ns BB_TIMELINE_ARG(&timing->component[c].lanes[lane].worker_interval));
#endif
        for (size_t b = lane; b < partitions; b += task.active_threads) {
            task.partial[b] = {};
            if constexpr (!Preindexed) {
                pre.rebuild_cell_index_range(task.offsets[b], task.offsets[b + 1], task.partial[b].stats);
            }
            if constexpr (Fixed)
                pre.run_partition_fused(b, task.filled[lane].counts[b], task.partial[b], decoder.components()[c].mwpm.single_predecode_grid());
            else
                pre.run_partition_fused(b, task.partial[b], decoder.components()[c].mwpm.single_predecode_grid());
        }
    }
    void run_import_lane(size_t c, size_t lane) {
        auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
        const auto& seeds = mwpm.predecoder.seed_view;
        // Balanced contiguous slices keep every object, radius, node, and match
        // endpoint single-writer. No helper touches the queue or tree arena.
        const size_t begin = seeds.active_count() * lane / task.active_threads;
        const size_t end = seeds.active_count() * (lane + 1) / task.active_threads;
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->lanes[task.captain + lane].import_worker_ns BB_TIMELINE_ARG(
            &timing->lanes[task.captain + lane].import_worker_interval));
        ComponentDecodePhaseScope component_phase(timing->component[c].lanes[lane].import_worker_ns BB_TIMELINE_ARG(
            &timing->component[c].lanes[lane].import_worker_interval));
#endif
        if (task.prepare_import)
            (mwpm.*preparing_import_range)(seeds, task.import_first, begin, end, task.import_scratch[lane]);
        else if (task.fixed_spans)
            (mwpm.*fixed_import_range)(seeds, task.import_first, begin, end);
        else
            mwpm.import_predecode_range(seeds.storage, task.import_first, begin, end);
    }
    void run_import(size_t id) noexcept {
        workers[id].job_error = nullptr;
        try {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
            run_import_lane(active_component, snapshots[id].lane);
#else
            run_import_lane(snapshots[id].component, snapshots[id].lane);
#endif
        } catch (...) {
            workers[id].job_error = std::current_exception();
        }
    }
    void run_import_prepare_lane(size_t c, size_t lane) {
        auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
        if (task.captained_import) {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope phase(timing->component[c].lanes[lane].import_events_ns);
#endif
            if (import_preparation == ImportPreparation::captained_overlap && task.active_threads > 1) {
                assert(lane > 0);
                const auto& seeds = mwpm.predecoder.seed_view;
                const size_t helper_count = task.active_threads - 1;
                const size_t begin = seeds.active_count() * (lane - 1) / helper_count;
                const size_t end = seeds.active_count() * lane / helper_count;
                auto& scratch = task.import_scratch[lane];
#if BB_STRUCTURED_PROFILE_TIMERS
                auto& worker = workers[task.captain + lane];
                import_pmu::Scope event_pmu(worker.import_counters, worker.import_aggregates[0], import_pmu_sample);
#endif
                mwpm.prepare_fixed_predecode_events_from_seeds(seeds, task.import_first, begin, end, scratch);
#if BB_STRUCTURED_PROFILE_TIMERS
                event_pmu.record_work(scratch.roots.size(), scratch.neighbor_scans);
#endif
                return;
            }
            auto roots = std::span<PredecodeImportRoot>(task.import_scratch[0].roots);
            const size_t event_threads = import_preparation == ImportPreparation::captained_serial ? 1 : task.active_threads;
            const size_t slice = import_preparation == ImportPreparation::captained_reverse ? event_threads - 1 - lane : lane;
            const size_t begin = roots.size() * slice / event_threads;
            const size_t end = roots.size() * (slice + 1) / event_threads;
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& worker = workers[task.captain + lane];
            import_pmu::Scope event_pmu(worker.import_counters, worker.import_aggregates[0], import_pmu_sample);
#endif
            task.import_scratch[lane].neighbor_scans =
                mwpm.prepare_predecode_import_events(roots.subspan(begin, end - begin));
#if BB_STRUCTURED_PROFILE_TIMERS
            event_pmu.record_work(end - begin, task.import_scratch[lane].neighbor_scans);
#endif
            return;
        }
        auto& scratch = task.import_scratch[lane];
        // Every dispatched lane MUST construct all roots before any fallible
        // event calculation. The captain commits all borrowed slots after join,
        // including when a kernel throws, so normal reset owns every root.
        const auto slots = task.import_root_slots.subspan(task.import_root_offsets[lane], scratch.roots.size());
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c].lanes[lane];
        {
            ComponentDecodePhaseScope phase(p.import_roots_ns);
#endif
            mwpm.construct_predecode_import_roots(scratch, slots);
#if BB_STRUCTURED_PROFILE_TIMERS
        }
        ComponentDecodePhaseScope phase(p.import_events_ns);
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& worker = workers[task.captain + lane];
        import_pmu::Scope event_pmu(worker.import_counters, worker.import_aggregates[0], import_pmu_sample);
#endif
        mwpm.prepare_predecode_import_events(scratch);
#if BB_STRUCTURED_PROFILE_TIMERS
        event_pmu.record_work(scratch.roots.size(), scratch.neighbor_scans);
#endif
    }
    void run_import_prepare(size_t id) noexcept {
        workers[id].job_error = nullptr;
        try {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
            run_import_prepare_lane(active_component, snapshots[id].lane);
#else
            run_import_prepare_lane(snapshots[id].component, snapshots[id].lane);
#endif
        } catch (...) {
            workers[id].job_error = std::current_exception();
        }
    }
    void finish_parallel_import(size_t c) {
        auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
        task.import_root_offsets[0] = 0;
        for (size_t lane = 0; lane < task.active_threads; ++lane)
            task.import_root_offsets[lane + 1] = task.import_root_offsets[lane] + task.import_scratch[lane].roots.size();
        const size_t roots = task.import_root_offsets[task.active_threads];
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope reserve(timing->component[c].import_root_reserve_ns);
#endif
            task.import_root_slots = mwpm.reserve_predecode_import_roots(roots);
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope dispatch(timing->component[c].import_prepare_dispatch_ns);
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                workers[task.captain + lane].command.store(import_prepare_job, std::memory_order_release);
        }
        std::exception_ptr captain_error;
        try { run_import_prepare_lane(c, 0); } catch (...) { captain_error = std::current_exception(); }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope join(timing->component[c].import_prepare_join_ns);
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                    pause();
        }
        mwpm.commit_predecode_import_roots(roots);
        task.import_root_slots = {};
        if (captain_error) std::rethrow_exception(captain_error);
        for (size_t lane = 1; lane < task.active_threads; ++lane)
            if (workers[task.captain + lane].job_error)
                std::rethrow_exception(workers[task.captain + lane].job_error);
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope publish(timing->component[c].import_publish_ns);
#endif
            mwpm.publish_predecode_import_events(
                std::span<const PredecodeImportScratch>(task.import_scratch).first(task.active_threads),
                mwpm.predecoder.seed_view.active_count());
        }
    }
    void import_overlapped(size_t c) {
        auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
        const auto& seeds = mwpm.predecoder.seed_view;
        const size_t helpers = task.active_threads - 1;
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope prepare(timing->component[c].import_scratch_ns);
#endif
            task.import_scratch[0].clear();
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                task.import_scratch[lane].roots.reserve(
                    seeds.active_count() * lane / helpers - seeds.active_count() * (lane - 1) / helpers);
        }
        task.import_first = collect_matches ? mwpm.prepare_predecode_import(seeds.active_count())
                                            : mwpm.prepare_packed_fixed_predecode_import();
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope dispatch(timing->component[c].import_prepare_dispatch_ns);
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                workers[task.captain + lane].command.store(import_prepare_job, std::memory_order_release);
        }
        // Helpers use only immutable predecode data and independent event
        // scratch. Persistent node/region/root state remains captain-owned.
        // Always join before propagating either side's exception or resetting.
        std::exception_ptr captain_error;
        try {
            PredecodePhaseScope<PredecodePhase::ImportConstruct> construct(mwpm.predecoder.stats);
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& worker = workers[task.captain];
            import_pmu::Scope construct_pmu(worker.import_counters, worker.import_aggregates[1], import_pmu_sample);
#endif
            const auto entry = collect_matches ? captained_import_no_scratch : packed_captained_import_no_scratch;
            (mwpm.*entry)(seeds, task.import_first, 0, seeds.active_count());
#if BB_STRUCTURED_PROFILE_TIMERS
            construct_pmu.record_work(mwpm.active_tree_count(), 0);
#endif
        } catch (...) { captain_error = std::current_exception(); }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope join(timing->component[c].import_prepare_join_ns);
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                    pause();
        }
        if (captain_error) std::rethrow_exception(captain_error);
        for (size_t lane = 1; lane < task.active_threads; ++lane)
            if (workers[task.captain + lane].job_error)
                std::rethrow_exception(workers[task.captain + lane].job_error);
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope publish(timing->component[c].import_publish_ns);
#endif
            mwpm.publish_predecode_import_events(
                std::span<const PredecodeImportScratch>(task.import_scratch).subspan(1, helpers), seeds.active_count());
        }
    }
    void import_on_captain(size_t c) {
        auto& task = local[c];
        if (import_preparation == ImportPreparation::captained_overlap && task.active_threads > 1) {
            import_overlapped(c);
            return;
        }
        auto& mwpm = decoder.components()[c].mwpm;
        const auto& seeds = mwpm.predecoder.seed_view;
        auto& scratch = task.import_scratch[0];
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope prepare(timing->component[c].import_scratch_ns);
#endif
            scratch.roots.reserve(seeds.active_count());
        }
        task.import_first = collect_matches ? mwpm.prepare_predecode_import(seeds.active_count())
                                            : mwpm.prepare_packed_fixed_predecode_import();
        {
            // Region/node and root writes stay on the following MWPM/extract
            // owner. Root construction shares this seed traversal.
            PredecodePhaseScope<PredecodePhase::ImportConstruct> construct(mwpm.predecoder.stats);
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& worker = workers[task.captain];
            import_pmu::Scope construct_pmu(worker.import_counters, worker.import_aggregates[1], import_pmu_sample);
#endif
            const auto entry = collect_matches ? captained_import_range : packed_captained_import_range;
            (mwpm.*entry)(seeds, task.import_first, 0, seeds.active_count(), scratch);
#if BB_STRUCTURED_PROFILE_TIMERS
            construct_pmu.record_work(scratch.roots.size(), 0);
#endif
        }
        if (import_preparation == ImportPreparation::captained_serial || task.active_threads == 1) {
            run_import_prepare_lane(c, 0);
        } else {
            {
    #if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->component[c].import_prepare_dispatch_ns);
    #endif
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    workers[task.captain + lane].command.store(import_prepare_job, std::memory_order_release);
            }
            std::exception_ptr captain_error;
            try { run_import_prepare_lane(c, 0); } catch (...) { captain_error = std::current_exception(); }
            {
    #if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->component[c].import_prepare_join_ns);
    #endif
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                        pause();
            }
            if (captain_error) std::rethrow_exception(captain_error);
            for (size_t lane = 1; lane < task.active_threads; ++lane) {
                if (workers[task.captain + lane].job_error)
                    std::rethrow_exception(workers[task.captain + lane].job_error);
                scratch.neighbor_scans += task.import_scratch[lane].neighbor_scans;
            }
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope publish(timing->component[c].import_publish_ns);
#endif
            mwpm.publish_predecode_import_events(std::span<const PredecodeImportScratch>(&scratch, 1), seeds.active_count());
        }
    }
    template <bool WholeComponentOwner = false>
    void import_component(size_t c) {
        audit_owner(c, 0);
        auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        ComponentDecodePhaseScope import_scope(p.import_ns BB_TIMELINE_ARG(&p.import_interval));
#endif
        if (!mwpm.flooder.queue.empty())
            throw std::invalid_argument("Import requires a drained queue.");
#if BB_STRUCTURED_PROFILE_COUNTERS
        mwpm.flooder.queue.profile = {};
#endif
        PredecodePhaseScope<PredecodePhase::Handoff> handoff(mwpm.predecoder.stats);
        task.prepare_import = !WholeComponentOwner && parallel_import && task.fixed_spans
            && import_preparation == ImportPreparation::parallel;
        task.captained_import = !WholeComponentOwner && parallel_import && task.fixed_spans
            && (import_preparation == ImportPreparation::captained
                || import_preparation == ImportPreparation::captained_serial
                || import_preparation == ImportPreparation::captained_reverse
                || import_preparation == ImportPreparation::captained_overlap);
        if (task.captained_import) {
#if BB_STRUCTURED_PROFILE_TIMERS
            timing->component[c].import_preparation_captained = true;
#endif
            import_on_captain(c);
            return;
        }
        if (task.prepare_import) {
#if BB_STRUCTURED_PROFILE_TIMERS
            timing->component[c].import_preparation_parallel = true;
            ComponentDecodePhaseScope scratch(timing->component[c].import_scratch_ns);
#endif
            const size_t count = mwpm.predecoder.seed_view.active_count();
            for (size_t lane = 0; lane < task.active_threads; ++lane)
                task.import_scratch[lane].roots.reserve(count * (lane + 1) / task.active_threads - count * lane / task.active_threads);
        }
        if (WholeComponentOwner || !parallel_import) {
            if (task.fixed_spans) {
                RegionId first;
                {
                    PredecodePhaseScope<PredecodePhase::HandoffRegions> regions(mwpm.predecoder.stats);
                    first = mwpm.prepare_predecode_import(mwpm.predecoder.seed_view.active_count());
                    PredecodePhaseScope<PredecodePhase::ImportConstruct> construct(mwpm.predecoder.stats);
                    (mwpm.*fixed_import_range)(mwpm.predecoder.seed_view, first, 0,
                        mwpm.predecoder.seed_view.active_count());
                }
                mwpm.finish_fixed_predecode_import(mwpm.predecoder.seed_view, first);
            } else {
                if (collect_matches) mwpm.import_predecode_state(mwpm.predecoder.seed_view.storage);
                else mwpm.import_packed_predecode_state();
            }
            return;
        }
        {
            PredecodePhaseScope<PredecodePhase::HandoffRegions> regions(mwpm.predecoder.stats);
            task.import_first = mwpm.prepare_predecode_import(mwpm.predecoder.seed_view.active_count());
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(p.import_dispatch_ns BB_TIMELINE_ARG(&p.import_dispatch_interval));
#endif
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    workers[task.captain + lane].command.store(import_job, std::memory_order_release);
            }
            std::exception_ptr captain_error;
            try { run_import_lane(c, 0); } catch (...) { captain_error = std::current_exception(); }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(p.import_join_ns BB_TIMELINE_ARG(&p.import_join_interval));
#endif
                // Always join the entire group before propagating an exception.
                // The outer X/Z join then makes decoder.reset safe for all slots.
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                        pause();
            }
            if (captain_error)
                std::rethrow_exception(captain_error);
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                if (workers[task.captain + lane].job_error)
                    std::rethrow_exception(workers[task.captain + lane].job_error);
        }
        if (task.prepare_import)
            finish_parallel_import(c);
        else if (task.fixed_spans)
            mwpm.finish_fixed_predecode_import(mwpm.predecoder.seed_view, task.import_first);
        else
            mwpm.finish_predecode_import(mwpm.predecoder.seed_view.storage, task.import_first);
    }
    template <bool Preindexed = false, bool Fill = false>
    void run_partition(size_t id) noexcept {
        workers[id].job_error = nullptr;
        if constexpr (Fill) {
            scatter_lane(snapshots[id].lane);
            wait_for_conflict_publication(snapshots[id].lane);
        }
        try {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
            run_lane<Preindexed>(active_component, snapshots[id].lane);
#else
            run_lane<Preindexed>(snapshots[id].component, snapshots[id].lane);
#endif
        } catch (...) {
            workers[id].job_error = std::current_exception();
        }
    }
    void clear_lane_cells(size_t c, size_t lane) noexcept {
        const auto& task = local[c];
        auto& mwpm = decoder.components()[c].mwpm;
        auto& pre = mwpm.predecoder;
        if (mwpm.has_pending_seed_cleanup()) {
            const auto count = pre.seed_view.active_count();
            if (task.import_first && import_reset_owner == ImportResetOwner::importers) {
                mwpm.reset_seed_nodes_range(
                    count * lane / task.active_threads, count * (lane + 1) / task.active_threads);
            } else if (lane == task.whole_import_owner_lane) {
                mwpm.reset_seed_nodes_range(0, count);
            }
        }
        if (pre.has_live_cell_state()) {
            for (size_t b = lane; b < partitions; b += task.active_threads)
                pre.clear_cell_state_range(task.offsets[b],
                    task.fixed_spans ? task.offsets[b] + task.valid_counts[b] : task.offsets[b + 1]);
            if (lane == task.conflict_owner_lane)
                pre.clear_cell_state_range(task.offsets[partitions], task.fixed_spans
                    ? task.offsets[partitions] + task.valid_counts[partitions] : task.offsets[partitions + 1]);
        }
        if (task.neutral_reset_end) {
            auto& pool = *mwpm.flooder.regions;
            if (task.import_first && import_reset_owner == ImportResetOwner::importers) {
                // These are the completed import's ID owners, not the next
                // shot's partition. Formal MWPM's extra IDs belong to captain.
                const auto count = pre.seed_view.active_count();
                const auto first = task.import_first;
                pool.prepare_neutral_range(
                    first + count * lane / task.active_threads,
                    first + count * (lane + 1) / task.active_threads);
                if (lane == 0)
                    pool.prepare_neutral_range(first + count, task.neutral_reset_end);
            } else if (lane == task.whole_import_owner_lane) {
                pool.prepare_neutral_range(1, task.neutral_reset_end);
            }
        }
    }
    void clear_component_cells(size_t c) noexcept {
        audit_owner(c, 2);
        auto& mwpm = decoder.components()[c].mwpm;
        auto& pre = mwpm.predecoder;
        auto& task = local[c];
        task.neutral_reset_end = mwpm.flooder.regions->take_neutral_reset_end();
        if (!pre.has_live_cell_state() && !task.neutral_reset_end && !mwpm.has_pending_seed_cleanup())
            return;
        if (task.route != SharedRoute::partitioned) {
            mwpm.clear_pending_seed_nodes();
            pre.clear_cell_state();
            if (task.neutral_reset_end)
                mwpm.flooder.regions->prepare_neutral_range(1, task.neutral_reset_end);
            task.neutral_reset_end = 0;
            return;
        }
#if BB_SHARED_COMPONENT_PARALLELISM == 1
        active_component = c;
#endif
        for (size_t lane = 1; lane < task.active_threads; ++lane)
            workers[task.captain + lane].command.store(cell_clear_job, std::memory_order_release);
        clear_lane_cells(c, 0);
        for (size_t lane = 1; lane < task.active_threads; ++lane)
            while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                pause();
        // All cell clears and neutral object construction are published before
        // the coordinator retires the flag, token, or seed/owner descriptors.
        mwpm.finish_seed_node_cleanup();
        pre.finish_cell_state_clear();
        task.neutral_reset_end = 0;
    }
    void publish_cleanup_owners() noexcept {
        owner_seed_generation = decoder.seed_storage_generation;
        component_owned_reset_pending = true;
    }
    void clear_cells_after_shot() noexcept {
        if (owner_seed_generation != decoder.seed_storage_generation) {
            // A public mutable serial API has retired the old shot and rebound
            // the backing. Its current view owns cleanup; stale bucket spans
            // and import ownership must not be applied to it.
            for (auto& component : decoder.components()) {
                auto& mwpm = component.mwpm;
                mwpm.clear_pending_seed_nodes();
                if (const auto end = mwpm.flooder.regions->take_neutral_reset_end())
                    mwpm.flooder.regions->prepare_neutral_range(1, end);
                mwpm.predecoder.clear_cell_state();
            }
            component_owned_reset_pending = false;
            return;
        }
        bool live = false;
        for (const auto& component : decoder.components())
            live |= component.mwpm.predecoder.has_live_cell_state() ||
                    component.mwpm.has_pending_seed_cleanup() ||
                    component.mwpm.flooder.regions->has_pending_neutral_reset();
        if (!live) {
            component_owned_reset_pending = false;
            return;
        }
        (this->*clear_entry)();
        component_owned_reset_pending = false;
    }
    void clear_whole_components() noexcept {
        // These descriptors belong to the prior shot. Normal/fallback and T1
        // use owner0; an active T2 whole prefix keeps C1 on owner1 through reset.
        const bool helper = decoder.components().size() == 2 && local[1].whole_import_owner_lane != 0;
        if (helper) {
            const auto owner = local[1].whole_import_owner_lane;
            workers[owner].whole_clear_component = 1;
            workers[owner].command.store(whole_component_clear_job, std::memory_order_release);
            clear_component_cells(0);
            while (workers[owner].command.load(std::memory_order_acquire) != done)
                pause();
        } else {
            for (size_t c = 0; c < decoder.components().size(); ++c)
                clear_component_cells(c);
        }
    }
    void clear_regular_components() noexcept {
#if BB_SHARED_COMPONENT_PARALLELISM == 2
        if (requested_threads != 1 && decoder.components().size() == 2 && local[0].route != SharedRoute::whole_serial) {
            const auto captain = local[1].captain;
            workers[captain].command.store(cell_clear_component_job, std::memory_order_release);
            clear_component_cells(0);
            while (workers[captain].command.load(std::memory_order_acquire) != done)
                pause();
        } else
#endif
        {
            for (size_t c = 0; c < decoder.components().size(); ++c)
                clear_component_cells(c);
        }
    }
    // P1 has one owner per component. Keep all mutable reset work on the
    // preceding shot's owner, including the predecoder view and pool state.
    // The coordinator retires only the already-cleared shared backing.
    void retire_component(size_t c) noexcept {
        audit_owner(c, 2);
        auto& component = decoder.components()[c];
        auto& mwpm = component.mwpm;
        if (owner_hard_reset) {
            mwpm.reset();
            mwpm.search_flooder.reset();
            mwpm.flooder.match_edges.clear();
        }
        clear_component_cells(c);
        mwpm.flooder.regions->cancel_neutral_reset();
        mwpm.predecoder.reset();
        component.seed_begin = component.seed_end = 0;
#if BB_PREDECODE_DIAGNOSTICS || BB_STRUCTURED_PROFILE_COUNTERS || BB_STRUCTURED_PROFILE_TIMERS
        mwpm.predecoder.stats = {};
#endif
    }
    bool p1_component_owners() const noexcept {
#if BB_SHARED_COMPONENT_PARALLELISM == 2
        return partitions == 1;
#else
        return false;
#endif
    }
    void retire_owned_shot(bool hard = false) {
        require_active();
        owner_hard_reset = hard || prefix_ready;
        // Direct mutable-serial calls can supersede the prior shared shot.
        // Those states belong to their caller, not stale worker descriptors.
        if (owner_seed_generation != decoder.seed_storage_generation) {
            if (owner_hard_reset) decoder.reset();
            else decoder.reset_after_shot();
        } else {
            const bool helper = requested_threads != 1 && decoder.components().size() == 2 &&
                local[0].route != SharedRoute::whole_serial;
            if (helper) {
                const auto captain = local[1].captain;
                workers[captain].command.store(retire_component_job, std::memory_order_release);
                retire_component(0);
                while (workers[captain].command.load(std::memory_order_acquire) != done) pause();
            } else {
                for (size_t c = 0; c < decoder.components().size(); ++c) retire_component(c);
            }
            decoder.retire_shot_storage();
            decoder.match_edges.clear();
            decoder.prefix_ready = false;
        }
        prefix_ready = false;
        component_owned_reset_pending = false;
    }
    void worker_loop(size_t id) noexcept {
        auto& worker = workers[id];
        while (true) {
            const auto command = worker.command.load(std::memory_order_acquire);
            if (command == stop)
                return;
            if (command == retire_component_job) {
                retire_component(snapshots[id].component);
                worker.command.store(done, std::memory_order_release);
            } else if (command == component_chain_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 2
                run_component<true>(snapshots[id].component);
#endif
                worker.command.store(done, std::memory_order_release);
            } else if (command == group_chain_job) {
                run_group_chain(snapshots[id].component);
                worker.command.store(done, std::memory_order_release);
            } else if (command == component_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                // CP1 never dispatches a component captain. Keep an unexpected
                // command from indexing the metadata's shared-component token.
                workers[id].job_error = std::make_exception_ptr(std::logic_error("CP1 cannot dispatch component jobs"));
#else
                run_component(snapshots[id].component);
#endif
                worker.command.store(done, std::memory_order_release);
            } else if (command == partition_job) {
                run_partition(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == partition_fill_job) {
                run_partition<true, true>(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == preindexed_partition_job) {
                run_partition<true>(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == batched_partition_fill_job) {
                run_batched_lane(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == group_prefix_job) {
                run_group_prefix(snapshots[id].component);
                worker.command.store(done, std::memory_order_release);
            } else if (command == group_partition_fill_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                run_group_lane(active_component, snapshots[id].lane);
#else
                run_group_lane(snapshots[id].component, snapshots[id].lane);
#endif
                worker.command.store(done, std::memory_order_release);
            } else if (command == component_tail_job) {
                run_component_tail(worker.tail_component, id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == whole_prefix_job) {
                run_whole_prefix_component(worker.whole_prefix_component, id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == whole_component_clear_job) {
                clear_component_cells(worker.whole_clear_component);
                worker.command.store(done, std::memory_order_release);
            } else if (command == import_job) {
                run_import(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == import_prepare_job) {
                run_import_prepare(id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == cell_clear_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                clear_lane_cells(active_component, snapshots[id].lane);
#else
                clear_lane_cells(snapshots[id].component, snapshots[id].lane);
#endif
                worker.command.store(done, std::memory_order_release);
            } else if (command == cell_clear_component_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                workers[id].job_error =
                    std::make_exception_ptr(std::logic_error("CP1 cannot dispatch component clear jobs"));
#else
                clear_component_cells(snapshots[id].component);
#endif
                worker.command.store(done, std::memory_order_release);
            } else if (command == owner_continuation_job) {
                // The command owns a captured component; CP1 snapshots carry
                // SHARED_COMPONENT and cannot identify this continuation.
                continue_component(worker.continuation_component, id);
                worker.command.store(done, std::memory_order_release);
            } else if (command == continuation_job) {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
                workers[id].job_error =
                    std::make_exception_ptr(std::logic_error("CP1 cannot dispatch continuation jobs"));
#else
                continue_component(snapshots[id].component, id);
#endif
                worker.command.store(done, std::memory_order_release);
            } else {
                pause();
            }
        }
    }
    template <bool Fixed = false>
    void finish_partition_predecode(size_t c) {
        auto& task = local[c];
        auto& pre = decoder.components()[c].mwpm.predecoder;
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
#endif
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope merge(p.partial_merge_ns BB_TIMELINE_ARG(&p.partial_merge_interval));
#endif
            for (size_t b = 0; b < partitions; ++b)
                pre.merge(task.partial[b]);
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope conflict(p.conflict_ns BB_TIMELINE_ARG(&p.conflict_interval));
#endif
            if constexpr (Fixed)
                pre.run_partition_fused(partitions, task.valid_counts[partitions], task.partial[partitions], decoder.components()[c].mwpm.single_predecode_grid());
            else
                pre.run_partition_fused(partitions, task.partial[partitions], decoder.components()[c].mwpm.single_predecode_grid());
            pre.merge(task.partial[partitions]);
            pre.finish();
        }
    }
    void run_batched_lane(size_t id) noexcept {
        auto& worker = workers[id];
        worker.job_error = nullptr;
        worker.error_component = SIZE_MAX;
        const auto lane = snapshots[id].lane;
        scatter_batched_lane(lane);
        // Both owners publish before any lane waits or enters Degree. A C0
        // exception therefore cannot suppress C1's publication or strand a peer.
        for (size_t c = 0; c < decoder.components().size(); ++c)
            if (lane == local[c].conflict_owner_lane)
                component_conflict_publications[c].ready.store(true, std::memory_order_release);
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            if (lane != local[c].conflict_owner_lane)
                wait_for_component_conflict(c, lane);
            try {
                run_lane<true>(c, lane);
            } catch (...) {
                worker.job_error = std::current_exception();
                worker.error_component = c;
                break;
            }
        }
    }
    void run_batched_interiors() {
#if BB_STRUCTURED_PROFILE_TIMERS
        ComponentDecodePhaseScope phase(timing->interior_ns BB_TIMELINE_ARG(&timing->interior_interval));
#endif
        const auto lanes = local[0].active_threads;
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            auto& task = local[c];
            task.conflict_owner_lane = lanes > 1 ? c : 0;
            component_conflict_publications[c].ready.store(false, std::memory_order_relaxed);
#if BB_STRUCTURED_PROFILE_TIMERS
            timing->component[c].conflict_owner_lane = task.conflict_owner_lane;
#endif
        }
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope dispatch(timing->interior_dispatch_ns BB_TIMELINE_ARG(
                &timing->interior_dispatch_interval));
#endif
            for (size_t lane = 1; lane < lanes; ++lane)
                workers[lane].command.store(batched_partition_fill_job, std::memory_order_release);
        }
        run_batched_lane(0);
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope join(timing->interior_join_ns BB_TIMELINE_ARG(&timing->interior_join_interval));
#endif
            for (size_t lane = 1; lane < lanes; ++lane)
                while (workers[lane].command.load(std::memory_order_acquire) != done)
                    pause();
        }
        // Every owner filled both components before its first fallible stage.
        // Publish both spans even if a lane stopped at an interior exception.
        decoder.publish_partitioned_shot();
        scatter_events = {};
        owner_scatter_pending = false;
        // Preserve component-first, then captain/lane-first error precedence.
        for (size_t c = 0; c < decoder.components().size(); ++c)
            for (size_t lane = 0; lane < lanes; ++lane)
                if (workers[lane].error_component == c)
                    std::rethrow_exception(workers[lane].job_error);
    }
    void run_component_tail(size_t c, size_t worker) noexcept {
        auto& task = local[c];
        task.error = nullptr;
        task.whole_import_owner_lane = worker;
        try {
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& p = timing->component[c];
            p.tail_worker = worker;
            ComponentDecodePhaseScope tail(p.tail_ns BB_TIMELINE_ARG(&p.tail_interval));
#endif
            finish_partition_predecode(c);
            if (!decoder.components()[c].mwpm.predecoder.complete)
                import_component<true>(c);
        } catch (...) {
            task.error = std::current_exception();
        }
    }
    void run_component_owned_prefix() {
        if (!owner_scatter_ready) {
            // The declared whole-shot fallback keeps its original entry.
            run_regular_components();
            return;
        }
        run_batched_interiors();
        if (decoder.components().size() == 2 && requested_threads > 1) {
            workers[1].tail_component = 1;
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->tail_dispatch_ns BB_TIMELINE_ARG(
                    &timing->tail_dispatch_interval));
#endif
                workers[1].command.store(component_tail_job, std::memory_order_release);
            }
            run_component_tail(0, 0);
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->tail_join_ns BB_TIMELINE_ARG(&timing->tail_join_interval));
#endif
                while (workers[1].command.load(std::memory_order_acquire) != done)
                    pause();
            }
        } else {
            for (size_t c = 0; c < decoder.components().size(); ++c)
                run_component_tail(c, 0);
        }
        publish_cleanup_owners();
    }
    void scatter_group_input(size_t c, uint32_t mask, std::array<size_t, 9>& counts
#if BB_STRUCTURED_PROFILE_TIMERS
                             , size_t& visits
#endif
                             ) noexcept {
        if (fixed_seed_input == FixedSeedInput::indexed) {
            decoder.scatter_indexed_fixed_group(c, mask, counts);
#if BB_STRUCTURED_PROFILE_TIMERS
            visits = 0;
            for (auto count : counts) visits += count;
#endif
        } else {
            const auto input = seed_layouts[c].bounded_input(scatter_events, mask, decoder.bound_detector_ranges[c]);
#if BB_STRUCTURED_PROFILE_TIMERS
            visits = input.size();
#endif
            decoder.scatter_fixed_group(input, c, mask, counts);
        }
    }
    void run_group_lane(size_t c, size_t lane) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        ComponentDecodePhaseScope job(p.lanes[lane].group_lane_ns BB_TIMELINE_ARG(&p.lanes[lane].group_lane_interval));
#endif
        auto& task = local[c];
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope fill(p.lanes[lane].group_seed_scatter_ns BB_TIMELINE_ARG(&p.lanes[lane].group_seed_scatter_interval));
#endif
            std::array<uint32_t, 2> masks{};
            for (size_t b = lane; b < partitions; b += task.active_threads)
                masks[c] |= uint32_t{1} << b;
            if (lane == 0 && fixed_construction == FixedConstruction::conflict_first) {
                // Publish only after the complete conflict index is visible.
                // Other interiors cannot read this lane's interior index across
                // the guard, so their work may overlap this lane's second fill.
                std::array<size_t, 9> conflict_counts{};
                {
#if BB_STRUCTURED_PROFILE_TIMERS
                    ComponentDecodePhaseScope phase(p.lanes[lane].conflict_construct_ns);
#endif
                    const uint32_t conflict_mask = uint32_t{1} << partitions;
                    scatter_group_input(c, conflict_mask, conflict_counts
#if BB_STRUCTURED_PROFILE_TIMERS
                                        , p.lanes[lane].conflict_input_visits
#endif
                                        );
                }
                component_conflict_publications[c].ready.store(true, std::memory_order_release);
                {
#if BB_STRUCTURED_PROFILE_TIMERS
                    ComponentDecodePhaseScope phase(p.lanes[lane].interior_construct_ns);
#endif
                    scatter_group_input(c, masks[c], task.filled[lane].counts
#if BB_STRUCTURED_PROFILE_TIMERS
                                        , p.lanes[lane].interior_input_visits
#endif
                                        );
                }
                // Both input paths write every count, including unowned
                // zeros. Merge the first pass only after the second completes.
                task.filled[lane].counts[partitions] = conflict_counts[partitions];
            } else {
                if (lane == 0) masks[c] |= uint32_t{1} << partitions;
                // Fused construction publishes conflict and ordinary seeds
                // together, regardless of the selected input traversal.
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(lane == 0
                    ? p.lanes[lane].fused_construct_ns : p.lanes[lane].interior_construct_ns);
#endif
                scatter_group_input(c, masks[c], task.filled[lane].counts
#if BB_STRUCTURED_PROFILE_TIMERS
                                    , lane == 0 ? p.lanes[lane].fused_input_visits : p.lanes[lane].interior_input_visits
#endif
                                    );
            }
#if BB_STRUCTURED_PROFILE_TIMERS
            p.lanes[lane].bounded_input_visits = p.lanes[lane].fused_input_visits
                + p.lanes[lane].interior_input_visits + p.lanes[lane].conflict_input_visits;
#endif
        }
        if (lane == 0) {
            if (fixed_construction == FixedConstruction::fused)
                component_conflict_publications[c].ready.store(true, std::memory_order_release);
        } else
            wait_for_component_conflict(c, lane);
        try {
            run_lane<true, true>(c, lane);
        } catch (...) {
            task.group_interior_errors[lane] = std::current_exception();
        }
    }
    template <bool Chain = false>
    void run_group_prefix(size_t c) noexcept {
        audit_owner(c, 0);
        auto& task = local[c];
#if BB_SHARED_COMPONENT_PARALLELISM == 1
        active_component = c;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        p.group_worker = task.captain;
        ComponentDecodePhaseScope group(p.group_prefix_ns BB_TIMELINE_ARG(&p.group_prefix_interval));
        p.fixed_spans = task.fixed_spans;
        std::optional<ComponentDecodePhaseScope> fixed_predecode;
        fixed_predecode.emplace(p.fixed_predecode_including_seed_ns BB_TIMELINE_ARG(&p.fixed_predecode_interval));
#endif
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope dispatch(p.dispatch_ns BB_TIMELINE_ARG(&p.dispatch_interval));
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                workers[task.captain + lane].command.store(group_partition_fill_job, std::memory_order_release);
        }
        run_group_lane(c, 0);
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope join(p.join_ns BB_TIMELINE_ARG(&p.join_interval));
#endif
            for (size_t lane = 1; lane < task.active_threads; ++lane)
                while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                    pause();
        }
        // Recover actual filled prefixes even when an interior failed. No
        // stale hole may become a cleanup seed during the outer error path.
        task.valid_counts = {};
        for (size_t lane = 0; lane < task.active_threads; ++lane)
            for (size_t b = 0; b <= partitions; ++b)
                task.valid_counts[b] += task.filled[lane].counts[b];
        auto& pre = decoder.components()[c].mwpm.predecoder;
        pre.seed_view.set_counts(std::span<const size_t>(task.valid_counts).first(partitions + 1));
        pre.initialize_seed_counts();
        // Group-local fill is joined; this component can now solve while the
        // other group still prepares. Publish even on failure for cleanup.
        if constexpr (Chain) pre.publish_cell_state();
#if BB_STRUCTURED_PROFILE_TIMERS
        p.fixed_bucket_seeds = task.valid_counts;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS && BB_STRUCTURED_PROFILE_COUNTERS
        p.seeds = pre.seed_view.active_count();
        p.conflict_seeds = task.valid_counts[partitions];
#endif
        for (size_t lane = 0; lane < task.active_threads; ++lane)
            if (task.group_interior_errors[lane]) return;
        // Import/clear ownership is a LOCAL lane; profile actors are absolute.
        task.whole_import_owner_lane = 0;
        try {
#if BB_STRUCTURED_PROFILE_TIMERS
            p.tail_worker = task.captain;
            ComponentDecodePhaseScope tail(p.tail_ns BB_TIMELINE_ARG(&p.tail_interval));
#endif
            finish_partition_predecode<true>(c);
#if BB_STRUCTURED_PROFILE_TIMERS
            fixed_predecode.reset();
#endif
            if (!decoder.components()[c].mwpm.predecoder.complete)
                import_component(c);  // I1 dispatches and joins this group's helpers.
        } catch (...) {
            task.error = std::current_exception();
        }
    }
    void run_group_chain(size_t c) noexcept {
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        ComponentDecodePhaseScope total(p.total_ns BB_TIMELINE_ARG(&p.total_interval));
#endif
        run_group_prefix<true>(c);
        if (local[c].error) return;
        for (size_t lane = 0; lane < local[c].active_threads; ++lane)
            if (local[c].group_interior_errors[lane]) return;
        continue_component(c, local[c].captain);
    }
    template <bool Chain = false>
    void run_group_components() {
        if (!owner_scatter_ready) {
            run_regular_components<Chain>();
            return;
        }
#if BB_STRUCTURED_PROFILE_TIMERS
        std::optional<ComponentDecodePhaseScope> group;
        if constexpr (!Chain) group.emplace(timing->group_ns BB_TIMELINE_ARG(&timing->group_interval));
#endif
        const auto count = decoder.components().size();
        for (size_t c = 0; c < count; ++c) {
            local[c].group_interior_errors = {};
            local[c].conflict_owner_lane = 0;
            component_conflict_publications[c].ready.store(false, std::memory_order_relaxed);
        }
#if BB_SHARED_COMPONENT_PARALLELISM == 2
        if (requested_threads > 1) {
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->group_dispatch_ns BB_TIMELINE_ARG(&timing->group_dispatch_interval));
#endif
                for (size_t c = 1; c < count; ++c)
                    workers[local[c].captain].command.store(Chain ? group_chain_job : group_prefix_job, std::memory_order_release);
            }
            if constexpr (Chain) run_group_chain(0);
            else run_group_prefix(0);
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->group_join_ns BB_TIMELINE_ARG(&timing->group_join_interval));
#endif
                for (size_t c = 1; c < count; ++c)
                    while (workers[local[c].captain].command.load(std::memory_order_acquire) != done)
                        pause();
            }
        } else
#endif
        {
            // Even a failed C0 must not suppress C1's fill or preparation.
            for (size_t c = 0; c < count; ++c) {
                if constexpr (Chain) run_group_chain(c);
                else run_group_prefix(c);
            }
        }
        // All groups and all I1 helpers have stopped touching the backing.
        // Publish every filled span before ordered errors can trigger reset.
        scatter_events = {};
        if constexpr (Chain) decoder.publish_chained_partitioned_shot();
        else decoder.publish_partitioned_shot();
        owner_scatter_pending = false;
        publish_cleanup_owners();
        for (size_t c = 0; c < count; ++c)
            for (size_t lane = 0; lane < local[c].active_threads; ++lane)
                if (local[c].group_interior_errors[lane])
                    std::rethrow_exception(local[c].group_interior_errors[lane]);
        // run() checks all tail/import errors afterward, in component order.
    }
    void run_whole_prefix_component(size_t c, size_t worker) noexcept {
        auto& task = local[c];
        task.error = nullptr;
#if BB_STRUCTURED_PROFILE_TIMERS
        auto& p = timing->component[c];
        p.prefix_worker = worker;
        ComponentDecodePhaseScope whole(p.whole_prefix_ns BB_TIMELINE_ARG(&p.whole_prefix_interval));
#endif
        {
#if BB_STRUCTURED_PROFILE_TIMERS
            ComponentDecodePhaseScope scatter(p.whole_seed_scatter_ns BB_TIMELINE_ARG(&p.whole_seed_scatter_interval));
#endif
            // Complete this owner's noexcept fill/index before any fallible
            // predecode/import. The other component has no dependency on it.
            decoder.scatter_component_shot(scatter_events, c);
        }
        try {
            if (collect_matches) prepare_timeline_for_mwpm(decoder.components()[c].mwpm);
            else prepare_packed_timeline_for_mwpm(decoder.components()[c].mwpm);
        } catch (...) {
            task.error = std::current_exception();
        }
    }
    void run_whole_component_prefix() {
        if (!whole_component_staged) {
            run_regular_components();
            return;
        }
        if (decoder.components().size() == 2 && requested_threads > 1) {
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->whole_prefix_dispatch_ns BB_TIMELINE_ARG(
                    &timing->whole_prefix_dispatch_interval));
#endif
                workers[1].whole_prefix_component = 1;
                workers[1].command.store(whole_prefix_job, std::memory_order_release);
            }
            run_whole_prefix_component(0, 0);
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->whole_prefix_join_ns BB_TIMELINE_ARG(
                    &timing->whole_prefix_join_interval));
#endif
                while (workers[1].command.load(std::memory_order_acquire) != done)
                    pause();
            }
        } else {
            // A captured C0 error never suppresses C1's fill and preparation.
            for (size_t c = 0; c < decoder.components().size(); ++c)
                run_whole_prefix_component(c, 0);
        }
        scatter_events = {};
        decoder.publish_staged_shot();
        whole_component_staged = false;
        publish_cleanup_owners();
        // run() checks component-ordered errors after both jobs and publication.
    }
    template <bool Chain = false>
    void run_regular_components() {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
        for (size_t c = 0; c < decoder.components().size(); ++c)
            run_component<Chain>(c);
#else
        if (requested_threads == 1 || decoder.components().size() == 1 || local[0].route == SharedRoute::whole_serial) {
            for (size_t c = 0; c < decoder.components().size(); ++c)
                run_component<Chain>(c);
        } else {
            const auto captain = local[1].captain;
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->dispatch_ns BB_TIMELINE_ARG(&timing->dispatch_interval));
#endif
                workers[captain].command.store(Chain ? component_chain_job : component_job, std::memory_order_release);
            }
            run_component<Chain>(0);
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->join_ns BB_TIMELINE_ARG(&timing->join_interval));
#endif
                while (workers[captain].command.load(std::memory_order_acquire) != done)
                    pause();
            }
        }
#endif
    }
    template <bool Chain = false>
    void run_component(size_t c) noexcept {
        audit_owner(c, 0);
        auto& task = local[c];
        task.error = nullptr;
        try {
#if BB_SHARED_COMPONENT_PARALLELISM == 1
            // The previous component has joined all helpers before returning.
            // Release/acquire on each command publishes this scalar task ID.
            active_component = c;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& p = timing->component[c];
            ComponentDecodePhaseScope total(p.total_ns BB_TIMELINE_ARG(&p.total_interval));
#endif
            auto& mwpm = decoder.components()[c].mwpm;
            auto& pre = mwpm.predecoder;
            // Component spans are final before dispatch. Each captain can now
            // bucket and index its own independent grid while the other does so.
            if (!owner_scatter_ready && task.route != SharedRoute::whole_serial) {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope prep(p.prepare_ns BB_TIMELINE_ARG(&p.prepare_interval));
#endif
                prepare_component(c);
            }
            const bool serial = task.route != SharedRoute::partitioned;
            if (!serial) {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope predecode_scope(p.predecode_ns BB_TIMELINE_ARG(&p.predecode_interval));
#endif
                const bool first_fill = owner_scatter_pending;
                if (first_fill)
                    // The command release makes this new-shot reset visible
                    // before a helper can read a previous shot's ready value.
                    conflict_publication.ready.store(false, std::memory_order_relaxed);
                const auto command = first_fill ? partition_fill_job :
                    owner_scatter_ready ? preindexed_partition_job : partition_job;
                {
#if BB_STRUCTURED_PROFILE_TIMERS
                    ComponentDecodePhaseScope dispatch(p.dispatch_ns BB_TIMELINE_ARG(&p.dispatch_interval));
#endif
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    workers[task.captain + lane].command.store(command, std::memory_order_release);
                }
                if (first_fill) {
                    scatter_lane<true>(0);
                    // Both components' conflict records are initialized. This
                    // must precede any fallible Degree/Grow operation so a
                    // captain exception cannot strand a waiting helper.
                    conflict_publication.ready.store(true, std::memory_order_release);
                }
                std::exception_ptr captain_error;
                try {
                    if (owner_scatter_ready)
                        run_lane<true>(c, 0);
                    else
                        run_lane(c, 0);
                } catch (...) { captain_error = std::current_exception(); }
                {
#if BB_STRUCTURED_PROFILE_TIMERS
                    ComponentDecodePhaseScope join(p.join_ns BB_TIMELINE_ARG(&p.join_interval));
#endif
                    for (size_t lane = 1; lane < task.active_threads; ++lane)
                        while (workers[task.captain + lane].command.load(std::memory_order_acquire) != done)
                            pause();
                }
                if (first_fill) {
                    // Every lane filled both components before its fallible
                    // predecode work. Publish before propagating any error.
                    decoder.publish_partitioned_shot();
                    scatter_events = {};
                    owner_scatter_pending = false;
                }
                if (captain_error)
                    std::rethrow_exception(captain_error);
                for (size_t lane = 1; lane < task.active_threads; ++lane)
                    if (workers[task.captain + lane].job_error)
                        std::rethrow_exception(workers[task.captain + lane].job_error);
                finish_partition_predecode(c);
            }
            const bool state_imported = !serial && !pre.complete;
            if (state_imported)
                import_component(c);
            if (prefix_mode) {
                if (serial) {
                    if (collect_matches) prepare_timeline_for_mwpm(mwpm);
                    else prepare_packed_timeline_for_mwpm(mwpm);
                }
                // Continue on this captain before joining the other component.
                // The deferred extractor leaves all post-shot cleanup owned by
                // the same buffer; ordinary Shared decode remains unchanged.
                if constexpr (Chain) continue_component(c, task.route == SharedRoute::whole_serial ? 0 : task.captain);
                return;
            }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                p.continuation_worker = task.route == SharedRoute::whole_serial ? 0 : task.captain;
                ComponentDecodePhaseScope continuation(p.continuation_ns BB_TIMELINE_ARG(&p.continuation_interval));
#endif
                audit_owner(c, 1);
                if (collect_matches)
                    decode_prepared_detection_events_to_normalized_match_edges(mwpm, serial, state_imported);
                else
                    task.result = decode_prepared_detection_events_for_up_to_64_observables(mwpm, serial, state_imported);
            }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope clear(p.clear_ns BB_TIMELINE_ARG(&p.clear_interval));
#endif
                // Extraction no longer needs cell state. Reuse the same owners
                // for required cleanup before publishing this component result.
                clear_component_cells(c);
            }
        } catch (...) {
            task.error = std::current_exception();
        }
    }
#if BB_TAILRESET_SYNDROME_STATS
    void capture_predecode_syndromes(size_t c) {
        // Each captain owns its component snapshot. Capture at the local
        // prefix/solve boundary so the other component can keep preparing.
        auto& counts = syndromes[c];
        counts = {};
        const auto& view = decoder.components()[c].mwpm.predecoder.seed_view;
        for (const auto& range : view.active_ranges()) {
            for (size_t k = range.begin; k < range.end; ++k) {
                const auto& seed = view.storage[k];
                ++counts.normalized;
                if (seed.mate == PredecodeSeed::UNMATCHED) ++counts.residual;
                else if (seed.mate == PredecodeSeed::BOUNDARY) ++counts.boundaries;
                else if (k < seed.mate) ++counts.pairs;
            }
        }
        if (counts.normalized != counts.residual + 2 * counts.pairs + counts.boundaries)
            throw std::logic_error("Predecode seed conservation failed");
    }
#endif
    void continue_component(size_t c, size_t worker) noexcept {
        audit_owner(c, 1);
        auto& task = local[c];
        task.error = nullptr;
        try {
            auto& mwpm = decoder.components()[c].mwpm;
#if BB_TAILRESET_SYNDROME_STATS
            capture_predecode_syndromes(c);
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
            auto& p = timing->component[c];
            p.continuation_worker = worker;
            ComponentDecodePhaseScope continuation(p.continuation_ns BB_TIMELINE_ARG(&p.continuation_interval));
#endif
            task.result = solve_prepared_detection_events_deferring_seed_nodes(mwpm);
        } catch (...) {
            task.error = std::current_exception();
        }
    }
    void continue_on_captains() {
#if BB_SHARED_COMPONENT_PARALLELISM == 2
        if (requested_threads != 1 && decoder.components().size() == 2 && local[0].route != SharedRoute::whole_serial) {
            const auto captain = local[1].captain;
            workers[captain].command.store(continuation_job, std::memory_order_release);
            continue_component(0, 0);
            while (workers[captain].command.load(std::memory_order_acquire) != done)
                pause();
        } else
#endif
        {
            for (size_t c = 0; c < decoder.components().size(); ++c)
                continue_component(c, 0);
        }
    }
    void continue_on_tail_owners() {
        // Even a C0 failure must finish before dispatching C1. All component
        // errors are inspected only after every continuation has joined.
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            const auto owner = local[c].whole_import_owner_lane;
            if (owner == 0) {
                continue_component(c, 0);
                continue;
            }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope dispatch(timing->continuation_dispatch_ns BB_TIMELINE_ARG(
                    &timing->continuation_dispatch_interval));
#endif
                workers[owner].continuation_component = c;
                workers[owner].command.store(owner_continuation_job, std::memory_order_release);
            }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope join(timing->continuation_join_ns BB_TIMELINE_ARG(
                    &timing->continuation_join_interval));
#endif
                while (workers[owner].command.load(std::memory_order_acquire) != done)
                    pause();
            }
        }
    }
    void run_legacy_component_chains() {
        // CP1-only alternate prefix/continuation ownership stays two-phase.
        (this->*prefix_entry)();
        for (size_t c = 0; c < decoder.components().size(); ++c)
            if (local[c].error) std::rethrow_exception(local[c].error);
        (this->*continuation_entry)();
    }
    void solve() {
        require_active();
        if (!prefix_ready)
            throw std::logic_error("solve_prepared requires prepare_for_mwpm.");
        (this->*continuation_entry)();
        prefix_ready = false;
        for (size_t c = 0; c < decoder.components().size(); ++c) {
            if (local[c].error) {
                if (p1_component_owners()) retire_owned_shot(true);
                else decoder.reset();
                component_owned_reset_pending = false;
                std::rethrow_exception(local[c].error);
            }
        }
    }
    template <bool Chain = false>
    void run(const std::vector<uint64_t>& events, bool matches, bool prefix = false) {
#if BB_STRUCTURED_PROFILE_TIMERS
        import_pmu_sample = import_pmu_sequence++ % import_pmu::ThreadCounters::sample_period() == 0;
#endif
        require_active();
        if (prefix_ready)
            throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
        // An implicit next shot must consume the previous component-owned
        // cleanup before any owner descriptors are overwritten below.
        if (p1_component_owners() && !decoder.shot_clean)
            retire_owned_shot();
        else if (component_owned_reset_pending)
            clear_cells_after_shot();
        prefix_mode = prefix;
        collect_matches = matches;
        owner_scatter_ready = owner_scatter_pending = whole_component_staged = false;
        scatter_events = {};
        for (auto& task : local) {
            task.route = SharedRoute::not_run;
            task.fixed_spans = false;
            task.import_first = NO_REGION;
            task.neutral_reset_end = 0;
            task.whole_import_owner_lane = 0;
            task.conflict_owner_lane = 0;
            task.result = {};
            task.error = nullptr;
        }
        try {
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(timing->partition_ns BB_TIMELINE_ARG(&timing->partition_interval));
                decoder.profile = {};
#endif
                (this->*prepare_entry)(events);
                for (size_t c = 0; c < decoder.components().size(); ++c) {
                    if (events.size() < shot_threshold) {
                        local[c].route = SharedRoute::whole_serial;
                        local[c].active_threads = 1;
                        if (!p1_fixed_index)
                            decoder.components()[c].mwpm.predecoder.rebuild_cell_index();
#if BB_STRUCTURED_PROFILE_TIMERS
                        auto& p = timing->component[c];
                        p.route = SharedRoute::whole_serial;
#if BB_STRUCTURED_PROFILE_COUNTERS
                        p.seeds = decoder.components()[c].mwpm.predecoder.seed_view.active_count();
#endif
                        p.active_threads = p.partitions = 1;
#endif
                    }
                }
            }
            owner_seed_generation = decoder.seed_storage_generation;
            if constexpr (Chain)
                (this->*chain_entry)();
            else if (prefix_mode)
                (this->*prefix_entry)();
            else
                run_regular_components();
            for (size_t c = 0; c < decoder.components().size(); ++c)
                if (local[c].error)
                    std::rethrow_exception(local[c].error);
            owner_seed_generation = decoder.seed_storage_generation;
            prefix_ready = prefix && !Chain;
            if (p1_component_owners()) component_owned_reset_pending = true;
        } catch (...) {
            scatter_events = {};
            owner_scatter_ready = owner_scatter_pending = whole_component_staged = false;
            if (p1_component_owners()) retire_owned_shot(true);
            else decoder.reset();
            component_owned_reset_pending = false;
            throw;
        }
    }
};
SharedComponentDecoder::SharedComponentDecoder() noexcept = default;
SharedComponentDecoder::SharedComponentDecoder(ComponentDecoder&& decoder, std::vector<int> cpus,
        size_t parts, size_t limit, size_t shot_limit, size_t width, bool parallel_import, PrefixSchedule schedule,
        ContinuationPlacement placement, TimePartitionCuts::Geometry geometry)
    : SharedComponentDecoder(std::move(decoder), std::move(cpus), parts, limit, shot_limit, width, parallel_import,
                             schedule, placement, geometry, SharedDecoderConfiguration::from_environment()) {}
SharedComponentDecoder::SharedComponentDecoder(ComponentDecoder&& decoder, std::vector<int> cpus,
        size_t parts, size_t limit, size_t shot_limit, size_t width, bool parallel_import, PrefixSchedule schedule,
        ContinuationPlacement placement, TimePartitionCuts::Geometry geometry, SharedDecoderConfiguration configuration)
    : state(std::make_unique<State>(std::move(decoder), std::move(cpus), parts, limit, shot_limit, width, parallel_import,
                                   schedule, placement, geometry, configuration)) {}
SharedComponentDecoder::~SharedComponentDecoder() {
    if (state && state->team_active) std::terminate();
}
SharedComponentDecoder::SharedComponentDecoder(SharedComponentDecoder&& other) { *this = std::move(other); }
SharedComponentDecoder& SharedComponentDecoder::operator=(SharedComponentDecoder&& other) {
    if ((state && state->team_active) || (other.state && other.state->team_active))
        throw std::logic_error("An active shared decoder cannot be moved.");
    if (this != &other) state = std::move(other.state);
    return *this;
}
SharedComponentDecoder::State& SharedComponentDecoder::checked() {
    if (!state) throw std::logic_error("Shared decoder is empty or moved from.");
    return *state;
}
const SharedComponentDecoder::State& SharedComponentDecoder::checked() const {
    if (!state) throw std::logic_error("Shared decoder is empty or moved from.");
    return *state;
}
void SharedComponentDecoder::run_with_team(const std::function<void()>& callback) { checked().team(callback); }
void SharedComponentDecoder::prepare_for_mwpm(const std::vector<uint64_t>& events) {
    auto& s = checked();
    if (s.prefix_ready)
        throw std::logic_error("Solve or reset the prepared MWPM shot before preparing another.");
    if (s.decoder.num_observables() > 64)
        throw std::invalid_argument("Too many observables for bit-packed matching.");
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
    s.timing = &profile;
    ComponentDecodePhaseScope prefix(profile.prefix_ns BB_TIMELINE_ARG(&profile.prefix_interval));
#endif
    s.run(events, false, true);
}
MatchingResult SharedComponentDecoder::decode_component_chains(const std::vector<uint64_t>& events) {
    auto& s = checked();
    if (s.decoder.num_observables() > 64)
        throw std::invalid_argument("Too many observables for bit-packed matching.");
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
    s.timing = &profile;
    // Whole-chain envelope. The legacy prefix_ns/group_ns windows stay unset;
    // per-component preparation/continuation windows retain their meanings.
    ComponentDecodePhaseScope total(profile.total_ns BB_TIMELINE_ARG(&profile.total_interval));
#endif
    s.run<true>(events, false, true);
#if BB_STRUCTURED_PROFILE_TIMERS
    ComponentDecodePhaseScope merge(profile.merge_ns BB_TIMELINE_ARG(&profile.merge_interval));
#endif
    MatchingResult result;
    for (size_t c = 0; c < s.decoder.components().size(); ++c) result += s.local[c].result;
    return result;
}
MatchingResult SharedComponentDecoder::solve_prepared() {
    auto& s = checked();
    s.solve();
#if BB_STRUCTURED_PROFILE_TIMERS
    ComponentDecodePhaseScope merge(profile.merge_ns BB_TIMELINE_ARG(&profile.merge_interval));
#endif
    MatchingResult result;
    for (size_t c = 0; c < s.decoder.components().size(); ++c)
        result += s.local[c].result;
    return result;
}
void SharedComponentDecoder::reset_after_shot() {
    auto& s = checked();
    s.require_active();
    if (s.p1_component_owners()) {
        s.retire_owned_shot();
        return;
    }
    if (s.prefix_ready) {
        s.decoder.reset();
        s.prefix_ready = false;
    }
    s.clear_cells_after_shot();
    s.decoder.reset_after_shot();
}
#if BB_STRUCTURED_PROFILE_TIMERS
const PredecodeTimingStats& SharedComponentDecoder::partition_timing(size_t component, size_t bucket) const {
    return checked().local.at(component).partial.at(bucket).stats;
}
#endif
MatchingResult SharedComponentDecoder::decode(const std::vector<uint64_t>& events) {
    auto& s = checked();
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
    s.timing = &profile;
    ComponentDecodePhaseScope total(profile.total_ns BB_TIMELINE_ARG(&profile.total_interval));
#endif
    if (s.decoder.num_observables() > 64)
        throw std::invalid_argument("Too many observables for bit-packed matching.");
    s.run(events, false);
#if BB_STRUCTURED_PROFILE_TIMERS
    ComponentDecodePhaseScope merge(profile.merge_ns BB_TIMELINE_ARG(&profile.merge_interval));
#endif
    MatchingResult result;
    for (size_t c = 0; c < s.decoder.components().size(); ++c) result += s.local[c].result;
    return result;
}
std::vector<SharedMatch> SharedComponentDecoder::decode_to_matches(const std::vector<uint64_t>& events) {
    auto& s = checked();
#if BB_STRUCTURED_PROFILE_TIMERS
    profile = {};
    s.timing = &profile;
    ComponentDecodePhaseScope total(profile.total_ns BB_TIMELINE_ARG(&profile.total_interval));
#endif
    s.run(events, true);
#if BB_STRUCTURED_PROFILE_TIMERS
    ComponentDecodePhaseScope merge(profile.merge_ns BB_TIMELINE_ARG(&profile.merge_interval));
#endif
    std::vector<SharedMatch> out;
    for (auto& c : s.decoder.components()) {
        for (const auto& edge : c.mwpm.flooder.match_edges) {
            const auto& graph = c.mwpm.flooder.graph;
            out.push_back({static_cast<int64_t>(c.global_detectors[graph.node_id(edge.loc_from)]),
                edge.loc_to ? static_cast<int64_t>(c.global_detectors[graph.node_id(edge.loc_to)]) : -1,
                edge.obs_mask});
        }
    }
    return out;
}
void SharedComponentDecoder::reset() {
    auto& s = checked();
    if (s.team_active && State::active_team != &s)
        throw std::logic_error("Only the active callback may reset this decoder.");
    if (s.p1_component_owners() && State::active_team == &s) s.retire_owned_shot(true);
    else s.decoder.reset();
    s.prefix_ready = false;
    s.component_owned_reset_pending = false;
}
ComponentDecoder& SharedComponentDecoder::serial_decoder() { return checked().decoder; }
const ComponentDecoder& SharedComponentDecoder::serial_decoder() const { return checked().decoder; }
size_t SharedComponentDecoder::team_size() const { return checked().actual_threads; }
const std::vector<SharedThreadSnapshot>& SharedComponentDecoder::thread_snapshots() const { return checked().snapshots; }
#if BB_STRUCTURED_PROFILE_TIMERS
void SharedComponentDecoder::reset_import_pmu_profiles() {
    auto& s = checked();
    s.import_pmu_sequence = 0;
    s.import_pmu_sample = false;
    for (auto& worker : s.workers) {
        worker.import_aggregates[0] = {};
        worker.import_aggregates[1] = {};
    }
}
const import_pmu::ThreadCounters& SharedComponentDecoder::import_worker_counters(size_t worker) const {
    return checked().workers.at(worker).import_counters;
}
const std::array<import_pmu::Aggregate, 3>& SharedComponentDecoder::import_worker_aggregates(size_t worker) const {
    return checked().workers.at(worker).import_aggregates;
}
#endif
SharedRoute SharedComponentDecoder::component_route(size_t c) const { return checked().local.at(c).route; }
size_t SharedComponentDecoder::active_predecode_threads(size_t c) const { return checked().local.at(c).active_threads; }
#if BB_COMPONENT_OWNER_AUDIT
std::array<uint64_t, 3> SharedComponentDecoder::lifecycle_thread_ids(size_t c) const {
    return checked().lifecycle_threads.at(c);
}
#endif
const char* SharedComponentDecoder::fixed_seed_construction_name() const {
    return checked().fixed_construction == State::FixedConstruction::conflict_first ? "conflict_first" : "fused";
}
bool SharedComponentDecoder::fixed_seed_construction_enabled() const {
    const auto& s = checked();
    return s.prepare_entry == &State::prepare_group_owner_shot && s.decoder.fixed_seed_storage;
}
const char* SharedComponentDecoder::fixed_seed_input_name() const {
    return checked().fixed_seed_input == State::FixedSeedInput::indexed ? "indexed" : "scan";
}
const char* SharedComponentDecoder::import_preparation_name() const {
    const auto mode = checked().import_preparation;
    return mode == State::ImportPreparation::captained_serial ? "captained_serial"
        : mode == State::ImportPreparation::captained_reverse ? "captained_reverse"
        : mode == State::ImportPreparation::captained_overlap ? "captained_overlap"
        : mode == State::ImportPreparation::captained ? "captained"
        : mode == State::ImportPreparation::parallel ? "parallel" : "serial";
}
const char* SharedComponentDecoder::import_reset_owner_name() const {
    return checked().import_reset_owner == State::ImportResetOwner::captain ? "captain" : "importers";
}
bool SharedComponentDecoder::import_preparation_enabled() const {
    const auto& s = checked();
    // P1 also owns fixed seed backing, but its serial prefixes do not enter
    // the fixed partition import-preparation route.
    return s.import_preparation != State::ImportPreparation::serial && s.parallel_import &&
        fixed_seed_construction_enabled();
}
size_t SharedComponentDecoder::fixed_seed_index_entries() const {
    return checked().decoder.fixed_bucket_detector_ids.size();
}

struct DoubleBufferComponentDecoder::State {
    using SharedState = SharedComponentDecoder::State;
    enum class Command : uint32_t { ready, decode, result, reset, stop };
    struct alignas(64) Slot {
        std::atomic<Command> command{Command::ready};
        const std::vector<uint64_t>* events = nullptr;
        MatchingResult result;
        std::exception_ptr error;
        bool dirty = false;
#if BB_TAILRESET_SYNDROME_STATS
        std::array<TailResetSyndromeCounts, 2> syndromes;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
        size_t shot = SIZE_MAX;
        DoubleBufferShotProfile decode_profile;
        DoubleBufferResetProfile reset_profile;
#endif
    };
    std::array<SharedComponentDecoder, 2> buffers;
    std::array<Slot, 2> slots;
    size_t requested_group, actual_threads = 0, sequence = 0;
    bool team_active = false;
    std::exception_ptr callback_error;
    using DecodeEntry = MatchingResult (State::*)(const std::vector<uint64_t>&);
    DecodeEntry decode_entry;
    using TeamEntry = void (State::*)(const std::function<void()>&);
    TeamEntry team_entry = &State::team_paired;
    using DrainEntry = void (State::*)();
    DrainEntry drain_entry = &State::drain_paired;
    inline static thread_local State* active = nullptr;
#if BB_STRUCTURED_PROFILE_TIMERS
    DoubleBufferShotProfile last_profile;
    std::array<DoubleBufferResetProfile, 2> completed_resets;
#endif
    State(SharedComponentDecoder&& even, SharedComponentDecoder&& odd, Schedule schedule)
        : buffers{std::move(even), std::move(odd)}, requested_group(buffers[0].checked().requested_threads) {
        if (buffers[1].checked().requested_threads != requested_group)
            throw std::invalid_argument("Double buffers require equally sized thread groups.");
        if (buffers[0].serial_decoder().num_observables() != buffers[1].serial_decoder().num_observables())
            throw std::invalid_argument("Double buffers require the same observable count.");
        if (schedule == Schedule::single_thread && (requested_group != 1 ||
            buffers[0].thread_snapshots()[0].requested_cpu != buffers[1].thread_snapshots()[0].requested_cpu))
            throw std::invalid_argument("Single-thread double buffers require one identical CPU per logical group.");
        std::vector<int> cpus;
        for (const auto& buffer : buffers) {
            for (const auto& thread : buffer.thread_snapshots()) {
                if (thread.requested_cpu < 0) continue;
                if (schedule != Schedule::single_thread &&
                    std::find(cpus.begin(), cpus.end(), thread.requested_cpu) != cpus.end())
                    throw std::invalid_argument("Double-buffer groups require disjoint explicit CPUs.");
                cpus.push_back(thread.requested_cpu);
            }
        }
        switch (schedule) {
            case Schedule::overlap: decode_entry = &State::decode_one<true>; break;
            case Schedule::serial_control: decode_entry = &State::decode_one<false>; break;
            case Schedule::single_thread:
                decode_entry = &State::decode_single;
                team_entry = &State::team_single;
                drain_entry = &State::drain_single;
                break;
            default: throw std::invalid_argument("Unknown double-buffer schedule.");
        }
        // Construction can consume previously used standalone decoders.
        // All state is made clean before the two independent teams acquire it.
        for (auto& buffer : buffers) buffer.reset();
    }
    void require_active() const {
        if (active != this)
            throw std::logic_error("Double-buffer decode requires its run_with_team callback.");
    }
    void reset_buffer(size_t b) {
        auto& slot = slots[b];
        if (!slot.dirty) return;
#if BB_STRUCTURED_PROFILE_TIMERS
        slot.reset_profile = {};
        slot.reset_profile.shot = slot.shot;
        slot.reset_profile.buffer = b;
        ComponentDecodePhaseScope phase(slot.reset_profile.reset_ns BB_TIMELINE_ARG(&slot.reset_profile.reset_interval));
#endif
        buffers[b].reset_after_shot();
        slot.dirty = false;
    }
    void compute(size_t b, const std::vector<uint64_t>& events) {
        auto& slot = slots[b];
        slot.dirty = true;
#if BB_STRUCTURED_PROFILE_TIMERS
        slot.decode_profile = {};
        slot.decode_profile.shot = slot.shot;
        slot.decode_profile.buffer = b;
        {
        ComponentDecodePhaseScope phase(slot.decode_profile.decode_ns BB_TIMELINE_ARG(&slot.decode_profile.decode_interval));
#endif
#if BB_TAILRESET_SYNDROME_STATS
        if (buffers[b].checked().decoder.components().size() != slot.syndromes.size())
            throw std::logic_error("Syndrome collection requires independent X/Z components");
#endif
        slot.result = buffers[b].decode_component_chains(events);
#if BB_TAILRESET_SYNDROME_STATS
        // The component chains have joined. Copy their pre-MWPM snapshots
        // before publishing this buffer's result and scheduling its reset.
        slot.syndromes = buffers[b].checked().syndromes;
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
        }
        slot.decode_profile.shared = buffers[b].profile;
        const auto& s = buffers[b].checked();
        slot.decode_profile.component_decode = s.decoder.profile;
        slot.decode_profile.component_count = s.decoder.components().size();
        slot.decode_profile.partition_count = s.partitions;
        for (size_t c = 0; c < s.decoder.components().size(); ++c) {
            slot.decode_profile.component[c] = s.decoder.components()[c].mwpm.predecoder.stats;
            if (s.local[c].route == SharedRoute::partitioned)
                for (size_t part = 0; part <= s.partitions; ++part)
                    slot.decode_profile.partition[c][part] = s.local[c].partial[part].stats;
        }
#endif
    }
    void acquire_ready() {
        while (slots[1].command.load(std::memory_order_acquire) != Command::ready)
            SharedState::pause();
#if BB_STRUCTURED_PROFILE_TIMERS
        completed_resets[1] = slots[1].reset_profile;
#endif
    }
    void acquire_result() {
        while (slots[1].command.load(std::memory_order_acquire) != Command::result)
            SharedState::pause();
    }
    void drain() { (this->*drain_entry)(); }
    void drain_paired() {
        require_active();
        // An exceptional odd decode may still be computing. Join before
        // discarding its result or asking its owning group to retire state.
        auto command = slots[1].command.load(std::memory_order_acquire);
        if (command == Command::decode) {
            acquire_result();
            command = Command::result;
        }
        if (command == Command::result)
            slots[1].command.store(Command::reset, std::memory_order_release);
        reset_buffer(0);
        acquire_ready();
#if BB_STRUCTURED_PROFILE_TIMERS
        completed_resets[0] = slots[0].reset_profile;
#endif
    }
    struct ActiveOwner {
        SharedState* previous;
        explicit ActiveOwner(SharedState& owner) : previous(SharedState::active_team) {
            SharedState::active_team = &owner;
        }
        ~ActiveOwner() { SharedState::active_team = previous; }
    };
    void drain_single() {
        require_active();
        for (size_t b = 0; b < 2; ++b) {
            ActiveOwner owner(buffers[b].checked());
            reset_buffer(b);
#if BB_STRUCTURED_PROFILE_TIMERS
            completed_resets[b] = slots[b].reset_profile;
#endif
        }
    }
    MatchingResult decode_single(const std::vector<uint64_t>& events) {
        require_active();
        [[maybe_unused]] const size_t shot = sequence++;
        const size_t b = shot & 1;
#if BB_STRUCTURED_PROFILE_TIMERS
        DoubleBufferShotProfile observation;
#endif
        try {
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(observation.prior_reset_ns BB_TIMELINE_ARG(&observation.prior_reset_interval));
#endif
                drain_single();
            }
            ActiveOwner owner(buffers[b].checked());
#if BB_STRUCTURED_PROFILE_TIMERS
            slots[b].shot = shot;
#endif
            compute(b, events);
#if BB_STRUCTURED_PROFILE_TIMERS
            last_profile = slots[b].decode_profile;
            last_profile.prior_reset_ns = observation.prior_reset_ns;
#if BB_STRUCTURED_PROFILE_TIMELINE
            last_profile.prior_reset_interval = observation.prior_reset_interval;
#endif
#endif
            return slots[b].result;
        } catch (...) {
            const auto error = std::current_exception();
            drain_single();
            std::rethrow_exception(error);
        }
    }
    template <bool Overlap>
    MatchingResult decode_one(const std::vector<uint64_t>& events) {
        require_active();
        const size_t b = sequence & 1;
        [[maybe_unused]] const size_t shot = sequence++;
#if BB_STRUCTURED_PROFILE_TIMERS
        last_profile = {};
        last_profile.shot = shot;
        last_profile.buffer = b;
#endif
        try {
            if constexpr (!Overlap) drain();
            if (b == 0) {
                // Buffer zero is reset during the preceding odd call (or by
                // drain). Buffer one's reset continues on its disjoint group.
#if BB_STRUCTURED_PROFILE_TIMERS
                slots[0].shot = shot;
#endif
                compute(0, events);
#if BB_STRUCTURED_PROFILE_TIMERS
                last_profile = slots[0].decode_profile;
#endif
                return slots[0].result;
            }
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(last_profile.ready_wait_ns BB_TIMELINE_ARG(&last_profile.ready_wait_interval));
#endif
                acquire_ready();
            }
            auto& slot = slots[1];
            slot.events = &events;
#if BB_STRUCTURED_PROFILE_TIMERS
            slot.shot = shot;
#endif
            slot.error = nullptr;
            slot.command.store(Command::decode, std::memory_order_release);
            if constexpr (Overlap) {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(last_profile.prior_reset_ns BB_TIMELINE_ARG(&last_profile.prior_reset_interval));
#endif
                reset_buffer(0);
            }
#if BB_STRUCTURED_PROFILE_TIMERS
            completed_resets[0] = slots[0].reset_profile;
#endif
            {
#if BB_STRUCTURED_PROFILE_TIMERS
                ComponentDecodePhaseScope phase(last_profile.result_wait_ns BB_TIMELINE_ARG(&last_profile.result_wait_interval));
#endif
                acquire_result();
            }
            // Release the borrowed input and copy result/exception while the
            // remote captain is parked at Result. Reset never sees this input.
            slot.events = nullptr;
            const auto result = slot.result;
            const auto error = slot.error;
#if BB_STRUCTURED_PROFILE_TIMERS
            last_profile.decode_ns = slot.decode_profile.decode_ns;
            last_profile.shared = slot.decode_profile.shared;
            last_profile.component_decode = slot.decode_profile.component_decode;
            last_profile.component = slot.decode_profile.component;
            last_profile.partition = slot.decode_profile.partition;
            last_profile.component_count = slot.decode_profile.component_count;
            last_profile.partition_count = slot.decode_profile.partition_count;
#if BB_STRUCTURED_PROFILE_TIMELINE
            last_profile.decode_interval = slot.decode_profile.decode_interval;
#endif
#endif
            slot.command.store(Command::reset, std::memory_order_release);
            if (error) std::rethrow_exception(error);
            if constexpr (!Overlap) acquire_ready();
            return result;
        } catch (...) {
            const auto error = std::current_exception();
            drain();
            std::rethrow_exception(error);
        }
    }
    void remote_loop() noexcept {
        auto& slot = slots[1];
        while (true) {
            const auto command = slot.command.load(std::memory_order_acquire);
            if (command == Command::stop) return;
            if (command == Command::decode) {
                try { compute(1, *slot.events); }
                catch (...) { slot.error = std::current_exception(); }
                slot.command.store(Command::result, std::memory_order_release);
            } else if (command == Command::reset) {
                // Decoder reset is a no-allocation cleanup operation. Every
                // path is owned by this captain with its original helpers.
                reset_buffer(1);
                slot.command.store(Command::ready, std::memory_order_release);
            } else {
                SharedState::pause();
            }
        }
    }
    static void stop_helpers(SharedState& s) {
        for (size_t id = 1; id < s.requested_threads; ++id)
            s.workers[id].command.store(SharedState::stop, std::memory_order_release);
        for (size_t id = 1; id < s.requested_threads; ++id)
            while (!s.workers[id].stopped.load(std::memory_order_acquire)) SharedState::pause();
    }
    void team(const std::function<void()>& callback) { (this->*team_entry)(callback); }
    void team_single(const std::function<void()>& callback) {
        if (team_active)
            throw std::logic_error("Double-buffer run_with_team requires an inactive decoder.");
        team_active = true;
        auto& odd = buffers[1].checked();
        odd.team_active = true;
        try {
            buffers[0].run_with_team([&] {
                actual_threads = buffers[0].team_size();
                odd.actual_threads = actual_threads;
                odd.snapshots = buffers[0].thread_snapshots();
                auto* previous_pipeline = active;
                active = this;
                std::exception_ptr error;
                try { callback(); }
                catch (...) { error = std::current_exception(); }
                try { drain_single(); }
                catch (...) { if (!error) error = std::current_exception(); }
                active = previous_pipeline;
                if (error) std::rethrow_exception(error);
            });
        } catch (...) {
            odd.team_active = team_active = false;
            throw;
        }
        odd.team_active = team_active = false;
    }
    void team_paired(const std::function<void()>& callback) {
#if !defined(_OPENMP)
        throw std::runtime_error("DoubleBufferComponentDecoder requires an OpenMP-enabled build.");
#else
        if (team_active || omp_get_level() != 0)
            throw std::logic_error("Double-buffer run_with_team requires an inactive decoder outside OpenMP.");
        SharedState::Affinity caller_mask;
        caller_mask.capture();
        for (auto& buffer : buffers) {
            auto& s = buffer.checked();
            for (size_t id = 0; id < requested_group; ++id) {
                auto& worker = s.workers[id];
                worker.command.store(SharedState::idle, std::memory_order_relaxed);
                worker.ready.store(false, std::memory_order_relaxed);
                worker.stopped.store(false, std::memory_order_relaxed);
                worker.startup_error = worker.teardown_error = worker.job_error = nullptr;
                s.snapshots[id].thread_id = 0;
                s.snapshots[id].affinity.clear();
            }
            s.team_active = true;
        }
        callback_error = nullptr;
        slots[1].command.store(Command::ready, std::memory_order_relaxed);
        team_active = true;
        const int requested = static_cast<int>(2 * requested_group);
        bool startup_ok = false;
        auto& pipeline = *this;
#pragma omp parallel num_threads(requested) default(none) shared(pipeline, requested, caller_mask, callback, startup_ok)
        {
            const size_t global = static_cast<size_t>(omp_get_thread_num());
            const size_t b = global / pipeline.requested_group;
            const size_t id = global % pipeline.requested_group;
            const int count = omp_get_num_threads();
            if (global == 0) pipeline.actual_threads = count;
            if (count == requested) {
                auto& s = pipeline.buffers[b].checked();
                SharedState::Affinity previous;
                try { s.initialize_thread(id, previous, caller_mask); }
                catch (...) { s.workers[id].startup_error = std::current_exception(); }
                if (id == 0) s.actual_threads = pipeline.requested_group;
#pragma omp barrier
                if (global == 0) {
                    startup_ok = true;
                    for (const auto& buffer : pipeline.buffers)
                        for (size_t k = 0; k < pipeline.requested_group; ++k)
                            startup_ok &= !buffer.checked().workers[k].startup_error;
                }
#pragma omp barrier
                if (startup_ok) {
                    if (id != 0) {
                        s.worker_loop(id);
                    } else {
                        auto* previous_team = SharedState::active_team;
                        SharedState::active_team = &s;
                        if (b == 0) {
                            auto* previous_pipeline = active;
                            active = &pipeline;
                            try { callback(); }
                            catch (...) { pipeline.callback_error = std::current_exception(); }
                            try { pipeline.drain(); }
                            catch (...) {
                                if (!pipeline.callback_error) pipeline.callback_error = std::current_exception();
                            }
                            pipeline.slots[1].command.store(Command::stop, std::memory_order_release);
                            active = previous_pipeline;
                        } else {
                            pipeline.remote_loop();
                        }
                        stop_helpers(s);
                        SharedState::active_team = previous_team;
                    }
                }
                s.finish_thread(id, previous);
                s.workers[id].stopped.store(true, std::memory_order_release);
            } else if (global == 0) {
                pipeline.callback_error = std::make_exception_ptr(
                    std::runtime_error("OpenMP runtime did not provide the requested double-buffer team."));
            }
        }
        team_active = false;
        for (auto& buffer : buffers) buffer.checked().team_active = false;
        try { caller_mask.restore(); }
        catch (...) { if (!callback_error) callback_error = std::current_exception(); }
        for (const auto& buffer : buffers)
            for (size_t id = 0; id < requested_group; ++id)
                if (buffer.checked().workers[id].startup_error)
                    std::rethrow_exception(buffer.checked().workers[id].startup_error);
        if (callback_error) std::rethrow_exception(callback_error);
        for (const auto& buffer : buffers)
            for (size_t id = 0; id < requested_group; ++id)
                if (buffer.checked().workers[id].teardown_error)
                    std::rethrow_exception(buffer.checked().workers[id].teardown_error);
#endif
    }
};
DoubleBufferComponentDecoder::DoubleBufferComponentDecoder() noexcept = default;
DoubleBufferComponentDecoder::DoubleBufferComponentDecoder(
        SharedComponentDecoder&& even, SharedComponentDecoder&& odd, Schedule schedule)
    : state(std::make_unique<State>(std::move(even), std::move(odd), schedule)) {}
DoubleBufferComponentDecoder::~DoubleBufferComponentDecoder() {
    if (state && state->team_active) std::terminate();
}
DoubleBufferComponentDecoder::DoubleBufferComponentDecoder(DoubleBufferComponentDecoder&& other) {
    *this = std::move(other);
}
DoubleBufferComponentDecoder& DoubleBufferComponentDecoder::operator=(DoubleBufferComponentDecoder&& other) {
    if ((state && state->team_active) || (other.state && other.state->team_active))
        throw std::logic_error("An active double-buffer decoder cannot be moved.");
    if (this != &other) state = std::move(other.state);
    return *this;
}
DoubleBufferComponentDecoder::State& DoubleBufferComponentDecoder::checked() {
    if (!state) throw std::logic_error("Double-buffer decoder is empty or moved from.");
    return *state;
}
const DoubleBufferComponentDecoder::State& DoubleBufferComponentDecoder::checked() const {
    if (!state) throw std::logic_error("Double-buffer decoder is empty or moved from.");
    return *state;
}
void DoubleBufferComponentDecoder::run_with_team(const std::function<void()>& callback) { checked().team(callback); }
MatchingResult DoubleBufferComponentDecoder::decode(const std::vector<uint64_t>& events) {
    auto& s = checked();
    return (s.*s.decode_entry)(events);
}
void DoubleBufferComponentDecoder::drain() { checked().drain(); }
size_t DoubleBufferComponentDecoder::team_size() const { return checked().actual_threads; }
size_t DoubleBufferComponentDecoder::group_size() const { return checked().requested_group; }
size_t DoubleBufferComponentDecoder::next_buffer() const { return checked().sequence & 1; }
#if BB_TAILRESET_SYNDROME_STATS
std::array<TailResetSyndromeCounts, 2> DoubleBufferComponentDecoder::syndrome_statistics() const {
    const auto& s = checked();
    s.require_active();
    if (!s.sequence) throw std::logic_error("No decoded shot to collect");
    // decode acquired the remote result before its reset command was published.
    // Reset does not mutate this slot snapshot; the next decode owns reuse.
    return s.slots[(s.sequence - 1) & 1].syndromes;
}
#endif
const std::vector<SharedThreadSnapshot>& DoubleBufferComponentDecoder::thread_snapshots(size_t buffer) const {
    return checked().buffers.at(buffer).thread_snapshots();
}
const char* DoubleBufferComponentDecoder::fixed_seed_construction_name(size_t buffer) const {
    return checked().buffers.at(buffer).fixed_seed_construction_name();
}
bool DoubleBufferComponentDecoder::fixed_seed_construction_enabled(size_t buffer) const {
    return checked().buffers.at(buffer).fixed_seed_construction_enabled();
}
const char* DoubleBufferComponentDecoder::fixed_seed_input_name(size_t buffer) const {
    return checked().buffers.at(buffer).fixed_seed_input_name();
}
const char* DoubleBufferComponentDecoder::import_preparation_name(size_t buffer) const {
    return checked().buffers.at(buffer).import_preparation_name();
}
const char* DoubleBufferComponentDecoder::import_reset_owner_name(size_t buffer) const {
    return checked().buffers.at(buffer).import_reset_owner_name();
}
bool DoubleBufferComponentDecoder::import_preparation_enabled(size_t buffer) const {
    return checked().buffers.at(buffer).import_preparation_enabled();
}
size_t DoubleBufferComponentDecoder::fixed_seed_index_entries(size_t buffer) const {
    return checked().buffers.at(buffer).fixed_seed_index_entries();
}
const ComponentDecoder& DoubleBufferComponentDecoder::serial_decoder(size_t buffer) const {
    const auto& s = checked();
    if (s.team_active) throw std::logic_error("Double-buffer metadata requires an inactive team.");
    return s.buffers.at(buffer).serial_decoder();
}
#if BB_COMPONENT_OWNER_AUDIT
std::array<uint64_t, 3> DoubleBufferComponentDecoder::lifecycle_thread_ids(size_t buffer, size_t component) const {
    const auto& s = checked();
    if (s.team_active) throw std::logic_error("Lifecycle audit requires a joined team.");
    return s.buffers.at(buffer).lifecycle_thread_ids(component);
}
#endif
#if BB_STRUCTURED_PROFILE_TIMERS
void DoubleBufferComponentDecoder::reset_import_pmu_profiles() {
    auto& s = checked();
    s.drain();
    for (auto& buffer : s.buffers) buffer.reset_import_pmu_profiles();
}
const import_pmu::ThreadCounters& DoubleBufferComponentDecoder::import_worker_counters(size_t buffer, size_t worker) const {
    return checked().buffers.at(buffer).import_worker_counters(worker);
}
const std::array<import_pmu::Aggregate, 3>& DoubleBufferComponentDecoder::import_worker_aggregates(size_t buffer, size_t worker) const {
    return checked().buffers.at(buffer).import_worker_aggregates(worker);
}
const DoubleBufferShotProfile& DoubleBufferComponentDecoder::shot_profile() const { return checked().last_profile; }
const DoubleBufferResetProfile& DoubleBufferComponentDecoder::completed_reset_profile(size_t buffer) const {
    return checked().completed_resets.at(buffer);
}
#endif
}  // namespace beyond_blossom::structured
