# Factory presets staging

Drop `.xml` preset files into this folder to ship them with the Windows
installer.

## Workflow

1. Open Ableton (or any DAW), load **Space Dust**, design a sound, then
   **save the preset from inside the plug-in UI**. The plug-in writes the
   `.xml` to the folder configured in `%ProgramData%\Space Dust\config.xml`
   (typically `Documents\Space Dust\Presets`).
2. From the project root, run `.\package-installer.ps1`. The script:
   - rebuilds the Release VST3,
   - copies all `*.xml` files from your Space Dust presets folder into
     this directory (`installer/Files/Presets/`),
   - compiles `installer/Output/SpaceDust-Synthesizer-1.0-Setup.exe`.
3. Commit the new/changed `.xml` files in this folder; they become part of
   the public factory preset set on the next push.

## How they install on the customer's machine

The Inno Setup `[Files]` entry uses:

```ini
Source: "Files\Presets\*.xml"; DestDir: "{code:GetPresetsDir}";
    Flags: ignoreversion onlyifdoesntexist skipifsourcedoesntexist
```

- `onlyifdoesntexist`: an upgrade/re-install will **never overwrite** a
  preset the customer has already modified locally.
- `skipifsourcedoesntexist`: the installer still compiles even if this
  folder happens to be empty (useful very early in development).

## Naming

Use plain filenames like `Init.xml`, `Pluck Lead.xml`, `808 Sub Bass.xml`.
The plug-in's preset menu sorts alphabetically.
