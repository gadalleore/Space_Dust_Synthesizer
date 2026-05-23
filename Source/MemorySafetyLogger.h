#pragma once

//==============================================================================
//  Space Dust — Memory Safety Logger
//  ----------------------------------
//  Always-on, real-time-safe development logger that tracks:
//      * voice lifecycle (create / destroy / steal / start / stop / legato)
//      * grain spawning / lifetime (GrainDelay)
//      * raw pointer ownership changes
//      * buffer / delay-line / filter state ownership
//      * out-of-bounds access (via SAFETY_CHECK_BOUNDS)
//      * audio-thread safety violations
//      * any object ctor / dtor pair you choose to instrument
//
//  Design:
//      - Producers (audio thread, message thread, UI thread) write into a
//        lock-free MPSC ring buffer (Vyukov-style). No allocations, no locks,
//        no syscalls on the producer path.
//      - A single background writer thread drains the ring every ~25 ms,
//        formats entries, resolves stack traces (DbgHelp), and writes to
//        disk via juce::FileOutputStream.
//      - On startup the writer thread purges log files older than 60 days,
//        so the log folder never grows without bound.
//
//  Build switch:
//      cmake -DENABLE_MEMORY_SAFETY_LOGGING=ON   → fully enabled
//      cmake -DENABLE_MEMORY_SAFETY_LOGGING=OFF  → every SAFETY_LOG_*
//                                                  macro compiles to a no-op,
//                                                  JUCE assertion logging is
//                                                  silenced, and Release
//                                                  builds emit zero log code.
//
//  Daily dev usage:
//      .\enable-safety-logging.ps1
//
//  Release / installer build:
//      .\disable-all-logging-for-release.ps1
//==============================================================================

#ifndef SPACEDUST_ENABLE_SAFETY_LOGGING
 #define SPACEDUST_ENABLE_SAFETY_LOGGING 0
#endif

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <thread>
#include <memory>

namespace spacedust
{

//==============================================================================
enum class SafetyEventType : uint16_t
{
    Generic = 0,
    Voice,      // voice creation, destruction, steal, start, stop, legato
    Grain,      // grain spawn, kill, mature
    Pointer,    // raw pointer ownership change
    Buffer,     // buffer alloc / free / resize
    Delay,      // delay-line state ownership / wrap
    Filter,     // filter state ownership / reset
    Thread,     // RT-thread safety violation (e.g. allocation on audio thread)
    Object,     // generic object ctor / dtor
    Assert      // soft assertion failure
};

enum class SafetySeverity : uint16_t
{
    Trace    = 0,
    Info     = 1,
    Warn     = 2,
    Error    = 3,
    Critical = 4
};

//==============================================================================
#if SPACEDUST_ENABLE_SAFETY_LOGGING

class MemorySafetyLogger
{
public:
    static MemorySafetyLogger& instance();

    /** Spin up the background writer thread and create today's log file.
        Idempotent: safe to call multiple times.
        Most callers should use addRef() instead — see refcount note below. */
    void start();

    /** Stop the writer thread, drain the ring, and close the file.
        Idempotent: safe to call multiple times.
        Most callers should use release() instead — see refcount note below. */
    void shutdown();

    /** Refcounted lifecycle for multi-instance hosts.
        The logger is a process-wide singleton shared across every Space Dust
        instance loaded into the same DLL. addRef() increments the live-instance
        count and calls start() on the 0→1 transition; release() decrements and
        calls shutdown() on the 1→0 transition. Both serialize via lifecycleMutex
        so a fast remove-then-add cannot race start() and shutdown().
        These are the canonical entry points from PluginProcessor ctor / dtor. */
    void addRef();
    void release();

    /** Real-time safe: never allocates, never blocks, never throws.
        If the ring is full the entry is dropped and an internal counter
        is incremented (reported at session end). */
    void enqueue (SafetyEventType type,
                  SafetySeverity  sev,
                  const void*     a1,
                  const void*     a2,
                  int32_t         id1,
                  int32_t         id2,
                  int32_t         iParam,
                  float           fParam,
                  const char*     fixedMsg,
                  const char*     fileLine) noexcept;

    /** NON-RT contexts only (ctor / dtor on message thread). Captures a
        Windows stack trace using CaptureStackBackTrace. Symbol resolution
        is deferred to the writer thread. */
    void enqueueWithStack (SafetyEventType type,
                           SafetySeverity  sev,
                           const void*     a1,
                           int32_t         id1,
                           const char*     fixedMsg,
                           const char*     fileLine) noexcept;

    /** Tag the calling thread as the real-time audio thread. Subsequent
        log entries from this thread carry an [RT] marker so violations
        stand out in the log. */
    void markAudioThread() noexcept;
    bool isAudioThread() const noexcept;

