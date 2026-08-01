/*
    FloatingShell.h
    ---------------
    Space Dust's real window: a plugin-owned, per-pixel-alpha desktop window the
    UI lives in, instead of the rectangle the host hands us.

    Ported from Sol Voice Tuner (2026-07-31). The finding this rests on is Sol's
    (63C-50, settled by experiment 2026-07-27): a host's editor rectangle is
    opaque -- Ableton Live and FL Studio both composite it against black, so an
    unpainted region renders as a black wedge, not as see-through. A window we
    create ourselves sits outside that compositing, so
    `ComponentPeer::windowIsSemiTransparent` gives us a layered window where
    alpha actually works.

    The host still gets a small opaque stub editor -- that is unavoidable. The
    stub owns this shell and keeps it positioned.

    Two deliberate differences from Sol's version:

      - CONTENT IS NOT SCALED HERE. Sol lays its content out at a logical size
        and scales it with a transform. Space Dust already does its own uniform
        scaling (mainView's transform plus the matching paintScale in the plate's
        paint), so the content simply fills the shell and scales itself. Adding
        Sol's transform on top would scale everything twice. The shell still owns
        the aspect lock and the size limits, because those belong to whoever owns
        the resizer.

      - THE KEYBOARD POLICY IS A CHOICE, NOT A CONSTANT. Sol refuses the keyboard
        everywhere. Space Dust cannot: the Standalone build has a playable QWERTY
        keyboard that must hold focus. See setKeyboardFocusPolicy below.

    Costs we take on by doing this, none of which JUCE handles for us:
      - focus and z-order are ours to manage
      - dragging and resizing are ours (there is no host title bar here)
      - the shell must be torn down with the editor or it leaks a desktop window
*/

#pragma once

// Space Dust includes JUCE modules individually rather than through the Projucer's
// JuceHeader.h aggregate (which this project does not generate). Everything used here
// -- Component, ComponentDragger, ComponentBoundsConstrainer, ResizableCornerComponent,
// ComponentPeer -- lives in juce_gui_basics.
#include <juce_gui_basics/juce_gui_basics.h>

class FloatingShell final : public juce::Component
{
public:
    /** Size of the grab area in the bottom-right used to resize the shell. */
    static constexpr int kResizerSize = 18;

    FloatingShell()
    {
        // The reason the whole class exists: never claim to fill our bounds, so
        // anything the content leaves unpainted stays truly transparent.
        setOpaque (false);

        resizer = std::make_unique<juce::ResizableCornerComponent> (this, &constrainer);
        addAndMakeVisible (*resizer);
    }

    ~FloatingShell() override
    {
        // Belt and braces: an editor that somehow skips its own teardown must
        // still not leave a desktop window behind.
        hideFromDesktop();
    }

    //--------------------------------------------------------------------------
    // Keyboard policy
    //--------------------------------------------------------------------------
    /** Decides whether this window may ever take keyboard focus.

        Pass FALSE for the plugin builds. Our own desktop window would otherwise
        become the keyboard's owner the moment it is clicked, and the DAW would
        stop seeing the spacebar -- you could not start and stop the transport
        while working the UI, which is not a trade anyone would make for a
        plugin.

        Two separate things get switched off, and only the second matters on
        Windows:

          - setWantsKeyboardFocus stops a control asking for focus itself
            (Slider and Button both ask by default);
          - setMouseClickGrabsKeyboardFocus is the one that counts. JUCE's Win32
            peer answers WM_MOUSEACTIVATE with MA_NOACTIVATE when the window's
            top-level component has it cleared, so Windows never activates us on
            a click. Mouse events still arrive; focus simply never moves.

        (windowIgnoresKeyPresses, set in showOnDesktop, covers macOS and Linux
        only -- the Win32 peer never reads it. Only the NSView, UIView and X11
        peers test that flag, which is why the work is done here as well.)

        Pass TRUE for the Standalone build. There is no host transport to
        protect, and the on-screen keyboard has to hold focus for the QWERTY
        keys to play notes.

        Applied to the whole subtree rather than to the handful of controls that
        happen to be clickable today, because a single component added later
        without it would quietly bring the bug back.

        The cost when focus is refused: no text field can work inside this
        window. Space Dust's only text entry is the preset-name dialog, which is
        a separate AlertWindow and so is unaffected. */
    void setKeyboardFocusPolicy (bool shellMayTakeFocus)
    {
        allowFocus = shellMayTakeFocus;
        applyFocusPolicy (*this);
    }

    /** Re-applies the policy to everything currently parented. Call after adding
        content, since children added since the last call have not been told. */
    void refreshFocusPolicy()
    {
        applyFocusPolicy (*this);
    }

