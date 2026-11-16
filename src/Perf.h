#pragma once
// Measurement harness for the optimization work (OPTIMIZATION_PLAN.md, stage 0).
//
// Compiled in only with the CMake option SVM_PERF=ON (defines SVM_PERF); in a
// normal build every macro below expands to nothing and this header adds no
// code, no data and no includes beyond <windows.h>.
//
// Records are formatted into an in-memory buffer and appended to perf.log next
// to the exe by PERF_FLUSH(), which the render loop calls from its tray timer
// (outside any measured span) and at exit - so no measured path ever waits on
// disk I/O. The low-level keyboard hook only bumps two atomics (call count and
// rdtsc cycles); it never formats, locks or writes.
//
// PERF_SCOPE owns a C++ object with a destructor: never use it inside a
// function that has a __try block (MSVC C2712, CLAUDE.md invariant #7).

#include <windows.h>

#ifdef SVM_PERF

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <intrin.h>

namespace Perf {

inline LONGLONG Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline double ToMs(LONGLONG ticks) {
    static const double freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (double)f.QuadPart; }();
    return (double)ticks * 1000.0 / freq;
}

struct Log {
    std::mutex m;
    std::string buf;
    LONGLONG t0 = Now();
};
inline Log& TheLog() { static Log log; return log; }

inline void Write(const char* fmt, ...) {
    char line[512];
    Log& log = TheLog();
    const double at = ToMs(Now() - log.t0);
    int n = snprintf(line, sizeof(line), "%10.3f ", at);
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
    line[n++] = '\n';
    line[n] = 0;
    std::lock_guard<std::mutex> lock(log.m);
    log.buf.append(line, n);
}

inline void Record(const char* name, double ms) { Write("T %s %.3f", name, ms); }

// Wall-clock milliseconds since the kernel created this process - includes the
// loader's work (static imports) that no in-process QPC stamp can see.
inline double MsSinceProcessStart() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER c, n;
    c.LowPart = created.dwLowDateTime; c.HighPart = created.dwHighDateTime;
    n.LowPart = now.dwLowDateTime;     n.HighPart = now.dwHighDateTime;
    return (double)(n.QuadPart - c.QuadPart) / 10000.0;
}

// Cross-thread end-to-end markers (QPC values, 0 = none pending).
inline std::atomic<LONGLONG> osdSeriesStart{0}; // first volume tick of a series dequeued
inline std::atomic<LONGLONG> osdFire{0};        // the tick that made ChangeVolume run
inline std::atomic<LONGLONG> osdShown{0};       // ShowOSD called (ChangeVolume returned true)
inline std::atomic<LONGLONG> trackStart{0};     // track-info key dequeued
inline std::atomic<LONGLONG> trackShown{0};     // ShowTrackInfo called
inline std::atomic<int> deviceRebuilt{0};       // RecreateDevice ran since the marker was set

// Counters (reset when dumped).
inline std::atomic<unsigned long long> hookCalls{0};
inline std::atomic<unsigned long long> hookCycles{0};
inline std::atomic<unsigned long long> loopWakes{0};
inline std::atomic<unsigned long long> workerWakes{0};
inline std::atomic<unsigned long long> hookTimerWakes{0};
inline std::atomic<unsigned long long> frames{0};

inline void DumpCounters() {
    Write("C hookCalls=%llu hookCyclesAvg=%llu loopWakes=%llu workerWakes=%llu hookTimerWakes=%llu frames=%llu",
          hookCalls.load(), hookCalls.load() ? hookCycles.load() / hookCalls.load() : 0ull,
          loopWakes.load(), workerWakes.load(), hookTimerWakes.load(), frames.load());
}

inline void Flush() {
    std::string out;
    {
        Log& log = TheLog();
        std::lock_guard<std::mutex> lock(log.m);
        out.swap(log.buf);
    }
    if (out.empty()) return;
    char path[MAX_PATH];
    DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
    while (len > 0 && path[len - 1] != '\\') --len;
    path[len] = 0;
    strcat_s(path, "perf.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "ab") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}

struct Scope {
    const char* name;
    LONGLONG t0;
    explicit Scope(const char* n) : name(n), t0(Now()) {}
    ~Scope() { Record(name, ToMs(Now() - t0)); }
};

struct HookScope {
    unsigned long long c0 = __rdtsc();
    ~HookScope() {
        hookCycles.fetch_add(__rdtsc() - c0, std::memory_order_relaxed);
        hookCalls.fetch_add(1, std::memory_order_relaxed);
    }
};

} // namespace Perf

#define PERF_CAT2(a, b) a##b
#define PERF_CAT(a, b) PERF_CAT2(a, b)
#define PERF_SCOPE(name) ::Perf::Scope PERF_CAT(perfScope_, __LINE__)(name)
#define PERF_HOOK_SCOPE() ::Perf::HookScope PERF_CAT(perfHook_, __LINE__)
#define PERF_LOG(...) ::Perf::Write(__VA_ARGS__)
#define PERF_COUNT(counter) ::Perf::counter.fetch_add(1, std::memory_order_relaxed)
#define PERF_FLUSH() ::Perf::Flush()
#define PERF_ONLY(...) __VA_ARGS__

#else

#define PERF_SCOPE(name) ((void)0)
#define PERF_HOOK_SCOPE() ((void)0)
#define PERF_LOG(...) ((void)0)
#define PERF_COUNT(counter) ((void)0)
#define PERF_FLUSH() ((void)0)
#define PERF_ONLY(...)

#endif
