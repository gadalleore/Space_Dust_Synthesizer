#!/usr/bin/env python3
"""Wrap unguarded SpaceDust_DebugLog.txt write blocks in #if JUCE_DEBUG / #endif.

The codebase has 25-ish copies of the pattern::

    try
    {
        juce::File logFile = juce::File::getSpecialLocation(...)
            .getChildFile(... "SpaceDust_DebugLog.txt" ...);
        juce::FileOutputStream out(logFile);
        if (out.openedOk()) { ... out.flush(); }
    }
    catch (...) {}

These write to the user's Documents folder unconditionally - we want them
Debug-only.  This script wraps each whole try/catch block in #if JUCE_DEBUG /
#endif.  Run from the repo root.
"""

from __future__ import annotations
import re
import sys
from pathlib import Path

# Files we know contain direct debug-log writes (PluginEditor + PluginProcessor).
TARGETS = [
    Path("Source/PluginEditor.cpp"),
    Path("Source/PluginProcessor.cpp"),
]

# Match: a `try { ... }catch (...) {}` block whose body references the debug
# log filename.  Non-greedy so we don't span unrelated try blocks.
PATTERN = re.compile(
    r"""
    (^[ \t]*)try            # capture leading indent
    \s*\{
    [^{}]*                  # body before nested braces
    (?:\{[^{}]*\}[^{}]*)*   # zero or more single-level nested {...}
    SpaceDust_DebugLog\.txt # require the log filename inside the block
    [^{}]*
    (?:\{[^{}]*\}[^{}]*)*
    \}\s*
    catch\s*\(\.\.\.\)\s*\{[^{}]*\}
    """,
    re.MULTILINE | re.VERBOSE | re.DOTALL,
)


def already_guarded(text: str, match_start: int) -> bool:
    """Skip blocks already inside a #if JUCE_DEBUG region (best-effort check)."""
    region = text[max(0, match_start - 200) : match_start]
    last_if = region.rfind("#if JUCE_DEBUG")
    last_endif = region.rfind("#endif")
    return last_if > last_endif


def transform(text: str) -> tuple[str, int]:
    """Wrap each matching block with #if JUCE_DEBUG / #endif.

    Returns (new_text, replacement_count).
    """
    count = 0

    def _wrap(m: re.Match) -> str:
        nonlocal count
        if already_guarded(text, m.start()):
            return m.group(0)
        indent = m.group(1)
        count += 1
        return (
            f"{indent}#if JUCE_DEBUG\n"
            f"{m.group(0)}\n"
            f"{indent}#endif"
        )

    return PATTERN.sub(_wrap, text), count


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    total = 0
    for rel in TARGETS:
        path = repo_root / rel
        original = path.read_text(encoding="utf-8")
        updated, n = transform(original)
        if n == 0:
            print(f"  {rel}: no unguarded blocks found")
            continue
        path.write_text(updated, encoding="utf-8")
        print(f"  {rel}: wrapped {n} block(s) in #if JUCE_DEBUG")
        total += n
    print(f"\nDone. Wrapped {total} block(s) total.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
