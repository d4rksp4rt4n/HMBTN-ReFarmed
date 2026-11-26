#!/usr/bin/env python3
"""Static recompiler: PS1 (MIPS R3000A) machine code -> C.

Every MIPS function becomes a C function `void f_<addr>(CPU *c)` (overlay functions are
prefixed with their module name).  Registers live in the CPU struct, memory accesses go
through the runtime's memory helpers, branches become gotos, jump tables become switch
statements and indirect calls go through the runtime dispatcher (call_addr).
"""
import struct, sys, os, json, re
from collections import defaultdict
from mips import decode, REG

GAME_LANGS = ['en', 'pt', 'id', 'de', 'es', 'ja']     # g_game_lang (runtime/psx_recomp.h)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# --extract DIR: the game files (default extract/; build.py --lang gives the translated disc's), and
# --lang the language of their text
ARGS = sys.argv[1:]
EXTRACT = os.path.abspath(ARGS[ARGS.index('--extract') + 1]) if '--extract' in ARGS else os.path.join(ROOT, 'extract')
LANG = ARGS[ARGS.index('--lang') + 1] if '--lang' in ARGS else 'en'


# ----------------------------------------------------------------------------------------
# modules
# ----------------------------------------------------------------------------------------
class Module:
    def __init__(self, name, base, data, code_lo, code_hi, ovl_id=None, file_index=None):
        self.name = name
        self.base = base
        self.data = data
        self.end = base + len(data)
        self.code_lo = code_lo
        self.code_hi = code_hi
        self.ovl_id = ovl_id
        self.file_index = file_index
        self._cache = {}
        self.strong = set()
        self.weak = set()
        self.funcs = {}          # start -> FuncInfo
        self.jtables = {}        # jr address -> list of targets
        self.notes = []

    def in_code(self, a):
        return self.code_lo <= a < self.code_hi and (a & 3) == 0

    def word(self, a):
        return struct.unpack_from('<I', self.data, a - self.base)[0]

    def insn(self, a):
        i = self._cache.get(a)
        if i is None:
            i = decode(a, self.word(a))
            self._cache[a] = i
        return i

    @property
    def prefix(self):
        return 'f' if self.ovl_id is None else 'o%s' % self.name


class FuncInfo:
    def __init__(self, start):
        self.start = start
        self.body = set()        # instruction addresses executed as normal instructions
        self.delay = set()       # addresses emitted as delay slots
        self.labels = set()
        self.calls = set()
        self.tails = set()
        self.dyn_jr = []


def load_modules():
    exe = open(os.path.join(EXTRACT, 'SLUS_011.15'), 'rb').read()
    mods = [Module('main', 0x80010000, exe[0x800:], 0x80011270, 0x80048B0C)]
    h = struct.unpack('<33I', open(os.path.join(EXTRACT, 'A_FILE.HDT'), 'rb').read())
    f = open(os.path.join(EXTRACT, 'A_FILE.BIN'), 'rb')
    ovl = json.load(open(os.path.join(ROOT, 'tools', 'overlays.json')))
    for o in ovl:
        idx = o['file']
        f.seek(h[idx])
        d = f.read(h[idx + 1] - h[idx])
        base = int(o['base'], 16)
        lo = int(o['code_lo'], 16)
        hi = int(o['code_hi'], 16)
        m = Module(o['name'], base, d, lo, hi, ovl_id=struct.unpack_from('<I', d, 0)[0], file_index=idx)
        mods.append(m)
    return mods


# ----------------------------------------------------------------------------------------
# analysis
# ----------------------------------------------------------------------------------------
def scan_constants(mod, addrs):
    """lui/addiu|ori constant pairs inside the given instruction addresses (straight-line tracking)."""
    out = set()
    hi = {}
    for a in sorted(addrs):
        i = mod.insn(a)
        if i.mn == 'lui':
            hi[i.rt] = (i.imm << 16, a)
            continue
        if i.mn in ('addiu', 'ori') and i.rs in hi and a - hi[i.rs][1] < 64:
            v = (hi[i.rs][0] + (i.imm if i.mn == 'ori' else i.simm)) & 0xFFFFFFFF
            out.add(v)
        d = i.dest()
        if d is not None:
            hi.pop(d, None)
    return out


def resolve_jump_table(mod, jr_addr, body_hint):
    """Recognise gcc jump-table dispatch ending at `jr rX`. Returns list of targets or None."""
    jr = mod.insn(jr_addr)
    X = jr.rs
    seq = []
    a = jr_addr - 4
    while a >= mod.code_lo and len(seq) < 24:
        seq.append(mod.insn(a))
        a -= 4
    # find lw X, off(B)
    lw = None
    k = 0
    for k, i in enumerate(seq):
        if i.dest() == X:
            if i.mn == 'lw':
                lw = i
            break
    if lw is None:
        return None
    B = lw.rs
    off = lw.simm
    base = None
    rest = seq[k + 1:]

    def const_of(reg, start):
        """value of reg if it is a lui(/addiu) constant, scanning rest[start:] backwards in time."""
        for n in range(start, len(rest)):
            i = rest[n]
            if i.dest() == reg:
                if i.mn == 'lui':
                    return i.imm << 16
                if i.mn == 'addiu':
                    hv = const_of(i.rs, n + 1)
                    return None if hv is None else (hv + i.simm) & 0xFFFFFFFF
                return None
        return None

    defB = None
    j = 0
    for j, i in enumerate(rest):
        if i.dest() == B:
            defB = i
            break
    if defB is None:
        return None
    if defB.mn == 'addu':
        for P in (defB.rs, defB.rt):
            cv = const_of(P, j + 1)
            if cv is not None:
                base = cv
                break
    elif defB.mn in ('lui', 'addiu'):
        base = const_of(B, j)
    if base is None:
        return None
    table = (base + off) & 0xFFFFFFFF
    # table size from sltiu on the index
    count = None
    for i in rest:
        if i.mn == 'sltiu' and i.simm > 0:
            count = i.simm
            break
    # locate table data (may be in this module or main)
    targets = []
    n = 0
    while True:
        if count is not None and n >= count:
            break
        if not (mod.base <= table + n * 4 < mod.end):
            break
        t = mod.word(table + n * 4)
        if not mod.in_code(t):
            break
        if count is None and n > 512:
            break
        targets.append(t)
        n += 1
    if not targets:
        return None
    return targets


def trace_function(mod, start, strong, all_strong_global):
    fi = FuncInfo(start)
    work = [start]
    seen = set()

    def is_other_start(t):
        return t != start and t in strong

    while work:
        a = work.pop()
        if a in seen:
            continue
        if not mod.in_code(a):
            fi.notes = getattr(fi, 'notes', []) + ['flow leaves code at %08x' % a]
            continue
        seen.add(a)
        fi.body.add(a)
        i = mod.insn(a)
        k = i.kind
        if i.has_delay:
            fi.delay.add(a + 4)
        if k == 'branch':
            t = i.target
            if mod.in_code(t) and not is_other_start(t):
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
            if i.mn.endswith('al'):
                fi.calls.add(t)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jump':
            t = i.target
            if mod.in_code(t) and not is_other_start(t) and t not in all_strong_global:
                work.append(t)
                fi.labels.add(t)
            else:
                fi.tails.add(t)
        elif k == 'call':
            fi.calls.add(i.target)
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'callr':
            nxt = a + 8
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
        elif k == 'jumpr':
            if i.rs == 31:
                pass
            else:
                tg = mod.jtables.get(a)
                if tg is None:
                    tg = resolve_jump_table(mod, a, None)
                    if tg is not None:
                        mod.jtables[a] = tg
                if tg:
                    for t in tg:
                        work.append(t)
                        fi.labels.add(t)
                else:
                    fi.dyn_jr.append(a)
        elif k == 'invalid':
            pass  # emitted as a trap; flow stops
        else:
            nxt = a + 4
            if is_other_start(nxt):
                fi.tails.add(nxt)
            else:
                work.append(nxt)
    return fi


