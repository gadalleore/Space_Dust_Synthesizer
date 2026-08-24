// =====================================================================
//  Trim session test
//  ---------------------------------------------------------------------
//  A trim session is what makes a marker sound while it is being dragged:
//  the slot's audio is decoded once, rebuilt many times, and written to
//  disk once at the end.
//
//  The GEOMETRY of a trim is WaveAnalysis::regionForTrim and is tested in
//  wavetable_test. What is tested here is the SEQUENCING, which is pure
//  state and needs no JUCE: what is legal when, whether anything actually
//  moved, and whether a last apply is still owed when the mouse comes up.
//
//  Build & run:
//      cmake --build build --config Release --target trimsession-test
//      ./build/trim_session_test/Release/trim_session_test.exe
// =====================================================================
#include "../../Source/TrimSession.h"

#include <cstdio>
#include <string>

namespace
{
    int failures = 0;

    void check(bool ok, const std::string& what)
    {
        if (!ok)
        {
            // Never the string "error :" -- MSBuild reads that as a build failure.
            std::printf("  FAIL  %s\n", what.c_str());
            ++failures;
        }
    }

    using Points = TrimSession::Points;
}

int main()
{
    std::printf("Trim session tests\n==================\n");

    std::printf("\nA new session is closed:\n");
    {
        TrimSession s;
        check(!s.isOpen(), "a new session claimed to be open");
        check(s.group() == -1, "a closed session named a group");
        check(s.slot() == -1, "a closed session named a slot");
        check(!s.wants(), "a closed session asked for work");
        std::printf("  closed, names nothing, asks for nothing\n");
    }

    std::printf("\nOpening records the slot:\n");
    {
        TrimSession s;
        check(s.open(3, 5, Points{100, 200}), "opening a fresh session was refused");
        check(s.isOpen(), "an opened session claimed to be closed");
        check(s.group() == 3, "the session forgot its group");
        check(s.slot() == 5, "the session forgot its slot");
        std::printf("  group 3, slot 5\n");
    }

    std::printf("\nThe opening numbers are already applied:\n");
    {
        // The slot on screen ALREADY plays with these numbers -- they are read
        // out of it. Republishing them would be a rebuild that changed nothing.
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        check(!s.wants(), "opening asked for a rebuild that would change nothing");
        std::printf("  no rebuild asked for\n");
    }

    std::printf("\nA still mouse costs nothing:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        check(s.wants(), "a moved marker did not ask for a rebuild");
        check(s.pendingPoints().start == 150, "the session lost the new start");
        check(s.pendingPoints().end == 200, "the session lost the end");
        s.applied();
        check(!s.wants(), "a marker that was applied asked to be applied again");
        s.pending(Points{150, 200});
        check(!s.wants(), "the same numbers asked for a second rebuild");
        std::printf("  one rebuild per move, none for a still mouse\n");
    }

    std::printf("\nClosing owes a last apply when the mouse outran the timer:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        s.applied();
        s.pending(Points{180, 200});   // moved again, no tick before mouse-up

        const auto c = s.close();
        check(c.wasOpen, "closing an open session said it was closed");
        check(c.applyOwed, "the last position of the drag was dropped");
        check(c.last.start == 180, "closing reported the wrong last start");
        check(c.last.end == 200, "closing reported the wrong last end");
        check(!s.isOpen(), "the session stayed open after closing");
        std::printf("  last position 180 survives the mouse coming up\n");
    }

    std::printf("\nClosing owes nothing when the timer kept up:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        s.pending(Points{150, 200});
        s.applied();

        const auto c = s.close();
        check(c.wasOpen, "closing an open session said it was closed");
        check(!c.applyOwed, "closing asked for a rebuild that would change nothing");
        std::printf("  no needless rebuild\n");
    }

    std::printf("\nClosing a session that was never opened is safe:\n");
    {
        TrimSession s;
        const auto c = s.close();
        check(!c.wasOpen, "closing a closed session said it had been open");
        check(!c.applyOwed, "closing a closed session asked for work");
        check(!s.isOpen(), "closing a closed session opened it");
        std::printf("  nothing happens\n");
    }

    std::printf("\nTwo gestures cannot interleave:\n");
    {
        TrimSession s;
        s.open(0, 0, Points{100, 200});
        check(!s.open(1, 2, Points{0, 0}), "a second slot was opened over the first");
        check(s.group() == 0 && s.slot() == 0, "the refused open moved the session");
        check(s.open(0, 0, Points{100, 200}), "reopening the same slot was refused");
        std::printf("  a different slot is refused, the same slot is not\n");
    }

    std::printf("\n%s\n", failures == 0 ? "All trim session tests passed."
                                        : "Some trim session tests FAILED.");
    return failures == 0 ? 0 : 1;
}
