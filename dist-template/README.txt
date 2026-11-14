HMBTN REFARMED - HARVEST MOON: BACK TO NATURE, NATIVE PC PORT
==============================================================

This is a native PC (Windows / Linux) build of Harvest Moon: Back to Nature (USA, SLUS-01115),
made from your own copy of the game. The PlayStation game code was statically recompiled into
native x86-64 code - there is no emulator and no PlayStation BIOS involved. The PlayStation
hardware the game talks to (graphics chip, sound chip, CD drive, controller port, memory card,
geometry coprocessor, timers) and the BIOS services it uses are reimplemented natively so the
game behaves exactly like the original. On top of that come sharper graphics, co-op, time-saving
options, optional mods and cheats.
Project page: https://github.com/d4rksp4rt4n/HMBTN-ReFarmed

RUNNING
-------
Open HarvestMoonBTN-Launcher.exe (or launcher.pyw, the same launcher run with Python) and press
"Save and play", or run HarvestMoonBTN.exe (Windows) / ./HarvestMoonBTN (Linux) directly.
The launcher sets everything up (English, Portuguese, Spanish), and when it says a new version
is out, "Update" replaces it and rebuilds the game; your saves, settings and textures stay.

Linux: requires SDL2 (normally already installed; otherwise e.g. "sudo apt install
libsdl2-2.0-0", "sudo dnf install SDL2" or "sudo pacman -S sdl2"). Make the file executable
if needed: chmod +x HarvestMoonBTN

{{GAME_DATA}}

SETTINGS
--------
The launcher's pages: Start (quick setup by your PC's cores), Video, Game (time savers), Mods,
Keyboard, Controller, Local co-op, Online co-op and Cheats. They are saved in hmbtn.ini next to
the game, a commented file you can also edit by hand.

* Graphics: internal resolution 2-4 (up to 8 with the Vulkan renderer), precise 3D (PGXP) without
  wobbling polygons, perspective-correct textures, widescreen (16:9, 16:10, 21:9,
  title screen and menus included) and a camera that moves at 60 fps (F9 compares).
* Vsync (on): no tearing, and on a 60 or 120 Hz screen the game keeps time with it, so the camera
  moves evenly. A 144 Hz screen shows 59.29 pictures a second unevenly: turn on G-Sync/FreeSync
  (for windowed and full screen), or set the screen to 60 or 120 Hz.
* The CPU renderer draws the picture with the processor, so no graphics card is needed; the
  resolution is what costs. On a slower PC pick the quick setup for its number of cores (2 cores:
  resolution 2, 4 cores: 3); every effect stays on. "renderer = vulkan" (experimental)
  lets the graphics card draw it instead, and falls back to the CPU without Vulkan.
* Texture packs: replacement images for the 2D sprites go in the "textures" folder, redrawn
  dialogue portraits in "textures\portraits" (resolution 2 or more; the launcher downloads them
  when "Redrawn portraits" is switched on).
* Time savers: shorter loading, quick menus and fades, faster text, saving without the slot
  menu, Start held in a cutscene runs it at full speed (it stops at each dialogue).

CONTROLS
--------
                 Keyboard                 Xbox controller
  D-pad          Arrow keys               D-pad / left stick
  Cross (X)      X / Space / K            A
  Circle (O)     Z / L                    B
  Square         S / J                    X
  Triangle       A / I                    Y
  L1 / R1        Q / W                    LB / RB
  L2 / R2        1 / 2                    LT / RT
  Start          Enter                    Start (Menu)
  Select         Right Shift / Backspace  Back (View)

  F11 or Alt+Enter   toggle fullscreen         Esc   leave fullscreen
  F12                screenshot (screenshots folder)
  F5                 cheats window, changes apply at once
  F9                 60 fps camera on / off, to compare
  Tab (hold)         fast-forward (can be disabled)
  F1 - F4            memory card 1 - 4 in slot 1 (shown in the window title)
  L1 / R1            on a diary list (saving or loading): previous / next memory card
  Triangle (hold)    on a diary list: hold 2 seconds to delete that diary
  Start (hold)       fast-forward a cutscene (stops at each dialogue)