def analyse(mods, extra):
    main = mods[0]
    # initial strong entries
    main.strong.add(0x80011278)
    for name, addrs in extra.items():
        for m in mods:
            if m.name == name:
                m.strong.update(int(x, 16) for x in addrs)

    def mod_for(m, t):
        if m.in_code(t):
            return m
        if main.in_code(t):
            return main
        return None

    # sweep starts: code following `jr ra` + delay slot (and nop padding)
    for m in mods:
        a = m.code_lo
        while a < m.code_hi - 8:
            if m.word(a) == 0x03E00008:
                b = a + 8
                while b < m.code_hi and m.word(b) == 0:
                    b += 4
                if b < m.code_hi and m.insn(b).kind != 'invalid':
                    m.weak.add(b)
            a += 4
        m.weak.add(m.code_lo)

    def promote(m, fi):
        new = 0
        for t in fi.calls:
            tm = mod_for(m, t)
            if tm is not None and t not in tm.strong:
                tm.strong.add(t)   # a jal target is always a function start
                new += 1
        for t in scan_constants(m, fi.body | fi.delay):
            own = mod_for(m, t)
            tms = [own] if own is not None else [o for o in mods if o is not m and o.in_code(t)]
            for tm in tms:
                ok = (tm.in_code(t) and tm.insn(t).kind != 'invalid') if tm is m else plausible_start(tm, t, strict=True)
                if t not in tm.strong and ok:
                    tm.strong.add(t)
                    new += 1
        return new

    rounds = 0
    while True:
        rounds += 1
        new = 0
        for m in mods:
            m.funcs = {}
            m.jtables = {}
            for s in sorted(m.strong | m.weak):
                m.funcs[s] = trace_function(m, s, m.strong, m.strong)
        for m in mods:
            for fi in list(m.funcs.values()):
                new += promote(m, fi)
        if new == 0 or rounds > 12:
            break
    # data words pointing into code (function pointer tables, callbacks) -> extra weak starts
    jt = defaultdict(set)
    for m in mods:
        for tg in m.jtables.values():
            jt[m.name].update(tg)
    extra_weak = defaultdict(set)
    for m in mods:
        for off in range(0, len(m.data) - 3, 4):
            a = m.base + off
            if m.in_code(a):
                continue
            v = m.word(a)
            for tm in ((m, main) if m is not main else (main,)):
                if tm.in_code(v) and v not in tm.funcs and v not in jt[tm.name] and plausible_start(tm, v):
                    extra_weak[tm.name].add(v)
    for m in mods:
        for w in sorted(extra_weak[m.name]):
            m.weak.add(w)
            m.funcs[w] = trace_function(m, w, m.strong, m.strong)
        m.weak -= m.strong
    return rounds


def plausible_start(m, t, strict=False):
    if not m.in_code(t):
        return False
    i = m.insn(t)
    if i.kind == 'invalid':
        return False
    if strict:
        return (i.mn == 'addiu' and i.rt == 29 and i.rs == 29 and i.simm < 0) or t == m.code_lo or             (m.base <= t - 8 and m.word(t - 8) == 0x03E00008)
    # previous instructions: jr ra / delay, or nop padding, or start of code
    p2 = t - 8
    if t == m.code_lo or t == m.base + 4:
        return True
    if m.base <= p2 and m.word(p2) == 0x03E00008:
        return True
    if m.base <= t - 4 and m.word(t - 4) == 0 and (i.mn == 'addiu' and i.rt == 29):
        return True
    if i.mn == 'addiu' and i.rt == 29 and i.rs == 29 and i.simm < 0:
        return True
    # j at end of previous function (tail jump) followed by delay
    if m.base <= p2:
        pi = m.insn(p2)
        if pi.kind in ('jump',) or (pi.kind == 'jumpr'):
            return True
    return False


# ----------------------------------------------------------------------------------------
# C emission
# ----------------------------------------------------------------------------------------
def R(n):
    return '0' if n == 0 else 'R[%d]' % n


# Game-specific hooks for PGXP on 2D sprites placed at GTE-projected points (field objects,
# characters): the object projection (8001DFD4) remembers each anchor's sub-pixel offset right
# after it stores the whole-pixel x (v1) / y (v0) to a0; a0 = 80128B88 is the player's (the
# reference for depth scaling). The sprite builders look the anchor up on entry and tag every
# word they write: 80016280 (rectangles; anchor x/y in its 7th/8th arguments at sp+24/28),
# 800165D8 (multi-part quads, e.g. villagers; 6th/7th at sp+20/24) and 80016C08 (single quads,
# e.g. the player; x in a3, y in the 5th argument at sp+16).
# The farm's field object loop (800D93C8) draws big objects at anchor (sp+48/52) + a y offset;
# at its last argument store (800D9590, x/y arguments at sp+16 / v0) the point becomes an alias.
# Widescreen: the model renderer (8001C0D4) skips every part (e.g. a piece of the ground) whose
# centre lies outside four planes; their setup (8001D348) turns the camera by the angles at
# 800491E0. The first two (read at 8001D45C / 8001D4A4) are the side planes, a little wider than
# the 4:3 view, and are opened up to the wider view. The farm's field objects are drawn for the
# tiles within 3600 units of the view's centre (800D92FC, called by the field object loop); the
# range is widened when it returns (800D9414).
# Full-screen menus (toolbox, Start menu, memory card screens) draw a scrolling tiled background:
# 800223D4 tiles the whole screen (24x24, x -12..344) through a callback (jalr at 80022498) that
# builds each tile with the sprite builder. Those tiles are tagged, so the GPU knows which picture
# is a menu, from its first frame and with or without a black screen before it, and widens it:
# g_menu_tile is the pattern's period across, 96 (four animals in turn, the next row starting one
# later, so the stripes run diagonally).
SITE_HOOKS = {0x8001E02C: '{ int k_ = R[4] == 0x80128B88u ? (p2_drawing() ? 3 : 1) : 0; '
                          'SPRITE_ANCHOR(R[3], R[2], k_); hud_fade_anchor((int16_t)R[3], (int16_t)R[2], k_); }',
              0x800D9590: 'SPRITE_ALIAS(MEM_R16(R[29] + 48u), MEM_R16(R[29] + 52u), MEM_R32(R[29] + 16u), R[2]);',
              0x8001D45C: 'R[4] = (uint32_t)gte_widescreen_angle((int32_t)R[4]);',
              0x8001D4A4: 'R[4] = (uint32_t)gte_widescreen_angle((int32_t)R[4]);',
              0x800D9414: 'FIELD_RANGE(R[29] + 24u);',
              0x80024ABC: 'g_hud_call = 1;',
              0x80024AC0: 'g_hud_call = 0;',
              0x800CA030: 'g_held_call = 1;',
              0x800CA034: 'g_held_call = 0;',
              0x800CC400: 'g_item_call = 1;',
              0x800CC404: 'g_item_call = 0;',
              0x8002249C: 'g_menu_tile = 96;',
              0x800224A0: 'g_menu_tile = 0;'}
ENTRY_HOOKS = {0x80016280: 'SPRITE_BEGIN(MEM_R16(R[29] + 24u), MEM_R16(R[29] + 28u));',
               0x800165D8: 'SPRITE_BEGIN(MEM_R16(R[29] + 20u), MEM_R16(R[29] + 24u));',
               0x80016C08: 'SPRITE_BEGIN(R[7], MEM_R16(R[29] + 16u));'}
