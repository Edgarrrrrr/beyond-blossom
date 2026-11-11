#ifndef PM_EXPERIMENT_IMPORT_PMU_H
#define PM_EXPERIMENT_IMPORT_PMU_H

// Cascade Lake MEM_LOAD_RETIRED hit/miss pairs from Intel perfmon CLX v1.25.
// https://github.com/intel/perfmon/blob/main/CLX/events/cascadelakex_core.json
// These are retired-load event ratios, not all-request cache probabilities.
// L1_HIT includes software prefetch/locked instructions; FB_HIT is a separate
// event, and split loads may satisfy more than one event qualification.
// snoop mode records MEM_LOAD_L3_HIT_RETIRED.XSNP_HIT / XSNP_HITM.
// These are peer-cache snoop outcomes, never a cache hit/miss denominator.
// https://perfmon-events.intel.com/platforms/cascadelakex/core-events/core/

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <chrono>
#include <sys/types.h>
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
#define PM_IMPORT_PMU_LINUX_X86 1
#include <cpuid.h>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#else
#define PM_IMPORT_PMU_LINUX_X86 0
#endif

namespace pm::structured::import_pmu {

struct Aggregate {
    uint64_t sampled_calls = 0, roots = 0, neighbor_checks = 0;
    uint64_t hit = 0, miss = 0, cycles = 0, instructions = 0, ref_cycles = 0;
    uint64_t wall_ns = 0, time_enabled_ns = 0, time_running_ns = 0, invalid_samples = 0;
};

struct Snapshot {
    uint64_t hit = 0, miss = 0, cycles = 0, instructions = 0, ref_cycles = 0, wall_ns = 0;
    uint64_t time_enabled_ns = 0, time_running_ns = 0;
    bool valid = false, not_multiplexed = false;
};

struct Audit {
    const char* mode = "off";
    const char* status = "disabled";
    bool configured = false, opened = false, closed = false;
    bool cap_user_rdpmc = false, not_multiplexed = false, mmap_read_consistent = false;
    pid_t owner_tid = 0;
    uint64_t hit_config = 0, miss_config = 0;
    uint64_t time_enabled_ns = 0, time_running_ns = 0, invalid_snapshots = 0;
    int error_errno = 0;
    std::array<uint64_t, 5> final_values{};  // cycles, instructions, hit, miss, reference cycles
};

#if PM_IMPORT_PMU_LINUX_X86
class ThreadCounters {
    struct Counter {
        int fd = -1;
        perf_event_mmap_page* page = nullptr;
        uint64_t id = 0;
    };
    struct CounterReading {
        uint64_t count = 0, enabled = 0, running = 0;
        bool valid = false, cap = false;
    };
    std::array<Counter, 5> counters_{};
    Audit audit_{};
    size_t page_size_ = 0;
    bool active_ = false;