    //--------------------------------------------------------------------------
    // Geometry
    //--------------------------------------------------------------------------
    /** Locks the corner to uniform scaling and bounds how far it can go.

        The aspect ratio makes the corner a ZOOM rather than a reshape: width and
        height only ever move together, so dragging it magnifies the whole
        interface instead of stranding fixed-pixel furniture in a larger window.
        The limits are supplied by the caller already derived along that same
        ratio, so no limit can demand a shape the aspect lock forbids. */
    void setAspectAndLimits (double aspect, int minW, int minH, int maxW, int maxH)
    {
        if (aspect <= 0.0)
            return;

        constrainer.setFixedAspectRatio (aspect);
        constrainer.setSizeLimits (minW, minH, maxW, maxH);
    }

    /** Puts this shell on screen as a layered top-level window. Separate from
        the constructor so the caller controls when the window appears. */
    void showOnDesktop()
    {
        if (isOnDesktop())
            return;

        addToDesktop (juce::ComponentPeer::windowIsTemporary
                    | juce::ComponentPeer::windowIsSemiTransparent
                    | juce::ComponentPeer::windowIgnoresKeyPresses);
        setAlwaysOnTop (true);
        setVisible (true);

        // Anything parented since the policy was last applied has to be told.
        applyFocusPolicy (*this);
    }

    void hideFromDesktop()
    {
        if (isOnDesktop())
            removeFromDesktop();
    }

    /** The UI that fills the shell. Not owned -- the caller keeps it alive. */
    void setContent (juce::Component* newContent)
    {
        if (content == newContent)
            return;

        if (content != nullptr)
            removeChildComponent (content);

        content = newContent;

        if (content != nullptr)
        {
            addAndMakeVisible (content);
            content->toBack();          // keep the resizer grabbable on top
            applyFocusPolicy (*content);
        }

        resized();
    }

    void resized() override
    {
        // Content fills the shell and does its own scaling -- see the header
        // comment. Nothing is transformed here.
        if (content != nullptr)
            content->setBounds (getLocalBounds());

        if (resizer != nullptr)
            resizer->setBounds (getWidth()  - kResizerSize,
                                getHeight() - kResizerSize,
                                kResizerSize, kResizerSize);
    }

    //--------------------------------------------------------------------------
    // Dragging -- there is no host title bar on a window we own, so the surface
    // itself moves it. Children that handle their own clicks (controls) consume
    // the event before it reaches here; the plate behind them is click-through,
    // so a press on empty background lands on the shell and drags the window.
    //--------------------------------------------------------------------------
    void mouseDown (const juce::MouseEvent& e) override
    {
        // Settle to home before the drag begins, so the position the dragger
        // starts from is the real one and not a shaken-out frame.
        setShakeOffset ({});

        dragging = true;
        dragger.startDraggingComponent (this, e);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        dragger.dragComponent (this, e, nullptr);
        home = getPosition();       // dragged: here is the new home
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = false;
        home = getPosition();
    }

    //--------------------------------------------------------------------------
    // Shake -- the window is thrown around by whatever is coming through it
    //--------------------------------------------------------------------------
    /** Where the window sits when it is not being thrown anywhere. Everything
        that positions the shell deliberately goes through this rather than
        setTopLeftPosition, or the next shake frame would snap it back. */
    void setHomePosition (juce::Point<int> p)
    {
        home = p;
        setTopLeftPosition (home + shakeOffset);
    }

    juce::Point<int> getHomePosition() const noexcept { return home; }

    /** Displaces the window from home. Ignored mid-drag: the pointer owns the
        window's position while the user has hold of it. */
    void setShakeOffset (juce::Point<int> offset)
    {
        if (dragging || offset == shakeOffset)
            return;

        shakeOffset = offset;
        setTopLeftPosition (home + shakeOffset);
    }

private:
    /** Strictly one-way: this only ever REMOVES focus, it never grants it.

        When focus is allowed there is nothing to do, and doing something would
        be actively wrong -- forcing setWantsKeyboardFocus(true) across the tree
        would make every knob and label demand focus, and the Standalone
        keyboard's careful focus handoff (StandaloneKeyboard::focusLost) would
        fight a hundred new claimants. Components keep whatever they asked for
        themselves. */
    void applyFocusPolicy (juce::Component& c)
    {
        if (allowFocus)
            return;

        c.setWantsKeyboardFocus (false);
        c.setMouseClickGrabsKeyboardFocus (false);

        for (auto* child : c.getChildren())
            if (child != nullptr)
                applyFocusPolicy (*child);
    }

    juce::Component* content = nullptr;

    /** Plugin builds refuse the keyboard so the DAW keeps its transport keys;
        Standalone accepts it so the QWERTY keyboard plays. Defaults to the safe
        one for a plugin. */
    bool allowFocus = false;

    juce::Point<int> home;
    juce::Point<int> shakeOffset;
    bool             dragging = false;

    juce::ComponentDragger           dragger;
    juce::ComponentBoundsConstrainer constrainer;
    std::unique_ptr<juce::ResizableCornerComponent> resizer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FloatingShell)
};