# Widescreen HUD: the routines that build the windows (800153BC background, 8001530C border
# lines, 8001526C corner pixels), text (80019070, one glyph) and portraits (80019E80) tag what they
# write as HUD, and so does the sprite builder when the HUD icon routine (80024A4C) calls it (jal
# at 80024AB8).
HUD_BUILDERS = (0x800153BC, 0x8001530C, 0x8001526C, 0x80019070, 0x80019E80)
ENTRY_HOOKS.update({a: 'HUD_BEGIN();' for a in HUD_BUILDERS})
# What the player holds over its head: the field code's item builder (800D7144, through 800D6F78)
# is called for it by the player's held item routine (800C9F20, jal at 800CA02C) at the player's
# position plus an offset, and for item icons elsewhere (menus). An item put down or thrown is
# drawn by the routine with the jal at 800CC3FC at a point it projected: its anchor, looked up
# with the position 800D6F78 gets (x in a3, y in the 5th argument at sp+16).
ENTRY_HOOKS[0x800D6F78] = 'ITEM_BEGIN(R[7], MEM_R16(R[29] + 16u));'
ENTRY_HOOKS[0x800D7144] = 'HELD_BEGIN();'
# The stamina and tiredness under the clock (runtime/fixes.c): every window goes through 80014FAC
# (x, y, width in a1-a3, height and colours on the stack); the clock is the one at 230,18, 74x28.
# The glyph routine (80019070) is watched for the arguments it is given.
ENTRY_HOOKS[0x80014FAC] = 'stamina_window(c);'
# Random names (randomnames, runtime/names.c): the naming screen is opened by 80023130, 80023170
# and 800231AC (who called tells what is named) and reads its buttons in 80023DBC.
ENTRY_HOOKS.update({a: 'names_open(c);' for a in (0x80023130, 0x80023170, 0x800231AC)})
ENTRY_HOOKS[0x80023DBC] = 'names_buttons(c);'
ENTRY_HOOKS[0x80019070] = 'if (ja_glyph(c)) return; HUD_BEGIN(); stamina_glyph_seen(c); friends_glyph_seen(c); hud_fade(c, 36);'   # 80019070 is a HUD builder too (keep its tag)
# The clock (and the stamina) see-through while the farmer is behind them (runtime/fixes.c): the
# window builders' semi-transparency flag on the stack (background +40, border lines +28, corner
# pixels +20), set while the clock routine (GF_FARM 800FB674) builds. Its window (800136D4 through
# 80022580, back at 800225A8) gets a drawing mode first, for the blending (clock_window_done).
ENTRY_HOOKS[0x800153BC] = 'HUD_BEGIN(); hud_fade(c, 40);'
ENTRY_HOOKS[0x8001530C] = 'HUD_BEGIN(); hud_fade(c, 28);'
ENTRY_HOOKS[0x8001526C] = 'HUD_BEGIN(); hud_fade(c, 20);'
ENTRY_HOOKS[0x800136D4] = 'clock_window(c);'
SITE_HOOKS[0x800225A8] = 'clock_window_done(c);'
# The Japanese disc's font (build.py --lang ja, runtime/japanese.c): loaded in place of the USA one, the
# glyphs given their 12x12 cells and the text 12 pixels a glyph
ENTRY_HOOKS[0x80017300] = 'if (ja_unpack(c)) return;'
SITE_HOOKS[0x80018FC0] = 'R[2] += (uint32_t)ja_advance();'
# ... and its numbers (8001A9xx writes a number's digits as glyphs: 0x40 + digit, 0x69 the minus; the
# Japanese font has them at 187 and 199)
SITE_HOOKS[0x8001AA04] = 'if (ja_advance()) R[9] = 199;'
SITE_HOOKS[0x80018EE4] = 'ja_macro(R[2]);'                     # ... and in the texts put in by number
SITE_HOOKS[0x8001AA58] = 'if (ja_advance()) R[2] += 187 - 64;'
# Save states (runtime/savestate.c): the top of the game's frame loop (80011C80, in 80011C24, which the
# main loop 80011370 calls for each scene and which never returns itself): the game between two frames,
# the native calls under it always the same. In the delay slot of its first jal, before the call.
SITE_HOOKS[0x80011C84] = 'savestate_point(c);'
TAG_STORES_IN = {0x80016280, 0x800165D8, 0x80016C08, 0x800D7144, *HUD_BUILDERS}
# Functions whose cost stays at the console's speed under the overclock (CYC): the libetc
# VSync wait (80032CFC) times out after (n-1) << 15 turns of its loop, so a faster loop would
# end the wait early and the game, which runs as fast as VSync(2) lets it, would speed up.
UNSCALED_CYCLES = {0x80032CFC}
# 80022794 animates the message/help window (6 steps of width, then N of height, one per
# game tick; counter pointer and N in its 6th/7th arguments at sp+24/28): quick_window
# (runtime/quickui.c) can move the counter to the end so it opens or closes at once. The
# caller (ra) tells the one window that must not be hurried (see quick_window).
ENTRY_HOOKS[0x80022794] = 'quick_window(R[31], MEM_R32(R[29] + 24u), MEM_R32(R[29] + 28u));'
# The screen fades start with their length in game ticks in a0: 800132B8 from black,
# 800132F0 to black, 80013328 to black for doors (gp+32 = kind, gp+344/346 = tick/length).
# The music fades in/out with them (8001FE00/8001FE34, same length in a0) and the game waits
# for both, so they are shortened alike.
for _a in (0x800132B8, 0x800132F0, 0x80013328, 0x8001FE00, 0x8001FE34):
    ENTRY_HOOKS[_a] = 'R[4] = quick_fade(R[4]);'
# The text printer (80018B98) shows one more character each time its timer runs out and
# stores the count at gp+464 (80018C94), then walks the text shown; where the walk ends
# (80018FF4) s0 is the next character and sp+112 is set in the call that walks an inserted
# name. quick_text_walked types a few more plain characters at once (textspeed).
SITE_HOOKS[0x80018C94] = 'quick_text();'
SITE_HOOKS[0x80018FF4] = 'quick_text_walked(R[16], MEM_R32(R[29] + 112u));'
# 80017AE8 ends the printer's wait for the button; besides the button, the end of a scroll of
# the box calls it, and that must not end a wait (quick_text_wait_kept).
ENTRY_HOOKS[0x80017AE8] = 'if (quick_text_wait_kept(R[31])) return;'
# libsnd's sequencer tick (8002A6E4, SsSeqCalledTbyT) is not run while fastload runs a loading
# pause at full speed, so the music does not run on unheard (quick_music_held).
ENTRY_HOOKS[0x8002A6E4] = 'if (quick_music_held()) return;'
# the CD loads of the frame (8002194C, after the farm's frame has built what it draws): not in the
# farm's frame run once more for farmer 2's window, which can't use the CD (runtime/p2view.c)
ENTRY_HOOKS[0x8002194C] = 'if (g_view2_run) return;'
# the sound effects (8001FABC: the sound in a0, keyed on by 800212A8 at once): online, the other
# farmer's menu sounds are not heard here (runtime/p2.c, spu.c)
ENTRY_HOOKS[0x8001FABC] = 'g_sfx_quiet = p2_sfx_quiet();'
SITE_HOOKS[0x8001FAD8] = 'g_sfx_quiet = 0;'
# the 2D drawing's small rooms, just set up by 8001231C / 80012598: 24000 bytes instead of 12000
# (runtime/fixes.c)
SITE_HOOKS[0x8001254C] = 'small_draw_room();'
SITE_HOOKS[0x800127C8] = 'small_draw_room();'
# the new game's birthday screen: the four seasons' icons (a1 at 80023504) and names (a2 at
# 80023530) spread over the box when a translated name is too long for 64 pixels (runtime/fixes.c)
SITE_HOOKS[0x80023504] = 'R[5] = season_icon_x(c, R[17], R[5]);'
SITE_HOOKS[0x80023530] = 'R[6] = season_name_x(R[17], R[6]);'

