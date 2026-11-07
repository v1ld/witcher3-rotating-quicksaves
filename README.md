# Rotating Quicksaves plugin (RotatingQuicksaves.asi)

The native half of the Witcher 3 mod **Rotating Quicksaves**. The Witcher 3 Remastered keeps only one quicksave: every F5 replaces the newest one. This plugin lifts that rule so the game can keep several. The mod's script then decides which quicksaves to keep. F5 stays the game's own quicksave key, so no key bindings change.

Install it together with the Rotating Quicksaves script mod (Nexus Mods), which prunes old quicksaves. On its own the plugin changes nothing you would notice: it waits for the script mod to check in, so without the script F5 keeps replacing your one quicksave as usual.

## What it changes

The hooks are installed once, when the game starts, and only if every target is found:

1. **Quicksaves stop replacing each other.** The game's save manager (`CUserProfileManagerWindowsSaving::PrepareSave`, virtual slot `0x80`) deletes the newest quicksave when a quicksave is requested with slot `-1`, which is what F5 does. The plugin hooks that slot and turns slot `-1` into `0` for quicksaves, which skips the deletion. Nothing else about saving changes. This rewrite is armed only after the script mod first asks for the status (it does so when a game loads), so the plugin alone can never make quicksaves pile up.
2. **A status answer for the script.** `GetNumSaveSlots` (virtual slot `0x108`) is an empty stub on PC that returns `-1`. For `SGT_ForcedCheckPoint` (value 4 in the engine's enum) the plugin answers `31000 + version` when active (1.0.0 answers 31100), or `30000 + reason` when inactive (1 switched off, 2 save manager not found, 3 save function check failed, 5 hook failed). Any other type still gets `-1`. Without the plugin the script sees `-1`.

It also logs every save request and every save-file delete or rename, with the calling address, to `RotatingQuicksaves.log`.

## Safety

- **No fixed addresses.** The vtables are found through the exe's RTTI (`.?AVCUserProfileManagerWindowsSaving@@`). The save function must reference the save-name format `%s_%lx_%lx_%lx`. No game code is patched; only virtual-table pointers are swapped.
- **All or nothing.** If any check fails, saving is not changed, and the reason goes to the log.
- **Chained hooks.** The plugin calls whatever pointer was in a vtable slot before it, so other plugins that hook the same slots keep working. Tested together with Unlock Save Slot Limits, which changes autosave and checkpoint limits by other means.
- **Writes nothing but its log.** No network, no registry, no other files. Save files are only ever deleted by the game's own code.
- **Off switch.** An empty file named `RotatingQuicksaves_disabled` next to the plugin turns all changes off.

Made for game build `0x6ADFBD7A` (patch 5.01; the game's own files number it 4.10).

## Building

You need a 64-bit Windows C compiler. The source is one C file with no dependencies.

**Linux or macOS (MinGW-w64)**

```sh
# Debian/Ubuntu: sudo apt install gcc-mingw-w64-x86-64   Arch: sudo pacman -S mingw-w64-gcc   macOS: brew install mingw-w64
make
```

**Windows, MSYS2**

Open the "MSYS2 MINGW64" shell, then:

```sh
pacman -S --needed mingw-w64-x86_64-gcc make
make CC=gcc
```

**Windows, Visual Studio**

Open "x64 Native Tools Command Prompt for VS" in this folder and run `build-msvc.bat`.

Each produces `RotatingQuicksaves.asi`. Without `make` the whole build is one command:

```sh
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -shared -s -static-libgcc -o RotatingQuicksaves.asi src/RotatingQuicksaves.c
```

## Using it in your own mod

The plugin does not depend on the Rotating Quicksaves script; any script mod can drive it. Two things are needed:

1. **Switch it on.** Call `theGame.GetNumSaveSlots(SGT_ForcedCheckPoint)` once, for example when a game loads. A value between 31000 and 32000 means the plugin is active (31100 for 1.0.0); `-1` means it is not installed, and `30000 + reason` means it switched itself off (see above). From that call on, quicksaves no longer replace each other.
2. **Delete old quicksaves yourself.** The plugin never deletes anything, so without pruning the quicksaves pile up. After a quicksave finishes, list the saves and delete the ones you don't want:

```
var saves : array< SSavegameInfo >;
var i : int;
theGame.ListSavedGames(saves);
for (i = 0; i < saves.Size(); i += 1)
{
	if (saves[i].slotType == SGT_QuickSave && /* your rule */)
		theGame.DeleteSavedGame(saves[i]);
}
```

The Rotating Quicksaves script (on Nexus Mods) is a complete example: it hooks the save started/completed events, keeps its own play-time stamps per quicksave, and applies its keep rule there.

## Installing

1. Install an ASI loader in `The Witcher 3/bin/x64_dx12/`: [Ultimate ASI Loader for The Witcher 3](https://www.nexusmods.com/witcher3/mods/13671) (Vortex-ready, `winmm.dll`), or any [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) variant. On Linux/Proton, add the launch option for the DLL name you use, for example `WINEDLLOVERRIDES="winmm=n,b" %command%`.
2. Copy `RotatingQuicksaves.asi` into `The Witcher 3/bin/x64_dx12/`.
3. Install the Rotating Quicksaves script mod.
4. Start the game and load a save, then check `bin/x64_dx12/RotatingQuicksaves.log`. It should say `ACTIVE`, then `script mod seen` after the load.

To remove it, delete the `.asi`. Quicksaves then replace each other again, as in the unmodded game.

## Version

1.0.0, the first public release.

## License

MIT, see LICENSE.
