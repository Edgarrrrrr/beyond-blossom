"""Add explicit Linux-affinity / Darwin-QoS platform boundaries to archived adapters."""
from pathlib import Path

def adapt(directory):
    directory=Path(directory)
    path=directory/'benchmark.cc'
    s=path.read_text()
    if 'BB_AE_PORTABLE_PLATFORM' in s:return
    s=s.replace('#include <linux/perf_event.h>','#define BB_AE_PORTABLE_PLATFORM 1\n#if defined(__linux__)\n#include <linux/perf_event.h>\n#else\n#include <pthread.h>\n#include <pthread/qos.h>\n#endif')
    s=s.replace('#include <sys/syscall.h>','#if defined(__linux__)\n#include <sys/syscall.h>\n#endif')
    start=s.index('    if (o.cpus.front() != o.main_cpu ||')
    end=s.index('    if ((o.execution == "serial"',start)
    s=s[:start]+'#if defined(__linux__)\n'+s[start:end]+'''#else
    if (std::any_of(o.cpus.begin(), o.cpus.end(), [](int cpu) { return cpu != -1; }))
        throw std::invalid_argument("Darwin uses QoS and requires -1 CPU placeholders");
#endif
'''+s[end:]
    start=s.index('    if (cpu < 0 || cpu >= CPU_SETSIZE)',s.index('void pin_main('))
    end=s.index('\n}',start)
    s=s[:start]+'#if defined(__linux__)\n'+s[start:end]+'''
#else
    if (cpu != -1) throw std::invalid_argument("Darwin does not support physical CPU pinning");
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0))
        throw std::runtime_error("Cannot set USER_INITIATED QoS");
#endif'''+s[end:]
    start=s.index('    cpu_set_t mask;',s.index('ThreadIdentity capture_thread('))
    end=s.index('\n}',start)
    s=s[:start]+'#if defined(__linux__)\n'+s[start:end]+'''
#else
    if (!tid || expected != -1) throw std::runtime_error("Invalid Darwin thread identity");
    return ThreadIdentity{tid, -1};
#endif'''+s[end:]
    start=s.index('    return {capture_thread(',s.index('ExecutionSnapshot capture_serial('))
    end=s.index('\n}',start)
    s=s[:start]+'#if defined(__linux__)\n'+s[start:end]+'''
#else
    uint64_t tid = 0;
    if (pthread_threadid_np(nullptr, &tid)) throw std::runtime_error("Cannot query Darwin thread ID");
    return {capture_thread(tid, -1)};
#endif'''+s[end:]
    start=s.index('        if (begin[i].socket != end[i].socket')
    end=s.index('    std::ofstream out(o.output / "execution.csv");',start)
    original=s[start:end]
    close=original.index('    }\n')
    s=s[:start]+'#if defined(__linux__)\n'+original[:close]+'#endif\n    }\n#if defined(__linux__)\n'+original[close+6:]+'#endif\n'+s[end:]
    s=s.replace('    std::ofstream threads(o.output / "execution_threads.csv");','''    std::ofstream platform(o.output / "placement.json");
#if defined(__linux__)
    platform << "{\\"physical_affinity_verified\\":true,\\"policy\\":\\"Linux CPU affinity\\"}\\n";
#else
    platform << "{\\"physical_affinity_verified\\":false,\\"policy\\":\\"Darwin USER_INITIATED QoS\\"}\\n";
#endif
    std::ofstream threads(o.output / "execution_threads.csv");''')
    start=s.index('            if (t.requested_cpu != o.cpus.at(global)')
    end=s.index('            if (single_thread',start)
    s=s[:start]+'#if defined(__linux__)\n'+s[start:end]+'''#else
            if (t.requested_cpu != -1 || !t.affinity.empty() || t.qos_class != QOS_CLASS_USER_INITIATED)
                throw std::runtime_error("Decoder Darwin QoS snapshot mismatch");
#endif
'''+s[end:]
    path.write_text(s)
    path=directory/'cache_counters.h';s=path.read_text()
    path.write_text('#if defined(__linux__)\n'+s+'''\n#else
class CacheReadCounters {
public:
    CacheReadCounters() { throw std::runtime_error("Cache PMU collection requires Linux"); }
    void start() {}
    void stop() {}
    void persist(const std::filesystem::path&, size_t) const {}
};
#endif
''')
    path=directory/'CMakeLists.txt';s=path.read_text()
    s=s.replace('target_compile_options(experiment_benchmark PRIVATE -O3 -mno-avx2)', '''target_compile_options(experiment_benchmark PRIVATE -O3)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    target_compile_options(experiment_benchmark PRIVATE -mno-avx2)
endif()''')
    path.write_text(s)

if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory',type=Path)
    adapt(parser.parse_args().directory)