# Hooks in one overlay only (overlays share load addresses). saveload: the three save/load
# screens ask "Select a MEMORY CARD slot" through the menu at 80136990 (0/1 = slot, -1 =
# cancel, anything else = still choosing); the hook right after each call can answer for the
# player (runtime/memcard.c; second argument 1 on the screens that read a diary). Each screen's opening (the delay slot of its jal 801372A0, state
# byte 80137584 = 0) starts a new visit.
_MAP_DRAWS = (0x8011E72C, 0x8011E7C4, 0x8011E86C, 0x8011E97C, 0x8011ED54)
MODULE_SITE_HOOKS = {
    # The farm/town control copies the pad state for the player each tick: held buttons to
    # 801231F4, newly pressed to 801231F8 (800C41C0 / its jal's delay slot 800C41C8);
    # quick_bag_filter drops R1 / R2 there when there is nothing to switch to.
    # the farm's tick (800C162C) ends at 800C1874: a second farmer's menu turn ends (runtime/p2.c)
    # the X action's target (800C7AC0: v1 = its kind - 1, a jump table of 49): farmer 2 does not
    # open the whole game's things (runtime/p2.c)
    # the summary page's farm score (800FB908): v0 is the sum just before it is stored at
    # 800FBDF0; the Power Berries and the Mystic Berry are added (runtime/fixes.c)
    'GF_FARM': {0x800C41C8: 'quick_bag_filter();', 0x800C1874: 'p2_tick_end();',
                0x800FBDEC: 'R[2] = farm_score_berries(R[2]);',
                # an area's things to look at are in place (800BCF54, after its jal at 800BD0B0):
                # the first house's pile of boxes opens the cabinet (runtime/fixes.c)
                0x800BD0B8: 'first_house_cabinet();',
                # walking through crops: the player's walk's collision mask (800CB114, after the
                # branches that join at 800CB1CC) and the field's answer for a crop (runtime/fixes.c)
                0x800CB1CC: 'R[10] = crop_walk_mask(c, R[10], R[18], R[16], R[29]);',
                0x800BE818: 'R[2] = crop_walk_solid(c, R[2], R[17], R[3], R[9]);',
                # a Harvest Sprite at work heads straight for its bed: the walk of the characters
                # (800D12B8) has just picked a way (800D196C, back at 800D1744: v0 the way, s2 the
                # walker, s3 the ways free, s5 the speed; runtime/sprites.c)
                0x800D1744: 'R[2] = sprite_walk_way(c, R[2]);',
                # the Harvest Sprites' spots at home (80110E34 copies one to the sprite in a0): the
                # spot's z, loaded at 80110E78, for the yellow one moves it off the chair (runtime/fixes.c)
                0x80110E78: 'R[6] = sprite_spot_z(R[6], R[4]);',
                # bulk food: the shelf's buy question (8011C298): its yes / no (s0 after the jal at
                # 8011C468) and the payment (a0 = minus the price, 8011C514) (runtime/fixes.c)
                0x8011C470: 'R[16] = bulk_food_yes(c, R[16]);',
                0x8011C514: 'R[4] = bulk_food_pay(c, R[4]);',
                0x8011BC54: 'R[4] = bulk_feed_most(c, R[4]);',   # fodder / chicken feed: how many (a0 = 99)
                # a storm night (800DE254 for weather 3 / 4) breaks the greenhouse (800C05F8) when
                # a roll of 100 is under 31 (v0 at 800C0624) (runtime/fixes.c)
                0x800C0624: 'R[2] = greenhouse_storm(R[2]);',
                # the bigger greenhouse (runtime/greenhouse.c): a field's address past the greenhouse's
                # 7 rows is one of the new rows (where each is worked out), its rows test in 8011E090,
                # the night's rows (800D7B8C, the height at sp+24), the floor paint once loaded
                # (8011E45C's end) and the model (the area loader, back from 8001BFA8)
                0x800BF684: 'R[4] = gh_addr(R[4]);', 0x800BF7E8: 'R[2] = gh_addr(R[2]);',
                0x800C5688: 'R[16] = gh_addr(R[16]);', 0x800CB804: 'R[3] = gh_addr(R[3]);',
                0x800CC794: 'R[5] = gh_addr(R[5]);', 0x800E8050: 'R[4] = gh_addr(R[4]);',
                0x800E81A0: 'R[16] = gh_addr(R[16]);',
                0x800BE7CC: 'R[8] = gh_addr(R[8]);',   # (800BE6E4: the greenhouse's as the farm's + 9000)
                0x8011E178: 'R[3] = gh_row_ok(R[6], R[3]);', 0x800D7BE8: 'gh_night_rows(c);',
                0x8011E558: 'gh_floor_paint(c);', 0x800DC070: 'gh_model(c);',
                # ... the greenhouse's things drawn (800D9F1C: rows to 800DA358, the field's base for the
                # loads at +30956), a field drawn by lot (800DAE1C, 49 of them) and the sprites' (80110658,
                # 801106BC; their field at 8010D440)
                0x800D9FB4: 'R[3] = gh_addr(R[3] + 30956u) - 30956u; R[2] = gh_addr(R[2] + 30956u) - 30956u;',
                0x800DA358: 'R[2] = gh_row_ok(R[17], R[2]);',
                0x800DAE1C: 'R[20] = gh_fields(R[20]);', 0x800DAE4C: 'R[4] = gh_addr(R[4]);',
                0x801106BC: 'R[20] = gh_fields(R[20]);', 0x80110738: 'R[4] = gh_addr(R[4]);',
                0x8010D440: 'R[4] = gh_addr(R[4]);',
                0x800FB664: 'g_clock_draw = 0;',     # back from the clock routine (jal at 800FB65C)
                0x800C7B70: 'R[3] = p2_x_target(R[3]);',
                # the map screen (SELECT) draws the map picture (320x240 in quads: the map and a
                # row of cloud strips under it) with the jal at 8011ED54 (8011ECC4), and while it
                # fades in and out with those at 8011E72C, 8011E7C4, 8011E86C, 8011E97C: tagged
                # with no background to repeat but white sides (-2), so the screen is wide
                # (runtime/gpu.c, edge_wide)
                # (set in each jal's delay slot, cleared where it returns, or in the delay slot of
                # the jal there: a hook on a jal itself isn't emitted)
                **{a + 4: 'g_menu_tile = -2;' for a in _MAP_DRAWS},
                **{a + 12 if a != 0x8011ED54 else a + 8: 'g_menu_tile = 0;' for a in _MAP_DRAWS}},
    'saveload': {0x8012EF28: 'R[3] = memcard_slot_menu(R[3], 0);',
                 0x80130678: 'R[3] = memcard_slot_menu(R[3], 1);',
                 0x80131654: 'R[3] = memcard_slot_menu(R[3], 1);',
                 0x8012E350: 'memcard_slot_visit();',
                 0x8012E5A4: 'memcard_slot_visit();',
                 0x8012E84C: 'memcard_slot_visit();',
                 # the diary lists (menu 80136B18: 0-2 = diary, -1 = cancel, 3+ = still
                 # choosing; copied to a0 / a1 right after the call): L1/R1 change card
                 0x8012F33C: 'R[4] = memcard_diary_menu(R[4], 0);',
                 0x80130B40: 'R[5] = memcard_diary_menu(R[5], 1);',
                 0x80131AE0: 'R[5] = memcard_diary_menu(R[5], 1);'},
    # the title screen's background (800B944C, set at its entry below; one return, delay slot
    # 800B9988) is a 320x240 meadow, drawn whole at 0,0 and twice more as scrolling see-through
    # 3x3 grids of it: tagged like the menus' tiles (period 320), so it is widened too. The
    # Natsume logo before it (800B9324, delay slot of its return 800B9364) is one sprite over a
    # white screen clear: tagged with no background to repeat (-1), so the screen is shown wide
    # with the logo keeping its shape
    'GF_TITLE': {0x800B9988: 'g_menu_tile = 0;', 0x800B9364: 'g_menu_tile = 0;'},
}
# ... and at their entry (per overlay too): with autoslot the slot menu is not drawn at all
MODULE_ENTRY_HOOKS = {'saveload': {0x80136990: 'if (memcard_slot_auto()) { R[2] = 2; return; }',
                                   # the bigger greenhouse's rows in the card's file (runtime/greenhouse.c)
                                   # ... and the building picked at the carpenter's (runtime/buildorder.c)
                                   0x80134C48: 'gh_card_save(); build_card_save();',
                                   0x80135380: 'gh_card_load(); build_card_load();'},
                      'GF_TITLE': {0x800B944C: 'g_menu_tile = 320;', 0x800B9324: 'g_menu_tile = -1;'},
                      # the player's drawing routine (a0 = its position): a second farmer is drawn
                      # by it first (runtime/p2.c)
                      # and its turn is taken once more by a second farmer after the player's;
                      # the faint (tiredness 100) is the whole game's, not the second farmer's
                      # the farm's frame (tick, drawing, HUD) is run once more first for the
                      # second farmer's window (runtime/p2view.c)
                      'GF_FARM': {0x800CA24C: 'p2_draw(c);', 0x800DB5CC: 'p2_tick(c);', 0x800C162C: 'cheats_tick(); p2_tick_begin();',
                                  0x800C14A0: 'machine_farm_frame(c); p2view_frame(c);',
                                  0x800DE3A0: 'if (p2_faint_skipped()) return;',
                                  # farmer 2 in the second machine stays on the farm (runtime/p2.c)
                                  0x800CD9A0: 'if (p2_way_out(c)) return;',
                                  0x800CC0F8: 'if (p2_thrown(c)) return;',
                                  # a thrown item's flight (800CC7CC, a0 = the item): falling near a
                                  # shipping bin, it goes in (runtime/fixes.c)
                                  0x800CC7CC: 'if (ship_reach(c)) { R[2] = 1; return; }',
                                  # the area's events under the farmer (800BDCE8): the X doors' boxes
                                  # wider for the player's look (runtime/fixes.c)
                                  0x800BDCE8: 'if (door_reach(c)) return;',
                                  # trading between the farmers in the storage menus (runtime/p2.c)
                                  0x800F7D34: 'if (p2_trade_move(c)) return;',
                                  0x800D57A4: 'if (p2_trade_kind(c)) return;',
                                  # what bumps into the player (animals) bumps into the second farmer too
                                  0x800BE4A8: 'if (p2_bump(c)) return;',
                                  # an item's picture: not the area's copy when its halves are on two
                                  # pictures (wrong colours, the hothouse eggplant) (runtime/fixes.c)
                                  0x800D688C: 'if (item_picture_from_disc(c)) { R[2] = 0; return; }',
                                  # the clock: what it builds goes to the right edge in widescreen and
                                  # turns see-through over the farmer (runtime/fixes.c)
                                  0x800FB674: 'clock_begin(c);',
                                  # bulk food: the shelf's buy question's new states (runtime/fixes.c)
                                  0x8011C298: 'if (bulk_food_state(c)) { R[2] = 0; return; }',
                                  # the bigger greenhouse: a field (grid, x, y), the fields emptied and
                                  # where people may walk in it (the rectangles of 800BE5A0)
                                  0x800DAF60: 'if (gh_field_at(c)) return;', 0x800D7B24: 'gh_clear();',
                                  0x800BE5A0: 'gh_walk_area();'}}
