# HMBTN ReFarmed — Harvest Moon: Back to Nature, native PC port

An unofficial, native **Windows and Linux** version of *Harvest Moon: Back to Nature*
(PlayStation, USA, SLUS-01115), made by **static recompilation**: the game's original
PlayStation program is translated ahead of time into C and compiled into a normal x86-64
executable. No emulator runs the game, and no PlayStation BIOS is needed.

ReFarmed is a fork of [GabrielBigardi/HarvestMoonBTN-Recomp](https://github.com/GabrielBigardi/HarvestMoonBTN-Recomp),
the port itself, and adds sharper graphics, co-op for two farmers, time-saving options, optional
gameplay mods and a launcher that builds the game from your disc and keeps it up to date.

> **No game files are included in this repository.** This is a "bring your own copy"
> project: the build reads the game from **your own disc image** and creates the native
> version on your computer. See [Why you need your own copy](#why-you-need-your-own-copy).

- Plays like the original: same game logic, same graphics, music and timing (NTSC, 59.29 fps)
- **Graphics:** internal resolution 2–4× (up to 8× with the Vulkan renderer), precise 3D
  vertices (PGXP) without the holes they open, perspective-correct textures, 2D objects that
  scale with depth, widescreen (16:9, 16:10, 21:9) including the title screen and menus, a
  camera that moves at 60 fps, texture packs and redrawn portraits
  ([docs/GRAPHICS.md](docs/GRAPHICS.md))
- **Co-op:** two farmers on one PC, or online with a friend ([Co-op](#co-op))
- **Time savers:** shorter loading, quicker menus and text, cutscenes at full speed, saving
  without the slot menu, hold-to-fast-forward
- **Mods and cheats**, all off unless you switch them on ([Mods and cheats](#mods-and-cheats))
- **Launcher** in English, Portuguese and Spanish: settings, building from your disc, updates
- Keyboard and game controller support (Xbox / XInput and other SDL-supported controllers), fully remappable
- Window or fullscreen (sharp-bilinear, sharp or smooth scaling, 4:3 or square pixels), screenshots
- Saving works as on the PlayStation, on a standard memory card file usable in emulators too

## Contents

- [Why you need your own copy](#why-you-need-your-own-copy)
- [Bring your own copy](#bring-your-own-copy)
- [Building](#building)
- [Playing](#playing)
- [Controls](#controls)
- [Configuration](#configuration)
- [Co-op](#co-op)
- [Mods and cheats](#mods-and-cheats)
- [Saves](#saves)
- [How it was ported](#how-it-was-ported)
- [Project layout](#project-layout)
- [Troubleshooting](#troubleshooting)
- [Status](#status)
- [License and legal](#license-and-legal)

## Why you need your own copy

*Harvest Moon: Back to Nature* is a commercial game whose program code, graphics, music
and text are copyrighted by their owners. This repository contains **only original work**:
the recompiler, a native reimplementation of the PlayStation hardware and BIOS, the
enhancements, the launcher and the build scripts. Nothing from the game is in it.

Everything that comes from the game is created locally, from your disc image, when you
build, and is excluded from git by [`.gitignore`](.gitignore):

| Created by the build | Contains | In the repository? |
|---|---|---|
| `extract/` | the game's executable and code files, read from your disc | no |
| `generated/` | the game's program, translated to C | no |
| `build/`, `dist/` | the compiled port: **the executable contains the game's translated code** | no |
| `HarvestMoonBTN.pak` (optional) | the game's data (graphics, music, text), repacked from your disc | no |

This is also why there is **no ready-made game to download**: a pre-built executable would
contain the game's program code (translated, but still the game's), and the data pack would
contain the game's data. The only download, the launcher, contains none of it: it turns your
copy into the native version on your own machine. Please don't
share your build output or your data pack.

## Bring your own copy

You need a disc image of **Harvest Moon: Back to Nature, USA version (SLUS-01115)** in
`.cue` + `.bin` format (raw Mode2/2352) made from a disc you own. Any dumping tool that
creates a raw BIN/CUE image works: for example ImgBurn or Redump's
[MPF](https://github.com/SabreTools/MPF) on Windows, or `cdrdao` on Linux. Images in
other formats (CHD, PBP, ISO with 2048-byte sectors) need to be converted back to
`.bin`/`.cue` first.

Only the USA release is supported, because the recompiled code is specific to that exact
program. The build checks the files it reads from your disc against these SHA-256 hashes
and stops with a clear message if they don't match:

| File on the disc | SHA-256 |
|---|---|
| `SLUS_011.15` | `34e3b35dcd3fb1cd9a7d4813b8a9ef987915826e3d436f1908ecbe2d4fdf1444` |
| `A_FILE.HDT` | `0cae44d6bdb29e078a182d7cd9a780c21e35dc54f55eb94c2d9ea8deb20c82dd` |
| `A_FILE.BIN` | `612f9ac8279a364c2fdd19cb75e63df2e892a6f52b6f1d8081ea8560b18decd4` |

## Building

### Windows, the easy way: the launcher

1. Download `HarvestMoonBTN-Launcher.exe` from
   [Releases](https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/releases/latest) and put it in an
   empty folder.
2. Open it, go to **Build from my disc**, pick your `.cue` and build. The first time it
   downloads only the parts of the compiler the build uses (about 65 MB, each file checked
   against a pinned SHA-256). Your disc's files and the build stay in `build-files/` there.
3. Press **Save and play**.

No Python or anything else is needed. To give the game to a friend, give them the launcher;
they build from their own disc.

**Updates.** The launcher checks GitHub for a newer release when it opens. **Update** replaces
the exe with the new one (checked against the SHA-256 GitHub gives for it) and rebuilds the game
with the new sources, from the files it took from your disc earlier, so the disc image isn't
needed again. Your saves, settings and textures stay. Both co-op players get the same version
this way.

Windows may warn about the launcher the first time ("Windows protected your PC"): it isn't
code-signed. Each release says the commit it was built from, and
`gh attestation verify HarvestMoonBTN-Launcher.exe --repo d4rksp4rt4n/HMBTN-ReFarmed` checks
that GitHub built it from this repository.

### Windows, from the source

Requirements: [Python 3.8+](https://www.python.org/downloads/) and about 1 GB of disk
space for the compiler. (On Windows the command may be `py` instead of `python`.)

```bat
git clone https://github.com/d4rksp4rt4n/HMBTN-ReFarmed.git
cd HMBTN-ReFarmed
python tools\fetch_deps.py
python build.py
```

1. `tools/fetch_deps.py` downloads the toolchain into `deps/`:
   [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (clang) and the
   [SDL2](https://libsdl.org) development files. Each download is checked against a pinned
   SHA-256. Nothing is installed system-wide.
2. Put your `.cue` and `.bin` into the [`disc/`](disc/) folder (any file names; git ignores
   everything in it), or pass the path with `--disc`.
3. `build.py` reads your disc image, recompiles the game code, compiles it and puts a
   ready-to-play folder in **`dist/HarvestMoonBTN-Windows-x64/`**. With PyInstaller installed
   (`pip install pyinstaller`) it also builds the launcher exe for that folder; without it the
   folder gets `launcher.pyw`, the same launcher run with Python.

### Linux

**With the launcher:** download `HarvestMoonBTN-Launcher-Linux` from
[Releases](https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/releases/latest), put it in an empty folder,
make it executable (`chmod +x HarvestMoonBTN-Launcher-Linux`) and open it; **Build from my disc**
works as on Windows and it updates itself the same way. Nothing needs installing, Bazzite and other
immutable systems included: the first build downloads [Zig](https://ziglang.org) (a self-contained
C/C++ compiler) and the SDL2 headers, about 65 MB, each checked against a pinned SHA-256.

**From the source:** Python 3.8+, a C and C++ compiler and the SDL2 development package.

```sh
sudo apt install build-essential python3 libsdl2-dev      # Debian / Ubuntu
sudo dnf install gcc gcc-c++ python3 SDL2-devel           # Fedora
sudo pacman -S base-devel python sdl2                     # Arch

git clone https://github.com/d4rksp4rt4n/HMBTN-ReFarmed.git
cd HMBTN-ReFarmed
python3 build.py
```

The result is in **`dist/HarvestMoonBTN-Linux-x64/`**; `launcher.pyw` there runs with Python
(and Tk). `renderer = vulkan` works there too (X11 or Wayland), with the system's Vulkan
driver (`libvulkan.so.1`, from Mesa or the NVIDIA driver); without it the game uses the CPU
renderer. To build the Linux version on Windows instead (cross-compiling with
[Zig](https://ziglang.org)), run `python tools\fetch_deps.py --linux` once and then
`python build.py --linux`.

### Build options

```
python build.py [--disc PATH] [--pak] [--zip] [--into DIR] [--linux] [--debug] [--jobs N] [--clean]
```

| Option | |
|---|---|
| `--disc PATH` | your disc image (`.cue` or `.bin`); default: the first one in `disc/` |
| `--pak` | also create the data pack (see [Playing](#playing)) so the game runs without the disc image |
| `--zip` | also create a `.zip` of the dist folder |
| `--into DIR` | put the game into `DIR` (a folder you play from) instead of `dist/`; nothing there is deleted |
| `--linux` | on Windows: cross-build the Linux version |
| `--debug` | unoptimised build with a console window, for development |
| `--jobs N` | number of parallel compiler processes (default: all CPU cores) |
| `--clean` | rebuild everything from scratch |

Later builds only redo the steps whose inputs changed.

### For developers

- After changing an `#include`, the link libraries or the toolchain, run
  `python tools/min_toolchain.py`: it rewrites `tools/toolchain.txt`, the files the launcher's
  build downloads (`python tools/fetch_deps.py --min` does the same download).
- `build.py` rebuilds the launcher exe when it is missing or older than `launcher.pyw`, the
  runtime or the tools; `python tools/build_launcher.py` does it alone. The exe isn't in git.
- **Releasing:** raise `VERSION` in `dist-template/launcher.pyw`, merge into main, then on
  GitHub run **Actions > Release launcher > Run workflow**. It builds the exe from main, checks
  its version, attests the build and makes the release, as a draft by default: try it, then
  publish it on the Releases page (launchers ignore drafts).
- Unit tests: `python tools/run_tests.py`. Internals and testing options:
  [docs/TECHNICAL.md](docs/TECHNICAL.md); the graphics work: [docs/GRAPHICS.md](docs/GRAPHICS.md).

## Playing

Open the launcher (`HarvestMoonBTN-Launcher.exe`, or `launcher.pyw` with Python) and press
**Save and play**, or run `HarvestMoonBTN.exe` (Windows) / `./HarvestMoonBTN` (Linux) directly.
The game needs its data (graphics, music, text), which comes from your own copy, in one of two
ways:

- **Disc image (default).** The game reads your `.cue`/`.bin`. `hmbtn.ini` remembers the
  image the folder was built from; if you move it, put the `.cue` and `.bin` into the
  `disc` folder next to the executable, set `disc =` in `hmbtn.ini`, or pass the path on
  the command line.
- **Data pack (`python build.py --pak`).** Your disc image is repacked into
  `HarvestMoonBTN.pak` (58 MB instead of 214 MB) next to the executable, and the disc image
  is no longer needed. The pack keeps the disc's exact sector layout (the game streams its
  music straight from the disc, interleaved with other data), and every block is checked
  against a checksum of your original disc on load, so the game sees exactly the same data.
  `HarvestMoonBTN --verify-data` checks the whole file.

## Controls

|  | Keyboard | Xbox controller |
|---|---|---|
| D-pad | Arrow keys | D-pad / left stick |
| ✕ Cross | X / Space / K | A |
| ○ Circle | Z / L | B |
| □ Square | S / J | X |
| △ Triangle | A / I | Y |
| L1 / R1 | Q / W | LB / RB |
| L2 / R2 | 1 / 2 | LT / RT |
| Start | Enter | Start (Menu) |
| Select | Right Shift / Backspace | Back (View) |

| Key | |
|---|---|
| F11 or Alt+Enter | toggle fullscreen (Esc leaves fullscreen) |
| F12 | screenshot (saved in `screenshots/`) |
| F5 | cheats window, changes apply at once (not in online co-op) |
| F9 | 60 fps camera on / off (with `interp = 1`), to compare |
| F1–F4 | put memory card 1–4 in slot 1 (3 diaries each; shown in the window title; not in online co-op) |
| L1 / R1 on a diary list | previous / next memory card (shown above the list); when loading, only cards that have a diary |
| Triangle (hold 2 s) on a diary list | delete the diary under the cursor |
| Tab (hold) | fast-forward (can be disabled) |
| START (hold, during a cutscene) | fast-forward the cutscene (`skipscenes`) |

Farmer 2's controls (second controller or numeric keypad) are under [Co-op](#co-op).
Controllers can be plugged in or out at any time. Any controller that SDL2 recognises as a
game controller works (Xbox, PlayStation, Switch Pro and many others) with the Xbox-style
layout above. All keys and buttons can be changed in the launcher or in `hmbtn.ini`.

## Configuration

The launcher edits the settings and starts the game. They live in `hmbtn.ini` next to the
executable, a commented file you can also edit by hand (remove the `#` in front of an option to
change it). The defaults below are those of a new install; how the graphics options work, and
what was learned about the game while making them: [docs/GRAPHICS.md](docs/GRAPHICS.md).

| Section | Setting | Default | |
|---|---|---|---|
| `[general]` | `disc` | (automatic) | path to your `.cue`/`.bin` (or a `.pak`) |
|  | `savedir` | `saves` | folder for the memory card file |
| `[video]` | `scale` | `3` | initial window size as a multiple of 320×240 |
|  | `fullscreen` | `0` | start in fullscreen |
|  | `scaling` | `sharp-bilinear` | `sharp-bilinear` = sharp pixels that stay even at any size, `sharp` = nearest pixel, `smooth` = bilinear |
|  | `aspect43` | `1` | `1` = 4:3 picture like a TV, `0` = square pixels |
|  | `vsync` | `1` | wait for the screen's refresh (no tearing); on a 60/120/180/240 Hz screen the game keeps time with it (about 1% faster than the console), so the camera moves evenly. At other rates (e.g. 144 Hz) G-Sync/FreeSync makes it even; `0` = don't wait |
|  | `renderer` | `cpu` | `vulkan` = the graphics card draws the picture (experimental; falls back to `cpu` without Vulkan) |
|  | `upscale` | `2` | internal resolution 1–4, 1–8 with `renderer = vulkan` (2+ = smoother 3D edges; 4 = 1280×960, 8 = 2560×1920) |
|  | `drawthreads` | `auto` | threads that draw the upscaled 3D with the CPU renderer, `0`–`4`; `auto` = by processor |
|  | `pgxp` | `1` | `1` = precise 3D vertex positions, no wobble; 2D objects get smaller further away, like the 3D ground (needs `upscale` 2+) |
|  | `crackfix` | `0` | `1` = close the thin cracks between models that `pgxp` opens, about 1 ms a frame at upscale 4; precise culling leaves few (needs `pgxp`) |
|  | `depthtest` | `1` | a depth test as DuckStation's PGXP depth buffer: solid 3D isn't drawn where something drawn before it is nearer (the ground over the bamboo shoot, shutter edges, step cracks); 2D objects keep the game's order; `0` = the game's order everywhere (needs `pgxp`) |
|  | `precisecull` | with `pgxp` | `0` = cull thin faces on whole pixels as the console does (table legs and lamp bases lose a side); the game sees it, so recordings and co-op carry it |
|  | `perspective` | `1` | `1` = perspective-correct textures (needs `pgxp`) |
|  | `widescreen` | `16:9` | `off`, `16:9`, `16:10` or `21:9`: wider 3D view; with `upscale` 2+ the HUD and 2D objects keep their shape and the title screen and menus fill the screen |
|  | `interp` | `1` | `1` = 60 fps camera: the camera and the 3D scenery move every VBlank while the game runs at 30 (needs `pgxp`; 3 extra CPU threads, shows 30 pictures a second where the PC can't keep up) |
|  | `texturepack` | `0` | `1` = draw 2D sprites (people, animals, field objects, text, HUD) with the replacement images in `textures/` next to the game ([texture packs](docs/GRAPHICS.md#texture-packs); needs `upscale` 2+) |
|  | `portraits` | `0` | `1` = draw the dialogue portraits with the redrawn images in `textures/portraits/` (same format as a texture pack, switched on and off on its own; needs `upscale` 2+; the launcher downloads them the first time) |
|  | `dumptextures` | `0` | `1` = write each 2D sprite texture the game shows to `textures/dump/`, to draw a texture pack from (needs `upscale` 2+) |
|  | `p2view_upscale` | `auto` | resolution of farmer 2's window in local co-op; `1` is the lightest |
| `[behaviour]` | `fastforward` | `1` | allow holding Tab to fast-forward |
|  | `fastcd` | `8` | shorter loading pauses: `1` = disc data 4× faster, `2` or more = that many times, `0` = original (music streaming is unaffected) |
|  | `autoslot` | `1` | saving and loading use MEMORY CARD slot 1 without the "Select a MEMORY CARD slot" menu; `0` = ask |
|  | `fastcard` | `1` | the game runs at full speed while the memory card is searched, loaded or saved; `0` = original wait |
|  | `quickui` | `1` | menu and message windows open and close at once, fades to and from black (doors too) are 4× shorter; `0` = original |
|  | `fastload` | `0` | `1` = the game runs at full speed while "Now Loading" or the black screen between places is shown; the music waits meanwhile (may crackle) |
|  | `textspeed` | `3` | characters the dialogue types at a time: `1` = original speed, `2`–`16` = faster |
|  | `skipscenes` | `1` | hold START during a cutscene to run it at full speed; it pauses while a dialogue box is open, so choices are never made for you; `0` = off |
|  | `overclock` | `200` | CPU speed in percent (`100` = the console, up to `400`): the game no longer drops frames when a lot is going on, e.g. running around the farm while switching tools; video, sound and timing stay the same |
|  | `emptybag` | `1` | R1 / R2 do nothing when there is no tool / item to switch to, so they no longer stop the player for a moment while running; `0` = original |
|  | `spritechair` | `1` | in the Harvest Sprites' house the yellow sprite stands a little farther back from the table, so the chair no longer cuts in and out of its feet as the camera moves; `0` = original spot |
|  | `earlycabinet`, `cropwalk`, `runthrough`, `staminabar`, `bulkfood` | `0` | the mods, see [Mods and cheats](#mods-and-cheats) |
|  | `split` | `0` | `1` = farmer 2 can go into buildings alone (online co-op only, experimental), see [Co-op](#co-op) |
|  | `record` | `0` | `1` = record every session to `recordings/` (button presses and the memory card at the start), to replay a problem exactly |
| `[cheats]` | `cheat_money`, `cheat_stamina`, `cheat_clock`, `cheat_bag`, `cheat_items`, `cheat_tools` | `0` | see [Mods and cheats](#mods-and-cheats) |
| `[netplay]` | `coop_address`, `coop_port`, `coop_delay`, `coop_p2` |  | the launcher's online co-op page |
| `[keyboard]` | `key_up` … `key_select` |  | comma-separated SDL key names, e.g. `key_cross = X,Space,K` |
| `[controller]` | `pad_up` … `pad_select` |  | SDL controller button names: `a b x y back start guide leftshoulder rightshoulder leftstick rightstick dpup dpdown dpleft dpright lefttrigger righttrigger` |
|  | `stick_as_dpad` | `1` | the left analog stick also works as the D-pad |
|  | `rumble` | `1` | the controller vibrates where the game shakes a DualShock (the port answers as one, as the original did); `0` = never |
| `[launcher]` | `language` |  | the launcher's language, `en`, `pt` or `es` (its flags set it); not set: the system's |

Command line options: `--fullscreen`, `--no-vsync`, `--scale N`, `--scaling MODE`, `--renderer cpu|vulkan`, `--upscale N`,
`--drawthreads N`, `--pgxp`, `--crackfix`, `--perspective`, `--no-sprite-depth`, `--widescreen W:H`,
`--interp` / `--no-interp`, `--texturepack` / `--no-texturepack`, `--portraits` / `--no-portraits`,
`--dumptextures`, `--fastcd`, `--no-autoslot`, `--card N`, `--overclock N`, `--savedir DIR`,
`--p2`, `--split`, `--p2view-upscale N`, `--host [PORT]`, `--join ADDRESS[:PORT]`, `--netdelay N`,
`--verify-data`, `--log 0..4`, and a disc image or data pack path. Testing options are described in
[docs/TECHNICAL.md](docs/TECHNICAL.md#testing-and-debugging).

## Co-op

**Two farmers on one PC** (launcher: **Local co-op > Play with two farmers**, or `--p2`;
experimental). You play the game's farmer; a second player gets a farmer of their own, in a
window of their own. Farmer 1 uses the keyboard or the first controller, farmer 2 the second
controller or the numeric keypad:

| | Controller 2 | Numeric keypad |
|---|---|---|
| Walk | D-pad | 8 4 5 6 |
| Use the tool | Square | 7 |
| Look, talk, open, trade | Cross | 9 |
| Run | Circle | 1 |
| Change tools | R1 | 3 |
| Put away / change the item | R2 | + |
| Whistle | L1 / L2 | / * |
| Rucksack | Triangle | 0 |
| Menu / Map | Start / Select | Enter / . |

- There is one of each tool: farmer 2 starts with nothing and takes tools from the tool chest.
- Trading: stand close in front of the other farmer and press Cross: first the tools, then
  the items, as many as the other's rucksack holds.
- Only what farmer 1 carries is saved, as in the original: when saving, farmer 2's tools and
  items go back to the tool chest, cabinet or fridge.
- **Farmer 2 goes into buildings alone** (`split`, online co-op only, experimental): on the farm, either farmer
  can go through a door while the other stays outside, each with their own picture. Time,
  money and the farm are shared. It uses about twice the processor while they are apart, so
  on one PC it stays off and farmer 2 follows farmer 1, as before.

**Online co-op** (launcher: **Online co-op**, or `--host` / `--join ADDRESS`). Each player runs
the game on their own PC. The host plays their own farm with their memory cards and settings;
the guest's own saves are not touched (co-op saves go to `saves\netplay`). Both need the **same
game version**, shown in the window title: build from the same launcher version, or the same
commit of `main`. The host's port is TCP 7845; the easiest way to connect is for both to join the
same Radmin VPN or ZeroTier network, and the host gives the guest that address. With **Two
farmers** on (`coop_p2`) the guest gets a farmer of their own; otherwise both control the same
farmer. The host's settings, mods and cheats count for both.

## Mods and cheats

**Mods** (the launcher's Mods page, `[behaviour]` in `hmbtn.ini`) change how the original game
plays. All are off by default; in online co-op the host's choice counts.

| Setting | |
|---|---|
| `earlycabinet` | Cabinet in the first house: the pile of boxes opens the cabinet before the house is extended |
| `runthrough` | Run through animals: running goes through the cows, the sheep and the horse; walking still pushes them |
| `cropwalk` | Walk through crops and weeds: you can walk through your planted crops, so the one in the middle of a 3×3 patch is always in reach |
| `staminabar` | Stamina under the clock: a window under the clock shows your stamina (S) and tiredness (F), which the game never shows |
| `bulkfood` | Buy food in bulk: at a shop shelf you choose how many: one stays in your hands, the rest go to the rucksack and then the fridge |

Two bugs of the original are always fixed: the farm score now reaches 100% (Power Berries and
the Mystic Berry were never counted), and the hothouse eggplant has the right colours when carried.

**Cheats** (the launcher's Cheats page, or **F5** in the game, `[cheats]` in `hmbtn.ini`):
money when you reach the farm, endless stamina, a stopped clock, the biggest rucksack,
materials at 999 and tools at their highest level. What you save with them stays in the save.
In co-op the host's cheats count for both.

## Saves

Saving works exactly as on the PlayStation: go to bed in your house and pick one of the
three diaries; **Continue** on the title screen loads them. The memory card is
`saves/card1.mcd`, a standard 128 KB raw memory card image as used by DuckStation, ePSXe,
PCSX and others, so you can bring an existing save from an emulator or take yours to one.
F1–F4 switch between four cards (`card1.mcd`, `card1_2.mcd`, `card1_3.mcd`, `card1_4.mcd`). If the game folder is not
writable, the cards are kept in `%APPDATA%\HarvestMoonBTN` (Windows) or
`~/.local/share/HarvestMoonBTN` (Linux).

## How it was ported

**Static recompilation.** An emulator reads the game's machine code while the game runs
and interprets (or JIT-compiles) it instruction by instruction. This port translates the
whole program **once, before it runs**:

1. **Extract.** `tools/iso.py` reads the ISO 9660 file system of your disc image and copies
   out the main executable (`SLUS_011.15`) and the archive that holds the game's 13 code
   overlays (title screen, farm, festivals and minigames, save/load and others, which the
   game swaps in and out of memory).
2. **Analyse.** `tools/recomp.py` decodes the MIPS R3000A machine code and finds every
   function: call targets, code addresses built in registers, function-pointer tables,
   switch jump tables. Analysis repeats until nothing new turns up (about 3,800 functions).
3. **Translate.** Each MIPS function becomes a C function. Registers become fields of a
   CPU structure, memory accesses go through small inline helpers, branches (including
   MIPS branch delay slots) become `goto`s, and jump tables become `switch` statements.
   Every block also counts the CPU cycles the original would take, because the game's
   timing depends on it.
4. **Compile.** The ~380,000 lines of generated C are compiled with clang/gcc into a native
   x86-64 program, together with the runtime.

**The runtime.** The translated game still believes it is running on a PlayStation, so
`runtime/` provides everything it talks to, written natively from public hardware
documentation: the graphics chip (a software renderer reproducing the PS1 GPU's exact
drawing rules), the geometry coprocessor (GTE), the sound chip (24 ADPCM voices with
envelopes, interpolation and reverb), the CD drive (including XA streamed music), the
controller and memory card port, DMA, timers and interrupts, plus the BIOS kernel
functions the game calls (file system, events, memory card files). This hardware layer is
a reimplementation, like the one an emulator contains, but the game's own code runs as
native machine code. SDL2 provides the window, input and audio. The optional Vulkan renderer
(`runtime/vk/`) uses [parallel-psx](runtime/vk/README.md) to draw the picture on the graphics card.

**Checking it.** The hardware pieces have unit tests (`python tools/run_tests.py`). The
whole game was checked by running millions of frames of random input, by comparing runs
frame by frame (screenshots, memory, audio and saves bit-identical between debug and
release builds and between disc image and data pack), and by playing: title screen, new
game, farm and town, saving and loading. Co-op is checked by replaying recorded sessions on
both sides and comparing their memory.

Details for developers: [docs/TECHNICAL.md](docs/TECHNICAL.md).

## Project layout

```
build.py              one-step build: disc image -> native game in dist/
disc/                 put your .cue/.bin here (ignored by git)
tools/
  fetch_deps.py       downloads the Windows toolchain into deps/ (--min: only what the build reads)
  toolchain.txt       those files with their SHA-256 (written by min_toolchain.py)
  min_toolchain.py    rewrites toolchain.txt from a clean build
  build_launcher.py   builds dist-template/HarvestMoonBTN-Launcher.exe, with the sources in it (build.py runs it)
  iso.py              disc image + ISO 9660 reader
  mips.py, mdis.py    MIPS decoder, disassembler / cross-reference helper
  recomp.py           the static recompiler (MIPS -> C)
  overlays.json       the 13 code overlays: archive index, load address, code range
  extra_entries.json  entry points that can't be found statically
  compile.py          compiles runtime + generated code
  makepak.py          builds the optional data pack from your disc image
  package.py          assembles the dist folders
  run_tests.py        runtime unit tests
  sheet.py            screenshot contact sheets (testing)
runtime/              the native PlayStation hardware + BIOS reimplementation, frontend, enhancements, co-op
  vk/                 the Vulkan renderer (parallel-psx and the glue to it)
  tests/              unit tests (GTE, GPU, SPU, memory card file system)
dist-template/        hmbtn.ini, README.txt, launcher.pyw, its pictures and licence text copied into each build
.github/workflows/    release.yml: builds and publishes the launcher
docs/TECHNICAL.md     internals
docs/GRAPHICS.md      the graphics enhancements (upscaling, PGXP, widescreen, texture packs...) and findings
LICENSE               MIT license for this port's source code
```

Created locally and ignored by git: `deps/` (toolchain), `extract/`, `generated/`,
`build/`, `dist/`, the launcher exe.

## Troubleshooting

- **"Game data not found"**: the game can't find your disc image. Put the `.cue` and
  `.bin` into the `disc` folder next to the executable, or set `disc =` in `hmbtn.ini`.
- **"does not match the USA release"** when building: your image is a different region or
  revision, or a bad dump. Only SLUS-01115 is supported.
- **The `.bin` named in the `.cue` is missing**: the `.cue` refers to the `.bin` by file
  name; keep both together and don't rename one without editing the other.
- **The launcher's build fails**: update the launcher first (older ones may lack a file the
  current sources need). If it still fails, report it with the build page's log.
- **Online co-op doesn't connect or says the versions differ**: both need the same game
  version (window title); the host's port 7845 must be reachable, which Radmin VPN or
  ZeroTier makes easy.
- **Vulkan renderer**: without a working Vulkan driver the game falls back to the CPU
  renderer (the log says so). Update the graphics driver, or use `renderer = cpu`.
- **Camera moves unevenly**: the game makes 59.29 pictures a second, which a 144 Hz screen can only
  show unevenly. Turn on G-Sync/FreeSync (for windowed and full screen: the game's fullscreen is a
  borderless window), or set the screen to 60 or 120 Hz, where `vsync` keeps the game in step with it.
- **Slow at high resolution**: the CPU renderer draws everything on the processor; lower
  `upscale`, or try `renderer = vulkan`.
- **No sound**: the game opens the default audio device when it starts and picks up a
  device plugged in later. Check `HarvestMoonBTN.log` next to the executable.
- **Controller not working**: make sure it works in other games; SDL2 must recognise it as
  a game controller. Buttons can be remapped in the launcher or `hmbtn.ini`.
- Anything else: the log file `HarvestMoonBTN.log` next to the executable usually says what
  went wrong.

## Status

The whole game is recompiled. Tested so far: boot, intro, title screen, new game, farm,
house, town and map, menus and tools, music and sound effects, saving and loading, and long
random-input runs without errors. The festivals and minigames are recompiled like the rest
of the game but haven't been played through extensively yet. Co-op, independent co-op and
the Vulkan renderer are experimental. The Linux version builds (natively or cross-compiled)
but has had less testing than the Windows version. Bug reports are welcome in the
[issue tracker](https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/issues); please attach
`HarvestMoonBTN.log`.

## License and legal

The source code in this repository (the recompiler, the runtime, the launcher and the scripts)
is released under the [MIT License](LICENSE). The license covers only this port's own code. It
grants no rights to *Harvest Moon: Back to Nature*, and it does not cover what the build generates
from your disc (the extracted files, the translated game code, the executable and the data pack),
which is derived from the game.

This is an unofficial fan project, not affiliated with or endorsed by Marvelous Inc.,
Natsume Inc., Victor Interactive Software or Sony Interactive Entertainment. *Harvest
Moon* and *Back to Nature* are trademarks of their respective owners. This repository
contains no code, graphics, music, text or other data from the game; you need your own
legally obtained copy, and what the build produces from it is for your personal use only.

The port uses [SDL2](https://libsdl.org) (zlib license, see
`dist-template/LICENSE-SDL2.txt`). The Vulkan renderer includes parallel-psx (ISC/MIT),
volk (MIT), SPIRV-Cross and the Vulkan headers (Apache-2.0); see
[runtime/vk/README.md](runtime/vk/README.md). The launcher exe is made with
[PyInstaller](https://pyinstaller.org). The toolchain downloaded by `tools/fetch_deps.py`
(llvm-mingw, SDL2, Zig) is used only to build and is not part of this repository.
Hardware information came from the public PlayStation documentation, above all
[psx-spx](https://psx-spx.consoledev.net/).
