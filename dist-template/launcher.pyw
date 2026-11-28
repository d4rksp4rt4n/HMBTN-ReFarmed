#!/usr/bin/env python3
"""Settings launcher: edits hmbtn.ini next to this file and starts the game.

Needs Python 3 with tkinter (part of the python.org installer). Double-click it, or run
"python launcher.pyw". Only options you change are written; comments in hmbtn.ini stay.
English, Portuguese or Spanish, as the system until a flag in the header picks one (kept in hmbtn.ini).
"""
import os, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(sys.executable if getattr(sys, 'frozen', False) else __file__))  # also as Launcher.exe
INI = os.path.join(HERE, 'hmbtn.ini')
EXE = os.path.join(HERE, 'HarvestMoonBTN.exe' if os.name == 'nt' else 'HarvestMoonBTN')
# the launcher's release: .github/workflows/release.yml publishes it on GitHub as v<VERSION>, with the Windows
# and the Linux launcher; a launcher finds newer ones there, updates itself and rebuilds the game
VERSION = '1.12.0'
RELEASES = 'https://api.github.com/repos/d4rksp4rt4n/HMBTN-ReFarmed/releases/latest'
LAUNCHER_EXE = 'HarvestMoonBTN-Launcher.exe' if os.name == 'nt' else 'HarvestMoonBTN-Launcher-Linux'   # its asset
# the redrawn dialogue portraits and title logo (portraits = 1) are not in the code: the launcher downloads
# them into textures/portraits the first time the option is on, and again for a newer pack. Which pack is
# portraits.json in the repository (version, url, sha256), so a new one needs no new launcher; this is
# the one to use when that cannot be read
PORTRAITS_INFO = 'https://raw.githubusercontent.com/d4rksp4rt4n/HMBTN-ReFarmed/main/portraits.json'
PORTRAITS = {'version': '2', 'sha256': '6c98a72b8d07bee754c258488079038fd2ec82cf4221fa950eade9f708d4a44d',
             'url': 'https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/releases/download/portraits-v2/portraits.zip'}
PORTRAITS_DIR = os.path.join(HERE, 'textures', 'portraits')
PORTRAITS_MARK = os.path.join(PORTRAITS_DIR, 'pack.txt')     # the version there (the game reads only <key>.png)
GAME_FILES = ('SLUS_011.15', 'A_FILE.HDT', 'A_FILE.BIN')    # what build.py reads from the disc (extract/)
# the build scripts and sources for the Build page: inside Launcher.exe (tools/build_launcher.py), or the
# checkout this folder was built from (dist/<name>); WORK gets the downloads, your game's files and the build
if getattr(sys, 'frozen', False):
    SRC, WORK = os.path.join(getattr(sys, '_MEIPASS', ''), 'src'), os.path.join(HERE, 'build-files')
else:
    SRC = WORK = os.path.dirname(os.path.dirname(HERE))
if not os.path.isfile(os.path.join(SRC, 'build.py')):
    SRC = None

if getattr(sys, 'frozen', False) and sys.platform.startswith('linux') and not os.environ.get('SSL_CERT_FILE'):
    # the OpenSSL inside the Linux launcher looks for CA certificates where the build machine (Ubuntu) keeps
    # them, so on Fedora, Bazzite, openSUSE... every https download fails "certificate verify failed". Point
    # it at this system's bundle; the environment also reaches the build scripts the Build page starts
    for _ca in ('/etc/ssl/certs/ca-certificates.crt',              # Debian, Ubuntu, Arch, Gentoo, Fedora 4x
                '/etc/pki/tls/certs/ca-bundle.crt',                # older Fedora, RHEL
                '/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem',
                '/etc/ssl/ca-bundle.pem',                          # openSUSE
                '/etc/ssl/cert.pem'):                              # Alpine, Void
        if os.path.isfile(_ca):
            os.environ['SSL_CERT_FILE'] = _ca
            break

if getattr(sys, 'frozen', False) and sys.argv[1:2] and sys.argv[1].endswith('.py'):
    # "Launcher.exe script.py ...": the Build page runs the build scripts with the Python this exe carries
    import runpy, traceback
    import collections, concurrent.futures, filecmp, glob, hashlib, json, lzma, platform, shutil, tarfile, \
        urllib.request, zipfile, zlib  # noqa: F401  (what the scripts import, so that PyInstaller packs it)
    if sys.stdout:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
        sys.stderr.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
    sys.argv = sys.argv[1:]
    sys.path.insert(0, os.path.dirname(os.path.abspath(sys.argv[0])))
    try:
        runpy.run_path(sys.argv[0], run_name='__main__')
    except SystemExit:
        raise
    except BaseException:
        traceback.print_exc()                   # to the Build page, not a window of its own
        sys.exit(1)
    sys.exit()

BUTTONS = ['up', 'down', 'left', 'right', 'cross', 'circle', 'square', 'triangle',
           'l1', 'r1', 'l2', 'r2', 'start', 'select']
# the languages of the game's text (build.py --lang); each built one is kept in languages/
GAME_LANGS = ['en', 'pt', 'id', 'de', 'es', 'ja']
# (page, ini section, key, kind)   kind: file, dir, spinN (1..N), bool, bool:1 (on unless set to 0),
# text, key (keyboard keys: typed, or caught with the Set/Add buttons), pad (controller buttons: typed or
# picked from PAD_BUTTONS), choice:a|b|c, edit:a|b|c (a choice that can also be typed)
OPTIONS = [
    ('video', 'video', 'renderer', 'choice:cpu|vulkan'),
    ('video', 'video', 'upscale', 'choice:1|2|3|4|5|6|8'),
    ('video', 'video', 'widescreen', 'choice:off|16:9|16:10|21:9'),
    ('video', 'video', 'pgxp', 'bool'),
    ('video', 'video', 'perspective', 'bool'),
    ('video', 'video', 'interp', 'bool'),
    ('video', 'video', 'vsync', 'bool:1'),
    ('video', 'video', 'texturepack', 'bool'),
    ('video', 'video', 'portraits', 'bool'),
    ('video', 'video', 'dumptextures', 'bool'),
    ('video', 'video', 'drawthreads', 'choice:auto|0|1|2|3|4'),
    ('video', 'video', 'scale', 'spin8'),
    ('video', 'video', 'fullscreen', 'bool'),
    ('video', 'video', 'scaling', 'choice:sharp-bilinear|sharp|smooth'),
    ('video', 'video', 'crt', 'choice:off|scanlines|crt'),
    ('video', 'video', 'aspect43', 'bool'),
    # the game's language, on its own page: the launcher switches the exe (see play)
    ('gamelang', 'launcher', 'game_lang', 'choice:' + '|'.join(GAME_LANGS)),
    ('game', 'general', 'disc', 'file'),
    ('game', 'general', 'savedir', 'dir'),
    ('game', 'behaviour', 'autoslot', 'bool'),
    ('game', 'behaviour', 'fastcard', 'bool'),
    ('game', 'behaviour', 'quickui', 'bool'),
    ('game', 'behaviour', 'fastload', 'bool'),
    ('game', 'behaviour', 'fastcd', 'choice:0|1|8|16'),
    ('game', 'behaviour', 'textspeed', 'spin16'),
    ('game', 'behaviour', 'skipscenes', 'bool'),
    ('game', 'behaviour', 'fastforward', 'bool'),
    ('game', 'behaviour', 'overclock', 'edit:100|150|200|300'),
    ('game', 'behaviour', 'emptybag', 'bool'),
    ('game', 'behaviour', 'record', 'bool'),
    # mods: changes to how the original game plays (off by default; online, the host's count)
    ('mods', 'behaviour', 'earlycabinet', 'bool'),
    ('mods', 'behaviour', 'cropwalk', 'bool'),
    ('mods', 'behaviour', 'staminabar', 'bool'),
    ('mods', 'behaviour', 'bulkfood', 'bool'),
    ('mods', 'behaviour', 'spritehire', 'bool'),
    ('mods', 'behaviour', 'sturdygreenhouse', 'bool'),
    ('mods', 'behaviour', 'biggreenhouse', 'bool'),
    ('mods', 'behaviour', 'buildorder', 'bool'),
    ('mods', 'behaviour', 'runthrough', 'bool'),
    ('mods', 'behaviour', 'bellherd', 'bool'),
    ('mods', 'behaviour', 'doorreach', 'bool'),
    ('mods', 'behaviour', 'animalmarks', 'bool'),
    ('mods', 'behaviour', 'mailmark', 'bool'),
    ('mods', 'behaviour', 'randomnames', 'bool'),
    ('mods', 'behaviour', 'friendpage', 'bool'),
    ('mods', 'behaviour', 'birthdays', 'bool'),
] + [('keyboard', 'keyboard', 'key_' + b, 'key') for b in BUTTONS + ['fastforward']] \
  + [('controller', 'controller', 'pad_' + b, 'pad') for b in BUTTONS + ['fastforward']] \
  + [('controller', 'controller', 'stick_as_dpad', 'bool'), ('controller', 'controller', 'rumble', 'bool:1')] \
  + [('coop', 'netplay', 'coop_address', 'text'), ('coop', 'netplay', 'coop_port', 'text'),
     ('coop', 'netplay', 'coop_delay', 'choice:3|2|4|5|6|8'), ('coop', 'netplay', 'coop_p2', 'bool')]   + [('p2', 'video', 'p2view_upscale', 'choice:auto|1|2|3|4'), ('p2', 'behaviour', 'split', 'bool')] \
  + [('cheats', 'cheats', 'cheat_money', 'edit:0|10000|100000|1000000|9999999'), ('cheats', 'cheats', 'cheat_stamina', 'bool'),
     ('cheats', 'cheats', 'cheat_clock', 'bool'), ('cheats', 'cheats', 'cheat_bag', 'bool'),
     ('cheats', 'cheats', 'cheat_items', 'bool'), ('cheats', 'cheats', 'cheat_tools', 'bool')]
COOP_PORT = '7845'

# Before/after pictures of an option (launcher-assets/options/<key>_off.png and _on.png): a little one beside
# the option, and both side by side when it is clicked. The captions: an option's own values, or None for
# the language's Off/On. An option without its two pictures shows none.
OPTION_PICTURES = {'widescreen': ('Off', '16:9'), 'upscale': ('1x', '4x'), 'staminabar': None, 'portraits': None,
                   'pgxp': None, 'scaling': ('sharp-bilinear', 'smooth')}

# Video presets by processor: the picture is drawn by the processor (no graphics card needed), and
# the internal resolution is what costs. Every effect stays on; only the resolution changes.
PRESETS = [
    ('preset2', {'upscale': '2'}),
    ('preset4', {'upscale': '3'}),
    ('preset6', {'upscale': '4'}),
]
PRESET_ALL = {'pgxp': '1', 'perspective': '1', 'interp': '1'}

# Internal resolutions are shown with their size in pixels (as the picture is shown: widened for
# widescreen), e.g. "4x  ·  1280x960"; the ini keeps the number. Above 4 only with Vulkan.
RES_KEYS = ('upscale', 'p2view_upscale')
KIND = {key: kind for page, section, key, kind in OPTIONS}

# the controller buttons the game knows (SDL_GameControllerGetButtonFromString, plus the two triggers)
PAD_BUTTONS = ['a', 'b', 'x', 'y', 'back', 'start', 'guide', 'leftshoulder', 'rightshoulder', 'lefttrigger',
               'righttrigger', 'dpup', 'dpdown', 'dpleft', 'dpright', 'leftstick', 'rightstick']
# Tk key names -> SDL key names (SDL_GetScancodeFromName), for the keys whose names differ
TK_TO_SDL = {
    'space': 'Space', 'BackSpace': 'Backspace', 'Return': 'Return', 'Escape': 'Escape', 'Tab': 'Tab',
    'minus': '-', 'equal': '=', 'bracketleft': '[', 'bracketright': ']', 'backslash': '\\',
    'semicolon': ';', 'apostrophe': "'", 'quoteright': "'", 'grave': '`', 'quoteleft': '`',
    'comma': ',', 'period': '.', 'slash': '/', 'Caps_Lock': 'CapsLock', 'Print': 'PrintScreen',
    'Scroll_Lock': 'ScrollLock', 'Pause': 'Pause', 'Insert': 'Insert', 'Delete': 'Delete',
    'Home': 'Home', 'End': 'End', 'Prior': 'PageUp', 'Next': 'PageDown',
    'Up': 'Up', 'Down': 'Down', 'Left': 'Left', 'Right': 'Right', 'Num_Lock': 'Numlock',
    'Shift_L': 'Left Shift', 'Shift_R': 'Right Shift', 'Control_L': 'Left Ctrl', 'Control_R': 'Right Ctrl',
    'Alt_L': 'Left Alt', 'Alt_R': 'Right Alt', 'Meta_L': 'Left Alt', 'Meta_R': 'Right Alt',
    'Win_L': 'Left GUI', 'Win_R': 'Right GUI', 'Super_L': 'Left GUI', 'Super_R': 'Right GUI',
    'KP_Divide': 'Keypad /', 'KP_Multiply': 'Keypad *', 'KP_Subtract': 'Keypad -', 'KP_Add': 'Keypad +',
    'KP_Enter': 'Keypad Enter', 'KP_Decimal': 'Keypad .', 'KP_Delete': 'Keypad .', 'KP_Separator': 'Keypad .',
    'KP_Insert': 'Keypad 0', 'KP_End': 'Keypad 1', 'KP_Down': 'Keypad 2', 'KP_Next': 'Keypad 3',
    'KP_Left': 'Keypad 4', 'KP_Begin': 'Keypad 5', 'KP_Right': 'Keypad 6', 'KP_Home': 'Keypad 7',
    'KP_Up': 'Keypad 8', 'KP_Prior': 'Keypad 9',
}
# Windows virtual-key codes of the numeric keypad: Tk on Windows may name them like the main keys
WIN_KEYPAD = dict([(0x60 + n, 'Keypad %d' % n) for n in range(10)] +
                  [(0x6A, 'Keypad *'), (0x6B, 'Keypad +'), (0x6D, 'Keypad -'), (0x6E, 'Keypad .'), (0x6F, 'Keypad /')])


def sdl_key_name(keysym, keycode=0):
    """the name hmbtn.ini wants for a key pressed in Tk, or None for one the game cannot bind"""
    if os.name == 'nt' and keycode in WIN_KEYPAD:
        return WIN_KEYPAD[keycode]
    if keysym in TK_TO_SDL:
        return TK_TO_SDL[keysym]
    if len(keysym) == 1 and keysym.isascii() and keysym.isalnum():
        return keysym.upper()
    if keysym.startswith('KP_') and keysym[3:].isdigit():
        return 'Keypad ' + keysym[3:]
    if re.fullmatch(r'F([1-9]|1[0-9]|2[0-4])', keysym):
        return keysym
    return None


def add_key(value, name, replace):
    """the bindings after Set (replace) or Add: comma-separated, each key once"""
    keys = [] if replace else [k.strip() for k in value.split(',') if k.strip()]
    if name.lower() not in (k.lower() for k in keys):
        keys.append(name)
    return ','.join(keys)


def res_label(n, ws):
    if not n.isdigit():
        return n
    k = int(n)
    w, h = 320 * k, 240 * k
    if ws and ws != 'off' and ':' in ws:
        a, b = ws.split(':')
        w = round(h * int(a) / int(b))
    return '%dx  \u00b7  %dx%d' % (k, w, h)


def res_values(key, ws, vulkan):
    vals = KIND[key].split(':', 1)[1].split('|')
    return [v for v in vals if not v.isdigit() or int(v) <= 4 or vulkan]


# option -> (what it needs, as a text key): greyed out until it holds
def _up2(v):
    return v('upscale') not in ('', '1')
REQUIRES = {
    'pgxp': (_up2, 'need_up2'),
    'perspective': (lambda v: _up2(v) and v('pgxp') == '1', 'need_pgxp'),
    'interp': (lambda v: _up2(v) and v('pgxp') == '1', 'need_pgxp'),
    'drawthreads': (_up2, 'need_up2'),
    'texturepack': (_up2, 'need_up2'),
    'portraits': (_up2, 'need_up2'),
    'dumptextures': (_up2, 'need_up2'),
}