# The Harvest Sprites hired through the Chef (spritehire, runtime/sprites.c; a site hook runs after
# its instruction): the talk's checks (8010DA04: +184 talked to once at 8010DB00, friendship >= 50
# at 8010DB14, spring's sixth talk at 8010DC38), the hire menu (8010E6E8; the days question's answer after its jal at 8010E928, the
# days stored at 8010E988) and the texts: written over once loaded (80022A94, after 80022FE0) and
# the window drawn each tick (80022B98)
MODULE_SITE_HOOKS['GF_FARM'].update({
    0x8010DB00: 'R[2] = sprite_talked(c, R[2]);',
    0x8010DB14: 'R[2] = sprite_friendship(c, R[2]);',
    0x8010DC38: 'R[2] = sprite_spring(R[2]);',
    0x8010E930: 'R[3] = R[2] = sprite_days_answer(c, R[3]);',
    0x8010E98C: 'sprite_hired(c);',
    # a sprite under contract asks about another job: the talk's text (a1 loaded at 8010DC98)
    0x8010DC98: 'sprite_talk_text(c);'})
# The town's friendships page (friendpage, runtime/friends.c; site hooks run after their
# instruction): the START menu (800FC1AC) draws its title with a1 = the farm's name (800FC220), the
# page after / before (800FE928: v0 = page < 8 at 800FE9E0; 800FEA00: the page below 0 at 800FEAB0)
# and the page's routine loaded from 80127308 (v1 at 800FC2D4)
MODULE_SITE_HOOKS['GF_FARM'].update({
    0x800FC220: 'R[5] = friend_title(c, R[5]);', 0x800FC23C: 'friend_title_drawn();',
    0x800FE9E0: 'R[2] = friend_next_wrap(R[2]);',
    0x800FEAB0: 'R[2] = friend_prev_wrap(R[2]);',
    0x800FC2D4: 'R[3] = friend_page(c, R[3]);'})
# Birthdays on the calendar (birthdays, runtime/friends.c): the hearts after the calendar's cursor
# is drawn (800FA4F4 called from 800FA1B0), the names when its day's events are shown (8002261C
# called from 800FA4C0) and once that text is loaded (80022A94, below)
MODULE_SITE_HOOKS['GF_FARM'][0x800FA1B8] = 'birthday_marks(c);'
# Widescreen: what is drawn above a character, at its projected point raised by a few pixels, a
# point never projected itself, takes the anchor below it (g_above_call): a character's balloon
# ("!", "...", the heart; 80107E2C, called by the people's, animals' and player's routines with
# y - 20 ... y - 50, building it with the jal at 80107EF0) and the hand pointing at the player
# hidden behind a wall ("Here!", the player's routine 800CA24C, jal at 800CA340, y - 50).
MODULE_SITE_HOOKS['GF_FARM'].update({0x80107EF4: 'g_above_call = 1;', 0x80107EF8: 'g_above_call = 0;',
                                     0x800CA344: 'g_above_call = 1;', 0x800CA348: 'g_above_call = 0;'})
ENTRY_HOOKS[0x8002261C] = 'birthday_text_asked(c);'
MODULE_ENTRY_HOOKS['GF_FARM'][0x8010E6E8] = 'sprite_menu(c);'
MODULE_ENTRY_HOOKS['GF_FARM'][0x8010CB6C] = 'sprite_night(c);'   # each sprite's night
MODULE_ENTRY_HOOKS['GF_FARM'][0x8010CD80] = 'sprite_update(c);'  # each sprite's frame
MODULE_ENTRY_HOOKS['GF_FARM'][0x801105C8] = 'sprite_pick_from(c);'  # a sprite picks its next bed ...
MODULE_ENTRY_HOOKS['GF_FARM'][0x80110658] = 'if (sprite_pick_bed(c)) return;'  # ... the nearest
MODULE_ENTRY_HOOKS['GF_FARM'][0x801103E8] = 'sprite_face_bed(c);'  # a sprite waters the bed ahead ...
MODULE_ENTRY_HOOKS['GF_FARM'][0x8011031C] = 'sprite_face_bed(c);'  # ... or harvests it: facing it
SITE_HOOKS[0x80022A94] = 'sprite_text_loaded(); build_text_loaded(); birthday_text_loaded();'
ENTRY_HOOKS[0x80022B98] = 'sprite_text_tick(c);'
SITE_HOOKS[0x80022CDC] = 'sprite_menu_answered(c, R[4]); build_menu_answered(R[4]);'   # a menu's answer, just stored at gp+836
# The carpenter's building order (buildorder, runtime/buildorder.c; site hooks run after their
# instruction): the offer's next state stored (801182EC), the building paid for (801185E4) and the
# night it is done (8011205C: v1 = s0 - 58); its menu's text and answer with the sprites' above
MODULE_SITE_HOOKS['GF_FARM'].update({
    0x801182EC: 'build_offer(c);',
    0x801185E4: 'build_started(c);',
    0x80117F90: 'R[3] = R[2] = build_next(R[2]);',   # the shop opens: the first not built
    0x80118040: 'R[3] = build_label(R[20]) << 2;',   # ... and the extension's name
    0x8011205C: 'R[3] = build_night(c, R[3]);'})
