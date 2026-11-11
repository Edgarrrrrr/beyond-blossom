#if defined(__linux__)
// Counter choice is fixed while compiling the adapter; decoder libraries stay unchanged.
class CacheReadCounters {
#if BB_PMU_CACHE_LEVEL == 2
    static constexpr uint32_t event_type = PERF_TYPE_RAW;
    static constexpr uint64_t access_config = 0xe124, miss_config = 0x2124;
    static constexpr const char* prefix = "l2";
    static constexpr const char* metric_prefix = "l2_load_miss";
    static constexpr const char* events = R"([{"name":"L2_RQSTS.ALL_DEMAND_DATA_RD","type":4,"config":57636},{"name":"L2_RQSTS.DEMAND_DATA_RD_MISS","type":4,"config":8484}])";
    static constexpr const char* definition = "Intel Cascade Lake accepted L2 data-read requests; denominator includes L1D hardware-prefetcher requests; code and RFO events separate";
#elif BB_PMU_CACHE_LEVEL == 3
    static constexpr uint32_t event_type = PERF_TYPE_HW_CACHE;
    static constexpr uint64_t access_config = 2, miss_config = 65538;
    static constexpr const char* prefix = "llc";
    static constexpr const char* metric_prefix = "l3_load_miss";
    static constexpr const char* events = R"([{"name":"LLC-loads","type":3,"config":2},{"name":"LLC-load-misses","type":3,"config":65538}])";
    static constexpr const char* definition = "Linux LL/READ/ACCESS and LL/READ/MISS events on Cascade Lake; demand data reads";
#else
#error "Compile with BB_PMU_CACHE_LEVEL=2 or 3"
#endif
    int leader_ = -1, misses_ = -1;
    uint64_t load_id_ = 0, miss_id_ = 0;
    std::array<uint64_t, 7> raw_{};
    [[noreturn]] static void fail(const char* operation) {
        throw std::runtime_error(std::string(operation) + ": " + std::strerror(errno));
    }
    static int open(uint64_t config, int group) {
        perf_event_attr attr{};
        attr.size = sizeof(attr);
        attr.type = event_type;
        attr.config = config;
        attr.disabled = group == -1;
        attr.pinned = group == -1;
        attr.exclude_kernel = attr.exclude_hv = 1;
        attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_ID | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
        const int fd = syscall(SYS_perf_event_open, &attr, 0, -1, group, PERF_FLAG_FD_CLOEXEC);
        if (fd < 0) fail("perf_event_open cache-read counter");
        return fd;
    }
public:
    CacheReadCounters() {
        leader_ = open(access_config, -1);
        try {
            misses_ = open(miss_config, leader_);
            if (ioctl(leader_, PERF_EVENT_IOC_ID, &load_id_) || ioctl(misses_, PERF_EVENT_IOC_ID, &miss_id_))
                fail("PERF_EVENT_IOC_ID");
        } catch (...) {
            if (misses_ >= 0) close(misses_);
            close(leader_);
            throw;
        }
    }
    CacheReadCounters(const CacheReadCounters&) = delete;
    CacheReadCounters& operator=(const CacheReadCounters&) = delete;
    ~CacheReadCounters() { close(misses_); close(leader_); }
    void start() {
        if (ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) ||
            ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP)) fail("Enable PMU group");
    }
    void stop() {
        if (ioctl(leader_, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP)) fail("Disable PMU group");
        const auto bytes = ::read(leader_, raw_.data(), sizeof(raw_));
        if (bytes != sizeof(raw_)) throw std::runtime_error("Incomplete PMU group read");
        if (raw_[0] != 2 || raw_[4] != load_id_ || raw_[6] != miss_id_ || !raw_[1] || raw_[1] != raw_[2])
            throw std::runtime_error("PMU group unavailable, mismatched, or multiplexed");
        if (!raw_[3] || raw_[5] > raw_[3]) throw std::runtime_error("Invalid cache-read counters");
    }
    void persist(const std::filesystem::path& path, size_t shots) const {
        std::ofstream out(path);
        out << std::setprecision(17)
            << "{\"schema_version\":1,\"mode\":\"pmu\",\"cache_level\":" << BB_PMU_CACHE_LEVEL
            << ",\"shots\":" << shots << ",\"" << prefix << "_loads\":" << raw_[3]
            << ",\"" << prefix << "_load_misses\":" << raw_[5]
            << ",\"" << metric_prefix << "_rate\":" << double(raw_[5]) / raw_[3]
            << ",\"" << metric_prefix << "_percent\":" << 100.0 * double(raw_[5]) / raw_[3]
            << ",\"time_enabled_ns\":" << raw_[1] << ",\"time_running_ns\":" << raw_[2]
            << ",\"group_raw\":[";
        for (size_t k = 0; k < raw_.size(); ++k) { if (k) out << ','; out << raw_[k]; }
        out << "],\"events\":" << events
            << R"(,"pid":0,"cpu":-1,"inherit":false,"pinned_group":true,"exclude_kernel":true,"exclude_hv":true,"multiplexed":false,"scaled":false,"latency_collected":false,"scope":"single calling thread, continuous normal decode loop and preallocated result stores; loading, construction, warmup, checking, CSV excluded","metric":"100 * misses / requests","event_definition":")"
            << definition << "\"}\n";
        if (!out) throw std::runtime_error("PMU persistence failed");
    }
};

#else
class CacheReadCounters {
public:
    CacheReadCounters() { throw std::runtime_error("Cache PMU collection requires Linux"); }
    void start() {}
    void stop() {}
    void persist(const std::filesystem::path&, size_t) const {}
};
#endif