    juce::File getLogDirectory() const { return logDir; }
    juce::File getCurrentLogFile() const { return logFile; }

private:
    MemorySafetyLogger();
    ~MemorySafetyLogger();

    void writerLoop();
    void purgeOldLogs();

    struct Entry; // defined in .cpp

    // 16 384 entries × ~320 B ≈ 5 MB resident.
    static constexpr size_t kRingCapacity = 1u << 14;
    std::unique_ptr<Entry[]> ring;

    alignas(64) std::atomic<uint64_t> writeSeq { 0 };
    alignas(64) std::atomic<uint64_t> readSeq  { 0 };
    alignas(64) std::atomic<uint64_t> dropped  { 0 };

    std::atomic<bool> running { false };
    std::thread       writer;

    std::mutex lifecycleMutex;
    int        refCount { 0 }; // guarded by lifecycleMutex

    juce::File logDir;
    juce::File logFile;
    // Monotonic anchor (steady_clock ns) so per-entry relMs is meaningful.
    // Mixing this with juce::Time::currentTimeMillis() produced negative t= values.
    uint64_t   sessionStartNs = 0;

    JUCE_DECLARE_NON_COPYABLE (MemorySafetyLogger)
};

#endif // SPACEDUST_ENABLE_SAFETY_LOGGING

} // namespace spacedust

//==============================================================================
//  PUBLIC MACROS  —  always defined.  No-ops when logging is disabled.
//==============================================================================
#if SPACEDUST_ENABLE_SAFETY_LOGGING

#define SAFETY_FILELINE_STR_(x) #x
#define SAFETY_FILELINE_STR(x)  SAFETY_FILELINE_STR_(x)
#define SAFETY_FILELINE         __FILE__ ":" SAFETY_FILELINE_STR(__LINE__)

#define SAFETY_LOG_RAW(type, sev, a1, a2, id1, id2, ip, fp, msg)                    \
    ::spacedust::MemorySafetyLogger::instance().enqueue (                           \
        (type), (sev),                                                              \
        static_cast<const void*>(a1), static_cast<const void*>(a2),                 \
        static_cast<int32_t>(id1), static_cast<int32_t>(id2),                       \
        static_cast<int32_t>(ip),   static_cast<float>(fp),                         \
        (msg), SAFETY_FILELINE)

// --- Voice ------------------------------------------------------------------
#define SAFETY_LOG_VOICE(voiceId, ptr, msg)                                         \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Voice,                            \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, (voiceId), 0, 0, 0.0f, (msg))

#define SAFETY_LOG_VOICE_NOTE(voiceId, ptr, midiNote, freqHz, msg)                  \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Voice,                            \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, (voiceId), (midiNote), 0, (freqHz), (msg))

#define SAFETY_LOG_VOICE_WARN(voiceId, ptr, msg)                                    \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Voice,                            \
                    ::spacedust::SafetySeverity::Warn,                              \
                    (ptr), nullptr, (voiceId), 0, 0, 0.0f, (msg))

// --- Grain ------------------------------------------------------------------
#define SAFETY_LOG_GRAIN(grainId, ptr, msg)                                         \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Grain,                            \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, (grainId), 0, 0, 0.0f, (msg))

#define SAFETY_LOG_GRAIN_DETAILED(grainId, ptr, durationSamples, pitchRatio, msg)   \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Grain,                            \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, (grainId), 0,                                   \
                    (durationSamples), (pitchRatio), (msg))

// --- Pointer ---------------------------------------------------------------
#define SAFETY_LOG_POINTER(oldPtr, newPtr, msg)                                     \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Pointer,                          \
                    ::spacedust::SafetySeverity::Info,                              \
                    (oldPtr), (newPtr), 0, 0, 0, 0.0f, (msg))

#define SAFETY_LOG_DANGLING(ptr, msg)                                               \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Pointer,                          \
                    ::spacedust::SafetySeverity::Error,                             \
                    (ptr), nullptr, 0, 0, 0, 0.0f, (msg))

// --- Buffer ----------------------------------------------------------------
#define SAFETY_LOG_BUFFER(ptr, sizeBytes, msg)                                      \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Buffer,                           \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, 0, 0,                                           \
                    static_cast<int32_t>(sizeBytes), 0.0f, (msg))

#define SAFETY_LOG_BUFFER_BOUNDS(ptr, requestedIndex, size, msg)                    \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Buffer,                           \
                    ::spacedust::SafetySeverity::Warn,                              \
                    (ptr), nullptr,                                                 \
                    static_cast<int32_t>(requestedIndex),                           \
                    static_cast<int32_t>(size), 0, 0.0f, (msg))

// --- Delay / Filter --------------------------------------------------------
#define SAFETY_LOG_DELAY(ptr, writeIdx, readIdx, msg)                               \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Delay,                            \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr,                                                 \
                    static_cast<int32_t>(writeIdx),                                 \
                    static_cast<int32_t>(readIdx),                                  \
                    0, 0.0f, (msg))