# ... and the farm outside as built (farmmix, runtime/farmmix.c): the area loader (800BCF54) keeps
# part 0's size (800BCF88: a2, the biggest of the season's farms) and has part 0 in its buffer
# (800BCFAC, before its first bytes are copied)
MODULE_SITE_HOOKS['GF_FARM'].update({
    0x800BCF88: 'R[6] = farm_area_size(R[16], R[6]);',
    0x800BCFAC: 'farm_area_loaded(R[16]);'})
# The bell herds the animals (bellherd, runtime/bell.c; site hooks run after their instruction):
# the cow's / sheep's roll for hearing it (800E169C / 800DFAC4), the goal it takes (800E16E4 /
# 800DFB0C), the walk's "near the goal" (800D1D90) and the door it went through (800D2CCC / 800D2D0C)
MODULE_SITE_HOOKS['GF_FARM'].update({
    0x800E169C: 'R[2] = bell_heard(c, R[2]);', 0x800DFAC4: 'R[2] = bell_heard(c, R[2]);',
    0x800E16E4: 'bell_sent(c);', 0x800DFB0C: 'bell_sent(c);',
    0x800D1D90: 'R[2] = bell_keep_going(c, R[2]);',
    0x800D2CCC: 'bell_through(c);', 0x800D2D0C: 'bell_through(c);'})
# The jukebox (F7, runtime/jukebox.c; a site hook runs after its instruction): the farm's music
# just picked by 800BCB5C (v0, back at 800BC888 in 800BC868)
MODULE_SITE_HOOKS['GF_FARM'][0x800BC88C] = 'R[2] = jukebox_song(R[2]);'
# The clock's AM / PM (A 0, P 15, M 12 in the USA font) and colon (102) in the Japanese font's capitals (runtime/japanese.c)
MODULE_SITE_HOOKS['GF_FARM'][0x800FB7B0] = 'if (ja_advance()) R[2] = 161 + 15;'
MODULE_SITE_HOOKS['GF_FARM'][0x800FB7BC] = 'if (ja_advance()) MEM_W16(R[22] - 24568u, 161);'
MODULE_SITE_HOOKS['GF_FARM'][0x800FB7C8] = 'if (ja_advance()) R[2] = 161 + 12;'
MODULE_SITE_HOOKS['GF_FARM'][0x800FB8B4] = 'if (ja_advance()) R[2] = 206;'           # ... and its colon
MODULE_SITE_HOOKS['GF_FARM'][0x800FB840] = 'ja_clock(R[22] - 24568u);'              # the time in 24 hours
MODULE_SITE_HOOKS['GF_FARM'][0x800FB8AC] = 'if (ja_advance()) R[6] = 256;'           # the colon's x
# Marks over the animals (animalmarks, runtime/marks.c; site hooks run after their instruction):
# after each animal's sprite (800CFE40 / 800D0198 with s0 the record), the first instruction past
# the call's delay slot
MODULE_SITE_HOOKS['GF_FARM'].update({a: 'animal_marks(c);' for a in (
    0x800D2EEC, 0x800DE840, 0x800E0418, 0x800E1F38, 0x80108C94, 0x8010B188)})
# The letter over the mailbox (mailmark, runtime/marks.c): after the farmer is drawn (800C9E90 on
# foot, 800CA580 on the horse)
MODULE_SITE_HOOKS['GF_FARM'].update({a: 'mail_mark(c);' for a in (0x800CA4CC, 0x800CA5C4)})
# ... and the texts those screens show (delay slot of each jal 8001AC10, the message box)
MODULE_TEXT_HOOKS = {'saveload': (0x8001AC10, 'R[6] = memcard_text(R[6]);')}


def with_hooks(i, s, fn_start, mod):
    if i.addr in SITE_HOOKS:
        s += ' ' + SITE_HOOKS[i.addr]
    hook = MODULE_SITE_HOOKS.get(mod.name, {}).get(i.addr)
    if hook:
        s += ' ' + hook
    if fn_start in TAG_STORES_IN and i.mn in ('sb', 'sh', 'sw'):
        s += ' SPRITE_TAG(%s);' % addr_expr(i)
    return s


def addr_expr(i):
    if i.rs == 0:
        return '0x%08Xu' % (i.simm & 0xFFFFFFFF)
    if i.simm == 0:
        return 'R[%d]' % i.rs
    return '(R[%d] + %du)' % (i.rs, i.simm & 0xFFFFFFFF) if i.simm > 0 else '(R[%d] - %du)' % (i.rs, -i.simm)


