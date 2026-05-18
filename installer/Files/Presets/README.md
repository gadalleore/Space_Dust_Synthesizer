# Factory presets staging

Drop `.sdpreset` files into this folder to ship them with the Windows
installer. The extension must match `PresetManager::presetExtension`
in `Source/PresetManager.h` — currently `.sdpreset`.

## Workflow

1. Open Ableton (or any DAW), load **Space Dust**, design a sound, then
   **save the preset from inside the plug-in UI**. The plug-in writes the
   `.sdpreset` file to the folder configured in
   `%ProgramData%\Space Dust\config.xml` (whatever you picked in the
   installer wizard).
2. From the project root, run `.\package-installer.ps1`. The script:
   - rebuilds the Release VST3,
   - copies all `*.sdpreset` files from your Space Dust presets folder
     into this directory (`installer/Files/Presets/`),
   - compiles `installer/Output/SpaceDust-Synthesizer-1.0-Setup.exe`.
3. Commit the new/changed `.sdpreset` files in this folder; they become
   part of the public factory preset set on the next push.

## How they install on the customer's machine

The Inno Setup `[Files]` entry uses:

```ini
Source: "Files\Presets\*.sdpreset"; DestDir: "{code:GetPresetsDir}";
    Flags: ignoreversion onlyifdoesntexist skipifsourcedoesntexist
```

- `onlyifdoesntexist`: an upgrade/re-install will **never overwrite** a
  preset the customer has already modified locally.
- `skipifsourcedoesntexist`: the installer still compiles even if this
  folder happens to be empty (useful very early in development).

## Naming

Use plain filenames like `Init.sdpreset`, `Pluck Lead.sdpreset`,
`808 Sub Bass.sdpreset`. The plug-in's preset menu sorts alphabetically.