# option -> (title, explanation)
TEXT = {
    'en': {
        'renderer': ('Renderer', 'cpu: the processor draws the picture (works on any PC). vulkan: the graphics '
                     'card draws it (experimental, needs a graphics driver with Vulkan): much lighter on the '
                     'processor, and resolutions up to 8x. Without Vulkan the game uses cpu.'),
        'upscale': ('Internal resolution', 'Draws the 3D 2, 3 or 4 times larger than the original 320x240, '
                    'for smoother edges (1 = original). The processor draws the picture, so each step costs a '
                    'lot more: 2 for 2 cores, 3 for 4 cores, 4 for 6 or more. With the Vulkan renderer the '
                    'graphics card draws it, up to 8x.'),
        'widescreen': ('Widescreen', 'Shows more of the farm and town at the sides. Videos stay 4:3. With '
                       'resolution 2 or more, people and the HUD keep their shape, and the title '
                       'screen and menus fill the screen too.'),
        'pgxp': ('Precise 3D (PGXP)', 'No more wobbling polygons, the PlayStation\'s trademark shimmer.'),
        'perspective': ('Perspective-correct textures', 'No warping on large floors and walls. Costs some '
                        'speed.'),
        'interp': ('60 fps camera', 'The camera and scenery move 60 times a second (the game itself still runs '
                   'at 30). Drawn on spare processor cores; F9 switches it while playing.'),
        'texturepack': ('Texture pack', 'Draws people, animals, field objects, text and the HUD with the images in '
                        'the textures folder (not the 3D world). See docs/GRAPHICS.md.'),
        'portraits': ('Redrawn portraits', "Characters' faces in conversations drawn anew in high resolution "
                      '(the images in textures/portraits). Off: the original portraits.'),
        'dumptextures': ('Dump textures', 'Writes each sprite texture the game shows to textures/dump, '
                         'to draw a texture pack from.'),
        'drawthreads': ('Drawing threads', 'Processor threads that draw the 3D together, each a share of the '
                        'picture (the picture is the same). auto = by processor; 0 = the game\'s thread alone.'),
        'scale': ('Window size', 'Multiple of 320x240, when not in fullscreen.'),
        'fullscreen': ('Start in fullscreen', 'F11 or Alt+Enter switches at any time.'),
        'scaling': ('Scaling to the screen', 'sharp-bilinear: sharp, even pixels (recommended). sharp: plain '
                    'nearest pixel. smooth: soft.'),
        'aspect43': ('4:3 picture', 'Like a CRT TV. Off: square pixels.'),
        'crt': ('CRT filter', 'scanlines: the lines of a TV picture, sharp. crt: a curved tube with a '
                'shadow mask and glow. F6 switches in the game, F7 adjusts it. Needs Vulkan (also with the CPU renderer).'),
        'vsync': ('Vsync', "Waits for the screen's refresh: no tearing. On a 60 or 120 Hz screen the game keeps "
                  'time with it, so the camera moves evenly (about 1% faster than the console). At 144 Hz and '
                  'other rates, G-Sync or FreeSync makes it even.'),
        'disc': ('Game disc', 'Your copy of the game: a .cue/.bin image or a .pak data pack. Empty: the disc '
                 'folder or HarvestMoonBTN.pak next to the game.'),
        'savedir': ('Memory card folder', 'Where the memory cards (card1.mcd ... card1_4.mcd) are kept. Empty: '
                    'the saves folder.'),
        'autoslot': ('Memory card slot 1 directly', 'Saving and loading skip the slot question.'),
        'fastcard': ('Fast memory card', 'No waiting while the card is searched, read or written.'),
        'quickui': ('Quick menus', 'Windows open at once, shorter fades.'),
        'fastload': ('Fast loading', 'Full speed on "Now Loading" and through doors.'),
        'fastcd': ('Disc speed', 'Shorter loading pauses: 0 = original, 1 = 4x, 8 = 8x, 16 = 16x. The music '
                   'is not affected.'),
        'textspeed': ('Text speed', 'How fast dialogue is typed: 1 = original, up to 16.'),
        'skipscenes': ('Fast-forward cutscenes', 'Hold START during a cutscene.'),
        'fastforward': ('Fast-forward with Tab / L3', 'Hold Tab (or L3 on the controller) to run the game at full speed.'),
        'overclock': ('Processor speed', 'In percent: 100 = the original PlayStation (slows down in busy '
                      'scenes), 200 = no slowdowns.'),
        'emptybag': ('Empty rucksack: R1 / R2 do nothing', 'So running doesn\'t stop when you press them by '
                     'accident.'),
        'earlycabinet': ('Cabinet in the first house', 'The pile of boxes opens the cabinet, before the house '
                         'is extended.'),
        'cropwalk': ('Walk through crops', 'You can walk through your planted crops, so the one in the '
                     'middle is always in reach. Weeds too.'),
        'staminabar': ('Stamina under the clock', 'A window under the clock shows your stamina (S) and '
                       'tiredness (F) with bars and numbers.'),
        'bulkfood': ('Buy food in bulk', 'At a shop shelf you choose how many to buy: one stays in your '
                     'hands, the rest go to the rucksack and then to the fridge.'),
        'spritehire': ('Hire the sprites through the Chef', 'The Chef asks how many of the free sprites go '
                       'and for how long: 1 week (200G each) or 30 days (450G each). Talk to a hired sprite to change its '
                       'job or renew the contract of all of them (left or right picks 1 week or 30 days). Sprites also work in spring, 3 times as fast, with their skills at the highest.'),
        'sturdygreenhouse': ('Storm-proof greenhouse', 'Typhoons and blizzards no longer break the greenhouse. With the building order on, Gotz builds it: 10000G and 700 lumber.'),
        'biggreenhouse': ('Bigger greenhouse', 'The greenhouse is 12 rows deeper: 7 x 19 fields instead of 7 x 7. With the building order on, Gotz builds it after the storm-proof one: 30000G and 500 lumber.'),
        'buildorder': ('Pick the building order', 'Gotz asks which extension to build next (hen house, kitchen, barn, bigger house, greenhouse) instead of the fixed order; each keeps its own price and lumber.'),
        'runthrough': ('Run through animals', 'Running goes through the cows, the sheep and the horse; walking still pushes them.'),
        'doorreach': ('Doors from wider', 'The doors you open with X (your house, the barn, the hen house, the shops in town) work from '
                      'a wider spot in front of them: you do not need to stand just before the middle of the door. '
                      'Off: only from the narrow original spot.'),
        'bellherd': ('The bell herds the animals', 'Ring the bell inside the barn: every cow and sheep there walks to the door on its own '
                     'and out to the pasture, where they stand in rows in front of the barn. Ring it on the pasture: they all walk back in, '
                     'each to the spot it left from. They can take up to a minute; the ones at the back of the barn go round the troughs and '
                     'may stay behind: ring again. It works on rainy days too. Off: the bell only calls some animals to you.'),
        'animalmarks': ('Marks over the animals', 'Over each cow, sheep, chicken, your horse and your dog, a small icon for each thing still left today: a speech bubble (talk), a brush, a milk bottle, scissors (shear) and a feed sack (the hens). Each icon goes away once it is done; an animal with nothing left shows nothing. They start again the next day.'),
        'mailmark': ('Letter over the mailbox', 'While a letter is waiting in the mailbox on your farm, a small envelope floats over it. It goes away once you read the letter.'),
        'friendpage': ('Town friendships page', 'One more page in the START menu (Left / Right): the 32 people of the town with their hearts (the bachelorettes in their heart colour); Up / Down pick one to see their portrait (the redrawn one with Redrawn portraits) and whether you talked to them and gave them a gift today.'),
        'birthdays': ('Birthdays on the calendar', 'The calendar in your house marks with a heart the days someone in town (or a Harvest Sprite) has a birthday, and X on that day tells whose birthday it is.'),
        'randomnames': ('Random names', 'On the naming screen, Select fills in a random name for the farmer, the farm, the dog, the horse, the baby or the animal; press it again for another. Your own lists go in names.txt next to the game.'),
        'record': ('Record sessions', 'Keeps your button presses in the recordings folder, for bug reports.'),
        'stick_as_dpad': ('Left stick moves too', 'The left stick works as the D-pad.'),
        'rumble': ('Rumble', 'The controller vibrates where the game shakes a DualShock.'),
        'coop_address': ('Host address', 'To join: the address your friend gave you (e.g. 26.12.34.56 or '
                         '26.12.34.56:7845).'),
        'coop_port': ('Port', 'The same on both PCs. Empty: 7845.'),
        'split': ('Farmer 2 goes into buildings alone (online co-op only, experimental)', 'On the farm, farmer 2 can go through a '
                  'door on its own (X at a door outside): it plays in its own window, in a second copy of the game, '
                  'while farmer 1 stays where it is. Time, money and the farm are the same for both. They are '
                  'together again when both are in the same place. Uses about twice the processor while apart. '
                  'Online: the host\'s choice counts, and each one sees and hears their own farmer.'),
        'p2view_upscale': ('Farmer 2\'s window resolution', 'auto = the same as the game\'s window. 1 is the '
                           'lightest (original 320x240), for a slower PC, but at 30 pictures a second; above the '
                           'game\'s does nothing.'),
        'coop_delay': ('Controls delay', 'In frames (1 = 17 ms). Higher if the game stutters on a slow '
                       'connection; the host\'s counts.'),
        'cheat_money': ('Money', 'Your money when you reach the farm, once each time the game opens; '
                        'then it changes as usual. Changed here with F5 in the game: at once. 0 = off; up to 9999999.'),
        'cheat_stamina': ('Endless stamina', 'The farmers never get tired and never faint.'),
        'cheat_clock': ('Stop the clock', 'Time stands still (shops and nightfall too). Sleeping still starts the next day.'),
        'cheat_bag': ('Big rucksack', 'The biggest rucksack (8 places) from the start, for both farmers.'),
        'cheat_items': ('Materials at most', 'Lumber, fodder, fish food and chicken feed always at 999.'),
        'cheat_tools': ('Tools at most', 'Sickle, hoe, axe, hammer and watering can at their highest level, for '
                        'both farmers.'),
        'coop_p2': ('Two farmers (experimental)', 'The friend gets a farmer of their own, who walks with a pose '
                    'of their own (stops at walls, fences and water; no tools yet). The host\'s choice counts.'),
    },
    'pt': {
        'renderer': ('Renderizador', 'cpu: o processador desenha a imagem (funciona em qualquer PC). vulkan: a '
                     'placa de vídeo desenha (experimental, precisa de um driver de vídeo com Vulkan): muito mais '
                     'leve para o processador, e resoluções até 8x. Sem Vulkan, o jogo usa cpu.'),
        'upscale': ('Resolução interna', 'Desenha o 3D 2, 3 ou 4 vezes maior que o original 320x240, com bordas '
                    'mais lisas (1 = original). Quem desenha é o processador, então cada nível pesa bem mais: '
                    '2 para 2 núcleos, 3 para 4 núcleos, 4 para 6 ou mais. Com o renderizador Vulkan quem '
                    'desenha é a placa de vídeo, até 8x.'),
        'widescreen': ('Tela larga', 'Mostra mais da fazenda e da cidade nas laterais. Título, menus e vídeos '
                       'continuam 4:3. Com resolução 2 ou mais, pessoas e o HUD mantêm a forma.'),
        'pgxp': ('3D preciso (PGXP)', 'Acaba com os polígonos tremendo, o tremido típico do PlayStation.'),
        'perspective': ('Texturas com perspectiva correta', 'Sem distorção em chãos e paredes grandes. Pesa um '
                        'pouco.'),
        'interp': ('Câmera a 60 fps', 'A câmera e o cenário se movem 60 vezes por segundo (o jogo continua a '
                   '30). Usa núcleos livres do processador; F9 liga e desliga durante o jogo.'),
        'texturepack': ('Pacote de texturas', 'Desenha pessoas, animais, objetos do campo, texto e o HUD com as '
                        'imagens da pasta textures (não o mundo 3D). Veja docs/GRAPHICS.md.'),
        'portraits': ('Retratos redesenhados', 'Rostos dos personagens nas conversas redesenhados em alta '
                      'resolução (as imagens de textures/portraits). Desligado: os retratos originais.'),
        'dumptextures': ('Exportar texturas', 'Grava cada textura de sprite que o jogo mostra em textures/dump, '
                         'para criar um pacote de texturas.'),
        'drawthreads': ('Threads de desenho', 'Threads do processador que desenham o 3D juntas, cada uma uma '
                        'parte da imagem (a imagem é a mesma). auto = pelo processador; 0 = só a do jogo.'),
        'scale': ('Tamanho da janela', 'Múltiplo de 320x240, fora da tela cheia.'),
        'fullscreen': ('Começar em tela cheia', 'F11 ou Alt+Enter alterna a qualquer momento.'),
        'scaling': ('Ajuste à tela', 'sharp-bilinear: pixels nítidos e iguais (recomendado). sharp: pixel mais '
                    'próximo. smooth: suave.'),
        'aspect43': ('Imagem 4:3', 'Como numa TV de tubo. Desligado: pixels quadrados.'),
        'crt': ('Filtro CRT', 'scanlines: as linhas da imagem de uma TV, nítidas. crt: um tubo curvo com '
                'máscara de sombra e brilho. F6 alterna no jogo, F7 ajusta. Precisa de Vulkan (também com o renderizador CPU).'),
        'vsync': ('Vsync', 'Espera a atualização da tela: sem cortes na imagem. Numa tela de 60 ou 120 Hz o jogo '
                  'acompanha o ritmo dela, e a câmera se move por igual (cerca de 1% mais rápido que o console). '
                  'Em 144 Hz e outras taxas, o G-Sync ou FreeSync deixa por igual.'),
        'disc': ('Disco do jogo', 'Sua cópia do jogo: imagem .cue/.bin ou pacote .pak. Vazio: a pasta disc ou '
                 'o HarvestMoonBTN.pak ao lado do jogo.'),
        'savedir': ('Pasta dos memory cards', 'Onde ficam os memory cards (card1.mcd ... card1_4.mcd). Vazio: a '
                    'pasta saves.'),
        'autoslot': ('Usar o slot 1 direto', 'Salvar e carregar sem perguntar o slot do memory card.'),
        'fastcard': ('Memory card rápido', 'Sem espera enquanto o card é procurado, lido ou gravado.'),
        'quickui': ('Menus rápidos', 'Janelas abrem na hora, transições mais curtas.'),
        'fastload': ('Carregamento rápido', 'Velocidade máxima no "Now Loading" e ao passar por portas.'),
        'fastcd': ('Velocidade do disco', 'Pausas de carregamento menores: 0 = original, 1 = 4x, 8 = 8x, '
                   '16 = 16x. A música não é afetada.'),
        'textspeed': ('Velocidade do texto', 'Quão rápido o diálogo aparece: 1 = original, até 16.'),
        'skipscenes': ('Acelerar cenas', 'Segure START durante uma cena.'),
        'fastforward': ('Acelerar com Tab / L3', 'Segure Tab (ou L3 no controle) para o jogo rodar em velocidade máxima.'),
        'overclock': ('Velocidade do processador', 'Em porcentagem: 100 = o PlayStation original (fica lento em '
                      'cenas cheias), 200 = sem lentidão.'),
        'emptybag': ('Mochila vazia: R1 / R2 não fazem nada', 'Para não parar de correr ao apertar sem querer.'),
        'earlycabinet': ('Armário na primeira casa', 'A pilha de caixas abre o armário antes de a casa ser '
                         'ampliada.'),
        'cropwalk': ('Andar sobre as plantações', 'Dá para passar por cima das plantações, então a do meio '
                     'sempre fica ao alcance. O mato também.'),
        'staminabar': ('Stamina embaixo do relógio', 'Uma janela embaixo do relógio mostra a stamina (S) e o '
                       'cansaço (F) com barras e números.'),
        'bulkfood': ('Comprar comida em lote', 'Na prateleira da loja você escolhe quantas comprar: uma '
                     'fica na mão, o resto vai para a mochila e depois para a geladeira.'),
        'spritehire': ('Contratar os duendes pelo Chef', 'O Chef pergunta quantos dos duendes livres vão e por '
                       'quanto tempo: 1 semana (200G cada) ou 30 dias (450G cada). Fale com um duende contratado para '
                       'trocar o trabalho dele ou renovar o contrato de todos (esquerda ou direita escolhe 1 semana ou 30 dias). Eles também trabalham na primavera, 3 vezes mais rápido e com as habilidades no máximo.'),
        'sturdygreenhouse': ('Estufa resistente', 'Tufões e nevascas não quebram mais a estufa. Com a ordem das ampliações ligada, o Gotz constrói: 10000G e 700 de madeira.'),
        'biggreenhouse': ('Estufa maior', 'A estufa fica 12 fileiras mais funda: 7 x 19 canteiros em vez de 7 x 7. Com a ordem das ampliações ligada, o Gotz amplia depois da resistente: 30000G e 500 de madeira.'),
        'buildorder': ('Escolher a ordem das ampliações', 'O Gotz pergunta qual ampliação construir (galinheiro, cozinha, estábulo, casa maior, estufa) em vez da ordem fixa; cada uma com seu preço e sua madeira.'),
        'runthrough': ('Correr atravessa os animais', 'Correndo você passa pelas vacas, ovelhas e pelo cavalo; andando continua empurrando.'),
        'doorreach': ('Portas com alcance maior', 'As portas que se abrem com X (a sua casa, o celeiro, o galinheiro, as lojas da cidade) '
                      'funcionam de um trecho mais largo na frente delas: não precisa ficar bem no meio da porta. '
                      'Desligado: só do ponto estreito do jogo original.'),
        'bellherd': ('Sino leva os animais', 'Toque o sino dentro do celeiro: todas as vacas e ovelhas de lá andam sozinhas até a porta '
                     'e saem para o pasto, onde ficam em fileiras na frente do celeiro. Toque o sino no pasto: todas voltam andando para dentro, '
                     'cada uma para o lugar de onde saiu. Elas levam até 1 minuto; as do fundo do celeiro contornam os cochos e às vezes ficam '
                     'para trás: toque de novo. Funciona também em dia de chuva. Desligado: o sino só chama alguns animais até você.'),
        'animalmarks': ('Marcas em cima dos animais', 'Em cima de cada vaca, ovelha, galinha, do cavalo e do cachorro, um ícone para cada coisa que ainda falta hoje: balão de fala (conversar), escova, garrafa de leite (ordenhar), tesoura (tosquiar) e saco de ração (galinhas). Cada ícone some quando você faz; animal sem nada faltando não mostra nada. Recomeça no dia seguinte.'),
        'mailmark': ('Cartinha na caixa de correio', 'Quando chega carta na caixa de correio da fazenda, aparece uma cartinha em cima dela. Some depois que você lê a carta.'),
        'friendpage': ('Página de amizades da cidade', 'Mais uma página no menu START (Esquerda / Direita): os 32 moradores com os corações (as garotas na cor do coração delas); Cima / Baixo escolhem um para ver o retrato (o redesenhado, com Retratos redesenhados) e se você já conversou e deu presente hoje.'),
        'birthdays': ('Aniversários no calendário', 'O calendário da sua casa marca com um coração os dias de aniversário dos moradores (e dos duendes), e o X no dia diz de quem é o aniversário.'),
        'randomnames': ('Nomes aleatórios', 'Na tela de nome, o Select sorteia um nome para o fazendeiro, a fazenda, o cachorro, o cavalo, o bebê ou o animal; aperte de novo para outro. Suas próprias listas vão no names.txt, na pasta do jogo.'),
        'record': ('Gravar sessões', 'Guarda os botões apertados na pasta recordings, para relatar bugs.'),
        'stick_as_dpad': ('Analógico esquerdo também anda', 'O analógico esquerdo funciona como o direcional.'),
        'rumble': ('Vibração', 'O controle vibra onde o jogo faz o DualShock vibrar.'),
        'coop_address': ('Endereço do anfitrião', 'Para entrar: o endereço que seu amigo passou (ex.: '
                         '26.12.34.56 ou 26.12.34.56:7845).'),
        'coop_port': ('Porta', 'A mesma nos dois PCs. Vazio: 7845.'),
        'split': ('Fazendeiro 2 entra nas construções sozinho (só coop online, experimental)', 'Na fazenda o fazendeiro 2 pode '
                  'passar por uma porta sozinho (X na porta, do lado de fora): ele joga na janela dele, numa segunda '
                  'cópia do jogo, e o fazendeiro 1 fica onde está. Hora, dinheiro e fazenda são os mesmos para os '
                  'dois. Eles se juntam de novo quando os dois estão no mesmo lugar. Usa cerca do dobro do '
                  'processador enquanto estão separados. No coop online vale a escolha do anfitrião, e cada um '
                  'vê e ouve o seu fazendeiro.'),
        'p2view_upscale': ('Resolução da janela do fazendeiro 2', 'auto = a mesma da janela do jogo. 1 é a mais '
                           'leve (original 320x240), para PC mais fraco, mas a 30 imagens por segundo; acima da do '
                           'jogo não muda nada.'),
        'coop_delay': ('Atraso dos controles', 'Em quadros (1 = 17 ms). Aumente se o jogo engasgar com internet '
                       'lenta; vale o do anfitrião.'),
        'cheat_money': ('Dinheiro', 'O dinheiro da fazenda ao entrar nela, uma vez a cada vez que o jogo abre; '
                        'depois ele muda normalmente. Mudado aqui pelo F5 no jogo: na hora. 0 = desligado; até 9999999.'),
        'cheat_stamina': ('Estamina infinita', 'Os fazendeiros nunca cansam e nunca desmaiam.'),
        'cheat_clock': ('Parar o relógio', 'O tempo não passa (lojas e noite também). Dormir ainda começa o dia seguinte.'),
        'cheat_bag': ('Mochila grande', 'A maior mochila (8 lugares) desde o começo, para os dois fazendeiros.'),
        'cheat_items': ('Materiais no máximo', 'Madeira, feno, comida de peixe e ração de galinha sempre em 999.'),
        'cheat_tools': ('Ferramentas no máximo', 'Foice, enxada, machado, marreta e regador no nível mais alto, para '
                        'os dois fazendeiros.'),
        'coop_p2': ('Dois fazendeiros (experimental)', 'O amigo ganha um fazendeiro próprio, que anda com pose '
                    'própria (para em paredes, cercas e água; ainda sem ferramentas). Vale a escolha do anfitrião.'),
    },
    'es': {
        'renderer': ('Renderizador', 'cpu: el procesador dibuja la imagen (funciona en cualquier PC). vulkan: '
                     'la tarjeta gráfica la dibuja (experimental, necesita un controlador de vídeo con Vulkan): '
                     'mucho más liviano para el procesador, y resoluciones hasta 8x. Sin Vulkan, el juego usa cpu.'),
        'upscale': ('Resolución interna', 'Dibuja el 3D 2, 3 o 4 veces más grande que el original 320x240, con '
                    'bordes más suaves (1 = original). Dibuja el procesador, así que cada nivel cuesta mucho más: '
                    '2 para 2 núcleos, 3 para 4 núcleos, 4 para 6 o más. Con el renderizador Vulkan dibuja la '
                    'tarjeta gráfica, hasta 8x.'),
        'widescreen': ('Pantalla panorámica', 'Muestra más de la granja y del pueblo a los lados. Título, menús y '
                       'vídeos siguen en 4:3. Con resolución 2 o más, las personas y el HUD mantienen su forma.'),
        'pgxp': ('3D preciso (PGXP)', 'Se acabaron los polígonos temblorosos, el temblor típico de PlayStation.'),
        'perspective': ('Texturas con perspectiva correcta', 'Sin deformación en suelos y paredes grandes. Cuesta '
                        'algo de velocidad.'),
        'interp': ('Cámara a 60 fps', 'La cámara y el escenario se mueven 60 veces por segundo (el juego sigue a '
                   '30). Usa núcleos libres del procesador; F9 lo alterna durante el juego.'),
        'texturepack': ('Paquete de texturas', 'Dibuja personas, animales, objetos del campo, texto y el HUD con las '
                        'imágenes de la carpeta textures (no el mundo 3D). Ver docs/GRAPHICS.md.'),
        'portraits': ('Retratos redibujados', 'Las caras de los personajes en las conversaciones redibujadas en '
                      'alta resolución (las imágenes de textures/portraits). Desactivado: los retratos originales.'),
        'dumptextures': ('Exportar texturas', 'Guarda cada textura de sprite que el juego muestra en textures/dump, '
                         'para crear un paquete de texturas.'),
        'drawthreads': ('Hilos de dibujo', 'Hilos del procesador que dibujan el 3D juntos, cada uno una parte de '
                        'la imagen (la imagen es la misma). auto = según el procesador; 0 = solo el del juego.'),
        'scale': ('Tamaño de la ventana', 'Múltiplo de 320x240, fuera de pantalla completa.'),
        'fullscreen': ('Empezar en pantalla completa', 'F11 o Alt+Enter la alterna en cualquier momento.'),
        'scaling': ('Ajuste a la pantalla', 'sharp-bilinear: píxeles nítidos y parejos (recomendado). sharp: '
                    'píxel más cercano. smooth: suave.'),
        'aspect43': ('Imagen 4:3', 'Como en un televisor de tubo. Apagado: píxeles cuadrados.'),
        'crt': ('Filtro CRT', 'scanlines: las líneas de la imagen de un televisor, nítidas. crt: un tubo curvo '
                'con máscara de sombra y brillo. F6 cambia en el juego, F7 lo ajusta. Necesita Vulkan (también con el '
                'renderizador CPU).'),
        'vsync': ('Vsync', 'Espera el refresco de la pantalla: sin cortes en la imagen. En una pantalla de 60 o '
                  '120 Hz el juego sigue su ritmo, y la cámara se mueve de forma pareja (cerca de 1% más rápido '
                  'que la consola). A 144 Hz y otras tasas, G-Sync o FreeSync la hace pareja.'),
        'disc': ('Disco del juego', 'Tu copia del juego: imagen .cue/.bin o paquete .pak. Vacío: la carpeta disc '
                 'o el HarvestMoonBTN.pak junto al juego.'),
        'savedir': ('Carpeta de memory cards', 'Donde se guardan las memory cards (card1.mcd ... card1_4.mcd). '
                    'Vacío: la carpeta saves.'),
        'autoslot': ('Usar directamente la ranura 1', 'Guardar y cargar sin preguntar la ranura de la memory '
                     'card.'),
        'fastcard': ('Memory card rápida', 'Sin esperas mientras la tarjeta se busca, se lee o se escribe.'),
        'quickui': ('Menús rápidos', 'Las ventanas se abren al instante, transiciones más cortas.'),
        'fastload': ('Carga rápida', 'Velocidad máxima en "Now Loading" y al cruzar puertas.'),
        'fastcd': ('Velocidad del disco', 'Pausas de carga más cortas: 0 = original, 1 = 4x, 8 = 8x, 16 = 16x. '
                   'La música no se ve afectada.'),
        'textspeed': ('Velocidad del texto', 'Qué tan rápido aparece el diálogo: 1 = original, hasta 16.'),
        'skipscenes': ('Acelerar escenas', 'Mantén START durante una escena.'),
        'fastforward': ('Acelerar con Tab / L3', 'Mantén Tab (o L3 en el mando) para que el juego corra a velocidad máxima.'),
        'overclock': ('Velocidad del procesador', 'En porcentaje: 100 = la PlayStation original (se ralentiza en '
                      'escenas cargadas), 200 = sin ralentizaciones.'),
        'emptybag': ('Mochila vacía: R1 / R2 no hacen nada', 'Para no dejar de correr si los pulsas sin querer.'),
        'earlycabinet': ('Armario en la primera casa', 'La pila de cajas abre el armario antes de ampliar la '
                         'casa.'),
        'cropwalk': ('Caminar sobre los cultivos', 'Puedes pasar por encima de tus cultivos, así el del medio '
                     'siempre queda al alcance. La maleza también.'),
        'staminabar': ('Energía bajo el reloj', 'Una ventana bajo el reloj muestra tu energía (S) y tu '
                       'cansancio (F) con barras y números.'),
        'bulkfood': ('Comprar comida por lotes', 'En el estante de la tienda eliges cuántas comprar: una '
                     'queda en tus manos, el resto va a la mochila y luego a la nevera.'),
        'spritehire': ('Contratar a los duendes con el Chef', 'El Chef pregunta cuántos de los duendes libres van y '
                       'por cuánto tiempo: 1 semana (200G cada uno) o 30 días (450G cada uno). Habla con un duende contratado '
                       'para cambiar su trabajo o renovar el contrato de todos (izquierda o derecha elige 1 semana o 30 días). También trabajan en primavera, 3 veces más rápido y con las habilidades al máximo.'),
        'sturdygreenhouse': ('Invernadero resistente', 'Los tifones y las ventiscas ya no rompen el invernadero. Con el orden de ampliaciones activo, Gotz lo construye: 10000G y 700 de madera.'),
        'biggreenhouse': ('Invernadero más grande', 'El invernadero tiene 12 filas más de fondo: 7 x 19 parcelas en vez de 7 x 7. Con el orden de ampliaciones activo, Gotz lo amplía después del resistente: 30000G y 500 de madera.'),
        'buildorder': ('Elegir el orden de las ampliaciones', 'Gotz pregunta qué ampliación construir (gallinero, cocina, establo, casa más grande, invernadero) en vez del orden fijo; cada una con su precio y su madera.'),
        'runthrough': ('Correr atraviesa los animales', 'Corriendo pasas a través de las vacas, las ovejas y el caballo; caminando sigues empujándolos.'),
        'doorreach': ('Puertas con más alcance', 'Las puertas que se abren con X (tu casa, el establo, el gallinero, las tiendas del pueblo) '
                      'funcionan desde un tramo más ancho frente a ellas: no hace falta quedarse justo en el medio de la puerta. '
                      'Desactivado: solo desde el punto estrecho del juego original.'),
        'bellherd': ('La campana lleva a los animales', 'Toca la campana dentro del establo: todas las vacas y ovejas de allí caminan solas hasta '
                     'la puerta y salen al prado, donde quedan en filas frente al establo. Tócala en el prado: todas vuelven caminando adentro, '
                     'cada una al lugar de donde salió. Pueden tardar hasta 1 minuto; las del fondo del establo rodean los comederos y a veces '
                     'se quedan atrás: toca otra vez. También funciona en días de lluvia. Desactivado: la campana solo llama a algunos animales.'),
        'animalmarks': ('Marcas sobre los animales', 'Sobre cada vaca, oveja, gallina, tu caballo y tu perro, un icono por cada cosa que aún falta hoy: bocadillo (hablar), cepillo, botella de leche (ordeñar), tijeras (esquilar) y saco de pienso (gallinas). Cada icono se va al hacerlo; un animal sin nada pendiente no muestra nada. Vuelven al día siguiente.'),
        'mailmark': ('Carta sobre el buzón', 'Cuando llega una carta al buzón de tu granja, aparece un sobre pequeño encima. Se va al leer la carta.'),
        'friendpage': ('Página de amistades del pueblo', 'Una página más en el menú START (Izquierda / Derecha): las 32 personas del pueblo con sus corazones (las chicas en el color de su corazón); Arriba / Abajo eligen una para ver su retrato (el redibujado, con Retratos redibujados) y si ya hablaste con ella y le diste un regalo hoy.'),
        'birthdays': ('Cumpleaños en el calendario', 'El calendario de tu casa marca con un corazón los días de cumpleaños de la gente del pueblo (y de los duendes), y X en ese día dice de quién es el cumpleaños.'),
        'randomnames': ('Nombres al azar', 'En la pantalla de nombre, Select elige un nombre al azar para el granjero, la granja, el perro, el caballo, el bebé o el animal; púlsalo otra vez para otro. Tus propias listas van en names.txt, junto al juego.'),
        'record': ('Grabar sesiones', 'Guarda los botones pulsados en la carpeta recordings, para reportar '
                   'errores.'),
        'stick_as_dpad': ('El stick izquierdo también mueve', 'El stick izquierdo funciona como la cruceta.'),
        'rumble': ('Vibración', 'El mando vibra donde el juego hace vibrar el DualShock.'),
        'coop_address': ('Dirección del anfitrión', 'Para entrar: la dirección que te pasó tu amigo (por ejemplo '
                         '26.12.34.56 o 26.12.34.56:7845).'),
        'coop_port': ('Puerto', 'El mismo en los dos PCs. Vacío: 7845.'),
        'split': ('El granjero 2 entra solo en los edificios (solo coop online, experimental)', 'En la granja el granjero 2 puede '
                  'pasar solo por una puerta (X en la puerta, desde fuera): juega en su ventana, en una segunda '
                  'copia del juego, y el granjero 1 se queda donde está. La hora, el dinero y la granja son los '
                  'mismos para los dos. Se juntan otra vez cuando los dos están en el mismo lugar. Usa cerca del '
                  'doble de procesador mientras están separados. En el coop online vale lo que elija el anfitrión, '
                  'y cada uno ve y oye a su granjero.'),
        'p2view_upscale': ('Resolución de la ventana del granjero 2', 'auto = la misma que la ventana del juego. 1 '
                           'es la más ligera (original 320x240), para un PC más lento, pero a 30 imágenes por segundo; '
                           'más que la del juego no cambia nada.'),
        'coop_delay': ('Retraso de los controles', 'En fotogramas (1 = 17 ms). Más alto si el juego se traba '
                       'con internet lenta; vale el del anfitrión.'),
        'cheat_money': ('Dinero', 'El dinero de la granja al llegar a ella, una vez cada vez que se abre el '
                        'juego; después cambia como siempre. Cambiado aquí con F5 en el juego: al momento. 0 = desactivado; hasta 9999999.'),
        'cheat_stamina': ('Energía infinita', 'Los granjeros nunca se cansan ni se desmayan.'),
        'cheat_clock': ('Parar el reloj', 'El tiempo no pasa (tiendas y noche también). Dormir sigue empezando el día siguiente.'),
        'cheat_bag': ('Mochila grande', 'La mochila más grande (8 huecos) desde el principio, para los dos granjeros.'),
        'cheat_items': ('Materiales al máximo', 'Madera, forraje, comida de peces y pienso de gallina siempre en 999.'),
        'cheat_tools': ('Herramientas al máximo', 'Hoz, azada, hacha, martillo y regadera en su nivel más alto, para '
                        'los dos granjeros.'),
        'coop_p2': ('Dos granjeros (experimental)', 'Tu amigo tiene su propio granjero, que camina con pose '
                    'propia (se detiene en paredes, vallas y agua; aún sin herramientas). Vale lo que elija el anfitrión.'),
    },
}
BUTTON_NAMES = {
    'en': {'up': 'Up', 'down': 'Down', 'left': 'Left', 'right': 'Right', 'cross': 'Cross (X)',
           'circle': 'Circle (O)', 'square': 'Square', 'triangle': 'Triangle', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT', 'fastforward': 'Fast-forward (hold)'},
    'pt': {'up': 'Cima', 'down': 'Baixo', 'left': 'Esquerda', 'right': 'Direita', 'cross': 'Xis (X)',
           'circle': 'Círculo (O)', 'square': 'Quadrado', 'triangle': 'Triângulo', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT', 'fastforward': 'Acelerar (segurar)'},
    'es': {'up': 'Arriba', 'down': 'Abajo', 'left': 'Izquierda', 'right': 'Derecha', 'cross': 'Equis (X)',
           'circle': 'Círculo (O)', 'square': 'Cuadrado', 'triangle': 'Triángulo', 'l1': 'L1', 'r1': 'R1',
           'l2': 'L2', 'r2': 'R2', 'start': 'START', 'select': 'SELECT', 'fastforward': 'Acelerar (mantener)'},
}
UI = {
    'en': {
        'subtitle': 'PC port settings', 'start': 'Start', 'video': 'Video', 'game': 'Game',
        'keyboard': 'Keyboard', 'controller': 'Controller',
        'welcome': 'Welcome to the farm!', 'welcome_sub': 'Choose the setup for your PC, then press Play. '
        'Everything can be fine-tuned on the other pages.',
        'data_ok': 'Game found: %s', 'data_missing': 'Game disc not found. Put the .cue/.bin in the disc folder '
        'or choose it on the Game page.',
        'pc': 'This PC: %d cores, %d processor threads. No graphics card needed: the processor draws the picture.',
        'quick': 'Quick setup', 'quick_sub': 'All effects stay on (precise 3D, perspective, 60 fps camera); only '
        'the resolution changes.',
        'preset2': 'PC with 2 cores', 'preset4': 'PC with 4 cores', 'preset6': 'PC with 6 or more cores',
        'preset2_d': 'Resolution 2 (640x480). Smooth on simple or older processors.',
        'preset4_d': 'Resolution 3 (960x720). Sharp, and smooth on most PCs.',
        'preset6_d': 'Resolution 4 (1280x960). The sharpest; needs a strong processor.',
        'use': 'Use this', 'in_use': 'In use', 'recommended': 'Recommended for this PC',
        'keys': 'Handy keys', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 in slot 1 (shown in the window title)'),
            ('L1 / R1  (Q / W)', 'On a diary list: previous / next memory card'),
            ('Triangle (hold)', 'On a diary list: hold 2 seconds to delete that diary'),
            ('START (hold)', 'Fast-forward a cutscene'),
            ('Tab / L3 (hold)', 'Fast-forward the game'),
            ('F5', 'Cheats, while playing'),
            ('F9', '60 fps camera on / off'),
            ('F11  /  Alt+Enter', 'Fullscreen'),
            ('F12', 'Screenshot (screenshots folder)')],
        'applied': '%s chosen and saved.',
        'video_sub': 'The picture is drawn by the processor: resolution is what costs the most.',
        'game_sub': 'Game files, memory cards and shortcuts that save time.',
        'mods': 'Mods', 'mods_sub': 'Changes to how the original game plays. All off unless you switch them '
                                    "on; in online co-op the host's choice counts.",
        'keyboard_sub': 'Set, then press the key; Add puts one more key on the same button. The names can '
        'also be typed, comma-separated (e.g. X,Space,K). Empty: the default.',
        'controller_sub': 'Pick a button from the list, or type several, comma-separated '
        '(e.g. a,dpdown). Empty: the default.',
        'key_set': 'Set', 'key_add': 'Add', 'key_wait': 'Press a key... (Esc: cancel)',
        'key_bad': 'That key cannot be used',
        'need_up2': 'Needs resolution 2 or more', 'need_pgxp': 'Needs Precise 3D (PGXP)',
        'pic_tip': 'Click to see it in the game', 'pic_close': 'Close',
        'on': 'On', 'off': 'Off', 'browse': 'Browse...', 'default': 'default: %s',
        'close': 'Close', 'save': 'Save', 'play': 'Save and play',
        'unsaved': 'Unsaved changes', 'saved': 'Saved', 'ask_save': 'Save your changes?',
        'missing': '%s is missing.',
        'coop': 'Online co-op',
        'coop_sub': 'Play with a friend over the internet: each one runs the game on their own PC and both '
        'control the same farmer. Your normal game is not affected.',
        'host': 'Host', 'host_d': 'Your farm: your memory cards and settings. Your friend joins you. Saving '
        'in co-op writes to your memory cards.',
        'join': 'Join', 'join_d': 'Your friend\'s farm. Fill in their address below. Your own saves are not '
        'touched (co-op saves go to saves\\netplay).',
        'host_btn': 'Host and play', 'join_btn': 'Join and play',
        'cheats': 'Cheats', 'cheats_sub': 'Changes to the game itself. What you save with them stays in the '
        'save. In co-op, the host\'s cheats count for both.',
        'p2': 'Local co-op', 'p2_sub': 'Two farmers on this PC, no internet: you play the game\'s farmer and a '
        'second player gets a farmer of their own, with a window of their own (experimental).',
        'local': 'Two farmers on this PC', 'local_d': 'Farmer 1 plays with the keyboard or the first controller; '
        'farmer 2 with the second controller or the numeric keypad.',
        'local_btn': 'Play with two farmers',
        'p2_keys': 'Farmer 2\'s controls', 'p2_head': ('', 'Controller 2', 'Numeric keypad'), 'p2_list': [
            ('Walk', 'D-pad', '8  4  5  6'), ('Use the tool', 'Square', '7'),
            ('Look, talk, open, trade', 'Cross', '9'), ('Run', 'Circle', '1'), ('Change tools', 'R1', '3'),
            ('Put away / change the item', 'R2', '+'), ('Whistle', 'L1 / L2', '/   *'),
            ('Rucksack', 'Triangle', '0'), ('Menu', 'Start', 'Enter'), ('Map', 'Select', '.')],
        'p2_tips': 'Good to know', 'p2_tips_list': [
            ('\u21c4', 'Trading: stand in front of the other farmer, close, and press Cross (9). First the tools, '
                        'then the items, as many as the other\'s rucksack holds.'),
            ('!', 'There is one of each tool: farmer 2 starts with nothing and takes tools from the tool chest.'),
            ('!', 'Only what farmer 1 carries is saved: when saving, farmer 2\'s tools go back to the chest.')],
        'my_ip': 'Addresses of this PC (give one to your friend): %s',
        'build_id': "Game version: %s. Your friend's must be the same (it is also in the game's window title).",
        'need_addr': 'Fill in the host address first.',
        'build': 'Build from my disc', 'build_sub': 'Makes the PC game from your own disc image, here in this '
        'folder. The first time, it also downloads the compiler (about 65 MB, checked file by file).',
        'pak': 'Data pack', 'pak_d': "Also copies the game's data into HarvestMoonBTN.pak: the disc image is not "
        'needed afterwards.',
        'build_btn': 'Build the game', 'building': 'Building... (this takes a few minutes)',
        'built': 'Done: the game is ready.', 'build_failed': 'The build stopped: the last lines above say why.',
        'need_cue': 'Choose your disc image (.cue or .bin) first.',
        'built_lang': 'The game built here is in %s.', 'lang_en': 'English', 'lang_pt': 'Portuguese',
        'lang_id': 'Indonesian', 'lang_de': 'German', 'lang_es': 'Spanish', 'lang_ja': 'Japanese',
        'gamelang': 'Game language', 'gamelang_sub': 'The language of the game\'s own text: dialogue, menus and '
        'items. This launcher\'s language is the flags at the top.',
        'gl_en': 'English', 'gl_en_d': 'The original text of the USA disc.',
        'gl_pt': 'Português (Brasil)', 'gl_pt_d': 'Fan translation by GabriPires. The first time it is downloaded '
        'and applied to a copy of your disc (disc-pt folder).',
        'gl_id': 'Bahasa Indonesia', 'gl_id_d': 'Fan translation (Indonesian edition of the Kucing NPC mod). '
        'The first time its text is downloaded and put onto a copy of your disc (disc-id folder).',
        'gl_de': 'Deutsch', 'gl_de_d': 'The German dialogue of the European release. Needs your own European '
        'disc in this folder, besides your USA one: SLES-02781, Rev 1 (English/German), as .bin + .cue (or a '
        '.zip with them), "Harvest Moon - Back to Nature (Europe) (En,De) (Rev 1)". Rev 0 and the French disc '
        'do not work. Its dialogue is put onto a copy of your disc (disc-de folder). Menus and items stay in '
        'English.',
        'gl_ja': '日本語', 'gl_ja_d': 'The text of the Japanese release: dialogue, menus and items, with its own '
        'font (kana and kanji). Needs your own Japanese disc in this folder, besides your USA one: SLPS-02489, '
        'Rev 1, as .bin + .cue (or a .zip with them), "Bokujou Monogatari - Harvest Moon (Japan) (Rev 1)". Its '
        'text is put onto a copy of your disc (disc-ja folder). The clock shows the time in 24 hours.',
        'gl_es': 'Español', 'gl_es_d': 'Fan translation of the dialogue, with its own font (ñ, accents). The '
        'first time its text is downloaded and put onto a copy of your disc (disc-es folder). Menus and items '
        'stay in English.',
        'gl_ready': 'Built: switches at once.', 'gl_first': 'The first time, Play builds the game in it '
        '(about a minute).',
        'gl_now': 'In use now.', 'gl_coop': 'Online co-op: both players need the same language. The texts '
        'of this port\'s own extras (building menu, sprites, memory card) stay in English in Indonesian, '
        'German and Spanish.',
        'update': 'Update to %s', 'update_note': 'Launcher %s is out.',
        'updating': 'Downloading the new launcher...', 'update_failed': 'The update failed: %s',
        'portraits_get': 'Downloading the redrawn portraits... %d%%',
        'portraits_failed': 'The redrawn portraits could not be downloaded (%s). The game shows the original '
                            'ones; they are downloaded again next time.',
        'ask_stop': 'A build is running. Stop it and close?',
        'build_note': 'The build-files folder holds files from your disc: it is for this PC only. To give the '
        'game to a friend, give them this launcher; they build from their own disc.',
        'how': 'How to connect', 'how_list': [
            ('1', 'Both need this same game folder version (the same HarvestMoonBTN.exe).'),
            ('2', 'Easiest: both install Radmin VPN or ZeroTier and join the same network; the host gives the '
                  'address it shows there (26.x.x.x).'),
            ('3', 'Or the host opens TCP port %s in the router and gives the internet address.'),
            ('4', 'The host presses Host and play first, then the friend presses Join and play. If Windows asks '
                  'about the firewall, allow it.'),
            ('!', 'Tab and F1-F4 are off in co-op; the window title shows the connection.')],
    },
    'pt': {
        'subtitle': 'Configurações do port para PC', 'start': 'Início', 'video': 'Vídeo', 'game': 'Jogo',
        'keyboard': 'Teclado', 'controller': 'Controle',
        'welcome': 'Bem-vindo à fazenda!', 'welcome_sub': 'Escolha a configuração do seu PC e clique em '
        'Jogar. Tudo pode ser ajustado nas outras páginas.',
        'data_ok': 'Jogo encontrado: %s', 'data_missing': 'Disco do jogo não encontrado. Coloque o .cue/.bin na '
        'pasta disc ou escolha na página Jogo.',
        'pc': 'Este PC: %d núcleos, %d threads. Não precisa de placa de vídeo: o processador desenha a imagem.',
        'quick': 'Configuração rápida', 'quick_sub': 'Todos os efeitos continuam ligados (3D preciso, '
        'perspectiva, câmera a 60 fps); só a resolução muda.',
        'preset2': 'PC com 2 núcleos', 'preset4': 'PC com 4 núcleos', 'preset6': 'PC com 6 ou mais núcleos',
        'preset2_d': 'Resolução 2 (640x480). Liso em processadores simples ou antigos.',
        'preset4_d': 'Resolução 3 (960x720). Nítido e liso na maioria dos PCs.',
        'preset6_d': 'Resolução 4 (1280x960). O mais nítido; pede um processador forte.',
        'use': 'Usar este', 'in_use': 'Em uso', 'recommended': 'Recomendado para este PC',
        'keys': 'Teclas úteis', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 no slot 1 (aparece no título da janela)'),
            ('L1 / R1  (Q / W)', 'Na lista de diários: memory card anterior / seguinte'),
            ('Triângulo (segurar)', 'Na lista de diários: segure 2 segundos para apagar o diário'),
            ('START (segurar)', 'Acelera uma cena'),
            ('Tab / L3 (segurar)', 'Acelera o jogo'),
            ('F5', 'Cheats, durante o jogo'),
            ('F9', 'Liga / desliga a câmera a 60 fps'),
            ('F11  /  Alt+Enter', 'Tela cheia'),
            ('F12', 'Captura de tela (pasta screenshots)')],
        'applied': '%s escolhido e salvo.',
        'video_sub': 'A imagem é desenhada pelo processador: a resolução é o que mais pesa.',
        'game_sub': 'Arquivos do jogo, memory cards e atalhos que poupam tempo.',
        'mods': 'Mods', 'mods_sub': 'Mudanças em como o jogo original funciona. Todas desligadas até você '
                                    'ligá-las; no coop online vale a escolha do anfitrião.',
        'keyboard_sub': 'Clique em Definir e aperte a tecla; Adicionar põe mais uma tecla no mesmo botão. '
        'Os nomes também podem ser digitados, separados por vírgula (ex.: X,Space,K). Vazio: o padrão.',
        'controller_sub': 'Escolha um botão na lista, ou digite vários separados por vírgula '
        '(ex.: a,dpdown). Vazio: o padrão.',
        'key_set': 'Definir', 'key_add': 'Adicionar', 'key_wait': 'Aperte uma tecla... (Esc: cancela)',
        'key_bad': 'Essa tecla não pode ser usada',
        'need_up2': 'Precisa de resolução 2 ou mais', 'need_pgxp': 'Precisa do 3D preciso (PGXP)',
        'pic_tip': 'Clique para ver no jogo', 'pic_close': 'Fechar',
        'on': 'Ligado', 'off': 'Desligado', 'browse': 'Procurar...', 'default': 'padrão: %s',
        'close': 'Fechar', 'save': 'Salvar', 'play': 'Salvar e jogar',
        'unsaved': 'Alterações não salvas', 'saved': 'Salvo', 'ask_save': 'Salvar as alterações?',
        'missing': '%s não foi encontrado.',
        'coop': 'Coop online',
        'coop_sub': 'Jogue com um amigo pela internet: cada um roda o jogo no próprio PC e os dois controlam o '
        'mesmo fazendeiro. O seu jogo normal não muda.',
        'host': 'Hospedar', 'host_d': 'A sua fazenda: seus memory cards e configurações. O amigo entra no seu '
        'jogo. Salvar no coop grava nos seus memory cards.',
        'join': 'Entrar', 'join_d': 'A fazenda do amigo. Preencha o endereço dele abaixo. Seus saves não são '
        'tocados (o que salvar no coop vai para saves\\netplay).',
        'host_btn': 'Hospedar e jogar', 'join_btn': 'Entrar e jogar',
        'cheats': 'Cheats', 'cheats_sub': 'Mudam o próprio jogo. O que você salvar com eles fica no save. No '
        'coop, valem os cheats do anfitrião para os dois.',
        'p2': 'Coop local', 'p2_sub': 'Dois fazendeiros neste PC, sem internet: você joga com o fazendeiro do '
        'jogo e outra pessoa ganha um fazendeiro próprio, com uma janela só dele (experimental).',
        'local': 'Dois fazendeiros neste PC', 'local_d': 'O fazendeiro 1 joga com o teclado ou o primeiro '
        'controle; o fazendeiro 2, com o segundo controle ou o teclado numérico.',
        'local_btn': 'Jogar com dois fazendeiros',
        'p2_keys': 'Controles do fazendeiro 2', 'p2_head': ('', 'Controle 2', 'Teclado numérico'), 'p2_list': [
            ('Andar', 'Direcional', '8  4  5  6'), ('Usar a ferramenta', 'Quadrado', '7'),
            ('Olhar, falar, abrir, trocar', 'X', '9'), ('Correr', 'Círculo', '1'),
            ('Trocar de ferramenta', 'R1', '3'), ('Guardar / trocar o item', 'R2', '+'),
            ('Assobiar', 'L1 / L2', '/   *'), ('Mochila', 'Triângulo', '0'), ('Menu', 'Start', 'Enter'),
            ('Mapa', 'Select', '.')],
        'p2_tips': 'Bom saber', 'p2_tips_list': [
            ('\u21c4', 'Troca: fique de frente para o outro fazendeiro, bem perto, e aperte X (9). Primeiro as '
                        'ferramentas, depois os itens, até onde cabe na mochila do outro.'),
            ('!', 'Há uma de cada ferramenta: o fazendeiro 2 começa sem nada e pega no baú de ferramentas.'),
            ('!', 'Só o que o fazendeiro 1 carrega é salvo: ao salvar, as ferramentas do 2 voltam para o baú.')],
        'my_ip': 'Endereços deste PC (passe um para o amigo): %s',
        'build_id': 'Versão do jogo: %s. A do amigo tem que ser a mesma (ela também fica no título da janela do jogo).',
        'need_addr': 'Preencha o endereço do anfitrião primeiro.',
        'build': 'Montar do meu disco', 'build_sub': 'Faz o jogo de PC a partir da imagem do seu próprio disco, '
        'aqui nesta pasta. Na primeira vez, também baixa o compilador (uns 65 MB, conferido arquivo por arquivo).',
        'pak': 'Pacote de dados', 'pak_d': 'Também copia os dados do jogo para HarvestMoonBTN.pak: depois a imagem '
        'do disco não é mais necessária.',
        'build_btn': 'Montar o jogo', 'building': 'Montando... (leva alguns minutos)',
        'built': 'Pronto: o jogo está montado.', 'build_failed': 'A montagem parou: as últimas linhas acima dizem por quê.',
        'need_cue': 'Escolha primeiro a imagem do disco (.cue ou .bin).',
        'built_lang': 'O jogo montado aqui está em %s.', 'lang_en': 'inglês', 'lang_pt': 'português',
        'lang_id': 'indonésio', 'lang_de': 'alemão', 'lang_es': 'espanhol', 'lang_ja': 'japonês',
        'gamelang': 'Idioma do jogo', 'gamelang_sub': 'A língua do texto do próprio jogo: falas, menus e itens. '
        'A língua deste launcher são as bandeiras lá em cima.',
        'gl_en': 'English', 'gl_en_d': 'O texto original do disco americano.',
        'gl_pt': 'Português (Brasil)', 'gl_pt_d': 'Tradução de fã do GabriPires. Na primeira vez é baixada e '
        'aplicada numa cópia do seu disco (pasta disc-pt).',
        'gl_id': 'Bahasa Indonesia', 'gl_id_d': 'Tradução de fã (edição indonésia do mod do Kucing NPC). Na '
        'primeira vez o texto dela é baixado e posto numa cópia do seu disco (pasta disc-id).',
        'gl_de': 'Deutsch', 'gl_de_d': 'As falas em alemão da versão europeia. Precisa do seu próprio disco '
        'europeu nesta pasta, além do americano: SLES-02781, Rev 1 (inglês/alemão), em .bin + .cue (ou um '
        '.zip com eles), "Harvest Moon - Back to Nature (Europe) (En,De) (Rev 1)". O Rev 0 e o disco francês '
        'não servem. As falas dele vão para uma cópia do seu disco (pasta disc-de). Menus e itens ficam em '
        'inglês.',
        'gl_ja': '日本語', 'gl_ja_d': 'O texto da versão japonesa: falas, menus e itens, com a fonte dela '
        '(kana e kanji). Precisa do seu próprio disco japonês nesta pasta, além do americano: SLPS-02489, '
        'Rev 1, em .bin + .cue (ou um .zip com eles), "Bokujou Monogatari - Harvest Moon (Japan) (Rev 1)". O '
        'texto dele vai para uma cópia do seu disco (pasta disc-ja). O relógio mostra a hora em 24 horas.',
        'gl_es': 'Español', 'gl_es_d': 'Tradução de fã das falas, com fonte própria (ñ, acentos). Na primeira '
        'vez o texto dela é baixado e posto numa cópia do seu disco (pasta disc-es). Menus e itens ficam em '
        'inglês.',
        'gl_ready': 'Montado: troca na hora.', 'gl_first': 'Na primeira vez, o Jogar monta o jogo nela '
        '(cerca de um minuto).',
        'gl_now': 'Em uso agora.', 'gl_coop': 'Coop online: os dois precisam da mesma língua. Os textos dos '
        'extras deste port (menu de obras, duendes, memory card) ficam em inglês no indonésio, no '
        'alemão e no espanhol.',
        'update': 'Atualizar para %s', 'update_note': 'Saiu o launcher %s.',
        'updating': 'Baixando o novo launcher...', 'update_failed': 'A atualização falhou: %s',
        'portraits_get': 'Baixando os retratos redesenhados... %d%%',
        'portraits_failed': 'Não deu para baixar os retratos redesenhados (%s). O jogo mostra os originais; '
                            'o download é tentado de novo na próxima vez.',
        'ask_stop': 'Uma montagem está em andamento. Parar e fechar?',
        'build_note': 'A pasta build-files guarda arquivos do seu disco: é só para este PC. Para dar o jogo a um '
        'amigo, dê este launcher; ele monta com o próprio disco.',
        'how': 'Como conectar', 'how_list': [
            ('1', 'Os dois precisam desta mesma versão do jogo (o mesmo HarvestMoonBTN.exe).'),
            ('2', 'Mais fácil: os dois instalam o Radmin VPN ou o ZeroTier e entram na mesma rede; o anfitrião '
                  'passa o endereço que aparece lá (26.x.x.x).'),
            ('3', 'Ou o anfitrião abre a porta TCP %s no roteador e passa o endereço da internet.'),
            ('4', 'O anfitrião clica em Hospedar e jogar primeiro; depois o amigo clica em Entrar e jogar. Se o '
                  'Windows perguntar sobre o firewall, permita.'),
            ('!', 'No coop, Tab e F1-F4 ficam desligados; o título da janela mostra a conexão.')],
    },
    'es': {
        'subtitle': 'Ajustes del port para PC', 'start': 'Inicio', 'video': 'Vídeo', 'game': 'Juego',
        'keyboard': 'Teclado', 'controller': 'Mando',
        'welcome': '¡Bienvenido a la granja!', 'welcome_sub': 'Elige la configuración de tu PC y pulsa Jugar. '
        'Todo se puede ajustar en las otras páginas.',
        'data_ok': 'Juego encontrado: %s', 'data_missing': 'No se encontró el disco del juego. Pon el .cue/.bin '
        'en la carpeta disc o elígelo en la página Juego.',
        'pc': 'Este PC: %d núcleos, %d hilos. No hace falta tarjeta gráfica: el procesador dibuja la imagen.',
        'quick': 'Configuración rápida', 'quick_sub': 'Todos los efectos siguen activos (3D preciso, '
        'perspectiva, cámara a 60 fps); solo cambia la resolución.',
        'preset2': 'PC con 2 núcleos', 'preset4': 'PC con 4 núcleos', 'preset6': 'PC con 6 o más núcleos',
        'preset2_d': 'Resolución 2 (640x480). Fluido en procesadores sencillos o antiguos.',
        'preset4_d': 'Resolución 3 (960x720). Nítido y fluido en la mayoría de PCs.',
        'preset6_d': 'Resolución 4 (1280x960). La más nítida; necesita un procesador potente.',
        'use': 'Usar este', 'in_use': 'En uso', 'recommended': 'Recomendado para este PC',
        'keys': 'Teclas útiles', 'keys_list': [
            ('F1 - F4', 'Memory card 1 - 4 en la ranura 1 (se ve en el título de la ventana)'),
            ('L1 / R1  (Q / W)', 'En la lista de diarios: memory card anterior / siguiente'),
            ('Triángulo (mantener)', 'En la lista de diarios: mantén 2 segundos para borrar ese diario'),
            ('START (mantener)', 'Acelera una escena'),
            ('Tab / L3 (mantener)', 'Acelera el juego'),
            ('F5', 'Trucos, durante el juego'),
            ('F9', 'Activa / desactiva la cámara a 60 fps'),
            ('F11  /  Alt+Enter', 'Pantalla completa'),
            ('F12', 'Captura de pantalla (carpeta screenshots)')],
        'applied': '%s elegido y guardado.',
        'video_sub': 'La imagen la dibuja el procesador: la resolución es lo que más cuesta.',
        'game_sub': 'Archivos del juego, memory cards y atajos que ahorran tiempo.',
        'mods': 'Mods', 'mods_sub': 'Cambios en cómo se juega el juego original. Todos apagados hasta que '
                                    'los actives; en el coop en línea cuenta la elección del anfitrión.',
        'keyboard_sub': 'Haz clic en Asignar y pulsa la tecla; Añadir pone una tecla más en el mismo botón. '
        'Los nombres también se pueden escribir, separados por comas (p. ej. X,Space,K). Vacío: el predeterminado.',
        'controller_sub': 'Elige un botón de la lista, o escribe varios separados por comas '
        '(p. ej. a,dpdown). Vacío: el predeterminado.',
        'key_set': 'Asignar', 'key_add': 'Añadir', 'key_wait': 'Pulsa una tecla... (Esc: cancelar)',
        'key_bad': 'Esa tecla no se puede usar',
        'need_up2': 'Necesita resolución 2 o más', 'need_pgxp': 'Necesita el 3D preciso (PGXP)',
        'pic_tip': 'Haz clic para verlo en el juego', 'pic_close': 'Cerrar',
        'on': 'Activado', 'off': 'Desactivado', 'browse': 'Buscar...', 'default': 'predeterminado: %s',
        'close': 'Cerrar', 'save': 'Guardar', 'play': 'Guardar y jugar',
        'unsaved': 'Cambios sin guardar', 'saved': 'Guardado', 'ask_save': '¿Guardar los cambios?',
        'missing': 'No se encontró %s.',
        'coop': 'Coop en línea',
        'coop_sub': 'Juega con un amigo por internet: cada uno corre el juego en su PC y los dos controlan al '
        'mismo granjero. Tu juego normal no cambia.',
        'host': 'Ser anfitrión', 'host_d': 'Tu granja: tus memory cards y ajustes. Tu amigo entra en tu juego. '
        'Guardar en el coop escribe en tus memory cards.',
        'join': 'Entrar', 'join_d': 'La granja de tu amigo. Escribe su dirección abajo. Tus partidas no se tocan '
        '(lo que guardes en el coop va a saves\\netplay).',
        'host_btn': 'Ser anfitrión y jugar', 'join_btn': 'Entrar y jugar',
        'cheats': 'Trucos', 'cheats_sub': 'Cambian el juego en sí. Lo que guardes con ellos queda en la partida. '
        'En el coop valen los trucos del anfitrión para los dos.',
        'p2': 'Coop local', 'p2_sub': 'Dos granjeros en este PC, sin internet: tú juegas con el granjero del '
        'juego y otra persona tiene su propio granjero, con una ventana propia (experimental).',
        'local': 'Dos granjeros en este PC', 'local_d': 'El granjero 1 juega con el teclado o el primer mando; '
        'el granjero 2, con el segundo mando o el teclado numérico.',
        'local_btn': 'Jugar con dos granjeros',
        'p2_keys': 'Controles del granjero 2', 'p2_head': ('', 'Mando 2', 'Teclado numérico'), 'p2_list': [
            ('Caminar', 'Cruceta', '8  4  5  6'), ('Usar la herramienta', 'Cuadrado', '7'),
            ('Mirar, hablar, abrir, intercambiar', 'X', '9'), ('Correr', 'Círculo', '1'),
            ('Cambiar de herramienta', 'R1', '3'), ('Guardar / cambiar el objeto', 'R2', '+'),
            ('Silbar', 'L1 / L2', '/   *'), ('Mochila', 'Triángulo', '0'), ('Menú', 'Start', 'Enter'),
            ('Mapa', 'Select', '.')],
        'p2_tips': 'Bueno saber', 'p2_tips_list': [
            ('\u21c4', 'Intercambio: ponte delante del otro granjero, muy cerca, y pulsa X (9). Primero las '
                        'herramientas, después los objetos, hasta lo que cabe en la mochila del otro.'),
            ('!', 'Hay una de cada herramienta: el granjero 2 empieza sin nada y las toma del baúl.'),
            ('!', 'Solo se guarda lo que lleva el granjero 1: al guardar, las herramientas del 2 vuelven al baúl.')],
        'my_ip': 'Direcciones de este PC (pásale una a tu amigo): %s',
        'build_id': 'Versión del juego: %s. La de tu amigo tiene que ser la misma (también está en el título de la ventana del juego).',
        'need_addr': 'Escribe primero la dirección del anfitrión.',
        'build': 'Crear desde mi disco', 'build_sub': 'Hace el juego de PC a partir de la imagen de tu propio '
        'disco, aquí en esta carpeta. La primera vez también descarga el compilador (unos 65 MB, comprobado '
        'archivo por archivo).',
        'pak': 'Paquete de datos', 'pak_d': 'También copia los datos del juego a HarvestMoonBTN.pak: después la '
        'imagen del disco ya no hace falta.',
        'build_btn': 'Crear el juego', 'building': 'Creando... (tarda unos minutos)',
        'built': 'Listo: el juego está creado.', 'build_failed': 'La creación se detuvo: las últimas líneas de '
        'arriba dicen por qué.',
        'need_cue': 'Elige primero la imagen del disco (.cue o .bin).',
        'built_lang': 'El juego construido aquí está en %s.', 'lang_en': 'inglés', 'lang_pt': 'portugués',
        'lang_id': 'indonesio', 'lang_de': 'alemán', 'lang_es': 'español', 'lang_ja': 'japonés',
        'gamelang': 'Idioma del juego', 'gamelang_sub': 'El idioma del texto del propio juego: diálogos, menús '
        'y objetos. El idioma de este launcher son las banderas de arriba.',
        'gl_en': 'English', 'gl_en_d': 'El texto original del disco de EE. UU.',
        'gl_pt': 'Português (Brasil)', 'gl_pt_d': 'Traducción de fans de GabriPires. La primera vez se descarga '
        'y se aplica a una copia de tu disco (carpeta disc-pt).',
        'gl_id': 'Bahasa Indonesia', 'gl_id_d': 'Traducción de fans (edición indonesia del mod de Kucing NPC). '
        'La primera vez su texto se descarga y se pone en una copia de tu disco (carpeta disc-id).',
        'gl_de': 'Deutsch', 'gl_de_d': 'Los diálogos en alemán de la versión europea. Necesita tu propio '
        'disco europeo en esta carpeta, además del estadounidense: SLES-02781, Rev 1 (inglés/alemán), en .bin '
        '+ .cue (o un .zip con ellos), "Harvest Moon - Back to Nature (Europe) (En,De) (Rev 1)". El Rev 0 y el '
        'disco francés no sirven. Sus diálogos se ponen en una copia de tu disco (carpeta disc-de). Los menús '
        'y objetos quedan en inglés.',
        'gl_ja': '日本語', 'gl_ja_d': 'El texto de la versión japonesa: diálogos, menús y objetos, con su '
        'propia fuente (kana y kanji). Necesita tu propio disco japonés en esta carpeta, además del '
        'estadounidense: SLPS-02489, Rev 1, en .bin + .cue (o un .zip con ellos), "Bokujou Monogatari - '
        'Harvest Moon (Japan) (Rev 1)". Su texto se pone en una copia de tu disco (carpeta disc-ja). El reloj '
        'muestra la hora en 24 horas.',
        'gl_es': 'Español', 'gl_es_d': 'Traducción de fans de los diálogos, con su propia fuente (ñ, acentos). '
        'La primera vez su texto se descarga y se pone en una copia de tu disco (carpeta disc-es). Los menús '
        'y objetos quedan en inglés.',
        'gl_ready': 'Construido: cambia al instante.', 'gl_first': 'La primera vez, Jugar construye el juego '
        'en él (cerca de un minuto).',
        'gl_now': 'En uso ahora.', 'gl_coop': 'Coop online: los dos necesitan el mismo idioma. Los textos de '
        'los extras de este port (menú de obras, duendes, memory card) quedan en inglés en indonesio, '
        'alemán y español.',
        'update': 'Actualizar a %s', 'update_note': 'Salió el launcher %s.',
        'updating': 'Descargando el nuevo launcher...', 'update_failed': 'La actualización falló: %s',
        'portraits_get': 'Descargando los retratos redibujados... %d%%',
        'portraits_failed': 'No se pudieron descargar los retratos redibujados (%s). El juego muestra los '
                            'originales; se descargan de nuevo la próxima vez.',
        'ask_stop': 'Hay una creación en curso. ¿Detenerla y cerrar?',
        'build_note': 'La carpeta build-files guarda archivos de tu disco: es solo para este PC. Para darle el '
        'juego a un amigo, dale este launcher; él lo crea con su propio disco.',
        'how': 'Cómo conectarse', 'how_list': [
            ('1', 'Los dos necesitan esta misma versión del juego (el mismo HarvestMoonBTN.exe).'),
            ('2', 'Lo más fácil: los dos instalan Radmin VPN o ZeroTier y entran en la misma red; el anfitrión '
                  'pasa la dirección que aparece ahí (26.x.x.x).'),
            ('3', 'O el anfitrión abre el puerto TCP %s en el router y pasa la dirección de internet.'),
            ('4', 'El anfitrión pulsa Ser anfitrión y jugar primero; luego el amigo pulsa Entrar y jugar. Si '
                  'Windows pregunta por el firewall, permítelo.'),
            ('!', 'En el coop, Tab y F1-F4 están desactivados; el título de la ventana muestra la conexión.')],
    },
}