def emit_simple(i, mod):
    """C for a non-control-flow instruction (string, may be empty)."""
    m = i.mn
    rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
    if i.word == 0:
        return ''

    def W(r, expr):
        if r == 0:
            return ''
        return 'R[%d] = %s;' % (r, expr)
    if m == 'sll':
        return W(rd, '%s << %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'srl':
        return W(rd, '%s >> %d' % (R(rt), sa)) if sa else W(rd, R(rt))
    if m == 'sra':
        return W(rd, '(uint32_t)((int32_t)%s >> %d)' % (R(rt), sa))
    if m == 'sllv':
        return W(rd, '%s << (%s & 31)' % (R(rt), R(rs)))
    if m == 'srlv':
        return W(rd, '%s >> (%s & 31)' % (R(rt), R(rs)))
    if m == 'srav':
        return W(rd, '(uint32_t)((int32_t)%s >> (%s & 31))' % (R(rt), R(rs)))
    if m == 'mfhi':
        return W(rd, 'c->hi')
    if m == 'mflo':
        return W(rd, 'c->lo')
    if m == 'mthi':
        return 'c->hi = %s;' % R(rs)
    if m == 'mtlo':
        return 'c->lo = %s;' % R(rs)
    if m == 'mult':
        return '{ int64_t m_ = (int64_t)(int32_t)%s * (int64_t)(int32_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)((uint64_t)m_ >> 32); }' % (R(rs), R(rt))
    if m == 'multu':
        return '{ uint64_t m_ = (uint64_t)%s * (uint64_t)%s; c->lo = (uint32_t)m_; c->hi = (uint32_t)(m_ >> 32); }' % (R(rs), R(rt))
    if m == 'div':
        return 'psx_div(c, %s, %s);' % (R(rs), R(rt))
    if m == 'divu':
        return 'psx_divu(c, %s, %s);' % (R(rs), R(rt))
    if m in ('add', 'addu'):
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s + %s' % (R(rs), R(rt)))
    if m in ('sub', 'subu'):
        return W(rd, '%s - %s' % (R(rs), R(rt)))
    if m == 'and':
        return W(rd, '%s & %s' % (R(rs), R(rt)))
    if m == 'or':
        if rt == 0:
            return W(rd, R(rs))
        if rs == 0:
            return W(rd, R(rt))
        return W(rd, '%s | %s' % (R(rs), R(rt)))
    if m == 'xor':
        return W(rd, '%s ^ %s' % (R(rs), R(rt)))
    if m == 'nor':
        return W(rd, '~(%s | %s)' % (R(rs), R(rt)))
    if m == 'slt':
        return W(rd, '(int32_t)%s < (int32_t)%s' % (R(rs), R(rt)))
    if m == 'sltu':
        return W(rd, '%s < %s' % (R(rs), R(rt)))
    if m in ('addi', 'addiu'):
        if rs == 0:
            return W(rt, '0x%Xu' % (i.simm & 0xFFFFFFFF))
        if i.simm == 0:
            return W(rt, R(rs))
        if i.simm > 0:
            return W(rt, '%s + %du' % (R(rs), i.simm))
        return W(rt, '%s - %du' % (R(rs), -i.simm))
    if m == 'slti':
        return W(rt, '(int32_t)%s < %d' % (R(rs), i.simm))
    if m == 'sltiu':
        return W(rt, '%s < 0x%Xu' % (R(rs), i.simm & 0xFFFFFFFF))
    if m == 'andi':
        return W(rt, '%s & 0x%Xu' % (R(rs), i.imm))
    if m == 'ori':
        return W(rt, '%s | 0x%Xu' % (R(rs), i.imm)) if rs else W(rt, '0x%Xu' % i.imm)
    if m == 'xori':
        return W(rt, '%s ^ 0x%Xu' % (R(rs), i.imm))
    if m == 'lui':
        return W(rt, '0x%08Xu' % (i.imm << 16))
    ae = addr_expr(i)
    if m == 'lb':
        return W(rt, '(uint32_t)(int32_t)(int8_t)MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lbu':
        return W(rt, 'MEM_R8(%s)' % ae) or 'MEM_R8(%s);' % ae
    if m == 'lh':
        return W(rt, '(uint32_t)(int32_t)(int16_t)MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lhu':
        return W(rt, 'MEM_R16(%s)' % ae) or 'MEM_R16(%s);' % ae
    if m == 'lw':
        return W(rt, 'MEM_R32(%s)' % ae) or 'MEM_R32(%s);' % ae
    if m == 'lwl':
        return W(rt, 'psx_lwl(%s, %s)' % (ae, R(rt)))
    if m == 'lwr':
        return W(rt, 'psx_lwr(%s, %s)' % (ae, R(rt)))
    if m == 'sb':
        return 'MEM_W8(%s, %s);' % (ae, R(rt))
    if m == 'sh':
        return 'MEM_W16(%s, %s);' % (ae, R(rt))
    if m == 'sw':
        return 'MEM_W32(%s, %s);' % (ae, R(rt))
    if m == 'swl':
        return 'psx_swl(%s, %s);' % (ae, R(rt))
    if m == 'swr':
        return 'psx_swr(%s, %s);' % (ae, R(rt))
    if m == 'lwc2':
        return 'gte_write_data(%d, MEM_R32(%s));' % (rt, ae)
    if m == 'swc2':
        if 12 <= rt <= 15:      # screen XY: also tagged for PGXP
            return 'PGXP_SWC2(%s, %d);' % (ae, rt)
        return 'MEM_W32(%s, gte_read_data(%d));' % (ae, rt)
    if m == 'mfc0':
        return W(rt, 'cop0_read(c, %d)' % rd) or 'cop0_read(c, %d);' % rd
    if m == 'mtc0':
        return 'cop0_write(c, %d, %s);' % (rd, R(rt))
    if m == 'rfe':
        return 'cop0_rfe(c);'
    if m == 'mfc2':
        return W(rt, 'gte_read_data(%d)' % rd) or 'gte_read_data(%d);' % rd
    if m == 'cfc2':
        return W(rt, 'gte_read_ctrl(%d)' % rd) or 'gte_read_ctrl(%d);' % rd
    if m == 'mtc2':
        return 'gte_write_data(%d, %s);' % (rd, R(rt))
    if m == 'ctc2':
        return 'gte_write_ctrl(%d, %s);' % (rd, R(rt))
    if m == 'gte_cmd':
        return 'gte_command(0x%07Xu);' % (i.word & 0x1FFFFFF)
    if m == 'syscall':
        return 'psx_syscall(c, 0x%08Xu);' % i.addr
    if m == 'break':
        return 'psx_break(c, 0x%08Xu, %du);' % (i.addr, (i.word >> 6) & 0xFFFFF)
    if m.startswith('cop0_'):
        return '/* %s */' % m
    if m == 'invalid':
        return 'psx_invalid(c, 0x%08Xu, 0x%08Xu); return;' % (i.addr, i.word)
    raise ValueError('unhandled %s at %08x' % (m, i.addr))


GTE_CYCLES = {0x01: 15, 0x06: 8, 0x0C: 6, 0x10: 8, 0x11: 8, 0x12: 8, 0x13: 19, 0x14: 13, 0x16: 44, 0x1B: 17,
              0x1C: 11, 0x1E: 14, 0x20: 30, 0x28: 5, 0x29: 8, 0x2A: 17, 0x2D: 5, 0x2E: 6, 0x30: 23, 0x3D: 5,
              0x3E: 5, 0x3F: 39}


def insn_cycles(i):
    """Approximate R3000A cost: 1 cycle per instruction, RAM loads stall ~4 cycles,
    multiply/divide results are read right away by compiled code (full latency)."""
    if i.kind == 'load':
        return 5
    m = i.mn
    if m in ('mult', 'multu'):
        return 7
    if m in ('div', 'divu'):
        return 36
    if m == 'gte_cmd':
        return 1 + GTE_CYCLES.get(i.word & 0x3F, 8)
    return 1


class Emitter:
    def __init__(self, mods):
        self.mods = mods
        self.main = mods[0]

    def fname(self, mod, a):
        return '%s_%08X' % (mod.prefix, a)

    def call_expr(self, mod, t):
        """C statement calling address t (direct when statically known)."""
        if t in (0xA0, 0xB0, 0xC0):
            return 'call_addr(c, 0x%Xu);' % t
        if mod.in_code(t) and t in mod.funcs:
            return '%s(c);' % self.fname(mod, t)
        if mod is not self.main and self.main.in_code(t) and t in self.main.funcs:
            return '%s(c);' % self.fname(self.main, t)
        return 'call_addr(c, 0x%08Xu);' % t

    def emit_function(self, mod, fi):
        out = []
        P = out.append
        name = self.fname(mod, fi.start)
        P('void %s(CPU *c) {' % name)
        P('  FUNC_ENTER(0x%08Xu);' % fi.start)
        if fi.start in ENTRY_HOOKS:
            P('  ' + ENTRY_HOOKS[fi.start])
        if fi.start in MODULE_ENTRY_HOOKS.get(mod.name, {}):
            P('  ' + MODULE_ENTRY_HOOKS[mod.name][fi.start])
        order = sorted(fi.body)
        if order[0] != fi.start:
            P('  goto L_%08X;' % fi.start)
            fi.labels.add(fi.start)
        labels = set(fi.labels)
        # any address that is a fallthrough target of a non-contiguous emission gets a label
        emitted_next = {}
        for k, a in enumerate(order):
            emitted_next[a] = order[k + 1] if k + 1 < len(order) else None
        # first pass to know which addresses need labels (gotos for discontinuities)
        need = set(labels)
        for a in order:
            i = mod.insn(a)
            if i.has_delay:
                ft = a + 8
            else:
                ft = a + 4
            if i.kind in ('jump', 'jumpr') or i.kind == 'invalid':
                continue
            if ft in fi.body and emitted_next.get(a) != ft:
                need.add(ft)
            if i.has_delay and (a + 4) in fi.body and emitted_next.get(a) == a + 4:
                # standalone copy of the delay slot is emitted next; fall through must skip it
                need.add(ft)
        cyc = 0
        cost = '%d' if fi.start in UNSCALED_CYCLES else 'CYC(%d)'

        def flush():
            nonlocal cyc
            if cyc:
                P('  c->cycles += %s;' % (cost % cyc))
                cyc = 0

        for a in order:
            i = mod.insn(a)
            if a in need:
                flush()
                P('L_%08X:' % a)
            body_c = None
            if not i.has_delay:
                cyc += insn_cycles(i)
                if i.mn == 'syscall':
                    flush()
                s = with_hooks(i, emit_simple(i, mod), fi.start, mod).strip()   # hooks on a nop too
                if s:
                    P('  ' + s + '  /* %08x: %s */' % (a, i))
                ft = a + 4
                if i.kind == 'invalid':
                    continue
                if ft not in fi.body or emitted_next.get(a) != ft:
                    flush()
                    if ft in fi.body:
                        P('  goto L_%08X;' % ft)
                    else:
                        P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
                continue
            # control transfer with delay slot
            d = mod.insn(a + 4) if mod.in_code(a + 4) else None
            cyc += 1 + (insn_cycles(d) if d is not None else 1)
            ds = with_hooks(d, emit_simple(d, mod), fi.start, mod).strip() if d is not None and not d.has_delay else ''
            if ds:
                th = MODULE_TEXT_HOOKS.get(mod.name)
                if th and i.mn == 'jal' and i.target == th[0]:
                    ds += ' ' + th[1]
            if d is not None and d.has_delay:
                ds = '/* branch in delay slot ignored */'
            dcomment = '  /* %08x: %s */' % (a + 4, d) if d is not None else ''
            P('  /* %08x: %s */' % (a, i))
            k = i.kind
            m = i.mn
            ft = a + 8
            flush()
            if k == 'branch':
                cond = {
                    'beq': '%s == %s' % (R(i.rs), R(i.rt)),
                    'bne': '%s != %s' % (R(i.rs), R(i.rt)),
                    'blez': '(int32_t)%s <= 0' % R(i.rs),
                    'bgtz': '(int32_t)%s > 0' % R(i.rs),
                    'bltz': '(int32_t)%s < 0' % R(i.rs),
                    'bgez': '(int32_t)%s >= 0' % R(i.rs),
                    'bltzal': '(int32_t)%s < 0' % R(i.rs),
                    'bgezal': '(int32_t)%s >= 0' % R(i.rs),
                }[m]
                always = (m == 'beq' and i.rs == i.rt) or (m == 'bgez' and i.rs == 0)
                link = m.endswith('al')
                t = i.target
                P('  {')
                if not always:
                    P('    int br_ = %s;' % cond)
                if link:
                    P('    R[31] = 0x%08Xu;' % (a + 8))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                guard = '' if always else 'if (br_) '
                if link:
                    P('    %s{ %s }' % (guard, self.call_expr(mod, t)))
                elif t in fi.body and t in fi.labels:
                    back = t <= a
                    if back:
                        P('    %s{ EVCHK(); goto L_%08X; }' % (guard, t))
                    else:
                        P('    %sgoto L_%08X;' % (guard, t))
                else:
                    P('    %s{ %s return; }' % (guard, self.call_expr(mod, t)))
                P('  }')
                if always and not link:
                    continue
            elif k == 'jump':
                t = i.target
                if ds:
                    P('  ' + ds + dcomment)
                if t in fi.body and t in fi.labels:
                    if t <= a:
                        P('  EVCHK(); goto L_%08X;' % t)
                    else:
                        P('  goto L_%08X;' % t)
                else:
                    P('  %s return;' % self.call_expr(mod, t))
                continue
            elif k == 'call':
                t = i.target
                P('  R[31] = 0x%08Xu;' % (a + 8))
                if ds:
                    P('  ' + ds + dcomment)
                P('  ' + self.call_expr(mod, t))
            elif k == 'callr':
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if i.rd:
                    P('    R[%d] = 0x%08Xu;' % (i.rd, a + 8))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                P('    call_addr(c, t_); }')
            elif k == 'jumpr':
                if i.rs == 31:
                    if ds:
                        P('  ' + ds + dcomment)
                    P('  RETURN_CHECK(0x%08Xu); return;' % a)
                    continue
                P('  { uint32_t t_ = %s;' % R(i.rs))
                if ds:
                    P('    ' + ds + dcomment)
                P('')
                tg = mod.jtables.get(a)
                if tg:
                    P('    switch (t_) {')
                    for t in sorted(set(tg)):
                        P('      case 0x%08Xu: goto L_%08X;' % (t, t))
                    P('      default: jumptable_miss(c, 0x%08Xu, t_); return;' % a)
                    P('    }')
                else:
                    P('    call_addr(c, t_); return;')
                P('  }')
                continue
            # fallthrough after conditional branch / call
            if ft in fi.body:
                if emitted_next.get(a) != ft or (a + 4) in fi.body:
                    P('  goto L_%08X;' % ft)
            else:
                P('  ' + self.call_expr(mod, ft) + ' return;  /* fallthrough */')
        P('}')
        return '\n'.join(out)


HEADER = '''/* generated by tools/recomp.py - do not edit */
#include "psx_recomp.h"
#define R (c->r)
'''


def generate(mods, outdir, per_file=60):
    em = Emitter(mods)
    os.makedirs(outdir, exist_ok=True)
    for f in os.listdir(outdir):
        if f.endswith('.c') or f.endswith('.h'):
            os.remove(os.path.join(outdir, f))
    decls = []
    tables = []
    files = []
    for m in mods:
        starts = sorted(m.funcs)
        for s in starts:
            decls.append('void %s(CPU *c);' % em.fname(m, s))
        chunk = []
        n = 0
        for k, s in enumerate(starts):
            chunk.append(em.emit_function(m, m.funcs[s]))
            if len(chunk) >= per_file or k == len(starts) - 1:
                fn = '%s_%03d.c' % (m.name, n)
                with open(os.path.join(outdir, fn), 'w') as fh:
                    fh.write(HEADER + '#include "funcs.h"\n\n' + '\n\n'.join(chunk) + '\n')
                files.append(fn)
                chunk = []
                n += 1
    with open(os.path.join(outdir, 'funcs.h'), 'w') as fh:
        fh.write('#pragma once\n' + '\n'.join(decls) + '\n')
    # module tables
    t = [HEADER, '#include "funcs.h"\n']
    for m in mods:
        t.append('static const FuncEntry tab_%s[] = {' % m.name)
        for s in sorted(m.funcs):
            t.append('  {0x%08Xu, %s, 0x%08Xu, 0x%08Xu},' % (s, em.fname(m, s), m.word(s), m.word(s + 4)))
        t.append('};')
    t.append('const ModuleDesc g_modules[] = {')
    for m in mods:
        ovl = 0xFFFFFFFF if m.ovl_id is None else m.ovl_id
        t.append('  {"%s", 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, 0x%08Xu, %d, tab_%s, %d},' % (
            m.name, m.base, m.end, m.code_lo, m.code_hi, ovl,
            -1 if m.file_index is None else m.file_index, m.name, len(m.funcs)))
    t.append('};')
    t.append('const int g_num_modules = %d;' % len(mods))
    # the language of the game's text, and as text for the launcher (which game its Build page made)
    t.append('const int g_game_lang = %d;' % GAME_LANGS.index(LANG))
    t.append('const char g_game_lang_tag[] = "HMBTN-LANG %s";' % LANG)
    with open(os.path.join(outdir, 'modules.c'), 'w') as fh:
        fh.write('\n'.join(t) + '\n')
    files.append('modules.c')
    return files


def main():
    mods = load_modules()
    extra = {}
    xp = os.path.join(ROOT, 'tools', 'extra_entries.json')
    if os.path.exists(xp):
        extra = json.load(open(xp))
    rounds = analyse(mods, extra)
    report = []
    for m in mods:
        nb = sum(len(f.body) for f in m.funcs.values())
        dyn = sum(len(f.dyn_jr) for f in m.funcs.values())
        report.append('%-9s funcs=%5d strong=%5d weak=%4d insns=%7d jtables=%4d dyn_jr=%d' % (
            m.name, len(m.funcs), len(m.strong), len(m.weak), nb, len(m.jtables), dyn))
    print('analysis rounds:', rounds)
    print('\n'.join(report))
    # dynamic jr listing for review
    with open(os.path.join(ROOT, 'build', 'dyn_jr.txt'), 'w') as fh:
        for m in mods:
            for f in sorted(m.funcs.values(), key=lambda f: f.start):
                for a in f.dyn_jr:
                    fh.write('%s %08x in %08x: %s\n' % (m.name, a, f.start, m.insn(a)))
    files = generate(mods, os.path.join(ROOT, 'generated'))
    print('generated %d files' % len(files))


if __name__ == '__main__':
    main()


def coverage_report(mods):
    for m in mods:
        cov = set()
        for f in m.funcs.values():
            cov |= f.body
            cov |= f.delay
        gaps = []
        a = m.code_lo
        start = None
        while a < m.code_hi:
            if a not in cov and m.word(a) != 0:
                if start is None:
                    start = a
            else:
                if start is not None:
                    gaps.append((start, a))
                    start = None
            a += 4
        if start is not None:
            gaps.append((start, m.code_hi))
        tot = sum((e - s) // 4 for s, e in gaps)
        print('%-9s uncovered nonzero words: %d in %d gaps' % (m.name, tot, len(gaps)))
        for s, e in gaps[:400]:
            if (e - s) >= 8:
                print('    %08x-%08x (%d)' % (s, e, (e - s) // 4))
