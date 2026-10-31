HARVEST MOON: BACK TO NATURE - NATIVE PC PORT
=============================================

This is a native PC (Windows / Linux) build of Harvest Moon: Back to Nature (USA, SLUS-01115),
made from your own copy of the game. The PlayStation game code was statically recompiled into
native x86-64 code - there is no emulator and no PlayStation BIOS involved. The PlayStation
hardware the game talks to (graphics chip, sound chip, CD drive, controller port, memory card,
geometry coprocessor, timers) and the BIOS services it uses are reimplemented natively so the
game behaves exactly like the original.

RUNNING
-------
Run HarvestMoonBTN.exe (Windows) or ./HarvestMoonBTN (Linux).

Linux: requires SDL2 (normally already installed; otherwise e.g. "sudo apt install
libsdl2-2.0-0", "sudo dnf install SDL2" or "sudo pacman -S sdl2"). Make the file executable
if needed: chmod +x HarvestMoonBTN

{{GAME_DATA}}

The picture is drawn by the processor, so no graphics card is needed; the internal resolution
(upscale) is what costs. On a slower PC, pick the preset for its number of cores in the Video
tab of launcher.pyw (2 cores: upscale 2, 4 cores: upscale 3); every effect stays on. The 3D
is drawn by several threads at once (drawthreads in hmbtn.ini), with the same picture.

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
  Tab (hold)         fast-forward (can be disabled in hmbtn.ini)
  F1 - F4            memory card 1 - 4 in slot 1 (shown in the window title)
  L1 / R1            on a diary list (saving or loading): previous / next memory card
  Triangle (hold)    on a diary list: hold 2 seconds to delete that diary
  Start (hold)       fast-forward a cutscene (stops at each dialogue)

All keys and buttons can be remapped in hmbtn.ini. Controllers can be plugged in at any time.

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
diary list (which then shows that card's diaries). The card
last used is chosen again next time. If the game folder is not writable, the cards
are kept in
%APPDATA%\HarvestMoonBTN (Windows) or ~/.local/share/HarvestMoonBTN (Linux).

ONLINE CO-OP
------------
Two players, each on their own PC with their own copy of the game, play one game together.
In launcher.pyw, page "Online co-op": one player hosts (waits for the friend), the other joins
with the host's address (a VPN such as Radmin VPN or Tailscale gives both PCs an address; the
page shows this PC's). By hand: HarvestMoonBTN --host [PORT] (default 7845) and
HarvestMoonBTN --join ADDRESS[:PORT]; --netdelay N sets the input delay in frames (default 3).
Both copies must be built from the same sources (the window title and the launcher's co-op page show the
version: "build" and 8 characters, the same on both PCs). The host sends its memory cards; the friend
plays on copies of them (saves\netplay), so the friend's own cards are not touched. In co-op,
Tab and F1-F4 are off and the window title shows the connection.
Two farmers (experimental, "Two farmers" on the co-op page, or --p2 on one PC with a second
controller or the numeric keypad: 8 4 5 6 move, 7 Square, 9 Cross, 1 Circle, 3 R1): the
friend gets a farmer of their own, run by the game's own player routines. There is one of each
tool: farmer 2 starts with none and takes them from the tool chest; the save and load screens
put them back in the chest.
With --p2 on one PC (launcher page "Local co-op") a second window follows farmer 2, at 60
pictures a second like the game's (it runs the farm's frame once more with the view on farmer 2).
Each window shows its own farmer's talks and menus. Farmer 2 also has its own stamina, can trade
with the player (Cross facing them), open the rucksack, Start menu and map. [video] p2view_upscale
(or --p2view-upscale N) gives that window a lower internal resolution, lighter for a slower PC;
--p2view 0 turns it off.

CHEATS
------
Launcher page "Cheats" (F5 in the game opens it over the game, changes apply at once): money,
endless stamina, stop the clock, big rucksack, materials and tools at most. Saved games keep
what they changed. In online co-op the host's count.

NOTES
-----
* Video runs at the original NTSC rate (59.29 fps) with the original 320x240 picture, scaled up.
* A log file (HarvestMoonBTN.log) is written next to the executable - useful if something
  goes wrong.
* This folder is for your personal use only - do not share it. The executable contains the
  game's program code translated from your copy, which, like the game's data, remains the
  property of its copyright holders (Victor Interactive Software / Marvelous, Natsume).
  SDL2 is used under the zlib license (LICENSE-SDL2.txt).