LINE = re.compile(r'^\s*([#;]?)\s*([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.*?)\s*$')


def read_ini(text):
    """key -> value; active lines win over commented examples ('#key = value')"""
    vals, active = {}, set()
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        hashed, k, v = m.groups()
        if not hashed:
            vals[k] = v
            active.add(k)
        elif k not in active:
            vals[k] = v
    return vals, active


def set_ini(text, section, key, value):
    """set (or with value '' comment out) one option, keeping everything else as it is"""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = LINE.match(line)
        if m and m.group(2) == key and not m.group(1):
            lines[i] = '%s = %s' % (key, value) if value else '#' + line.lstrip()
            return '\n'.join(lines) + '\n'
    if not value:
        return text
    for i, line in enumerate(lines):          # above the commented example, if there is one
        m = LINE.match(line)
        if m and m.group(2) == key:
            lines.insert(i, '%s = %s' % (key, value))
            return '\n'.join(lines) + '\n'
    head = '[%s]' % section
    if head not in (l.strip() for l in lines):
        lines += ['', head]
    i = [l.strip() for l in lines].index(head)
    lines.insert(i + 1, '%s = %s' % (key, value))
    return '\n'.join(lines) + '\n'


def screen_area(root):
    """(left, top, width, height) of all the monitors together"""
    if os.name == 'nt':
        try:
            import ctypes
            m = ctypes.windll.user32.GetSystemMetrics
            if m(78) > 0 and m(79) > 0:
                return m(76), m(77), m(78), m(79)     # the virtual screen
        except Exception:
            pass
    return 0, 0, root.winfo_screenwidth(), root.winfo_screenheight()


