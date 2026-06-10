# macOS packaging support files

This directory is used by `package-macos.sh` (run from the project root).

## Layout after a packaging run

- `payload/` — the install layout that will land on the user's machine:
  - `Library/Audio/Plug-Ins/VST3/Space Dust.vst3`
  - `Library/Audio/Plug-Ins/Components/Space Dust.component`
  - `Applications/Space Dust.app` (when the Standalone choice is selected)
  - The uninstaller .command is always installed (background, not tied to a visible choice)
- `scripts/` — files that travel inside the component packages and are executed by the Installer:
  - `postinstall` — bash script that installs the factory presets into the console user's
    `~/Documents/Space Dust/Presets` (only new files) and writes
    `~/Library/Application Support/Space Dust/config.xml`.
  - `Presets/*.sdpreset` — the 50 factory presets (copied from the shared
    `installer/Files/Presets/` at packaging time).
  - `README.txt` — the user-facing preset folder documentation (renamed from
    `installer/Support/README-Presets.txt`).
- `Distribution.xml` — the choice UI definition (two visible choices: Plugins
  and Standalone; uninstaller is always installed via hidden choice) used by
  `productbuild`.

## How it relates to the Windows side

- Factory presets are the single source of truth in `installer/Files/Presets/`.
- Both `package-installer.ps1` and `package-macos.sh` consume that folder.
- The Windows installer also writes a `config.xml` (in `%APPDATA%` or `ProgramData`)
  with the same `<SpaceDustConfig presetFolder="..."/>` shape that the macOS
  postinstall produces.

See `MAC_BUILD.md` (section 6) and the header comments in `package-macos.sh` for
full usage and notarization instructions.

These files contain **no changes to the plugin source code** — they are pure
packaging / distribution.