    static pid_t tid() noexcept {
        static thread_local const pid_t value = static_cast<pid_t>(syscall(SYS_gettid));
        return value;
    }
    static uint64_t clock_ns() noexcept {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    static uint64_t rdtsc() noexcept {
        uint32_t lo, hi;
        asm volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) :: "memory");
        return (uint64_t(hi) << 32) | lo;
    }
    static uint64_t rdpmc(uint32_t index) noexcept {
        uint32_t lo, hi;
        asm volatile("lfence\n\trdpmc\n\tlfence" : "=a"(lo), "=d"(hi) : "c"(index) : "memory");
        return (uint64_t(hi) << 32) | lo;
    }
    static bool supported_cpu() noexcept {
        unsigned eax, ebx, ecx, edx;
        if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx) ||
            ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e)
            return false;
        if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
        unsigned family = (eax >> 8) & 15;
        unsigned model = (eax >> 4) & 15;
        if (family == 15) family += (eax >> 20) & 255;
        if (family == 6 || family == 15) model |= ((eax >> 16) & 15) << 4;
        return family == 6 && model == 85;
    }
    static CounterReading read_page(const Counter& counter, bool require_active) noexcept {
        CounterReading answer;
        if (!counter.page) return answer;
        const auto* p = counter.page;
        for (unsigned attempt = 0; attempt < 1000; ++attempt) {
            const uint32_t sequence = __atomic_load_n(&p->lock, __ATOMIC_ACQUIRE);
            if (sequence & 1) continue;
            std::atomic_signal_fence(std::memory_order_seq_cst);
            const uint64_t capabilities = p->capabilities;
            const bool cap = capabilities & (uint64_t{1} << 2);
            const bool cap_time = capabilities & (uint64_t{1} << 3);
            const bool short_time = capabilities & (uint64_t{1} << 5);
            const uint32_t index = p->index;
            const uint16_t width = p->pmc_width;
            uint64_t count = uint64_t(p->offset);
            uint64_t enabled = p->time_enabled, running = p->time_running;
            bool valid = cap && (!require_active || index != 0) && (!index || (width > 0 && width <= 64));
            // Kernel UAPI time conversion, inside the same mmap sequence lock.
            // Always extrapolate for per-phase enabled/running deltas, even
            // when no scaling is needed. A frozen disabled page needs no delta.
            if (index && cap_time) {
                uint64_t cyc = rdtsc();
                const unsigned shift = p->time_shift;
                const uint64_t mult = p->time_mult, offset = p->time_offset;
                if (short_time) cyc = p->time_cycles + ((cyc - p->time_cycles) & p->time_mask);
                if (shift >= 64) valid = false;
                else {
                    const uint64_t quot = cyc >> shift;
                    const uint64_t rem = cyc & ((uint64_t{1} << shift) - 1);
                    const uint64_t delta = offset + quot * mult + ((rem * mult) >> shift);
                    enabled += delta;
                    running += delta;
                }
            } else if (require_active && !cap_time) {
                valid = false;
            }
            if (index && cap && width > 0 && width <= 64) {
                const uint64_t raw = rdpmc(index - 1);
                const int64_t signed_pmc = width == 64 ? int64_t(raw)
                    : (int64_t(raw << (64 - width)) >> (64 - width));
                count += uint64_t(signed_pmc);
            }
            std::atomic_signal_fence(std::memory_order_seq_cst);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&p->lock, __ATOMIC_RELAXED) == sequence)
                return {count, enabled, running, valid, cap};
        }
        return answer;
    }
    void cleanup() noexcept {
        for (auto& counter : counters_) {
            if (counter.page) munmap(counter.page, page_size_);
            if (counter.fd >= 0) ::close(counter.fd);
            counter = {};
        }
        active_ = false;
    }
    [[noreturn]] void fail(const char* stage, int number = errno) {
        audit_.error_errno = number;
        audit_.status = stage;
        cleanup();
        throw std::runtime_error(std::string("import PMU ") + stage + ": " + std::strerror(number));
    }

  public:
    ThreadCounters() = default;
    ThreadCounters(const ThreadCounters&) = delete;
    ThreadCounters& operator=(const ThreadCounters&) = delete;
    ~ThreadCounters() {
        // An open group must be closed on the worker that owns it. Main-thread
        // object destruction after worker teardown sees only closed fds.
        if (active_) {
            if (audit_.owner_tid != tid()) std::terminate();
            cleanup();
        }
    }
    bool enabled() const noexcept { return active_; }
    const Audit& audit() const noexcept { return audit_; }
    static size_t sample_period() noexcept { return 64; }

    void open_from_environment() {
        if (active_) throw std::logic_error("import PMU group already open");
        audit_ = {};
        const char* mode = std::getenv("PM_IMPORT_PMU");
        if (!mode || !*mode || !std::strcmp(mode, "off")) return;
        if (!std::strcmp(mode, "l1")) { audit_.mode = "l1"; audit_.hit_config = 0x01d1; audit_.miss_config = 0x08d1; }
        else if (!std::strcmp(mode, "l2")) { audit_.mode = "l2"; audit_.hit_config = 0x02d1; audit_.miss_config = 0x10d1; }
        else if (!std::strcmp(mode, "llc")) { audit_.mode = "llc"; audit_.hit_config = 0x04d1; audit_.miss_config = 0x20d1; }
        else if (!std::strcmp(mode, "snoop")) { audit_.mode = "snoop"; audit_.hit_config = 0x02d2; audit_.miss_config = 0x04d2; }
        else throw std::invalid_argument("PM_IMPORT_PMU must be off/l1/l2/llc/snoop");
        audit_.configured = true;
        audit_.owner_tid = tid();
        if (!supported_cpu()) fail("unsupported_cpu", ENOTSUP);
        const long size = sysconf(_SC_PAGESIZE);
        if (size <= 0) fail("page_size", EINVAL);
        page_size_ = size_t(size);
        const std::array<uint64_t, 5> configs{PERF_COUNT_HW_CPU_CYCLES, PERF_COUNT_HW_INSTRUCTIONS,
            audit_.hit_config, audit_.miss_config, PERF_COUNT_HW_REF_CPU_CYCLES};
        for (size_t i = 0; i < counters_.size(); ++i) {
            perf_event_attr attr{};
            attr.size = sizeof(attr);
            attr.type = i < 2 || i == 4 ? PERF_TYPE_HARDWARE : PERF_TYPE_RAW;
            attr.config = configs[i];
            attr.disabled = i == 0;
            attr.pinned = i == 0;
            attr.exclude_kernel = true;
            attr.exclude_hv = true;
            attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
                               PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;
            auto& counter = counters_[i];
            counter.fd = int(syscall(SYS_perf_event_open, &attr, 0, -1,
                                     i == 0 ? -1 : counters_[0].fd, PERF_FLAG_FD_CLOEXEC));
            if (counter.fd < 0) fail("open_failed");
            if (ioctl(counter.fd, PERF_EVENT_IOC_ID, &counter.id) < 0) fail("id_failed");
            void* mapped = mmap(nullptr, page_size_, PROT_READ, MAP_SHARED, counter.fd, 0);
            if (mapped == MAP_FAILED) fail("mmap_failed");
            counter.page = static_cast<perf_event_mmap_page*>(mapped);
        }
        if (ioctl(counters_[0].fd, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) < 0) fail("reset_failed");
        if (ioctl(counters_[0].fd, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0) fail("enable_failed");
        active_ = true;
        audit_.opened = true;
        audit_.cap_user_rdpmc = true;
        const Snapshot initial = snapshot();
        if (!initial.valid || !initial.not_multiplexed) fail("unscheduled_or_rdpmc_unavailable", EBUSY);
        audit_.status = "active";
    }

    Snapshot snapshot() noexcept {
        Snapshot result;
        if (!active_) return result;
        if (audit_.owner_tid != tid()) {
            ++audit_.invalid_snapshots;
            return result;
        }
        std::array<CounterReading, 5> readings;
        bool valid = true, unmultiplexed = true;
        for (size_t i = 0; i < readings.size(); ++i) {
            readings[i] = read_page(counters_[i], true);
            valid &= readings[i].valid;
            unmultiplexed &= readings[i].enabled == readings[i].running;
            audit_.cap_user_rdpmc &= readings[i].cap;
        }
        result.cycles = readings[0].count;
        result.instructions = readings[1].count;
        result.hit = readings[2].count;
        result.miss = readings[3].count;
        result.ref_cycles = readings[4].count;
        result.time_enabled_ns = readings[0].enabled;
        result.time_running_ns = readings[0].running;
        result.wall_ns = clock_ns();
        result.not_multiplexed = unmultiplexed;
        result.valid = valid && unmultiplexed;
        if (!result.valid) ++audit_.invalid_snapshots;
        return result;
    }

    void close_on_owner() {
        if (!active_) { audit_.closed = true; return; }
        if (audit_.owner_tid != tid()) throw std::logic_error("import PMU close on non-owner thread");
        if (ioctl(counters_[0].fd, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0) fail("disable_failed");
        std::array<uint64_t, 13> values{};
        if (::read(counters_[0].fd, values.data(), sizeof(values)) != ssize_t(sizeof(values)))
            fail("final_group_read_failed", errno ? errno : EIO);
        if (values[0] != 5) fail("final_group_size_mismatch", EIO);
        audit_.time_enabled_ns = values[1];
        audit_.time_running_ns = values[2];
        audit_.not_multiplexed = values[1] == values[2] && values[2] > 0;
        audit_.mmap_read_consistent = true;
        for (size_t i = 0; i < counters_.size(); ++i) {
            bool found = false;
            for (size_t j = 0; j < counters_.size(); ++j) {
                if (values[4 + 2 * j] == counters_[i].id) {
                    audit_.final_values[i] = values[3 + 2 * j];
                    found = true;
                }
            }
            const CounterReading frozen = read_page(counters_[i], false);
            audit_.mmap_read_consistent &= found && frozen.valid && frozen.count == audit_.final_values[i];
        }
        audit_.closed = true;
        cleanup();
        if (!audit_.not_multiplexed || !audit_.mmap_read_consistent || audit_.invalid_snapshots) {
            audit_.status = "validation_failed";
            throw std::runtime_error("import PMU validation failed (multiplexing, mmap/read mismatch or invalid snapshot)");
        }
        audit_.status = "passed";
    }
};
#else
// Phase wall clocks remain available on other platforms. These Intel-specific
// counters cannot be requested there or reported as measured hardware events.
class ThreadCounters {
    Audit audit_{};
  public:
    bool enabled() const noexcept { return false; }
    const Audit& audit() const noexcept { return audit_; }
    static size_t sample_period() noexcept { return 64; }
    void open_from_environment() {
        audit_ = {};
        const char* mode = std::getenv("PM_IMPORT_PMU");
        if (!mode || !*mode || !std::strcmp(mode, "off")) return;
        audit_.configured = true;
        audit_.status = "unsupported_platform";
        throw std::invalid_argument("PM_IMPORT_PMU hardware events require Linux on supported Intel x86 CPUs.");
    }
    Snapshot snapshot() noexcept { return {}; }
    void close_on_owner() noexcept { audit_.closed = true; }
};
#endif

class Scope {
    ThreadCounters* counters_ = nullptr;
    Aggregate* aggregate_ = nullptr;
    Snapshot start_{};
    uint64_t roots_ = 0, neighbor_checks_ = 0;
  public:
    Scope(ThreadCounters& counters, Aggregate& aggregate, bool sample) noexcept {
        if (sample && counters.enabled()) {
            counters_ = &counters;
            aggregate_ = &aggregate;
            start_ = counters.snapshot();
        }
    }
    void record_work(uint64_t roots, uint64_t neighbor_checks) noexcept {
        roots_ = roots; neighbor_checks_ = neighbor_checks;
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope() {
        if (!aggregate_) return;
        const Snapshot end = counters_->snapshot();
        ++aggregate_->sampled_calls;
        if (!start_.valid || !end.valid || end.hit < start_.hit || end.miss < start_.miss ||
            end.cycles < start_.cycles || end.instructions < start_.instructions || end.ref_cycles < start_.ref_cycles ||
            end.wall_ns < start_.wall_ns || end.time_enabled_ns < start_.time_enabled_ns ||
            end.time_running_ns < start_.time_running_ns) {
            ++aggregate_->invalid_samples;
            return;
        }
        aggregate_->roots += roots_;
        aggregate_->neighbor_checks += neighbor_checks_;
        aggregate_->ref_cycles += end.ref_cycles - start_.ref_cycles;
        aggregate_->hit += end.hit - start_.hit;
        aggregate_->miss += end.miss - start_.miss;
        aggregate_->cycles += end.cycles - start_.cycles;
        aggregate_->instructions += end.instructions - start_.instructions;
        aggregate_->wall_ns += end.wall_ns - start_.wall_ns;
        aggregate_->time_enabled_ns += end.time_enabled_ns - start_.time_enabled_ns;
        aggregate_->time_running_ns += end.time_running_ns - start_.time_running_ns;
    }
};

}  // namespace pm::structured::import_pmu
#endif
