#pragma once

/**
    One drag of a start or end marker, from the mouse going down to it coming up.

    Moving a marker used to be heard only after the key was released and pressed
    again, because putting a marker into effect decodes the slot's audio again,
    rebuilds it and writes the index file -- work that is right once a gesture is
    finished and wrong sixty times a second.

    A session is what lets the middle of that happen often and the ends happen
    once. The audio is decoded when the session opens and held; the rebuild runs
    on a timer while the mouse moves; the index file is written when it closes.

    This class holds none of that. It holds only WHEN each of them is allowed and
    whether anything has actually moved -- which is pure state, needs no JUCE, and
    is therefore the part that can be tested without a window or a disk. The
    geometry of a trim is WaveAnalysis::regionForTrim and is tested separately.
*/
class TrimSession
{
public:
    /** Where the two markers stand, in samples of the imported file. */
    struct Points
    {
        int start = 0;
        int end = 0;
    };

    /** What close() found. applyOwed is the one that matters: a fast drag can
        move the markers after the last timer tick, and without this that final
        position would be dropped on the floor. */
    struct Closing
    {
        bool wasOpen = false;
        Points last {};
        bool applyOwed = false;
    };

    bool isOpen() const noexcept { return openFlag; }

    /** The slot being dragged, or -1 for both when nothing is. */
    int group() const noexcept { return openFlag ? groupIndex : -1; }
    int slot() const noexcept { return openFlag ? slotIndex : -1; }

    /** Begin a gesture on a slot, with the markers where they already stand.

        Those opening numbers count as ALREADY APPLIED, because they were read
        out of the slot and the slot already plays with them. Treating them as
        pending would make every mouse-down rebuild a slot into exactly what it
        already was.

        Refused, changing nothing, while a DIFFERENT slot is open: two gestures
        must never interleave. Opening the same slot again is accepted and does
        nothing, so a stray second mouse-down cannot lose the drag. */
    bool open (int newGroup, int newSlot, Points initial) noexcept
    {
        if (openFlag && (groupIndex != newGroup || slotIndex != newSlot))
            return false;

        openFlag = true;
        groupIndex = newGroup;
        slotIndex = newSlot;
        pendingPos = initial;
        appliedPos = initial;
        return true;
    }

    /** Where the markers have been dragged to. Costs nothing and may be called
        on every mouse move. */
    void pending (Points p) noexcept { pendingPos = p; }

    /** Whether a rebuild would change anything. False for a still mouse, which
        is what keeps a paused drag free. */
    bool wants() const noexcept
    {
        return openFlag && (pendingPos.start != appliedPos.start
                            || pendingPos.end != appliedPos.end);
    }

    Points pendingPoints() const noexcept { return pendingPos; }

    /** Record that the pending numbers were put into the slot. */
    void applied() noexcept { appliedPos = pendingPos; }

    /** End the gesture, and say whether the last position still needs applying. */
    Closing close() noexcept
    {
        Closing result;
        result.wasOpen = openFlag;
        result.last = pendingPos;
        result.applyOwed = wants();

        openFlag = false;
        groupIndex = -1;
        slotIndex = -1;
        return result;
    }

private:
    bool openFlag = false;
    int groupIndex = -1;
    int slotIndex = -1;

    /** Where the mouse has dragged to, and where the slot was last rebuilt to.
        They differ exactly when a rebuild is owed. */
    Points pendingPos {};
    Points appliedPos {};
};
