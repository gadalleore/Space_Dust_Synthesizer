#include "MemorySafetyLogger.h"

#if SPACEDUST_ENABLE_SAFETY_LOGGING

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>

#if JUCE_WINDOWS
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <DbgHelp.h>
 #pragma comment (lib, "DbgHelp.lib")
#endif

namespace spacedust
{
//==============================================================================
namespace
{
    constexpr int kMaxMsgChars      = 168;
    constexpr int kMaxFileLineChars = 80;
    constexpr int kMaxStackFrames   = 12;
    constexpr int kPurgeDaysOld     = 60;          // delete logs older than 2 months
    constexpr int kDrainSleepMs     = 25;          // writer thread cadence

    thread_local bool tlsIsAudioThread = false;

    inline uint64_t nowNs() noexcept
    {
        return (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds> (
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    inline uint64_t currentThreadId() noexcept
    {
       #if JUCE_WINDOWS
        return (uint64_t) ::GetCurrentThreadId();
       #else
        return (uint64_t) std::hash<std::thread::id>{} (std::this_thread::get_id());
       #endif
    }

    inline void safeCopy (char* dst, size_t cap, const char* src) noexcept
    {
        if (cap == 0)         return;
        if (src == nullptr) { dst[0] = 0; return; }
        size_t i = 0;
        for (; i + 1 < cap && src[i] != 0; ++i)
            dst[i] = src[i];
        dst[i] = 0;
    }

    const char* eventName (SafetyEventType e) noexcept
    {
        switch (e)
        {
            case SafetyEventType::Voice:   return "VOICE";
            case SafetyEventType::Grain:   return "GRAIN";
            case SafetyEventType::Pointer: return "PTR  ";
            case SafetyEventType::Buffer:  return "BUF  ";
            case SafetyEventType::Delay:   return "DELAY";
            case SafetyEventType::Filter:  return "FILT ";
            case SafetyEventType::Thread:  return "RT!  ";
            case SafetyEventType::Object:  return "OBJ  ";
            case SafetyEventType::Assert:  return "ASRT ";
            default:                       return "GEN  ";
        }
    }

    const char* sevName (SafetySeverity s) noexcept
    {
        switch (s)
        {
            case SafetySeverity::Trace:    return "TRC";
            case SafetySeverity::Info:     return "INF";
            case SafetySeverity::Warn:     return "WRN";
            case SafetySeverity::Error:    return "ERR";
            case SafetySeverity::Critical: return "CRT";
            default:                       return "???";
        }
    }
}

//==============================================================================
struct MemorySafetyLogger::Entry
{
    // seq drives the Vyukov MPSC protocol.
    //   producer waits until seq == pos, then bumps seq to pos+1 after writing.
    //   consumer waits until seq == pos+1, then bumps seq to pos+kCapacity after reading.
    alignas(64) std::atomic<uint64_t> seq { 0 };

    uint64_t       timestampNs   = 0;
    uint64_t       threadId      = 0;
    SafetyEventType evt          = SafetyEventType::Generic;
    SafetySeverity sev           = SafetySeverity::Info;
    const void*    a1            = nullptr;
    const void*    a2            = nullptr;
    int32_t        id1           = 0;
    int32_t        id2           = 0;
    int32_t        iParam        = 0;
    float          fParam        = 0.0f;
    char           msg [kMaxMsgChars]      {};
    char           loc [kMaxFileLineChars] {};
    int            stackFrames   = 0;
    void*          stack [kMaxStackFrames] {};
    bool           onAudioThread = false;
};

//==============================================================================
MemorySafetyLogger& MemorySafetyLogger::instance()
{
    static MemorySafetyLogger inst;
    return inst;
}

MemorySafetyLogger::MemorySafetyLogger()
    : ring (new Entry [kRingCapacity])
{
    // Initialise the Vyukov sequence numbers so producers start at seq[i] == i.
    for (size_t i = 0; i < kRingCapacity; ++i)
        ring[i].seq.store (i, std::memory_order_relaxed);
}

MemorySafetyLogger::~MemorySafetyLogger()
{
    shutdown();
}

//==============================================================================
void MemorySafetyLogger::start()
{
    bool expected = false;
    if (! running.compare_exchange_strong (expected, true))
        return;

    logDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                .getChildFile ("63C")
                .getChildFile ("Space Dust")
                .getChildFile ("Logs")
                .getChildFile ("Safety");

    logDir.createDirectory();

    // Background-purge old logs. Cheap; runs on the calling (message) thread once.
    purgeOldLogs();

    // Anchor session-relative timestamps to the SAME clock used by enqueue()
    // (std::chrono::steady_clock). Using juce::Time::currentTimeMillis() here
    // mixed clock domains and produced large negative t= values.
    sessionStartNs = nowNs();

    auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d_%H-%M-%S");
   #if JUCE_WINDOWS
    auto pid   = (int) ::GetCurrentProcessId();
   #else
    auto pid   = (int) ::getpid();
   #endif

    logFile = logDir.getChildFile ("SpaceDust_Safety_" + stamp
                                   + "_PID" + juce::String (pid) + ".log");

    writer = std::thread ([this] { writerLoop(); });
}

void MemorySafetyLogger::shutdown()
{
    bool expected = true;
    if (! running.compare_exchange_strong (expected, false))
        return;

    if (writer.joinable())
        writer.join();
}

void MemorySafetyLogger::addRef()
{
    std::lock_guard<std::mutex> lock (lifecycleMutex);
    if (refCount++ == 0)
        start();
}

void MemorySafetyLogger::release()
{
    std::lock_guard<std::mutex> lock (lifecycleMutex);
    if (refCount <= 0) { refCount = 0; return; }     // unmatched release: ignore rather than underflow
    if (--refCount == 0)
        shutdown();
}

void MemorySafetyLogger::markAudioThread() noexcept   { tlsIsAudioThread = true; }
bool MemorySafetyLogger::isAudioThread() const noexcept { return tlsIsAudioThread; }

//==============================================================================
//  RT-SAFE producer path.  No allocations, no syscalls, no locks.
//==============================================================================
void MemorySafetyLogger::enqueue (SafetyEventType type, SafetySeverity sev,
                                  const void* a1, const void* a2,
                                  int32_t id1, int32_t id2,
                                  int32_t iParam, float fParam,
                                  const char* fixedMsg,
                                  const char* fileLine) noexcept
{
    if (! running.load (std::memory_order_relaxed))
        return;

    uint64_t pos = writeSeq.load (std::memory_order_relaxed);
    for (;;)
    {
        Entry& slot = ring[pos & (kRingCapacity - 1)];
        const uint64_t seq  = slot.seq.load (std::memory_order_acquire);
        const int64_t  diff = (int64_t) seq - (int64_t) pos;
        if (diff == 0)
        {
            if (writeSeq.compare_exchange_weak (pos, pos + 1,
                                                std::memory_order_relaxed))
                break;
        }
        else if (diff < 0)
        {
            // Ring is full — drop the entry rather than block the audio thread.
            dropped.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        else
        {
            pos = writeSeq.load (std::memory_order_relaxed);
        }
    }

    Entry& slot = ring[pos & (kRingCapacity - 1)];
    slot.timestampNs   = nowNs();
    slot.threadId      = currentThreadId();
    slot.evt           = type;
    slot.sev           = sev;
    slot.a1            = a1;
    slot.a2            = a2;
    slot.id1           = id1;
    slot.id2           = id2;
    slot.iParam        = iParam;
    slot.fParam        = fParam;
    safeCopy (slot.msg, sizeof (slot.msg), fixedMsg);
    safeCopy (slot.loc, sizeof (slot.loc), fileLine);
    slot.stackFrames   = 0;
    slot.onAudioThread = tlsIsAudioThread;

    slot.seq.store (pos + 1, std::memory_order_release);
}

//==============================================================================
//  Non-RT path: captures the stack via DbgHelp.  Producer-side cost is just
//  CaptureStackBackTrace (no allocations, no symbol resolution).
//==============================================================================
void MemorySafetyLogger::enqueueWithStack (SafetyEventType type, SafetySeverity sev,
                                            const void* a1, int32_t id1,
                                            const char* fixedMsg,
                                            const char* fileLine) noexcept
{
    if (! running.load (std::memory_order_relaxed))
        return;

    uint64_t pos = writeSeq.load (std::memory_order_relaxed);
    for (;;)
    {
        Entry& slot = ring[pos & (kRingCapacity - 1)];
        const uint64_t seq  = slot.seq.load (std::memory_order_acquire);
        const int64_t  diff = (int64_t) seq - (int64_t) pos;
        if (diff == 0)
        {
            if (writeSeq.compare_exchange_weak (pos, pos + 1,
                                                std::memory_order_relaxed))
                break;
        }
        else if (diff < 0)
        {
            dropped.fetch_add (1, std::memory_order_relaxed);
            return;
        }
        else
        {
            pos = writeSeq.load (std::memory_order_relaxed);
        }
    }

    Entry& slot = ring[pos & (kRingCapacity - 1)];
    slot.timestampNs   = nowNs();
    slot.threadId      = currentThreadId();
    slot.evt           = type;
    slot.sev           = sev;
    slot.a1            = a1;
    slot.a2            = nullptr;
    slot.id1           = id1;
    slot.id2           = 0;
    slot.iParam        = 0;
    slot.fParam        = 0.0f;
    safeCopy (slot.msg, sizeof (slot.msg), fixedMsg);
    safeCopy (slot.loc, sizeof (slot.loc), fileLine);
    slot.onAudioThread = tlsIsAudioThread;

   #if JUCE_WINDOWS
    const USHORT n = ::CaptureStackBackTrace (1, kMaxStackFrames, slot.stack, nullptr);
    slot.stackFrames = (int) n;
   #else
    slot.stackFrames = 0;
   #endif

    slot.seq.store (pos + 1, std::memory_order_release);
}

//==============================================================================
void MemorySafetyLogger::purgeOldLogs()
{
    if (! logDir.isDirectory())
        return;

    const auto now = juce::Time::getCurrentTime();
    juce::Array<juce::File> files;
    logDir.findChildFiles (files, juce::File::findFiles, false, "SpaceDust_Safety_*.log");

    for (auto& f : files)
    {
        const auto age = (now - f.getLastModificationTime()).inDays();
        if (age > (double) kPurgeDaysOld)
            f.deleteFile();
    }
}

//==============================================================================
//  Writer-thread helpers (single thread → can use juce::String safely).
//==============================================================================
namespace
{
   #if JUCE_WINDOWS
    std::atomic<bool> symInitialised { false };

    juce::String formatStack (void** frames, int count)
    {
        if (count <= 0)
            return {};

        if (! symInitialised.exchange (true))
        {
            ::SymSetOptions (SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            ::SymInitialize (::GetCurrentProcess(), nullptr, TRUE);
        }

        juce::String out;
        const HANDLE proc = ::GetCurrentProcess();
        constexpr DWORD nameLen = 512;

        // Use the explicit *W* variant of SymFromAddr and the matching SYMBOL_INFOW
        // struct. JUCE compiles Windows with UNICODE defined, and the un-suffixed
        // SymFromAddr macro was already mapping to SymFromAddrW under the hood —
        // writing WCHARs into the ANSI SYMBOL_INFO::Name buffer we had before,
        // which is what produced the UTF-16LE "Chinese character" mojibake.
        union {
            SYMBOL_INFOW  sym;
            unsigned char raw [sizeof (SYMBOL_INFOW) + nameLen * sizeof (WCHAR)];
        } scratch {};
        scratch.sym.SizeOfStruct = sizeof (SYMBOL_INFOW);
        scratch.sym.MaxNameLen   = nameLen;

        for (int i = 0; i < count; ++i)
        {
            const DWORD64 addr = (DWORD64) frames[i];
            DWORD64 disp = 0;

            char nameBuf [nameLen + 1] { '<','u','n','r','e','s','o','l','v','e','d','>', 0 };

            if (::SymFromAddrW (proc, addr, &disp, &scratch.sym)
                && scratch.sym.NameLen > 0
                && scratch.sym.NameLen <= nameLen)
            {
                // Symbol names are pure ASCII; verify every WCHAR is printable
                // 7-bit, then narrow to char for display. Anything else means
                // garbage — fall back to "<unresolved>".
                const DWORD n = juce::jmin<DWORD> (scratch.sym.NameLen, nameLen);
                bool allPrintable = true;
                for (DWORD k = 0; k < n; ++k)
                {
                    const WCHAR wc = scratch.sym.Name[k];
                    if (wc < 0x20 || wc > 0x7E) { allPrintable = false; break; }
                }
                if (allPrintable)
                {
                    for (DWORD k = 0; k < n; ++k)
                        nameBuf[k] = static_cast<char> (scratch.sym.Name[k] & 0x7F);
                    nameBuf[n] = 0;
                }
                // else: leave nameBuf as "<unresolved>"
            }

            // Use std::snprintf (guaranteed narrow / single-byte char) rather than
            // juce::String::formatted. On Windows JUCE's formatted() calls
            // _vsnwprintf under the hood, which interprets %s as wchar_t* and
            // reads our char* nameBuf two bytes at a time — that's what produced
            // the UTF-8-encoded "灓捡䑥獵..." CJK garbage. The symbol resolution
            // itself was correct.
            char frameLine [nameLen + 64];
            std::snprintf (frameLine, sizeof (frameLine),
                           "      #%02d 0x%p  %s\n",
                           i, frames[i], nameBuf);
            out += frameLine;
        }
        return out;
    }
   #else
    juce::String formatStack (void**, int) { return {}; }
   #endif
}

void MemorySafetyLogger::writerLoop()
{
    juce::FileOutputStream out (logFile);
    if (! out.openedOk())
        return;

    out.setNewLineString ("\r\n");
    out << "=== Space Dust Memory-Safety Log ===\r\n"
        << "Session start : " << juce::Time::getCurrentTime().toString (true, true) << "\r\n"
        << "PID           : "
       #if JUCE_WINDOWS
        << (int) ::GetCurrentProcessId()
       #else
        << (int) ::getpid()
       #endif
        << "\r\n"
        << "Log file      : " << logFile.getFullPathName() << "\r\n"
        << "Ring capacity : " << (int) kRingCapacity << " entries\r\n"
        << "Header columns: [t=ms_since_session_start] TID=osThreadId EVT=type SEV=lvl [RT?] "
           "a1=primaryPtr a2=secondaryPtr id1/id2/i/f msg \"…\" @ file:line\r\n\r\n";
    out.flush();

    juce::String batch;
    batch.preallocateBytes (64 * 1024);

    const auto drain = [&] (bool flushNow)
    {
        for (;;)
        {
            const uint64_t pos = readSeq.load (std::memory_order_relaxed);
            Entry& slot = ring [pos & (kRingCapacity - 1)];
            const uint64_t seq  = slot.seq.load (std::memory_order_acquire);
            const int64_t  diff = (int64_t) seq - (int64_t) (pos + 1);
            if (diff < 0) break;     // empty
            if (diff > 0)            // shouldn't happen with one consumer
                break;

            const double relMs = (double) ((int64_t) slot.timestampNs
                                             - (int64_t) sessionStartNs) / 1'000'000.0;

            char line [768];
            std::snprintf (line, sizeof (line),
                "[t=%12.3fms] TID=%-6llu %s %s %s a1=%p a2=%p "
                "id1=%-5d id2=%-5d i=%-6d f=%-10.4f msg=\"%s\" @ %s\r\n",
                relMs,
                (unsigned long long) slot.threadId,
                eventName (slot.evt),
                sevName   (slot.sev),
                slot.onAudioThread ? "[RT]" : "    ",
                slot.a1, slot.a2,
                (int) slot.id1, (int) slot.id2,
                (int) slot.iParam, (double) slot.fParam,
                slot.msg, slot.loc);
            batch += line;

            if (slot.stackFrames > 0)
                batch += formatStack (slot.stack, slot.stackFrames);

            // Release slot back to producers.
            slot.seq.store (pos + kRingCapacity, std::memory_order_release);
            readSeq.store  (pos + 1,             std::memory_order_relaxed);

            if (batch.length() > 32 * 1024)
            {
                out << batch;
                batch.clear();
            }
        }

        if (flushNow || batch.length() > 0)
        {
            out << batch;
            batch.clear();
            out.flush();
        }
    };

    while (running.load (std::memory_order_acquire))
    {
        drain (false);
        std::this_thread::sleep_for (std::chrono::milliseconds (kDrainSleepMs));
    }

    // Final drain at shutdown.
    drain (true);

    const auto drops = dropped.load();
    if (drops > 0)
        out << "\r\n*** " << (int64_t) drops
            << " entries DROPPED due to ring saturation. Increase kRingCapacity. ***\r\n";

    out << "\r\n=== Session end: "
        << juce::Time::getCurrentTime().toString (true, true) << " ===\r\n";
    out.flush();
}

} // namespace spacedust

#endif // SPACEDUST_ENABLE_SAFETY_LOGGING
