# Space Dust - QA Suite

Automated quality-assurance scripts for the Space Dust VST3. Windows-first
(PowerShell 5.1+). Covers static code health, pluginval validation, installer
smoke testing, and crash-marker hygiene.

---

## Quick start

```powershell
# One-time: install JUCE somewhere, set the env var, and run the build once
$env:JUCE_DIR = "C:\path\to\JUCE"   # adjust to your local JUCE checkout
cmake -B build -DJUCE_DIR=$env:JUCE_DIR
cmake --build build --config Release -j 1

# Run the entire QA pipeline (downloads pluginval the first time)
.\run-full-qa.ps1
```

A green `ALL QA CHECKS PASSED` line means everything passed; otherwise the
summary table at the end shows which step failed.

---

## What gets checked

### Static code health (run-full-qa.ps1, step 1)

| Check | What it verifies | Why it matters |
|-------|------------------|----------------|
| `listener-balance` | `addParameterListener` / `removeParameterListener` IDs match in `PluginProcessor.cpp` | Unmatched listeners cause crash-on-reload loops in Ableton (root cause of the 2026-05-08 incident) |
| `safe-param-reads` | No raw `*apvts.getRawParameterValue()` outside the `safeGetParam` helper | Wrong/missing param ID → null deref crash |
| `safe-mode-marker` | `setStateInformation` writes/clears the crash-loop marker | Breaks crash-recover-crash loops; lost state is better than a bricked session |
| `no-orphan-new` | All `new` calls are inside known ownership-transfer patterns | Catches `new` without a corresponding owner |
| `utf8-safety-helpers` | `safeString()` is in use | Prevents `juce_String.cpp:327` assertions from bad UTF-8 |

### pluginval (run-pluginval.ps1, step 2)

Runs Tracktion's [pluginval](https://github.com/Tracktion/pluginval) at
**strictness level 10** with `--repeat 3 --randomise`. This exercises:
- Plugin instantiation/destruction (memory leak detection)
- Editor open/close cycles (UI lifecycle bugs)
- Parameter automation under stress (real-time safety)
- State save/restore with garbage and edge-case inputs
- Threading correctness (audio thread vs message thread)
- Sample-rate changes and buffer-size changes

### Installer smoke test (test-installer.ps1, step 4)

Compiles the Inno Setup installer (if `ISCC.exe` is installed), runs it
silently, and verifies that:
- The VST3 bundle lands at `C:\Program Files\Common Files\VST3\Space Dust.vst3\`
- `moduleinfo.json` parses as valid JSON with a factory class entry

### Crash-marker hygiene (step 5)

Checks for a stale `%APPDATA%\SpaceDust\state_restore_in_progress.marker`
file. If present, the *previous* host session crashed mid-restore and the
safe-mode fallback hasn't been triggered yet. Useful for catching repeated
silent failures.

---

## Script reference

### `download-pluginval.ps1`

Downloads the latest Windows pluginval into `./tools/pluginval/`. Re-runs are
no-ops unless `-Force` is passed.

### `run-pluginval.ps1 [-Strictness 10] [-Repeat 3] [-NoBuild]`

Builds Release if needed (serial `-j 1` to avoid the JUCE SharedCode race),
runs pluginval, writes `build/pluginval-report.log`.

### `test-installer.ps1 [-SkipPackage]`

Compiles + runs installer silently. Pass `-SkipPackage` if you've already got
a built installer EXE you want to verify.

### `run-full-qa.ps1 [-SkipBuild] [-SkipPluginval] [-SkipInstaller]`

Master orchestrator. Returns exit code 0 on full pass, 1 if any step fails.

---

## Optional: leak detection

### AddressSanitizer (MSVC)

```powershell
cmake -B build-asan -DENABLE_ASAN=ON
cmake --build build-asan --config Debug -j 1
```

Detects heap/stack overflows, use-after-free, double-free at runtime. Open
the resulting VST3 in a host and run normal workflows; ASan reports to stderr
on detection. Significant slowdown - do not ship.

### Visual Leak Detector (KindDragon/vld)

```powershell
cmake -B build-vld -DENABLE_VLD=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-vld --config Debug -j 1
```

Then copy `vld_x64.dll` and `dbghelp.dll` from
`C:\Program Files (x86)\Visual Leak Detector\bin\Win64\` into the host's
`Program\` folder. Leak report prints to Visual Studio Output (or DebugView)
when the host exits.

> JUCE already runs `LeakedObjectDetector` on plugin unload, which is usually
> more useful than VLD for in-plugin C++ leaks. VLD adds value for raw
> `new`/`malloc`, OS handles, and third-party leaks JUCE doesn't track.

---

## CI

`.github/workflows/qa.yml` runs the same suite on every push to `main` and
every PR. Artifacts uploaded on each run:
- `SpaceDust-VST3` - the built plugin
- `pluginval-report` - full pluginval log
- `SpaceDust-Installer` - signed installer (main branch only)

---

## Known limitations

- pluginval can't fully test MPE behaviour - that needs an actual MPE source.
  See the `project-mpe-testing-next` memory note for the manual test plan with
  Magyar's MPE Emulator in FL Studio.
- The installer smoke test installs to the system VST3 path; running it
  overwrites whatever Space Dust copy is currently installed. The dev script
  `copy_vst.ps1` is the lighter alternative for everyday iteration.
- Ableton Live 10 caches plugin info in `PluginScanner.txt`; if you change the
  VST3 binary, Ableton may not pick it up until the next plugin scan. The
  `build-and-launch.ps1` script handles the rescan dance.

---

## Background

The QA suite exists because a Space Dust instance bricked a Live 10 session
on 2026-05-08 (4 crashes in 2 minutes during session recovery). Forensic
analysis traced the cause to a class of bugs - dangling parameter listeners
during plugin destruction - that had been historically introduced and
silently fixed twice in this codebase. The static checks here are designed
to catch that family of bug at commit time rather than discovering it again
in a live session.

See `docs/FIX_SUMMARY.md` for the full history of fixes.