All keys and buttons can be remapped in the launcher or hmbtn.ini. Controllers can be plugged
in at any time. They vibrate where the game shakes a DualShock (rumble = 0 in [controller], or
the launcher's Controller page, turns it off).

SAVES
-----
Saving works exactly as on the PlayStation: go to bed in your house and pick one of the three
diaries (Continue on the title screen loads them). The memory card is stored in
saves\card1.mcd - a standard 128 KB raw memory card image, compatible with DuckStation, ePSXe,
PCSX and others, so you can copy an existing card here to continue an old game, or take your
card to an emulator.
For more than three diaries there are four cards, three diaries each: saves\card1.mcd,
card1_2.mcd, card1_3.mcd and card1_4.mcd. On a diary list (saving or loading) L1 / R1 put
the previous / next card in slot 1 and show its diaries ("Memory card N" above the list);
when loading, cards without a diary are skipped. F1-F4 pick a card directly, also on a
diary list (which then shows that card's diaries). The card last used is chosen again next
time. If the game folder is not writable, the cards are kept in
%APPDATA%\HarvestMoonBTN (Windows) or ~/.local/share/HarvestMoonBTN (Linux).

LOCAL CO-OP (experimental)
--------------------------
Two farmers on this PC: launcher page "Local co-op", "Play with two farmers" (or --p2). Farmer 1
plays with the keyboard or the first controller; farmer 2 with the second controller or the
numeric keypad, in a window of its own:

  Walk 8 4 5 6 (D-pad)      Use the tool 7 (Square)   Look, talk, open, trade 9 (Cross)
  Run 1 (Circle)            Change tools 3 (R1)       Put away / change the item + (R2)
  Whistle / * (L1 / L2)     Rucksack 0 (Triangle)     Menu Enter (Start)    Map . (Select)

There is one of each tool: farmer 2 starts with none and takes them from the tool chest. Trade
by standing close in front of the other farmer and pressing Cross. Only what farmer 1 carries
is saved, as in the original: the save screen puts farmer 2's tools and items in the chest,
cabinet or fridge. "Farmer 2 goes into buildings alone" (experimental) lets either farmer go
through a door on the farm while the other stays outside; it uses about twice the processor
while they are apart. "Farmer 2's window resolution" (p2view_upscale) lightens that window for
a slower PC.

ONLINE CO-OP
------------
Two players, each on their own PC with their own copy of the game, play one game together.
In the launcher, page "Online co-op": one player hosts (waits for the friend), the other joins
with the host's address. The easiest way: both join the same Radmin VPN or ZeroTier network, and
the host gives that address (the page shows this PC's). By hand: HarvestMoonBTN --host [PORT]
(default 7845, TCP) and HarvestMoonBTN --join ADDRESS[:PORT]; --netdelay N sets the input delay
in frames (default 3).
Both copies must be the same version: the window title and the co-op page show it ("build" and
8 characters, the same on both PCs). Updating both launchers to the same release does it. The
host sends its memory cards; the friend plays on copies of them (saves\netplay), so the friend's
own cards are not touched. The host's settings, mods and cheats count for both. In co-op, Tab,
F1-F4 and F5 are off and the window title shows the connection. With "Two farmers" on the
friend gets a farmer of their own; otherwise both control the same farmer.

MODS
----
Launcher page "Mods". They change how the original game plays and are all off unless you switch
them on:
* Cabinet in the first house: the pile of boxes opens the cabinet before the house is extended.
* Walk through crops: the crop in the middle of a 3x3 patch is always in reach.
* Stamina under the clock: shows your stamina (S) and tiredness (F), which the game never shows.
* Buy food in bulk: at a shop shelf you choose how many to buy.
Always fixed, mods or not: the farm score reaches 100%, and the hothouse eggplant has the right
colours when carried.

CHEATS
------
Launcher page "Cheats" (F5 in the game opens it over the game, changes apply at once): money,
endless stamina, stop the clock, big rucksack, materials and tools at most. Saved games keep
what they changed. In online co-op the host's count.

NOTES
-----
* The game runs at the original NTSC rate (59.29 fps) and timing; the picture is the original
  320x240 one, drawn at a higher resolution if you choose.
* A log file (HarvestMoonBTN.log) is written next to the executable - useful if something
  goes wrong. Bug reports: https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/issues
* This folder is for your personal use only - do not share it. The executable contains the
  game's program code translated from your copy, which, like the game's data, remains the
  property of its copyright holders (Victor Interactive Software / Marvelous, Natsume).
  To give the game to a friend, give them the launcher: they build from their own disc.
* SDL2 is used under the zlib license (LICENSE-SDL2.txt). The Vulkan renderer includes
  parallel-psx (ISC/MIT), volk (MIT), SPIRV-Cross and the Vulkan headers (Apache-2.0); their
  licences are in runtime/vk of the sources.