def window_spot(saved, w, h, area):
    """'WxH+X+Y' for the window: the one saved last time if it is still on a screen, else w x h centred"""
    left, top, sw, sh = area
    m = re.match(r'^(\d+)x(\d+)\+(-?\d+)\+(-?\d+)$', saved.strip())     # Tk writes "940x700+-8+0"
    if m:
        sw_, sh_, sx, sy = (int(g) for g in m.groups())
        # its title bar must be reachable: at least 120 x 40 of it inside the screens
        if sw_ >= 200 and sh_ >= 150 and sx + sw_ - 120 >= left and sx + 120 <= left + sw \
                and sy >= top - 10 and sy + 40 <= top + sh:
            return '%dx%d+%d+%d' % (min(sw_, sw), min(sh_, sh), sx, sy)
    w, h = min(w, sw), min(h, sh)
    return '%dx%d+%d+%d' % (w, h, left + (sw - w) // 2, top + max(0, (sh - h) // 2 - 20))


def place_window(root, saved, w, h, maxed):
    try:
        area = screen_area(root)
        if not saved:                            # the first time: centred on the main screen
            area = (0, 0, root.winfo_screenwidth(), root.winfo_screenheight())
        root.geometry(window_spot(saved, w, h, area))
        if maxed and os.name == 'nt':
            root.state('zoomed')
    except Exception:
        root.geometry('%dx%d' % (w, h))


def selftest():
    t = '[video]\n# 1 = smooth\nsmooth = 0\n[keyboard]\n#key_up = Up\n'
    v, act = read_ini(t)
    assert v == {'smooth': '0', 'key_up': 'Up'} and act == {'smooth'}, v
    assert set_ini(t, 'video', 'smooth', '1') == t.replace('smooth = 0', 'smooth = 1')
    assert set_ini(t, 'keyboard', 'key_up', 'W') == t.replace('#key_up', 'key_up = W\n#key_up')
    assert set_ini(t, 'video', 'smooth', '') == t.replace('smooth = 0', '#smooth = 0')
    assert set_ini(t, 'general', 'disc', 'a.cue').endswith('[general]\ndisc = a.cue\n')
    assert read_ini(set_ini(t, 'keyboard', 'key_up', 'W'))[0]['key_up'] == 'W'
    scr = (0, 0, 1920, 1080)
    assert window_spot('', 940, 700, scr) == '940x700+490+170'
    assert window_spot('1200x800+-8+-8', 940, 700, scr) == '1200x800+-8+-8'
    assert window_spot('1200x800+3000+100', 940, 700, scr) == '940x700+490+170'    # its monitor is gone
    assert cpu_cores()[0] >= 1 and language() in UI and preset_for(2) == 0 and preset_for(8) == 2
    assert all(set(TEXT[l]) == set(TEXT['en']) and set(UI[l]) == set(UI['en']) for l in UI)
    assert mix('#000000', '#ffffff', 0.5) == '#808080'
    assert sdl_key_name('a') == 'A' and sdl_key_name('KP_8') == 'Keypad 8' and sdl_key_name('KP_Up') == 'Keypad 8'
    assert sdl_key_name('Shift_R') == 'Right Shift' and sdl_key_name('F12') == 'F12' and sdl_key_name('exclam') is None
    assert add_key('X, Space', 'K', False) == 'X,Space,K' and add_key('X,Space', 'x', False) == 'X,Space'
    assert add_key('X,Space', 'Up', True) == 'Up'
    assert vtuple('v1.10.0') > vtuple('1.9.3') > vtuple('1.9') and vtuple('v2') == (2,)
    assert prepare_progress('zig            downloading https://x/zig.tar.xz', 0) == 'progress 0.02 Downloading the compiler'
    assert prepare_progress('recompiling the game code (MIPS -> C) ...', 1) == 'progress 0.2 Recompiling the game code'
    assert prepare_progress('built /x/HarvestMoonBTN', 1) == 'progress 0.8 Game compiled'
    assert prepare_progress('warning: unused variable', 1) is None
    import tempfile
    cur = os.path.join(tempfile.gettempdir(), 'hmbtn_selftest.cur')
    write_cursor(cur, [[(255, 255, 255), None]] * 29, (1, 0))
    assert os.path.getsize(cur) == 22 + 40 + 32 * 32 * 4 + 32 * 4
    os.remove(cur)
    print('selftest ok')


def cpu_cores():
    """(physical cores, logical processors)"""
    threads = os.cpu_count() or 1
    try:
        if os.name == 'nt':
            import ctypes
            from ctypes import wintypes
            k32 = ctypes.windll.kernel32
            n = wintypes.DWORD(0)
            k32.GetLogicalProcessorInformationEx(0, None, ctypes.byref(n))     # 0: RelationProcessorCore
            buf = ctypes.create_string_buffer(n.value)
            if k32.GetLogicalProcessorInformationEx(0, buf, ctypes.byref(n)):
                cores = off = 0
                while off < n.value:
                    rel, size = struct.unpack_from('<II', buf.raw, off)
                    cores += rel == 0
                    off += size
                if cores:
                    return cores, threads
        else:
            ids = set()
            phys = None
            for line in open('/proc/cpuinfo'):
                k, _, v = line.partition(':')
                if k.strip() == 'physical id':
                    phys = v.strip()
                elif k.strip() == 'core id':
                    ids.add((phys, v.strip()))
            if ids:
                return len(ids), threads
    except Exception:
        pass
    return max(1, threads // 2) if threads > 2 else threads, threads


def preset_for(cores):
    return 0 if cores <= 2 else 1 if cores <= 5 else 2


def language():
    try:
        if os.name == 'nt':
            import ctypes
            return {0x16: 'pt', 0x0A: 'es'}.get(ctypes.windll.kernel32.GetUserDefaultUILanguage() & 0x3FF, 'en')
        import locale
        lang = (locale.getlocale()[0] or os.environ.get('LANG', '')).lower()[:2]
        return lang if lang in ('pt', 'es') else 'en'
    except Exception:
        return 'en'


def game_data(disc):
    """the game data the game will use (as it looks for it), or None"""
    if disc:
        p = disc if os.path.isabs(disc) else os.path.join(HERE, disc)
        if os.path.exists(p):
            return p
    pak = os.path.join(HERE, 'HarvestMoonBTN.pak')
    if os.path.exists(pak):
        return pak
    d = os.path.join(HERE, 'disc')
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(('.cue', '.bin')):
                return os.path.join(d, f)
    return None


# looks: harvestmoonbtn.com.br, which dresses like the game's menus. Colours from its stylesheet, pictures
# from its assets (launcher-assets next to this file; without them the launcher still works, plainer).
def local_ips():
    """this PC's IPv4 addresses a friend could reach (LAN, Radmin VPN, ZeroTier...), main one first"""
    import socket
    ips = []
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(('8.8.8.8', 80))                    # sends nothing: only picks the outgoing interface
        ips.append(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        ips += socket.gethostbyname_ex(socket.gethostname())[2]
    except OSError:
        pass
    return [ip for i, ip in enumerate(ips) if ip not in ips[:i] and not ip.startswith('127.')]


def vtuple(v):
    """'v1.2.3' -> (1, 2, 3)"""
    return tuple(int(x) for x in re.findall(r'\d+', v)[:3])


def latest_release():
    """(version, download address, sha256 or None) of a newer launcher on GitHub, or None"""
    import json, urllib.request
    try:
        req = urllib.request.Request(RELEASES, headers={'User-Agent': 'HarvestMoonBTN-Launcher',
                                                        'Accept': 'application/vnd.github+json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            rel = json.load(r)
        asset = next(a for a in rel.get('assets', []) if a['name'] == LAUNCHER_EXE)
        if vtuple(rel['tag_name']) <= vtuple(VERSION):
            return None
    except Exception:                               # offline, no release yet...: no news
        return None
    digest = asset.get('digest') or ''
    return rel['tag_name'].lstrip('v'), asset['browser_download_url'], digest[7:] if digest.startswith('sha256:') else None


def self_update(url, sha):
    """put the new launcher in place of this one (Windows lets a running exe be renamed, not overwritten)"""
    import hashlib, urllib.request
    me = sys.executable
    with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'HarvestMoonBTN-Launcher'}),
                                timeout=120) as r:
        data = r.read()
    if sha and hashlib.sha256(data).hexdigest() != sha:
        raise OSError('the download is damaged (checksum)')
    with open(me + '.new', 'wb') as f:
        f.write(data)
    os.chmod(me + '.new', 0o755)           # Linux: runnable
    if os.path.exists(me + '.old'):
        os.remove(me + '.old')
    os.replace(me, me + '.old')
    os.replace(me + '.new', me)


def copy_sources():
    """the scripts this exe carries, beside the build's own files (WORK)"""
    import shutil
    if SRC != WORK:
        os.makedirs(WORK, exist_ok=True)
        for n in ('build.py', 'tools', 'runtime', 'dist-template'):
            s, d = os.path.join(SRC, n), os.path.join(WORK, n)
            shutil.copytree(s, d, dirs_exist_ok=True) if os.path.isdir(s) else shutil.copy2(s, d)


def build_commands(disc, make_pak, lang):
    """the Build page's commands (run in WORK) and their environment: the compiler, downloaded (Linux: Zig,
    unless a launcher.pyw on a PC with its own compiler), then build.py into this folder"""
    import shutil
    own_cc = os.name != 'nt' and not getattr(sys, 'frozen', False) and shutil.which('cc')
    cmds = [] if own_cc else [[sys.executable, os.path.join(WORK, 'tools', 'fetch_deps.py'), '--min']]
    cmds.append([sys.executable, os.path.join(WORK, 'build.py'), '--into', HERE] +
                (['--disc', disc] if disc else []) + (['--pak'] if make_pak else []) +
                (['--lang', lang] if lang != 'en' else []))
    return cmds, dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1', PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8')


# RecompLauncher (recomplauncher.json): "--prepare --disc=<image>" is the Build page without a window. It
# prints the build's lines, and "progress <0..1> <text>" lines for the launcher's setup screen
PREPARED = os.path.join(WORK, 'prepared.txt')   # what the last finished --prepare built (recomplauncher.json: done)
PREPARE_STEPS = [('toolchain ready', 0.1, 'Compiler ready'), ('disc image:', 0.12, 'Reading your disc'),
                 ('recompiling the game code', 0.2, 'Recompiling the game code'),
                 ('built ', 0.8, 'Game compiled'), ('building the data pack', 0.85, 'Making the data pack'),
                 ('packaged', 0.95, 'Putting the game together')]


def prepare_progress(line, cmd):
    """the progress line for a line the build printed, or None"""
    if cmd == 0 and ' downloading ' in line:
        return 'progress 0.02 Downloading the compiler'
    for start, at, text in PREPARE_STEPS:
        if line.startswith(start):
            return 'progress %g %s' % (at, text)
    return None


def prepare():
    """build the game from the disc given as --disc=<.cue or .bin> (without it: again from the files the last
    build took from the disc), with the language chosen in hmbtn.ini. 0 when the game is ready"""
    if sys.stdout:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace', line_buffering=True)
    disc = next((a[7:] for a in sys.argv if a.startswith('--disc=')), None)
    if not SRC:
        print('This launcher has no build scripts in it. Download it again from the releases page.')
        return 1
    loaded, active = read_ini(open(INI, encoding='utf-8').read() if os.path.exists(INI) else '')
    lang = loaded.get('game_lang') if 'game_lang' in active and loaded.get('game_lang') in GAME_LANGS else 'en'
    pak = os.path.join(HERE, 'HarvestMoonBTN.pak')
    if disc and not os.path.isfile(disc):
        print('Your disc image is not there any more: %s. Pick it again.' % disc)
        return 1
    if not disc and not (os.path.exists(pak) and all(os.path.exists(os.path.join(WORK, 'extract', n))
                                                      for n in GAME_FILES)):
        print('Pick your disc image (the .cue file of the USA release, SLUS-01115).')
        return 1
    # the same release, language and disc as the last finished run: nothing to do
    want = 'version = %s\nlang = %s\n' % (VERSION, lang)
    seen = 'disc = %s %d\n' % (os.path.abspath(disc), os.path.getsize(disc)) if disc else ''
    try:
        last = open(PREPARED, encoding='utf-8').read()
    except OSError:
        last = None
    if last and last.startswith(want) and (not disc or last == want + seen) and os.path.exists(EXE) \
            and os.path.exists(pak):
        print('progress 1 Already built')
        return 0
    if last is not None:
        os.remove(PREPARED)                 # a run cut short never looks finished
    copy_sources()
    # with the disc: the data pack too, so the game plays without it; without it, the pack made before stays
    cmds, env = build_commands(disc, bool(disc), lang)
    print('progress 0 Getting ready', flush=True)
    for i, cmd in enumerate(cmds):
        try:
            p = subprocess.Popen(cmd, cwd=WORK, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.STDOUT, encoding='utf-8', errors='replace',
                                 creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
        except OSError as e:
            print('The build could not start: %s' % e)
            return 1
        for line in p.stdout:
            print(line, end='')
            step = prepare_progress(line, i)
            if step:
                print(step, flush=True)
        if p.wait():
            print('The build stopped (code %d). Copy the log and report it at '
                  'https://github.com/d4rksp4rt4n/HMBTN-ReFarmed/issues' % p.returncode)
            return 1
    if not seen:                            # built again without the disc: still from the one picked before
        seen = ''.join(l + '\n' for l in (last or '').splitlines() if l.startswith('disc = '))
    with open(PREPARED, 'w', encoding='utf-8') as f:
        f.write(want + seen)
    print('progress 1 Ready to play')
    return 0


def portraits_info():
    """the pack portraits.json names (into PORTRAITS); offline or a bad file: the built-in one stays"""
    import json, urllib.request
    try:
        with urllib.request.urlopen(urllib.request.Request(PORTRAITS_INFO, headers={'User-Agent': 'HarvestMoonBTN-Launcher'}),
                                    timeout=10) as r:
            info = json.load(r)
        version, url, sha = str(info['version']).strip(), str(info['url']), str(info['sha256']).lower()
        if version and url.startswith('https://') and re.fullmatch(r'[0-9a-f]{64}', sha):
            PORTRAITS.update(version=version, url=url, sha256=sha)
    except Exception:
        pass


def portraits_ready():
    try:
        with open(PORTRAITS_MARK, encoding='utf-8') as f:
            return f.read().strip() == PORTRAITS['version']
    except OSError:
        return False


def get_portraits(progress):
    """download the portraits' zip, check it and put its images in textures/portraits (other images
    there are kept); progress(percent) along the way"""
    import hashlib, io, urllib.request, zipfile
    pack = dict(PORTRAITS)
    with urllib.request.urlopen(urllib.request.Request(pack['url'], headers={'User-Agent': 'HarvestMoonBTN-Launcher'}),
                                timeout=60) as r:
        total, data = int(r.headers.get('Content-Length') or 0), bytearray()
        while True:
            chunk = r.read(1 << 16)
            if not chunk:
                break
            data += chunk
            if total:
                progress(min(99, len(data) * 100 // total))
    if hashlib.sha256(data).hexdigest() != pack['sha256']:
        raise OSError('the download is damaged (checksum)')
    os.makedirs(PORTRAITS_DIR, exist_ok=True)
    with zipfile.ZipFile(io.BytesIO(bytes(data))) as z:
        for info in z.infolist():
            name = info.filename
            if '/' in name or '\\' in name or not name.lower().endswith('.png'):
                continue                            # only the images, right in the folder
            with open(os.path.join(PORTRAITS_DIR, name), 'wb') as f:
                f.write(z.read(info))
    with open(PORTRAITS_MARK, 'w', encoding='utf-8') as f:
        f.write(pack['version'] + '\n')
    progress(100)


def game_lang():
    """the language of the built game's text (runtime/psx_recomp.h keeps it as text in the executable)"""
    try:
        with open(EXE, 'rb') as f:
            m = re.search(rb'HMBTN-LANG ([a-z]{2})', f.read())
        return m.group(1).decode() if m else None
    except OSError:
        return None


def build_id():
    """the co-op version the game was built with (runtime/netplay.c keeps it as text in the executable)"""
    try:
        with open(EXE, 'rb') as f:
            m = re.search(rb'HMBTN-BUILD 0x([0-9a-f]{8})', f.read())
        return m.group(1).decode() if m else None
    except OSError:
        return None


ASSETS = os.path.join(HERE, 'launcher-assets')
if not os.path.isdir(ASSETS) and SRC:      # a Launcher.exe on its own: the pictures it carries
    ASSETS = os.path.join(SRC, 'dist-template', 'launcher-assets')
BG, CARD, LINE_C = '#ffffff', '#ffffff', '#e6d7b8'
# .game-box: a pale-to-golden gradient in a 4px frame, light above and left, brown right and below.
# WOOD marks the widgets that take the gradient's colour at their height.
WOOD, WOOD_TOP, WOOD_BOT, WOOD_HI = '#f4d6a3', '#f2debb', '#f6cd8b', '#ecc07c'
BOX_LIGHT, BOX_DARK = '#fcebd0', '#b38c47'
SITE_BG = '#cfdcf2'                                 # under the tiled background, until it is drawn
TEXT_C, MUTED, WARN, OK = '#212529', '#0000ff', '#b5651d', '#2e7d32'
RED, BLUE, NAV_C, SWITCH = '#ff0000', '#0000ff', '#343a40', '#3b4b95'


def mix(a, b, t):
    """the colour a fraction t of the way from a to b"""
    a, b = [int(a[i:i + 2], 16) for i in (1, 3, 5)], [int(b[i:i + 2], 16) for i in (1, 3, 5)]
    return '#%02x%02x%02x' % tuple(round(x + (y - x) * t) for x, y in zip(a, b))


def write_cursor(path, rows, hotspot):
    """a Windows .cur file from rows of (r, g, b) or None (see-through), at most 64x64"""
    n = 32 if max(len(rows), max(len(r) for r in rows)) <= 32 else 64
    rows = [list(r) + [None] * (n - len(r)) for r in rows] + [[None] * n] * (n - len(rows))
    pix, mask = b'', b''
    for row in reversed(rows):                          # bottom-up, as bitmaps are
        for c in row:
            pix += bytes([c[2], c[1], c[0], 255]) if c else b'\0\0\0\0'
        mask += int(''.join('1' if c is None else '0' for c in row), 2).to_bytes(n // 8, 'big')
    img = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, len(pix) + len(mask), 0, 0, 0, 0) + pix + mask
    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 2, 1) + struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, hotspot[0],
                                                           hotspot[1], len(img), 22) + img)


def main():
    import tkinter as tk
    from tkinter import ttk, filedialog, messagebox, font as tkfont

    text = open(INI, encoding='utf-8').read() if os.path.exists(INI) else ''
    if not text and SRC:                    # a new folder: the first save writes the commented hmbtn.ini
        text = open(os.path.join(SRC, 'dist-template', 'hmbtn.ini'), encoding='utf-8').read()
    loaded, active = read_ini(text)
    cores, threads = cpu_cores()
    # --page NAME opens that page; --live (F5 in the game): the game is running, every change is saved
    # at once and the game takes it up (the cheats), and there is no Play
    live = '--live' in sys.argv
    # --after-update: a new Launcher.exe just replaced the old one, and rebuilds the game with its sources
    rebuild = '--after-update' in sys.argv and bool(SRC) and os.path.exists(EXE)
    first = sys.argv[sys.argv.index('--page') + 1] if '--page' in sys.argv[:-1] else \
        'build' if SRC and (rebuild or not os.path.exists(EXE)) else 'start'
    if getattr(sys, 'frozen', False):
        try:
            os.remove(sys.executable + '.old')      # left by the last update
        except OSError:
            pass
    lang = loaded.get('language') if 'language' in active else None     # the flag chosen last time
    state = {'lang': lang if lang in UI else language(), 'page': first, 'dirty': False, 'note': ''}

    if os.name == 'nt':             # sharp text on scaled screens (Tk then sizes fonts for the DPI)
        try:
            import ctypes
            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        except Exception:
            pass
        try:                        # its own taskbar button, with the port's icon instead of Python's
            ctypes.windll.shell32.SetCurrentProcessExplicitAppUserModelID('HarvestMoonBTN.Launcher')
        except Exception:
            pass
    root = tk.Tk()
    if live:                                        # over the game's window
        root.attributes('-topmost', True)
    k = max(1.0, root.winfo_fpixels('1i') / 96.0)     # pixel sizes below are for 96 DPI

    def px(n):
        return int(n * k)
    zoom = 2 if k >= 1.5 else 1                       # the site's little pictures, doubled on scaled screens

    def picture(name, big=True, **kw):
        """a picture from launcher-assets (doubled with zoom if big), or None"""
        try:
            im = tk.PhotoImage(file=os.path.join(ASSETS, name), **kw)
        except Exception:
            return None
        return im.zoom(zoom) if big and zoom > 1 else im
    P = {'pointer': picture('game-pointer.png'), 'heart': picture('heart-level.png'),
         'click': picture('game-click-icon.png'), 'tile': picture('background.png', False),
         'header': picture('header-%d.png' % min((420, 520, 640), key=lambda w: abs(w - px(420))), False),
         'flag_en': picture('flag-en.png'), 'flag_es': picture('flag-es.png'), 'flag_pt': picture('flag-pt.png'),
         'dialog': [picture('dialog-end.gif', format='gif -index %d' % i) for i in (0, 1)]}

    cursor = 'hand2'               # all over the window: the game's glove, if Windows takes it
    if os.name == 'nt' and P['pointer']:
        try:
            import tempfile
            im = P['pointer']
            rows = [[None if im.transparency_get(x, y) else im.get(x, y) for x in range(im.width())]
                    for y in range(im.height())]
            tip = next((max(x for x, c in enumerate(r) if c), y) for y, r in enumerate(rows) if any(r))
            cur = os.path.join(tempfile.gettempdir(), 'hmbtn_pointer.cur')
            write_cursor(cur, rows, tip)
            root.configure(cursor='@' + cur.replace('\\', '/').replace(' ', '\\ '))
            cursor = root.cget('cursor')
        except Exception:
            pass
    root.configure(cursor=cursor)  # inherited by every widget; text fields keep their I-beam
    root.option_add('*Toplevel.cursor', cursor)
    root.option_add('*TCombobox*Listbox.cursor', cursor)
    root.title('Harvest Moon: Back to Nature')
    try:                           # the port's icon, the same one as in HarvestMoonBTN.exe
        if os.name == 'nt':
            root.iconbitmap(default=os.path.join(ASSETS, 'icon.ico'))
        else:
            root.iconphoto(True, tk.PhotoImage(file=os.path.join(ASSETS, 'icon.png')))
    except Exception:
        pass
    root.minsize(px(780), px(560))
    place_window(root, loaded.get('window', ''), px(940), px(700), loaded.get('window_max') == '1')
    root.configure(bg=BG)
    fam = 'Segoe UI' if os.name == 'nt' else 'TkDefaultFont'
    # the site's sizes: 20px titles, 16px menu, 14px text
    F = {'h1': (fam, 15), 'h2': (fam, 14, 'bold'), 'b': (fam, 10, 'bold'), 'n': (fam, 10),
         's': (fam, 10), 'nav': (fam, 12), 'navb': (fam, 12, 'bold')}

    st = ttk.Style(root)
    st.theme_use('clam')
    st.configure('TCombobox', fieldbackground='white', background=WOOD_TOP, arrowcolor=SWITCH, padding=4,
                 bordercolor=BOX_DARK)
    st.map('TCombobox', fieldbackground=[('readonly', 'white'), ('disabled', '#eee9dc')])
    st.configure('TEntry', fieldbackground='white', padding=4, bordercolor=BOX_DARK)
    st.configure('TSpinbox', fieldbackground='white', arrowcolor=SWITCH, padding=4, bordercolor=BOX_DARK)
    st.configure('Vertical.TScrollbar', background=WOOD_TOP, troughcolor=BG, bordercolor=BOX_DARK,
                 arrowcolor=BOX_DARK)
    root.option_add('*TCombobox*Listbox.font', F['n'])

    def T(k):
        return UI[state['lang']][k]

    # one variable per option, kept across rebuilds (language switch)
    vars_ = {}
    for page, section, key, kind in OPTIONS:
        # a path from a commented example ("#disc = C:\Games\...") is not a real setting
        start = loaded.get(key, '') if (key in active or kind not in ('file', 'dir')) else ''
        if kind.startswith(('choice:', 'bool:')) and not start:
            start = kind.split(':')[1].split('|')[0]     # the game's default
        vars_[key] = tk.StringVar(value=start)
    for key in RES_KEYS:
        vars_[key].set(res_label(vars_[key].get().strip(), vars_['widescreen'].get().strip()))
    if 'game_lang' not in loaded and loaded.get('game_pt') == '1':
        vars_['game_lang'].set('pt')            # the switch older launchers had for Portuguese

    def val(k):
        v = vars_[k].get().strip()
        return v.split()[0].rstrip('x') if k in RES_KEYS and v else v      # "4x  ·  1280x960" -> 4

    res_boxes = {}                               # resolution choices: relabelled by refresh

    widgets = {}                                 # key -> (set_enabled, note label)

    def refresh():
        ws, vulkan = val('widescreen'), val('renderer') == 'vulkan'
        for key, box in res_boxes.items():
            if not box.winfo_exists():
                continue
            box.configure(values=[res_label(v, ws) for v in res_values(key, ws, vulkan)])
            cur = val(key)
            if cur.isdigit() and int(cur) > 4 and not vulkan:
                cur = '4'                        # the CPU renderer goes up to 4
            if vars_[key].get() != res_label(cur, ws):
                vars_[key].set(res_label(cur, ws))
        for key, (cond, why) in REQUIRES.items():
            if key in widgets:
                ok = cond(val)
                widgets[key][0](ok)
                widgets[key][1].configure(text='' if ok else T(why))
        status.configure(text=state['note'] or (T('unsaved') if state['dirty'] else ''),
                         fg=WARN if state['dirty'] else OK)

    def changed(*_):
        state['dirty'] = True
        state['note'] = ''
        if live:
            save()
        refresh()

    # ---- widgets ----
    def raised(parent, bg=WOOD, d=4):
        """the site's .game-box: a 4px frame, light above and left, brown right and below; a WOOD one also
        gets its gradient. Returns (outer, inner)."""
        outer = tk.Frame(parent, bg=BOX_DARK)
        rim = tk.Frame(outer, bg=BOX_LIGHT)
        rim.pack(fill='both', expand=True, padx=(0, px(d)), pady=(0, px(d)))
        inner = tk.Frame(rim, bg=bg)
        inner.pack(fill='both', expand=True, padx=(px(d), 0), pady=(px(d), 0))
        if bg == WOOD:
            inner.grad = tk.Label(inner, bd=0, bg=WOOD, anchor='nw')
            inner.bind('<Configure>', lambda e: inner.after_idle(shade, inner), add='+')
            windows.append(inner)
        return outer, inner
    windows = []

    def shade_all():
        windows[:] = [w for w in windows if w.winfo_exists()]
        for w in windows:
            shade(w)

    def shade(win):
        """paint the gradient behind a window, and give each WOOD widget in it the colour at its height
        (Tk widgets can't be see-through)"""
        if not win.winfo_exists():
            return
        w, h = max(1, win.winfo_width()), max(1, win.winfo_height())
        span = max(h, px(160))           # a small box gets only the top of the gradient (labels can't blend)
        if getattr(win, 'size', None) != (w, h):
            win.size = (w, h)
            img = tk.PhotoImage(width=1, height=h)
            img.put(' '.join('{%s}' % mix(WOOD_TOP, WOOD_BOT, y / span) for y in range(h)))
            win.img = img.zoom(w, 1)
            win.grad.configure(image=win.img)
            # place() counts from inside the padding; the gradient goes under the padding too
            win.grad.place(x=-int(str(win.cget('padx'))), y=-int(str(win.cget('pady'))), width=w, height=h)
            win.grad.lower()

        def walk(parent, top):
            for c in parent.winfo_children():
                if c is win.grad or not c.winfo_exists():
                    continue
                y = top + c.winfo_y()
                if getattr(c, 'tint', None) or (c.winfo_class() in ('Label', 'Frame', 'Canvas')
                                                and str(c.cget('bg')) == WOOD):
                    c.tint = mix(WOOD_TOP, WOOD_BOT, min(1, max(0, (y + c.winfo_height() / 2) / span)))
                    c.configure(bg=c.tint)
                walk(c, y)
        walk(win, 0)

    def button(parent, label, cmd, accent=False, small=False):
        """a .game-box button; the main one bold, with the game's 'press' arrow"""
        f, inner = raised(parent, WOOD_TOP, 3)
        b = tk.Label(inner, text=label, bg=WOOD_TOP, fg=TEXT_C, cursor=cursor, font=F['b'] if accent else F['n'],
                     padx=10 if small else 16, pady=4 if small else 7)
        if accent and P['click']:
            b.configure(image=P['click'], compound='right')
        b.pack()
        b.bind('<Button-1>', lambda e: cmd())
        b.bind('<Enter>', lambda e: b.configure(bg=WOOD_HI))
        b.bind('<Leave>', lambda e: b.configure(bg=WOOD_TOP))
        return f

    def plaque(parent, label, font, bg=BG):
        """the site's .box-title: a title on a .game-box"""
        fnt = tkfont.Font(root, font=font)
        w, h, d = fnt.measure(label) + px(80), fnt.metrics('linespace') + px(20), px(4)
        c = tk.Canvas(parent, width=w, height=h, bg=bg, highlightthickness=0)
        for y in range(h):
            c.create_line(0, y, w, y, fill=mix(WOOD_TOP, WOOD_BOT, y / max(1, h - 1)))
        c.create_rectangle(0, 0, w, d, width=0, fill=BOX_LIGHT)
        c.create_rectangle(0, 0, d, h, width=0, fill=BOX_LIGHT)
        c.create_polygon(w, 0, w, h, 0, h, d, h - d, w - d, h - d, w - d, d, width=0, fill=BOX_DARK)
        c.create_text(w / 2, h / 2, text=label, font=font, fill=TEXT_C)
        return c

    def toggle(parent, var):
        """an on/off switch for a '1'/'0' variable"""
        f = tk.Frame(parent, bg=CARD)
        c = tk.Canvas(f, width=px(46), height=px(24), bg=CARD, highlightthickness=0, cursor=cursor)
        c.pack(side='left')
        lab = tk.Label(f, bg=CARD, font=F['s'], fg=TEXT_C, width=11, anchor='w')
        lab.pack(side='left', padx=(6, 0))
        en = [True]

        def draw(*_):
            on = var.get() == '1'
            c.delete('all')
            col = (SWITCH if on else '#c9bfa6') if en[0] else '#e2dccb'
            c.create_oval(px(1), px(1), px(23), px(23), fill=col, outline=col)
            c.create_oval(px(23), px(1), px(45), px(23), fill=col, outline=col)
            c.create_rectangle(px(12), px(1), px(34), px(23), fill=col, outline=col)
            x = px(24) if on else px(3)
            c.create_oval(x, px(3), x + px(18), px(21), fill='white', outline='white')
            lab.configure(text=T('on') if on else T('off'))

        def click(e):
            if en[0]:
                var.set('0' if var.get() == '1' else '1')
        c.bind('<Button-1>', click)
        tid = var.trace_add('write', draw)
        c.bind('<Destroy>', lambda e: var.trace_remove('write', tid))   # rebuilt pages (language) get new ones
        draw()

        def enable(ok):
            en[0] = ok
            c.configure(cursor=cursor)
            draw()
        return f, enable

    def flag(parent, lang, bg):
        """the flag pictures (drawn plainly if one is missing)"""
        im = P.get('flag_' + lang)
        if im:
            return tk.Label(parent, image=im, bg=bg, bd=0, cursor=cursor)
        w, h = 32 * zoom, 32 * zoom
        c = tk.Canvas(parent, width=w, height=h, highlightthickness=0, cursor=cursor, bg=bg)
        y0, y1 = 4 * zoom, 29 * zoom                      # the site's flags fill rows 4 to 28
        if lang == 'pt':                                  # Brazil: green, yellow diamond, blue globe
            c.create_rectangle(0, y0, w, y1, width=0, fill='#009c3b')
            c.create_polygon(w / 2, y0 + 2 * zoom, w - 3 * zoom, (y0 + y1) / 2, w / 2, y1 - 2 * zoom, 3 * zoom,
                             (y0 + y1) / 2, fill='#ffdf00')
            r = (y1 - y0) * 0.24
            c.create_oval(w / 2 - r, (y0 + y1) / 2 - r, w / 2 + r, (y0 + y1) / 2 + r, width=0, fill='#002776')
            c.create_rectangle(0, y0, w, y0 + 3 * zoom, width=0, fill='#40b46c')      # the site's shine on top
        else:                                             # a picture missing: its colours, plainly
            c.create_rectangle(0, y0, w, y1, width=0, fill='#c60b1e' if lang == 'es' else '#012169')
        return c

    def draw_header(c, w, h):
        """the site's banner, centred over its background"""
        c.delete('art')
        tile(c, w, h)
        if P['header']:
            c.create_image(w // 2, 0, image=P['header'], anchor='n', tags='art')
        c.coords('langs', w - px(14), px(12))

    def tile(c, w, h):
        """the site's background picture, repeated"""
        c.delete('tile')
        im = P['tile']
        if im:
            for y in range(0, h, im.height()):
                for x in range(0, w, im.width()):
                    c.create_image(x, y, image=im, anchor='nw', tags=('tile', 'art'))
        c.tag_lower('tile')

    def scroll_page(parent):
        """a page that scrolls; returns its inner frame"""
        outer = tk.Frame(parent, bg=BG)
        cv = tk.Canvas(outer, bg=BG, highlightthickness=0)
        sb = ttk.Scrollbar(outer, orient='vertical', command=cv.yview)
        inner = tk.Frame(cv, bg=BG)
        win = cv.create_window(0, 0, window=inner, anchor='nw')
        cv.configure(yscrollcommand=sb.set)
        inner.bind('<Configure>', lambda e: cv.configure(scrollregion=cv.bbox('all')))
        cv.bind('<Configure>', lambda e: cv.itemconfigure(win, width=e.width))
        cv.pack(side='left', fill='both', expand=True)
        sb.pack(side='right', fill='y')

        def wheel(e):
            if inner.winfo_height() > cv.winfo_height():
                cv.yview_scroll(int(-e.delta / 120) or (-1 if e.delta > 0 else 1), 'units')
        cv.bind('<Enter>', lambda e: root.bind_all('<MouseWheel>', wheel))
        cv.bind('<Leave>', lambda e: root.unbind_all('<MouseWheel>'))
        return outer, inner

    def card(parent, pad=16, wood=False):
        """a white box, or (wood) a wooden note like the site's"""
        if wood:
            outer, f = raised(parent, WOOD)
            f.configure(padx=pad, pady=pad - 4)
        else:
            outer = f = tk.Frame(parent, bg=CARD, highlightthickness=1, highlightbackground=LINE_C, padx=pad,
                                 pady=pad - 4)
        outer.pack(fill='x', padx=24, pady=(0, 12))
        return f

    hearts = None
    if P['heart']:                                       # .heart-level.lv3: the heart repeated three times
        hearts = tk.PhotoImage(width=P['heart'].width() * 3, height=P['heart'].height())
        hearts.tk.call(hearts, 'copy', P['heart'], '-to', 0, 0, hearts.width(), hearts.height())

    def section(parent, title, sub=''):
        tk.Label(parent, text=title, font=F['h2'], bg=BG, fg=RED, anchor='w').pack(fill='x', padx=24,
                                                                                  pady=(10, 0))
        if sub:
            tk.Label(parent, text=sub, font=F['s'], bg=BG, fg=MUTED, anchor='w', justify='left',
                     wraplength=px(700)).pack(fill='x', padx=24, pady=(0, 8))

    def page_head(parent, title, sub):
        plaque(parent, title, F['h1']).pack(pady=(18, 0))
        tk.Label(parent, text=sub, font=F['n'], bg=BG, fg=BLUE, justify='center',
                 wraplength=px(640)).pack(fill='x', padx=24, pady=(10, 14))

    catching = {}                               # the note of the row a Set/Add button is waiting for

    def catch_key(var, note, replace):
        """the next key pressed goes into var (Set: instead of what is there; Add: after it)"""
        stop_catch()
        note.configure(text=T('key_wait'))
        root.focus_set()                         # not typed into a box as well

        def got(e):
            stop_catch()
            if e.keysym == 'Escape':
                return 'break'
            name = sdl_key_name(e.keysym, e.keycode)
            if name:
                var.set(add_key(var.get(), name, replace))
            else:
                note.configure(text=T('key_bad'))
            return 'break'
        root.bind_all('<KeyPress>', got)
        catching['note'] = note

    def stop_catch():
        if catching:
            root.unbind_all('<KeyPress>')
            if catching['note'].winfo_exists():
                catching['note'].configure(text='')
            catching.clear()

    pics = {}

    def option_pictures(key):
        """(off, on, thumbnail) of an option, or None"""
        if key not in pics:
            off, on = (picture(os.path.join('options', '%s_%s.png' % (key, s)), False) for s in ('off', 'on'))
            pics[key] = (off, on, on.subsample(4)) if key in OPTION_PICTURES and off and on else None
        return pics[key]

    def show_pictures(key, title, desc):
        """the option's before and after, side by side, in a window of its own"""
        off, on, _ = option_pictures(key)
        w = tk.Toplevel(root, bg=BG, padx=px(18), pady=px(14))
        w.title(title)
        w.transient(root)
        w.resizable(False, False)
        tk.Label(w, text=title, font=F['h2'], bg=BG, fg=BLUE, anchor='w').pack(fill='x')
        if desc:
            tk.Label(w, text=desc, font=F['s'], bg=BG, fg=TEXT_C, anchor='w', justify='left',
                     wraplength=off.width() + on.width() + px(24)).pack(fill='x', pady=(2, 10))
        row = tk.Frame(w, bg=BG)
        row.pack()
        caps = OPTION_PICTURES[key] or (T('off'), T('on'))
        for i, (im, cap) in enumerate(zip((off, on), caps)):
            col = tk.Frame(row, bg=BG)
            col.pack(side='left', anchor='n', padx=(0 if i == 0 else px(12), 0))
            plaque(col, T('off') if cap == 'Off' else cap, F['b']).pack(pady=(0, 6))
            outer, f = raised(col, WOOD_TOP, 4)
            outer.pack()
            tk.Label(f, image=im, bg=WOOD_TOP, bd=0).pack()
        tk.Frame(w, bg=BG, height=px(10)).pack()
        button(w, T('pic_close'), w.destroy, small=True).pack(anchor='e')
        w.bind('<Escape>', lambda e: w.destroy())
        w.bind('<Button-1>', lambda e: w.destroy() if e.widget in (w,) else None)
        w.update_idletasks()
        x = root.winfo_rootx() + (root.winfo_width() - w.winfo_reqwidth()) // 2
        y = root.winfo_rooty() + (root.winfo_height() - w.winfo_reqheight()) // 2
        w.geometry('+%d+%d' % (max(0, x), max(0, y)))
        w.grab_set()
        w.focus_set()

    def option_row(parent, key, kind, title, desc, first):
        if not first:
            tk.Frame(parent, bg=LINE_C, height=1).pack(fill='x', pady=8)
        row = tk.Frame(parent, bg=CARD)
        row.pack(fill='x')
        row.columnconfigure(0, weight=1)
        txt = tk.Frame(row, bg=CARD)
        txt.grid(row=0, column=0, sticky='w')
        tk.Label(txt, text=title, font=F['b'], bg=CARD, fg=BLUE, anchor='w').pack(fill='x')
        if desc:
            d = tk.Label(txt, text=desc, font=F['s'], bg=CARD, fg=MUTED, anchor='w', justify='left',
                         wraplength=px(430))
            d.pack(fill='x')
            # lines break where the control beside it begins
            row.bind('<Configure>', lambda e, d=d: d.configure(
                wraplength=max(px(200), e.width - ctl.winfo_reqwidth() - px(28))))
        note = tk.Label(txt, text='', font=F['s'], bg=CARD, fg=WARN, anchor='w')
        note.pack(fill='x')
        ctl = tk.Frame(row, bg=CARD)
        ctl.grid(row=0, column=1, sticky='e', padx=(16, 0))
        shots = option_pictures(key)
        if shots:                                          # the little picture: click for before and after
            outer, f = raised(ctl, WOOD, 3)
            outer.pack(side='left', padx=(0, px(14)))
            th = tk.Label(f, image=shots[2], bg=WOOD, bd=0, cursor=cursor)
            th.pack()
            th.bind('<Button-1>', lambda e: show_pictures(key, title, desc))
            hint = tk.Label(txt, text='▸ ' + T('pic_tip'), font=F['s'], bg=CARD, fg=RED, anchor='w',
                            cursor=cursor)
            hint.pack(fill='x', before=note)
            hint.bind('<Button-1>', lambda e: show_pictures(key, title, desc))
        var = vars_[key]
        if kind.startswith('bool'):
            w, enable = toggle(ctl, var)
            w.pack()
        elif kind == 'key':
            w = ttk.Entry(ctl, textvariable=var, width=18, font=F['n'])
            w.pack(side='left')
            for label, replace in ((T('key_set'), True), (T('key_add'), False)):
                button(ctl, label, lambda var=var, note=note, replace=replace: catch_key(var, note, replace),
                       small=True).pack(side='left', padx=(6, 0))
            enable = lambda ok, w=w: w.configure(state='normal' if ok else 'disabled')
        elif kind.startswith(('choice:', 'edit:')) or kind == 'pad':
            w = ttk.Combobox(ctl, values=PAD_BUTTONS if kind == 'pad' else kind.split(':', 1)[1].split('|'),
                             width=16, font=F['n'],
                             textvariable=var, state='readonly' if kind.startswith('choice:') else 'normal')
            w.pack()
            if key in RES_KEYS:
                res_boxes[key] = w
            enable = lambda ok, w=w, ro=kind.startswith('choice:'): w.configure(
                state=('readonly' if ro else 'normal') if ok else 'disabled')
        elif kind.startswith('spin'):
            w = ttk.Spinbox(ctl, from_=1, to=int(kind[4:]), width=6, font=F['n'], textvariable=var)
            w.pack()
            enable = lambda ok, w=w: w.configure(state='normal' if ok else 'disabled')
        else:
            w = ttk.Entry(ctl, textvariable=var, width=34 if kind in ('file', 'dir') else 22, font=F['n'])
            w.pack(side='left')
            enable = lambda ok, w=w: w.configure(state='normal' if ok else 'disabled')
            if kind in ('file', 'dir'):
                def browse(kind=kind):
                    p = (filedialog.askopenfilename(filetypes=[('Game data', '*.cue *.bin *.pak'), ('*', '*')])
                         if kind == 'file' else filedialog.askdirectory())
                    if p:
                        var.set(os.path.normpath(p))
                button(ctl, T('browse'), browse, small=True).pack(side='left', padx=(6, 0))
        widgets[key] = (enable, note)

    def options_page(parent, page):
        rows = [(key, kind) for pg, sec, key, kind in OPTIONS if pg == page]
        c = card(parent)
        for i, (key, kind) in enumerate(rows):
            if page in ('keyboard', 'controller') and key[:4] in ('key_', 'pad_'):
                b = key[4:]
                dflt = loaded.get(key, '') if key not in active else ''
                option_row(c, key, kind, BUTTON_NAMES[state['lang']][b],
                           T('default') % dflt if dflt else '', i == 0)
            else:
                title, desc = TEXT[state['lang']][key]
                option_row(c, key, kind, title, desc, i == 0)

    def start_page(parent):
        page_head(parent, T('welcome'), T('welcome_sub'))
        c = card(parent, wood=True)
        data = game_data(val('disc'))
        if data and os.path.abspath(data).startswith(os.path.abspath(HERE)):
            data = os.path.relpath(data, HERE)
        first = tk.Label(c, text=('\u2714  ' + T('data_ok') % data) if data else ('\u26a0  ' + T('data_missing')),
                         font=F['n'], bg=WOOD, fg=OK if data else WARN, anchor='w', justify='left')
        first.pack(fill='x')
        last = tk.Frame(c, bg=WOOD)
        last.pack(fill='x', pady=(6, 0))
        pc = tk.Label(last, text='\U0001f5a5  ' + T('pc') % (cores, threads), font=F['n'], bg=WOOD, fg=TEXT_C,
                      anchor='w', justify='left')
        pc.pack(side='left')
        # lines break at the box's width, leaving room for the arrow
        c.bind('<Configure>', lambda e: [first.configure(wraplength=max(px(200), e.width - px(36))),
                                         pc.configure(wraplength=max(px(200), e.width - px(60)))])
        if all(P['dialog']):                              # the game's blinking "more" arrow, as on the site
            arrow = tk.Label(last, image=P['dialog'][0], bg=WOOD, bd=0)
            arrow.pack(side='right', anchor='s')

            def blink(i=0):
                if arrow.winfo_exists():
                    arrow.configure(image=P['dialog'][i])
                    arrow.after(450, blink, 1 - i)
            blink()

        section(parent, T('quick'), T('quick_sub'))
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24)
        rec = preset_for(cores)
        for i, (name, vals) in enumerate(PRESETS):
            row.columnconfigure(i, weight=1, uniform='p')
            in_use = all(val(k) == v for k, v in {**PRESET_ALL, **vals}.items())
            outer, f = raised(row, WOOD)
            outer.grid(row=0, column=i, sticky='nsew', padx=(0 if i == 0 else 8, 0))
            f.configure(padx=14, pady=12)
            if i == rec and hearts:                       # the recommended one: three of the game's hearts
                tk.Label(f, image=hearts, bg=WOOD, bd=0, anchor='w').pack(fill='x')
            r = tk.Label(f, text=T('recommended') if i == rec else ' ', font=F['s'], bg=WOOD, fg=RED, anchor='w',
                         justify='left')
            r.pack(fill='x')
            labs = [tk.Label(f, text=T(name), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                    tk.Label(f, text=T(name + '_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
            labs[0].pack(fill='x')
            labs[1].pack(fill='x', pady=(4, 10))
            # lines break at the card's width, whatever the window's
            f.bind('<Configure>', lambda e, labs=labs + [r]: [l.configure(wraplength=max(px(80), e.width - px(34)))
                                                               for l in labs])
            if in_use:
                tk.Label(f, text='\u2714 ' + T('in_use'), font=F['b'], bg=WOOD, fg=OK, anchor='w').pack(fill='x')
            else:
                def use(name=name, vals=vals):
                    for k, v in {**PRESET_ALL, **vals}.items():
                        vars_[k].set(res_label(v, val('widescreen')) if k in RES_KEYS else v)
                    save()                                # kept at once: it is the whole choice
                    state['note'] = '✔ ' + T('applied') % T(name)
                    show('start')
                    refresh()
                button(f, T('use'), use, accent=i == rec, small=True).pack(anchor='w')

        tk.Frame(parent, bg=BG, height=8).pack()
        section(parent, T('keys'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        for i, (key, what) in enumerate(T('keys_list')):
            tk.Label(c, text=key, font=F['b'], bg=CARD, fg=BLUE, anchor='w').grid(row=i, column=0, sticky='w',
                                                                                  pady=2)
            tk.Label(c, text=what, font=F['n'], bg=CARD, fg=BLUE, anchor='w').grid(row=i, column=1, sticky='w',
                                                                                    padx=(18, 0), pady=2)

    def coop_page(parent):
        page_head(parent, T('coop'), T('coop_sub'))
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24, pady=(0, 12))
        for i, (name, cmd) in enumerate((('host', coop_host), ('join', coop_join))):
            outer, f = raised(row, WOOD)
            row.columnconfigure(i, weight=1, uniform='c')
            outer.grid(row=0, column=i, sticky='nsew', padx=(0 if i == 0 else 8, 0))
            f.configure(padx=14, pady=12)
            labs = [tk.Label(f, text=T(name), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                    tk.Label(f, text=T(name + '_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
            labs[0].pack(fill='x')
            labs[1].pack(fill='x', pady=(4, 10))
            f.bind('<Configure>', lambda e, labs=labs: [l.configure(wraplength=max(px(80), e.width - px(34)))
                                                        for l in labs])
            button(f, T(name + '_btn'), cmd, accent=True, small=True).pack(anchor='w')
        ips = local_ips()
        if ips:
            tk.Label(parent, text=T('my_ip') % '   '.join(ips), font=F['b'], bg=BG, fg=BLUE, anchor='w',
                     justify='left', wraplength=px(640)).pack(fill='x', padx=24, pady=(0, 10))
        bid = build_id()
        if bid:
            tk.Label(parent, text=T('build_id') % bid, font=F['b'], bg=BG, fg=BLUE, anchor='w',
                     justify='left', wraplength=px(640)).pack(fill='x', padx=24, pady=(0, 10))
        options_page(parent, 'coop')
        section(parent, T('how'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        c.columnconfigure(1, weight=1)
        for i, (n, what) in enumerate(T('how_list')):
            tk.Label(c, text=n, font=F['b'], bg=CARD, fg=RED, anchor='nw').grid(row=i, column=0, sticky='nw', pady=2)
            w = tk.Label(c, text=what.replace('%s', val('coop_port') or COOP_PORT), font=F['n'], bg=CARD,
                         fg=BLUE, anchor='w', justify='left', wraplength=px(560))
            w.grid(row=i, column=1, sticky='w', padx=(14, 0), pady=2)

    def p2_page(parent):
        page_head(parent, T('p2'), T('p2_sub'))
        c = card(parent, wood=True)
        labs = [tk.Label(c, text=T('local'), font=F['h2'], bg=WOOD, fg=BLUE, anchor='w', justify='left'),
                tk.Label(c, text=T('local_d'), font=F['s'], bg=WOOD, fg=TEXT_C, anchor='w', justify='left')]
        labs[0].pack(fill='x')
        labs[1].pack(fill='x', pady=(4, 10))
        c.bind('<Configure>', lambda e: [l.configure(wraplength=max(px(80), e.width - px(34))) for l in labs])
        button(c, T('local_btn'), lambda: play(['--p2']), accent=True, small=True).pack(anchor='w')
        tk.Frame(parent, bg=BG, height=12).pack()
        options_page(parent, 'p2')
        section(parent, T('p2_keys'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        for k, h in enumerate(T('p2_head')):
            tk.Label(c, text=h, font=F['s'], bg=CARD, fg=MUTED, anchor='w').grid(row=0, column=k, sticky='w',
                                                                                 padx=(0 if k == 0 else 18, 0))
        for i, row in enumerate(T('p2_list')):
            for k, what in enumerate(row):
                tk.Label(c, text=what, font=F['n'] if k == 0 else F['b'], bg=CARD, fg=BLUE, anchor='w').grid(
                    row=i + 1, column=k, sticky='w', padx=(0 if k == 0 else 18, 0), pady=2)
        section(parent, T('p2_tips'))
        tk.Frame(parent, bg=BG, height=6).pack()
        c = card(parent)
        c.columnconfigure(1, weight=1)
        for i, (n, what) in enumerate(T('p2_tips_list')):
            tk.Label(c, text=n, font=F['b'], bg=CARD, fg=RED, anchor='nw').grid(row=i, column=0, sticky='nw', pady=2)
            tk.Label(c, text=what, font=F['n'], bg=CARD, fg=BLUE, anchor='w', justify='left',
                     wraplength=px(560)).grid(row=i, column=1, sticky='w', padx=(14, 0), pady=2)

    job = {'proc': None, 'log': [], 'ok': None, 'view': None}    # the build, kept across rebuilt pages

    def want_lang():
        v = val('game_lang')
        return v if v in GAME_LANGS else 'en'

    def lang_built(lang):
        built = game_lang() or 'en'
        return lang == built or (os.path.isfile(lang_exe(lang)) and
                                 (lang == 'en' or os.path.isdir(os.path.join(HERE, 'disc-' + lang))))

    lang_rows = {}

    def lang_marks():
        """the status line under each language"""
        for lang, lab in lang_rows.items():
            if not lab.winfo_exists():
                continue
            if lang == (game_lang() or 'en'):
                text, col = T('gl_now'), OK
            elif lang_built(lang):
                text, col = T('gl_ready'), OK
            else:
                text, col = T('gl_first'), MUTED
            lab.configure(text=text, fg=col)

    def gamelang_page(parent):
        page_head(parent, T('gamelang'), T('gamelang_sub'))
        c = card(parent)
        lang_rows.clear()
        for i, lang in enumerate(GAME_LANGS):
            if i:
                tk.Frame(c, bg=LINE_C, height=1).pack(fill='x', pady=8)
            r = tk.Radiobutton(c, text=T('gl_' + lang), value=lang, variable=vars_['game_lang'], font=F['b'],
                               bg=CARD, fg=BLUE, activebackground=CARD, activeforeground=RED, selectcolor=CARD,
                               anchor='w', cursor=cursor, highlightthickness=0, bd=0)
            r.pack(fill='x')
            desc = T('gl_%s_d' % lang)
            tk.Label(c, text=desc, font=F['s'], bg=CARD,
                     fg=MUTED, anchor='w', justify='left', wraplength=px(600)).pack(fill='x', padx=(px(24), 0))
            lab = tk.Label(c, text='', font=F['s'], bg=CARD, anchor='w')
            lab.pack(fill='x', padx=(px(24), 0))
            lang_rows[lang] = lab
        tk.Label(parent, text=T('gl_coop'), font=F['s'], bg=BG, fg=MUTED, anchor='w', justify='left',
                 wraplength=px(640)).pack(fill='x', padx=24)
        lang_marks()

    def build_page(parent):
        page_head(parent, T('build'), T('build_sub'))
        c = card(parent)
        option_row(c, 'disc', 'file', *TEXT[state['lang']]['disc'], True)
        option_row(c, 'build_pak', 'bool', T('pak'), T('pak_d'), False)
        lang = game_lang()
        if lang:
            tk.Label(c, text=T('built_lang') % T('lang_' + lang), font=F['s'], bg=CARD, fg=OK,
                     anchor='w').pack(fill='x', pady=(6, 0))
        row = tk.Frame(parent, bg=BG)
        row.pack(fill='x', padx=24, pady=(0, 10))
        button(row, T('build_btn'), start_build, accent=True).pack(side='left')
        note = tk.Label(row, font=F['b'], bg=BG, anchor='w', justify='left')
        note.pack(side='left', padx=14)
        c = card(parent, pad=8)
        log = tk.Text(c, height=14, font=('Consolas' if os.name == 'nt' else 'TkFixedFont', 9), bg=CARD,
                      fg=TEXT_C, bd=0, wrap='word', state='disabled')
        log.pack(fill='both', expand=True)
        tk.Label(parent, text=T('build_note'), font=F['s'], bg=BG, fg=MUTED, anchor='w', justify='left',
                 wraplength=px(640)).pack(fill='x', padx=24)
        job['view'] = (log, note)
        show_log()

    def show_log():
        if not (job['view'] and job['view'][0].winfo_exists()):
            return
        log, note = job['view']
        log.configure(state='normal')
        log.delete('1.0', 'end')
        log.insert('end', ''.join(job['log'][-400:]))
        log.see('end')
        log.configure(state='disabled')
        note.configure(text=T('building') if job['proc'] else '' if job['ok'] is None else
                       '✔ ' + T('built') if job['ok'] else '⚠ ' + T('build_failed'),
                       fg=WARN if job['ok'] is False else OK)

    def start_build():
        import queue, threading
        if job['proc']:
            return
        disc = game_data(val('disc'))
        make_pak = val('build_pak') == '1'
        if not disc or not disc.lower().endswith(('.cue', '.bin')):
            disc = None                     # a rebuild can do with the files a build took from the disc
            if make_pak and os.path.exists(os.path.join(HERE, 'HarvestMoonBTN.pak')):
                make_pak = False            # no disc image to make it from, and the pack made before stays
            if make_pak or not all(os.path.exists(os.path.join(WORK, 'extract', n)) for n in GAME_FILES):
                messagebox.showwarning('Harvest Moon', T('need_cue'))
                return
        save()
        copy_sources()
        cmds, env = build_commands(disc, make_pak, want_lang())
        lines = queue.Queue()
        job.update(log=[], ok=None, proc=True)

        def work():
            for cmd in cmds:
                try:
                    p = subprocess.Popen(cmd, cwd=WORK, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                         stderr=subprocess.STDOUT, encoding='utf-8', errors='replace',
                                         creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
                except OSError as e:
                    lines.put('%s\n' % e)
                    return lines.put(False)
                job['proc'] = p
                for line in p.stdout:
                    lines.put(line)
                if p.wait():
                    return lines.put(False)
            lines.put(True)
        threading.Thread(target=work, daemon=True).start()

        def poll():
            done = None
            while not lines.empty():
                x = lines.get()
                if isinstance(x, bool):
                    done = x
                else:
                    job['log'].append(x)
            if done is None:
                show_log()
                root.after(150, poll)
                return
            job.update(proc=None, ok=done)
            if done:                        # the game is there now: the pages that show it
                build()
                if 'play_after_build' in state:
                    return launch(state.pop('play_after_build'))
            state.pop('play_after_build', None)
            show_log()
        poll()

    # ---- window ----
    body = tk.Frame(root, bg=BG)
    body.pack(fill='both', expand=True)
    foot_o, foot = raised(root, WOOD)
    foot_o.pack(fill='x', side='bottom')
    status = tk.Label(foot, text='', font=F['b'], bg=WOOD, anchor='w')

    def save():
        nonlocal text
        if os.path.exists(INI):     # as it is now: the game's overlay (F7) may have saved to it meanwhile
            text = open(INI, encoding='utf-8').read()
        for page, section, key, kind in OPTIONS:
            v = val(key)
            if kind.startswith('spin') and v.isdigit():     # typed numbers too: keep them in range
                v = str(min(max(int(v), 1), int(kind[4:])))
                vars_[key].set(v)
            was = loaded.get(key, '') if (key in active or kind not in ('file', 'dir')) else ''
            if not was and kind.startswith(('choice:', 'bool:')):
                was = kind.split(':')[1].split('|')[0]      # the game's default: nothing to write
            if v != was:
                text = set_ini(text, section, key, v)
        with open(INI, 'w', encoding='utf-8') as f:
            f.write(text)
        loaded.update(read_ini(text)[0])
        active.update(read_ini(text)[1])
        state['dirty'] = False
        state['note'] = '\u2714 ' + T('saved')
        refresh()

    def set_lang(l):
        """switch the language and keep it for next time, at once (other unsaved changes stay unsaved)"""
        nonlocal text
        state['lang'] = l
        text = set_ini(text, 'launcher', 'language', l)
        with open(INI, 'w', encoding='utf-8') as f:
            f.write(text)
        build()

    def fetch_portraits(then=None):
        """download the redrawn portraits in the background (the progress in the status line), then then()"""
        if then:
            state['portraits_then'] = then
        if state.get('portraits_job'):
            return
        if portraits_ready():
            state.pop('portraits_then', lambda: None)()
            return
        import threading
        state['portraits_job'] = True
        prog = [0, None]                         # percent, then 'ok' or the error

        def work():
            try:
                get_portraits(lambda p: prog.__setitem__(0, p))
                prog[1] = 'ok'
            except Exception as e:
                prog[1] = e
        threading.Thread(target=work, daemon=True).start()

        def poll():
            if prog[1] is None:
                state['note'] = T('portraits_get') % prog[0]
                refresh()
                return root.after(200, poll)
            state['portraits_job'] = False
            state['note'] = ''
            refresh()
            if prog[1] != 'ok':
                messagebox.showwarning('Harvest Moon', T('portraits_failed') % prog[1])
            state.pop('portraits_then', lambda: None)()
        poll()

    # another language: the copy built before comes at once (the Build page shows which one is there)
    vars_['game_lang'].trace_add('write', lambda *_: (switch_lang(), lang_marks()))
    # switched on: the portraits come at once (and before the game starts, if that comes first)
    vars_['portraits'].trace_add('write', lambda *_: val('portraits') == '1' and fetch_portraits())

    def check_portraits():
        """read portraits.json in the background; with the option on, a newer pack comes at once"""
        import threading
        done = []

        def work():
            portraits_info()
            done.append(1)
        threading.Thread(target=work, daemon=True).start()

        def poll():
            if not done:
                return root.after(300, poll)
            if val('portraits') == '1' and not portraits_ready():
                fetch_portraits()
        poll()

    def lang_exe(lang):
        """the copy of the game in that language the build keeps (build.py keep_language)"""
        name, ext = os.path.splitext(os.path.basename(EXE))
        return os.path.join(HERE, 'languages', '%s-%s%s' % (name, lang, ext))

    def switch_lang():
        """the game in the language picked, from the copy built before; False when it must be built"""
        want = want_lang()
        if (game_lang() or 'en') == want:
            return True
        copy = lang_exe(want)
        if not os.path.isfile(copy) or (want != 'en' and not os.path.isdir(os.path.join(HERE, 'disc-' + want))):
            return False
        try:
            import shutil
            shutil.copy2(copy, EXE)
        except OSError:                     # the game is running (F5): it is switched at the next Play
            return False
        return True

    def play(args=()):
        save()
        if not os.path.exists(EXE) or not switch_lang():
            if SRC:
                state['play_after_build'] = list(args)
                show('build')
                if os.path.exists(EXE):     # another language: built now, then the game starts
                    start_build()
            else:
                messagebox.showerror('Harvest Moon', T('missing') % EXE)
            return
        if val('portraits') == '1' and not portraits_ready():
            fetch_portraits(lambda: launch(args))
            return
        launch(args)

    def launch(args):
        # the game opens this launcher again with F5: it must not inherit this one's PyInstaller
        # variables (its files are gone by then: "Failed to load Python DLL")
        env = dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1')
        subprocess.Popen([EXE, *args], cwd=HERE, env=env)
        # the launcher stays open beside the game (the co-op's settings, another start)

    def coop_port():
        p = val('coop_port')
        return p if p.isdigit() else COOP_PORT

    def coop_host():
        play(['--host', coop_port(), '--netdelay', val('coop_delay') or '3'] + (['--p2'] if val('coop_p2') == '1' else []))

    def coop_join():
        a = val('coop_address')
        if not a:
            messagebox.showwarning('Harvest Moon', T('need_addr'))
            show('coop')
            return
        if a.count(':') != 1 and not a.startswith('['):   # no port given (IPv6 needs [addr]:port)
            a = ('[%s]' % a if ':' in a else a) + ':' + coop_port()
        play(['--join', a])

    def update():
        if job['proc']:
            return
        state['note'] = T('updating')
        refresh()
        root.update()
        try:
            self_update(*state['update'][1:])
        except Exception as e:
            state['note'] = ''
            refresh()
            messagebox.showerror('Harvest Moon', T('update_failed') % e)
            return
        if state['dirty']:
            save()
        keep_window()
        subprocess.Popen([sys.executable, '--after-update'], cwd=HERE,
                         env=dict(os.environ, PYINSTALLER_RESET_ENVIRONMENT='1'))
        root.destroy()

    def check_updates():
        """ask GitHub in the background; a newer launcher gets an Update button"""
        import threading
        found = []
        threading.Thread(target=lambda: found.append(latest_release()), daemon=True).start()

        def poll():
            if not found:
                return root.after(300, poll)
            if found[0]:
                state['update'] = found[0]
                state['note'] = T('update_note') % found[0][0]
                build()
        poll()

    def close():
        if job['proc']:
            if not messagebox.askyesno('Harvest Moon', T('ask_stop')):
                return
            if job['proc'] is not True:
                job['proc'].kill()
        if state['dirty']:
            a = messagebox.askyesnocancel('Harvest Moon', T('ask_save'))
            if a is None:
                return
            if a:
                save()
        keep_window()
        root.destroy()
    root.protocol('WM_DELETE_WINDOW', close)

    def keep_window():
        """the window's place and size for next time, in [launcher] (as it is on disk: the game may have
        saved to it meanwhile)"""
        nonlocal text
        try:
            if os.path.exists(INI):
                text = open(INI, encoding='utf-8').read()
            zoomed = root.state() == 'zoomed'
            if not zoomed:                       # maximized: the size from before stays
                text = set_ini(text, 'launcher', 'window', root.geometry())
            text = set_ini(text, 'launcher', 'window_max', '1' if zoomed else '0')
            with open(INI, 'w', encoding='utf-8') as f:
                f.write(text)
        except Exception:
            pass

    def build():
        for w in body.winfo_children():
            w.destroy()
        for w in foot.winfo_children():
            if w is status:
                w.pack_forget()
            elif w is not foot.grad:
                w.destroy()
        widgets.clear()
        # header: the site's banner, the flags on a little .game-box
        hh = P['header'].height() if P['header'] else px(110)
        head = tk.Canvas(body, height=hh, highlightthickness=0, bg=SITE_BG)
        head.pack(fill='x')
        langs_o, langs = raised(head, WOOD, 3)
        langs.configure(padx=4, pady=2)
        head.create_window(0, 0, window=langs_o, anchor='ne', tags='langs')
        for l in ('en', 'es', 'pt'):
            on = l == state['lang']
            # the language in use stands on a darker square, the others brighten under the mouse
            b = tk.Frame(langs, bg=WOOD_HI if on else WOOD, padx=2)
            b.pack(side='left', padx=1)
            fl = flag(b, l, WOOD_HI if on else WOOD)
            fl.pack()
            for w in (b, fl):
                w.bind('<Button-1>', lambda e, l=l: set_lang(l))
                if not on:
                    w.bind('<Enter>', lambda e, b=b, fl=fl: [x.configure(bg='white') for x in (b, fl)])
                    w.bind('<Leave>', lambda e, b=b, fl=fl: [x.configure(bg=getattr(x, 'tint', WOOD))
                                                             for x in (b, fl)])
            if on:                                        # keeps its colour, not the gradient's
                b.tint = fl.tint = None
        head.bind('<Configure>', lambda e: draw_header(head, e.width, e.height))
        # the site's background, with the menu box and the white page on it
        main_ = tk.Canvas(body, highlightthickness=0, bg=SITE_BG)
        main_.pack(fill='both', expand=True)
        main_.bind('<Configure>', lambda e: tile(main_, e.width, e.height))
        m, sw = px(15), px(210)
        side_o, side = raised(main_, WOOD)
        side_o.place(x=m, y=m + px(8), width=sw)
        side.configure(padx=px(4), pady=px(4))
        holder = tk.Frame(main_, bg=BG)
        holder.place(x=2 * m + sw, y=m + px(8), relwidth=1, width=-(3 * m + sw), relheight=1,
                     height=-(2 * m + px(8)))
        pages, navs = {}, {}
        names = ('start', 'video', 'game', 'gamelang', 'mods', 'keyboard', 'controller', 'p2', 'coop', 'cheats')
        for name in names + (('build',) if SRC else ()):
            # a nav-link; the mouse pointer is already the game's glove
            n = tk.Label(side, text=T(name), font=F['nav'], bg=WOOD, fg=NAV_C, anchor='w', padx=px(10),
                         pady=px(6), cursor=cursor)
            n.pack(fill='x')
            n.bind('<Button-1>', lambda e, name=name: show(name))
            navs[name] = n
            outer, inner = scroll_page(holder)
            pages[name] = outer
            if name == 'start':
                start_page(inner)
            elif name == 'coop':
                coop_page(inner)
            elif name == 'p2':
                p2_page(inner)
            elif name == 'build':
                build_page(inner)
            elif name == 'gamelang':
                gamelang_page(inner)
            else:
                page_head(inner, T(name), T(name + '_sub'))
                options_page(inner, name)
            tk.Frame(inner, bg=BG, height=12).pack()
        state['pages'], state['navs'] = pages, navs
        # footer
        tk.Label(foot, text='v' + VERSION, font=F['s'], bg=WOOD, fg=MUTED).pack(side='left', padx=(16, 0))
        if not live:
            button(foot, T('play'), play, accent=True).pack(side='right', padx=(6, 16), pady=10)
            button(foot, T('save'), save).pack(side='right', padx=6, pady=10)
        button(foot, T('close'), close).pack(side='right', padx=6, pady=10)
        if state.get('update') and not live:
            button(foot, T('update') % state['update'][0], update, accent=True).pack(side='right', padx=6, pady=10)
        status.pack(side='left', padx=16)           # last: on a narrow window the text gives way, not a button
        show(state['page'])
        refresh()

    def show(name):
        stop_catch()                               # a Set/Add left waiting does not take keys on another page
        if name == 'start' and 'pages' in state:   # the presets show what is in use now
            old = state['pages'].pop('start')
            outer, inner = scroll_page(old.master)
            start_page(inner)
            state['pages']['start'] = outer
            old.destroy()
        state['page'] = name
        for n, p in state['pages'].items():
            p.pack_forget()
            state['navs'][n].configure(font=F['navb'] if n == name else F['nav'],
                                          fg=TEXT_C if n == name else NAV_C)
        state['pages'][name].pack(fill='both', expand=True)
        root.after(50, shade_all)

    for v in vars_.values():
        v.trace_add('write', changed)
    # the Build page's, kept in [launcher] (the rebuild after an update makes the pack again); never chosen:
    # on if a pack is already there
    vars_['build_pak'] = tk.StringVar(value=loaded.get('build_pak') if loaded.get('build_pak') in ('0', '1') else
                                      '1' if os.path.exists(os.path.join(HERE, 'HarvestMoonBTN.pak')) else '0')

    def keep_pak(*_):
        nonlocal text
        try:
            if os.path.exists(INI):
                text = open(INI, encoding='utf-8').read()
            text = set_ini(text, 'launcher', 'build_pak', val('build_pak'))
            with open(INI, 'w', encoding='utf-8') as f:
                f.write(text)
            loaded['build_pak'] = val('build_pak')
        except Exception:
            pass
    vars_['build_pak'].trace_add('write', keep_pak)
    build()
    if rebuild:
        start_build()
    elif getattr(sys, 'frozen', False) and not live and os.environ.get('RECOMPLAUNCHER') != '1':
        check_updates()     # only an exe can replace itself; RecompLauncher brings its updates itself
    check_portraits()
    root.mainloop()


if __name__ == '__main__':
    if '--version' in sys.argv:
        print(VERSION)
    elif '--prepare' in sys.argv:
        sys.exit(prepare())
    else:
        selftest() if '--selftest' in sys.argv else main()
