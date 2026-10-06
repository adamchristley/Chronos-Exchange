#pragma once

#include <stdexcept>
#include <string>

#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif

namespace chronos {

inline bool pin_current_thread(int core) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
#else
    (void)core;
    return false;
#endif
}

} // namespace chronos