#define SAFETY_LOG_FILTER(ptr, msg)                                                 \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Filter,                           \
                    ::spacedust::SafetySeverity::Info,                              \
                    (ptr), nullptr, 0, 0, 0, 0.0f, (msg))

// --- RT-thread violation ---------------------------------------------------
#define SAFETY_LOG_RT_VIOLATION(ptr, msg)                                           \
    SAFETY_LOG_RAW (::spacedust::SafetyEventType::Thread,                           \
                    ::spacedust::SafetySeverity::Error,                             \
                    (ptr), nullptr, 0, 0, 0, 0.0f, (msg))

// --- Object ctor / dtor (captures stack — NON-RT only) ---------------------
#define SAFETY_LOG_OBJECT_CTOR(ptr, msg)                                            \
    ::spacedust::MemorySafetyLogger::instance().enqueueWithStack (                  \
        ::spacedust::SafetyEventType::Object,                                       \
        ::spacedust::SafetySeverity::Info,                                          \
        static_cast<const void*>(ptr), 0, (msg), SAFETY_FILELINE)

#define SAFETY_LOG_OBJECT_DTOR(ptr, msg)                                            \
    ::spacedust::MemorySafetyLogger::instance().enqueueWithStack (                  \
        ::spacedust::SafetyEventType::Object,                                       \
        ::spacedust::SafetySeverity::Info,                                          \
        static_cast<const void*>(ptr), 0, (msg), SAFETY_FILELINE)

// --- Soft-assert (RT-safe) -------------------------------------------------
#define SAFETY_ASSERT(cond, msg)                                                    \
    do {                                                                            \
        if (! (cond)) {                                                             \
            SAFETY_LOG_RAW (::spacedust::SafetyEventType::Assert,                   \
                            ::spacedust::SafetySeverity::Error,                     \
                            nullptr, nullptr, 0, 0, 0, 0.0f,                        \
                            "ASSERT FAILED: " #cond " — " msg);                     \
        }                                                                           \
    } while (0)

// --- Bounds check (RT-safe) ------------------------------------------------
#define SAFETY_CHECK_BOUNDS(ptr, idx, size, msg)                                    \
    do {                                                                            \
        const int32_t _sd_idx = static_cast<int32_t>(idx);                          \
        const int32_t _sd_sz  = static_cast<int32_t>(size);                         \
        if (_sd_idx < 0 || _sd_idx >= _sd_sz) {                                     \
            SAFETY_LOG_BUFFER_BOUNDS ((ptr), _sd_idx, _sd_sz, (msg));               \
        }                                                                           \
    } while (0)

// --- Lifecycle -------------------------------------------------------------
#define SAFETY_MARK_AUDIO_THREAD()                                                  \
    ::spacedust::MemorySafetyLogger::instance().markAudioThread()

#define SAFETY_LOGGER_START()                                                       \
    ::spacedust::MemorySafetyLogger::instance().addRef()

#define SAFETY_LOGGER_SHUTDOWN()                                                    \
    ::spacedust::MemorySafetyLogger::instance().release()

//==============================================================================
#else   // SPACEDUST_ENABLE_SAFETY_LOGGING == 0  →  every macro is a no-op
//==============================================================================

#define SAFETY_LOG_RAW(...)               ((void) 0)
#define SAFETY_LOG_VOICE(...)             ((void) 0)
#define SAFETY_LOG_VOICE_NOTE(...)        ((void) 0)
#define SAFETY_LOG_VOICE_WARN(...)        ((void) 0)
#define SAFETY_LOG_GRAIN(...)             ((void) 0)
#define SAFETY_LOG_GRAIN_DETAILED(...)    ((void) 0)
#define SAFETY_LOG_POINTER(...)           ((void) 0)
#define SAFETY_LOG_DANGLING(...)          ((void) 0)
#define SAFETY_LOG_BUFFER(...)            ((void) 0)
#define SAFETY_LOG_BUFFER_BOUNDS(...)     ((void) 0)
#define SAFETY_LOG_DELAY(...)             ((void) 0)
#define SAFETY_LOG_FILTER(...)            ((void) 0)
#define SAFETY_LOG_RT_VIOLATION(...)      ((void) 0)
#define SAFETY_LOG_OBJECT_CTOR(...)       ((void) 0)
#define SAFETY_LOG_OBJECT_DTOR(...)       ((void) 0)
#define SAFETY_ASSERT(...)                ((void) 0)
#define SAFETY_CHECK_BOUNDS(...)          ((void) 0)
#define SAFETY_MARK_AUDIO_THREAD()        ((void) 0)
#define SAFETY_LOGGER_START()             ((void) 0)
#define SAFETY_LOGGER_SHUTDOWN()          ((void) 0)

#endif // SPACEDUST_ENABLE_SAFETY_LOGGING
