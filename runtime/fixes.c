/* Fixes to the original game. Hooks emitted by tools/recomp.py. */
#include "psx.h"
#include "psx_recomp.h"
#include "text_pt.h"
#include <string.h>

/* The farm score on the summary page (the "%" next to the name, 800FB908) adds up to 100:
 * buildings 10, recipes 8, the hearts of wife, child, dog and horse 10 each, chickens 10, cows
 * and sheep 20, Power Berries 10 and the Mystic Berry 2. The page counts the Power Berries (its
 * berry row) but never adds them, nor the Mystic Berry, so a complete farm stops at 88. The sum
 * is in v0 just before it is stored (800FBDF0); this adds them. The score is worked out each
 * time the page opens, so old saves read right at once. */
#define POWER_BERRIES 0x800712BCu   /* halfword: Power Berries eaten (10 in all) */
#define BERRY_FLAGS 0x80071A6Cu     /* word: 0x200 = the Mystic Berry eaten */

uint32_t farm_score_berries(uint32_t score) {
    int16_t power = (int16_t)MEM_R16(POWER_BERRIES);
    if (power > 10) power = 10;
    if (power > 0) score += (uint32_t)power;
    if (MEM_R32(BERRY_FLAGS) & 0x200u) score += 2;
    return score;
}

/* The cabinet in the first house: the pile of boxes there is a thing X looks at (kind 16) that does
 * nothing; it opens the cabinet (kind 23). An area's things to look at are
 * copied to 8009F78C when it loads (800BCF54; the pointer to them is stored at 800BD0B4): its map
 * number, then from +16 up to 50 of 44 bytes, the kind at +2. Maps 80-83 are the first house. */
#define AREA_THINGS 0x8009F78Cu
#define FIRST_HOUSE 80u
#define NOTHING_KIND 16
#define CABINET_KIND 23

void first_house_cabinet(void) {
    if (!g_cfg.earlycabinet || MEM_R32(AREA_THINGS + 4) - FIRST_HOUSE >= 4) return;
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t kind = AREA_THINGS + 16 + i * 44 + 2;
        if (MEM_R16(kind) == NOTHING_KIND) MEM_W16(kind, CABINET_KIND);
    }
}

/* Walking through crops: the player's walk asks for collisions at 800CB114 (a2 in s2: 56 when it
 * looks ahead, 0 for the step itself, 24 for what X faces; the mask in t2) and the farm's field
 * answers at 800BE6E4 (the query in s1, its mask at +20; the point tested made a field, x / y at
 * sp+32 / sp+34, by 8011E090 in the greenhouse (s0 = 1) or 800D7EF8): a field thing is solid
 * when its picture says so (v0 after 800BE818, with the item's record in v1 and its class byte,
 * +3, in t1). Here the walk's mask gets 0x100000 and a planted crop (record flags 4040h, a class)
 * is not solid then, nor a weed (field 64: record flags 8000h, +14 the item it gives, 91h; the
 * records are 40 bytes from 8004B1A4, stones 58 and branches 62 give 92h / 93h and stay solid).
 * What X faces is tested close to the player: standing on a crop, the crop under the player was
 * what X faced, and a Harvest Sprite or the shipping basket beside could not be reached. That
 * test gets 0x200000: a crop or weed in the player's own field is not solid for it (the one in
 * front still is, to pick it). */
#define CROP_WALK 0x100000u
#define CROP_FACE 0x200000u
#define CROP_FLAGS 0x4040u
#define WEED_FLAGS 0x8000u
#define WEED_ITEM 0x91u
#define PLAYER_POS 0x80071B00u     /* the player's x, y, z (words) */

/* Running through the big animals: the walk's mask (above) has 0x2 / 0x4 for the cows and the sheep
 * (the 20 records of 184 bytes from 800749F4: 352-355 and 356-358; a hit gives 0x10000 and the
 * player pushes them), 0x100 for the horse on the farm (800759E4, on its area only) and 0x200 for
 * the horse in its stable (80075AD4, record 350). While the player runs they are left out: the
 * player runs through them, and walking still pushes. Running = the run button held (bit 0x20 of
 * the buttons the walk reads, 801231F4: the second farmer's own in its turn) or the run state (+3C
 * = 4; walking is 2): pushing an animal puts the player back in the walking state with the button
 * still held. Asked for every farmer's walk (the second farmer's struct is the player's in its turn). */
#define ANIMAL_BITS 0x306u
#define PLAYER_STATE 0x3Cu
#define STATE_RUN 4
#define WALK_BUTTONS 0x801231F4u
#define RUN_BUTTON 0x20u

static uint32_t run_through_mask(uint32_t mask, uint32_t player) {
    if (!g_cfg.runthrough) return mask;
    if ((int16_t)MEM_R16(player + PLAYER_STATE) != STATE_RUN && !(MEM_R32(WALK_BUTTONS) & RUN_BUTTON)) return mask;
    return mask & ~ANIMAL_BITS;
}



/* What X faces is tested 128 ahead of the player (800CB114, its point at sp+32 / sp+40), and a field
 * is about 400 across: standing on the fields, as cropwalk lets the player do, the point is mostly
 * in the player's own field, so the crop in front was only reached from the last 128 of the field
 * (the player had to line up). On the farm and in the greenhouse, when the point is in the player's
 * own field: if the next field that way has a ripe crop or a weed the point goes into it, else a
 * ripe crop under the player is what X faces. Rows grow towards -z. */
#define FIELD_RECORDS 0x8004B1A4u
#define FARM_GRID 0x80071DFCu
#define GH_GRID 0x80074124u
#define AREA_KIND 0x800A0466u
#define MAP_NOW 0x800A046Cu
uint32_t gh_addr(uint32_t a);

static uint32_t game_call(CPU *c, uint32_t at, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3);
static int face_own_ripe;   /* this X test: the crop under the player is ripe and counts */

/* the field (col, row) under x, z as the game works it out (800D7EF8 the farm, 8011E090 the
 * greenhouse: the farm's divides by 399, so a field is not quite 400 there); 0 out of the grid */
static int field_at(CPU *c, uint32_t sp, int gh, int32_t x, int32_t z, int *col, int *row) {
    uint32_t p = sp - 0x40u;
    MEM_W32(p, (uint32_t)x);
    MEM_W32(p + 4u, 0);
    MEM_W32(p + 8u, (uint32_t)z);
    if (game_call(c, gh ? 0x8011E090u : 0x800D7EF8u, p, p + 16u, p + 18u, 0)) return 0;
    *col = (int16_t)MEM_R16(p + 16u);
    *row = (int16_t)MEM_R16(p + 18u);
    return 1;
}

static uint32_t field_addr(int gh, int col, int row) {
    return gh ? gh_addr(GH_GRID + (uint32_t)((row * 7 + col) * 8)) : FARM_GRID + (uint32_t)((row * 45 + col) * 8);
}

/* a crop ready to pick (80110790, the sprites' test) or a weed */
static int ripe_in(CPU *c, int gh, int col, int row) {
    uint32_t at = field_addr(gh, col, row);
    uint32_t rec = FIELD_RECORDS + (uint32_t)MEM_R16(at) * 40u;
    if (MEM_R16(rec) == WEED_FLAGS && MEM_R16(rec + 14) == WEED_ITEM) return 1;
    if (MEM_R16(rec) != CROP_FLAGS || (int8_t)MEM_R8(rec + 3) == -1) return 0;
    return game_call(c, 0x80110790u, at, 0, 0, 0) != 0;
}

static void crop_face_point(CPU *c, uint32_t sp, uint32_t player) {
    face_own_ripe = 0;
    uint16_t kind = MEM_R16(AREA_KIND);
    int gh = kind == 6;
    if (!gh && kind != 1 && MEM_R32(MAP_NOW) - 20u > 3u) return;
    int32_t x = (int32_t)MEM_R32(player + 256), z = (int32_t)MEM_R32(player + 264);
    int32_t px = (int32_t)MEM_R32(sp + 32), pz = (int32_t)MEM_R32(sp + 40);
    int col, row, pc, pr;
    if (!field_at(c, sp, gh, x, z, &col, &row) || !field_at(c, sp, gh, px, pz, &pc, &pr)) return;
    if (pc != col || pr != row) return;                    /* the point is past the field already */
    int32_t dx = px - x, dz = pz - z;
    if (dx == 0 && dz == 0) return;
    int32_t sx = dx > 0 ? 8 : dx < 0 ? -8 : 0, sz = dz > 0 ? 8 : dz < 0 ? -8 : 0;
    if ((dx < 0 ? -dx : dx) >= (dz < 0 ? -dz : dz)) sz = 0; else sx = 0;
    for (int k = 0; k < 60; k++) {                         /* on to the next field that way */
        px += sx;
        pz += sz;
        int nc, nr;
        if (!field_at(c, sp, gh, px, pz, &nc, &nr)) break;
        if (nc == col && nr == row) continue;
        if (ripe_in(c, gh, nc, nr)) {
            MEM_W32(sp + 32, (uint32_t)(px + sx));
            MEM_W32(sp + 40, (uint32_t)(pz + sz));
            return;
        }
        break;
    }
    face_own_ripe = ripe_in(c, gh, col, row);              /* else the ripe one under the player */
}

uint32_t crop_walk_mask(CPU *c, uint32_t mask, uint32_t why, uint32_t player, uint32_t sp) {
    if (why == 56 || why == 0) mask = run_through_mask(mask, player);
    if (!g_cfg.cropwalk) return mask;
    if (why == 56 || why == 0) return mask | CROP_WALK;
    if (why == 24) { crop_face_point(c, sp, player); return mask | CROP_FACE; }
    return mask;
}

/* the player's own field, made as the point tested was (0 out of the grid) */
static int own_field(CPU *c) {
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    uint32_t p = regs[29] - 0x40u;             /* below the field's frame, above the callee's */
    for (int k = 0; k < 3; k++) MEM_W32(p + 4u * k, MEM_R32(PLAYER_POS + 4u * k));
    c->r[29] = regs[29] - 0x80u;
    c->r[4] = p;
    c->r[5] = p + 16u;
    c->r[6] = p + 18u;
    call_addr(c, regs[16] ? 0x8011E090u : 0x800D7EF8u);
    int out = c->r[2] != 0;
    int same = !out && MEM_R16(p + 16u) == MEM_R16(regs[29] + 32u) && MEM_R16(p + 18u) == MEM_R16(regs[29] + 34u);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
    return same;
}

uint32_t crop_walk_solid(CPU *c, uint32_t solid, uint32_t query, uint32_t record, uint32_t cls) {
    uint32_t mask = MEM_R32(query + 20);
    if (!solid || !(mask & (CROP_WALK | CROP_FACE))) return solid;
    int crop = ((int8_t)cls != -1 && MEM_R16(record) == CROP_FLAGS) ||
               (MEM_R16(record) == WEED_FLAGS && MEM_R16(record + 14) == WEED_ITEM);
    if (!crop) return solid;
    if (mask & CROP_WALK) return 0;
    return own_field(c) && !face_own_ripe ? 0 : solid;
}

/* Shipping bins that take what falls near them (shipreach): a thrown item (52 bytes, a0 of its
 * flight 800CC7CC: +2 the item, +12 its fall speed, +36/+40/+44 x, y, z) goes in a bin only when it
 * lands on the bin's own floor polygons (the floor under a point, 800CE550, keeps its polygon at
 * gp+688; bit 0x80 of its first byte = a bin, 800BF3D0), and one that meets the bin's side bounces
 * off it (800CC878). Thrown from in front of the hen house's bin the egg mostly fell outside. While
 * the item falls to about the bin's top, the floor is looked at around it too (up to SHIP_REACH
 * away; over the bin itself the game's own landing is left to do it); a bin there takes it, as
 * the game does (800CCAA8: 800F3BCC ships it, sound 19, 800CCBE8 ends the throw, 8001AF70(7, 1)). Items 101 and 390 fly their own way, and an item the table at
 * 8006C858 (40 bytes, +18) does not let go in a bin is left to the game.
 * The look around is up to 17 floor lookups a frame while the item falls, each a search of the
 * ground's polygons (800CE550 tries up to five heights): charged to the game's CPU time it took
 * a throw from 30 frames a second down to 12-20 at overclock 200 (6 at 100). With shipfree it
 * costs no game time (the console never did it); recordings made before it took the time. */
#define SHIP_REACH 240
#define SHIP_ABOVE 40               /* the item down to about the bin's top */
#define ITEM_TABLE 0x8006C858u
#define BIN_POLY 688u               /* gp+: the polygon under the last point looked at */

static int bin_near(CPU *c, uint32_t p, int32_t x, int32_t y, int32_t z, int32_t above) {
    MEM_W32(p, (uint32_t)x);
    MEM_W32(p + 4u, (uint32_t)y);
    MEM_W32(p + 8u, (uint32_t)z);
    c->r[4] = p;
    call_addr(c, 0x800CE550u);
    if (c->r[2]) return 0;                                  /* no floor there */
    uint32_t poly = MEM_R32(c->r[28] + BIN_POLY);
    return poly && (MEM_R8(poly) & 0x80u) && (int32_t)MEM_R32(p + 4u) - y <= above;
}

int ship_reach(CPU *c) {
    if (!g_cfg.shipreach) return 0;
    uint32_t s = c->r[4];
    int16_t id = (int16_t)MEM_R16(s + 2u);
    if (id <= 0 || id == 101 || id == 390 || (int32_t)MEM_R32(s + 12u) <= 0) return 0;
    if (!MEM_R16(MEM_R32(ITEM_TABLE) + (uint32_t)id * 40u + 18u)) return 0;
    static const int8_t dir[8][2] = {{2, 0}, {-2, 0}, {0, 2}, {0, -2}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    uint32_t gp = regs[28], poly = MEM_R32(gp + BIN_POLY);
    uint32_t p = regs[29] - 0x40u;
    int32_t x = (int32_t)MEM_R32(s + 36u), y = (int32_t)MEM_R32(s + 40u), z = (int32_t)MEM_R32(s + 44u);
    uint32_t scale = g_cycle_scale;
    if (g_cfg.shipfree) g_cycle_scale = 0;                 /* the look around: no game time */
    c->r[29] = regs[29] - 0x80u;
    int found = bin_near(c, p, x, y, z, 0x7FFFFFF) ? -1 : 0;   /* over it already: the game's own */
    for (int ring = 1; ring <= 2 && !found; ring++)
        for (int k = 0; k < 8 && !found; k++) {
            c->r[29] = regs[29] - 0x80u;
            int32_t d = SHIP_REACH * ring / 4;              /* 2 = straight, 1+1 = diagonal (~0.7) */
            int32_t dx = dir[k][0] * d, dz = dir[k][1] * d;
            if (dir[k][0] && dir[k][1]) { dx = dx * 3 / 2; dz = dz * 3 / 2; }
            found = bin_near(c, p, x + dx, y, z + dz, SHIP_ABOVE);
        }
    g_cycle_scale = scale;
    MEM_W32(gp + BIN_POLY, poly);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
    if (found <= 0) return 0;
    c->r[29] = regs[29] - 0x40u;
    c->r[4] = (uint16_t)id;
    call_addr(c, 0x800F3BCCu);
    c->r[4] = 19;
    call_addr(c, 0x8001FABCu);
    c->r[4] = s;
    call_addr(c, 0x800CCBE8u);
    c->r[4] = 7;
    c->r[5] = 1;
    call_addr(c, 0x8001AF70u);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
    return 1;
}

/* Doors that open from wider (doorreach): the doors outside that open with X (events of kind 9000
 * in the area's list, 8009F78C + 16: +8 the way the farmer must face, 0 down / z-, 1 left / x-,
 * 2 up / z+, 3 right / x+; the box x / z from +14 / +18 to +20 / +24) are taken by 800BDCE8 (a0
 * the list, a1 the farmer's position) only with the farmer inside that box; the house's is 388
 * across, so the farmer had to stand just there. With the option the boxes of those doors are made
 * DOOR_SIDE wider each side and DOOR_OUT longer on the side the farmer comes from, for the
 * player's (and farmer 2's, in the player's place) look while 800BDCE8 runs, and put back after:
 * its answer for X (800C8C84) and for going through (800CDA44) is the same. The doors that are
 * walked through (kind 0) and the ways out are left as they are. */
#define DOOR_LIST 0x8009F78Cu
#define DOOR_KIND 0x9000u
#define DOOR_SIDE 160
#define DOOR_OUT 120

int door_box(uint32_t e, int32_t box[4]) {
    box[0] = (int16_t)MEM_R16(e + 14); box[1] = (int16_t)MEM_R16(e + 18);
    box[2] = (int16_t)MEM_R16(e + 20); box[3] = (int16_t)MEM_R16(e + 24);
    if (!g_cfg.doorreach || MEM_R16(e + 2) == 0xFFFF || MEM_R16(e) != DOOR_KIND) return 0;
    int16_t dir = (int16_t)MEM_R16(e + 8);
    if (dir < 0 || dir > 3) return 0;
    if (dir == 1 || dir == 3) { box[1] -= DOOR_SIDE; box[3] += DOOR_SIDE; }
    else { box[0] -= DOOR_SIDE; box[2] += DOOR_SIDE; }
    if (dir == 0) box[3] += DOOR_OUT;           /* faces z-: comes from z+ */
    else if (dir == 1) box[2] += DOOR_OUT;      /* faces x-: comes from x+ */
    else if (dir == 2) box[1] -= DOOR_OUT;
    else box[0] -= DOOR_OUT;
    return 1;
}

int door_reach(CPU *c) {
    static int inside;
    if (inside || !g_cfg.doorreach || c->r[4] != DOOR_LIST || c->r[5] != PLAYER_POS) return 0;
    static const uint32_t at[4] = {14, 18, 20, 24};
    uint16_t kept[50][4];
    uint64_t wide = 0;
    for (uint32_t i = 0; i < 50; i++) {
        uint32_t e = DOOR_LIST + 16 + i * 44;
        int32_t box[4];
        if (!door_box(e, box)) continue;
        wide |= 1ull << i;
        for (int k = 0; k < 4; k++) { kept[i][k] = MEM_R16(e + at[k]); MEM_W16(e + at[k], (uint16_t)box[k]); }
    }
    if (!wide) return 0;
    inside = 1;
    call_addr(c, 0x800BDCE8u);
    inside = 0;
    for (uint32_t i = 0; i < 50; i++)
        if (wide & (1ull << i))
            for (int k = 0; k < 4; k++) MEM_W16(DOOR_LIST + 16 + i * 44 + at[k], kept[i][k]);
    return 1;
}

/* The yellow Harvest Sprite at home (spritechair): the sprites stand at their spots in their house,
 * which 80110E34 copies each tick from a table of 7 (80127EE0, 16 bytes: x, y, z, facing) to the
 * sprite (a0; its spot at +172, its x, y, z at +56). The yellow one's (spot 3, -240, 0, -146) puts
 * its flat picture on the last chair behind the table, so the chair cuts in and out of its feet as
 * the camera moves (on the console too). Its z (a2 after 80110E78) gets SPRITE_BACK: up the screen,
 * away from the table; 90 is the least that clears the seat in the sessions tried. */
#define SPRITE_YELLOW_SPOT 3
#define SPRITE_BACK 100

uint32_t sprite_spot_z(uint32_t z, uint32_t actor) {
    return g_cfg.spritechair && MEM_R16(actor + 172) == SPRITE_YELLOW_SPOT ? z + SPRITE_BACK : z;
}

/* The stamina and tiredness under the clock (staminabar): the game never shows them. Every window
 * goes through 80014FAC (a0 the ordering table's entry, a1-a3 x / y / width, the height and the
 * colours on the stack from +16); the clock is the one at 230,18, 74x28, with two rows of text
 * (the glyph routine 80019070: a0 the entry before the window's, a1 where the primitive goes, a2 the
 * letter, a3 x, then y, r, g, b, the palette and semi-transparency (+36: GP0 66h instead of 64h; the
 * clock turns it on and off every 5 minutes, so it is never copied) on the stack; it returns where
 * the next goes). When the
 * clock opens, a window of the same size opens under it with the game's own routines: a row for the
 * stamina (S, a bar of what is left of the most, the number) and one for the tiredness (F, of 100).
 * The primitives come from the game's allocator (80011F5C, the size in a0). In the second window
 * (p2view.c) the numbers are farmer 2's. */
#define CLOCK_X 230
#define CLOCK_Y 18
#define CLOCK_W 74
#define CLOCK_H 28
#define WINDOW_AT 0x80014FACu
#define GLYPH_AT 0x80019070u
#define ALLOC_AT 0x80011F5Cu
#define PLAYER_STAMINA 0x80071A12u      /* what is left, the most, the tiredness (halfwords) */
#define LETTER(c) (g_game_lang == 5 ? 161 + (c) - 'A' : (c) - 'A')   /* the font: A-Z from 0, a-z 0x20, 0-9 0x40 */
#define DIGIT(d) game_digit(d)
#define RGB(r, g, b) ((uint32_t)(b) << 16 | (uint32_t)(g) << 8 | (uint32_t)(r))
#define BAR_X (CLOCK_X + 12)
#define BAR_W 28
#define BAR_H 6

static uint32_t glyph_args[6];          /* the last glyph's stack arguments (+16 to +36) */
static int fade_now;                    /* the clock is see-through this frame */
static int glyph_known, stamina_drawing, others_drawing;

/* the port's own letters (friends.c: the hearts) are not the game's: their colours stay theirs */
void stamina_glyph_others(int on) { others_drawing = on; }

void stamina_glyph_seen(CPU *c) {
    if (stamina_drawing || others_drawing) return;
    for (int i = 0; i < 6; i++) glyph_args[i] = MEM_R32(c->r[29] + 16 + (uint32_t)i * 4);
    glyph_known = 1;
}

static uint32_t call4(CPU *c, uint32_t at, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    c->r[4] = a0;
    c->r[5] = a1;
    c->r[6] = a2;
    c->r[7] = a3;
    call_addr(c, at);
    return c->r[2];
}

static uint32_t glyph(CPU *c, uint32_t ot, uint32_t p, int letter, int x, int y) {
    uint32_t sp = c->r[29];
    MEM_W32(sp + 16, (uint32_t)y);
    for (int i = 1; i < 5; i++) MEM_W32(sp + 16 + (uint32_t)i * 4, glyph_args[i]);
    MEM_W32(sp + 36, (uint32_t)fade_now);    /* +36 non-zero: a semi-transparent letter (the clock's blinks every 5 minutes) */
    return call4(c, GLYPH_AT, ot, p, (uint32_t)letter, (uint32_t)x);
}

/* an untextured rectangle (GP0 60h) at p, put first in the table's entry ot (drawn before what is
 * already there) */
static uint32_t tile(uint32_t ot, uint32_t p, int x, int y, int w, int h, uint32_t rgb) {
    MEM_W32(p, 3u << 24 | (MEM_R32(ot) & 0xFFFFFFu));
    MEM_W32(ot, (MEM_R32(ot) & 0xFF000000u) | (p & 0xFFFFFFu));
    MEM_W32(p + 4, (fade_now ? 0x62000000u : 0x60000000u) | rgb);     /* 62h: semi-transparent */
    MEM_W32(p + 8, (uint32_t)(uint16_t)y << 16 | (uint16_t)x);
    MEM_W32(p + 12, (uint32_t)h << 16 | (uint32_t)w);
    HUD_BEGIN();                        /* widescreen: with the clock */
    SPRITE_TAG(p + 8);
    return p + 16;
}

static uint32_t stamina_row(CPU *c, uint32_t ot, uint32_t p, int y, int letter, int value, int most, uint32_t rgb) {
    p = glyph(c, ot, p, letter, CLOCK_X + 2, y);
    if (value < 0) value = 0;
    if (value > 999) value = 999;
    int fill = most > 0 ? (value > most ? BAR_W : value * BAR_W / most) : 0;
    if (fill) p = tile(ot, p, BAR_X, y + 4, fill, BAR_H, rgb);          /* first: drawn last */
    p = tile(ot, p, BAR_X, y + 4, BAR_W, BAR_H, RGB(40, 32, 24));
    int x = BAR_X + BAR_W + 4 + 16;
    do {
        p = glyph(c, ot, p, DIGIT(value % 10), x, y);
        value /= 10;
        x -= g_game_lang == 5 ? 10 : 8;       /* the Japanese digits are 10 pixels wide */
    } while (value);
    return p;
}

/* The clock (GF_FARM 800FB674: its window through 80022580 / 800136D4 at 228,16, 78x32, the date,
 * the time and the weather icon) sets g_clock_draw while it builds: in widescreen what it builds
 * keeps to the right edge (HUD_BEGIN), the stamina window under it too.
 * The clock see-through over the farmer: the game projects the farmer (the anchor at 8001E02C,
 * its feet, kind 1; farmer 2 is kind 3, the one the second window follows) after it builds the HUD,
 * so the clock uses where the farmer was in the frame before. While the farmer is behind the clock
 * (and the stamina window under it), every part of them is built semi-transparent: the window
 * builders and the glyph routine have a flag for it on the stack (hud_fade). The blending of
 * untextured parts is the drawing mode's in effect (a cave's or a lake's can subtract): the window
 * gets one of its own, the glyphs' (E100060Eh, 50% / 50%), drawn first. Kept apart for the game,
 * the second window's run and the second machine. */
int g_clock_draw;
static uint32_t clock_ot;
#define FADE_BODY 40                    /* how high the farmer is above its feet */
#define FADE_HOLD 20                    /* frames it stays see-through after (no blinking) */
static struct { int x0, y0, x1, y1, seen, on, hold; } fade_at[3];

static int fade_slot(void) { return g_machine ? 2 : g_view2_run ? 1 : 0; }

void hud_fade_anchor(int x, int y, int kind) {
    int i = fade_slot();
    if (kind != (i == 1 ? 3 : 1))
        return;
    if (!fade_at[i].seen) {
        fade_at[i].x0 = fade_at[i].x1 = x;
        fade_at[i].y0 = fade_at[i].y1 = y;
        fade_at[i].seen = 1;
    }
    if (x < fade_at[i].x0) fade_at[i].x0 = x;
    if (x > fade_at[i].x1) fade_at[i].x1 = x;
    if (y < fade_at[i].y0) fade_at[i].y0 = y;
    if (y > fade_at[i].y1) fade_at[i].y1 = y;
    fade_at[i].on = 0;                  /* the HUD of this frame is built */
}

void hud_fade(CPU *c, uint32_t flag) {
    if (g_clock_draw && fade_at[fade_slot()].on)
        MEM_W16(c->r[29] + flag, 1);
}

void clock_begin(CPU *c) {
    (void)c;
    int f = fade_slot(), bottom = CLOCK_Y + CLOCK_H + (g_cfg.staminabar && glyph_known ? 2 + CLOCK_H : 0);
    g_clock_draw = 1;
    clock_ot = 0;
    if (fade_at[f].seen && fade_at[f].x0 < CLOCK_X + CLOCK_W + 8 && fade_at[f].x1 > CLOCK_X - 8
        && fade_at[f].y0 - FADE_BODY < bottom && fade_at[f].y1 > CLOCK_Y - 2)
        fade_at[f].hold = FADE_HOLD;
    else if (fade_at[f].hold > 0)
        fade_at[f].hold--;
    fade_at[f].on = fade_at[f].hold > 0;
    fade_at[f].seen = 0;
}

void clock_window(CPU *c) {
    if (g_clock_draw)
        clock_ot = c->r[4];
}

void stamina_window(CPU *c) {
    uint32_t sp = c->r[29];
    if (stamina_drawing || (int16_t)c->r[5] != CLOCK_X || (int16_t)c->r[6] != CLOCK_Y
        || (int16_t)c->r[7] != CLOCK_W || (int16_t)MEM_R32(sp + 16) != CLOCK_H)
        return;
    if (!g_cfg.staminabar || !glyph_known)
        return;
    fade_now = g_clock_draw && fade_at[fade_slot()].on;
    int16_t now, most, tired;
    if (!g_view2_run || !p2_stamina(&now, &most, &tired)) {
        now = (int16_t)MEM_R16(PLAYER_STAMINA);
        most = (int16_t)MEM_R16(PLAYER_STAMINA + 2);
        tired = (int16_t)MEM_R16(PLAYER_STAMINA + 4);
    }
    uint32_t regs[32], hi = c->hi, lo = c->lo, ot = c->r[4];
    memcpy(regs, c->r, sizeof regs);
    stamina_drawing = 1;
    uint32_t fsp = sp - 0x80, y = CLOCK_Y + CLOCK_H + 2;
    for (uint32_t i = 0; i < 6; i++) MEM_W32(fsp + 16 + i * 4, MEM_R32(sp + 16 + i * 4));   /* the clock's colours */
    c->r[29] = fsp;
    call4(c, WINDOW_AT, ot, CLOCK_X, y, CLOCK_W);
    c->r[29] = fsp;
    uint32_t p = call4(c, ALLOC_AT, 8 * 28 + 4 * 16, 0, 0, 0);
    c->r[29] = fsp;
    p = stamina_row(c, ot - 4, p, (int)y + 1, LETTER('S'), now, most, RGB(72, 200, 80));
    stamina_row(c, ot - 4, p, (int)y + 15, LETTER('F'), tired, 100, RGB(232, 104, 48));
    stamina_drawing = 0;
    fade_now = 0;
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
}

/* A call of a game routine from a hook: every register is kept (the routine's frame goes below the
 * hook's), only v0 comes back */
static uint32_t game_call(CPU *c, uint32_t at, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    c->r[29] = regs[29] - 0x80;
    uint32_t v = call4(c, at, a0, a1, a2, a3);
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
    return v;
}

/* After the clock's window is built (back from 800136D4 at 800225A8): see-through, its drawing mode
 * (50% / 50%, the glyphs' own) goes first in its table entry, drawn before it */
void clock_window_done(CPU *c) {
    if (!g_clock_draw || !clock_ot || !fade_at[fade_slot()].on)
        return;
    uint32_t p = game_call(c, ALLOC_AT, 8, 0, 0, 0);
    MEM_W32(p, 1u << 24 | (MEM_R32(clock_ot) & 0xFFFFFFu));
    MEM_W32(p + 4, 0xE100060Eu);
    MEM_W32(clock_ot, (MEM_R32(clock_ot) & 0xFF000000u) | (p & 0xFFFFFFu));
    clock_ot = 0;
}

/* An item's picture (800D6A94: in the hands, the rucksack, the menus) comes from the area's own copy
 * when the area has one (800D688C: a crop of the field or the hothouse), else from the disc. That copy
 * draws the crop in the field: its two halves can sit on different pictures, and the item shown with
 * them gets wrong colours (the hothouse eggplant held up). Such a copy (and
 * always item 235's) is not used: the picture is read from the disc */
#define AREA_PICTURES 0x800A0378u   /* -> the area: +10 its list of pictures (-1 none) */
#define PICTURE_IDS 0x80126254u     /* -> the list's items: a count, then the item numbers (halfwords) */
#define PICTURE_AT 0x80125528u      /* -> where each is, from 800957F8 (words, from the second) */
#define PICTURES_BASE 0x800957F8u
#define PICTURE_CALLER 0x800D6B20u  /* where 800D6A94's call of 800D688C returns */

/* the picture's part (its frame, from the header's tables; 0: none) as 800D6A94 finds it */
static uint32_t picture_part(uint32_t p, uint32_t which) {
    uint32_t frames = p + MEM_R32(p + 24), parts = p + MEM_R32(p + 20);
    uint32_t list = p + MEM_R32(p + 28), cells = p + MEM_R32(p + 32);
    uint32_t h = MEM_R16(cells + MEM_R16(list + which));
    if (h > 0xFFFDu) return 0;
    uint32_t e = parts + h * 4;
    return frames + MEM_R16(e + 2) + (uint32_t)((int16_t)MEM_R16(e) * 12 - 12);
}

static int area_picture_mixed(int16_t item) {
    uint32_t area = MEM_R32(AREA_PICTURES);
    if (!area) return 0;
    int16_t k = (int16_t)MEM_R16(area + 10);
    if (k == -1) return 0;
    uint32_t ids = MEM_R32(PICTURE_IDS + (uint32_t)k * 4), at = MEM_R32(PICTURE_AT + (uint32_t)k * 4);
    if (!ids) return 0;
    int16_t n = (int16_t)MEM_R16(ids);
    for (int i = 1; i <= n; i++) {
        if ((int16_t)MEM_R16(ids + (uint32_t)i * 2) != item) continue;
        uint32_t p = MEM_R32(at + (uint32_t)i * 4) + PICTURES_BASE;
        uint32_t a = picture_part(p, 2), b = picture_part(p, 6);
        if (!a || !b) return 0;
        uint32_t rects = p + MEM_R32(p + 16);
        uint32_t ra = rects + MEM_R16(a + 2) * 8, rb = rects + MEM_R16(b + 2) * 8;
        return MEM_R16(ra) != MEM_R16(rb) || MEM_R16(ra + 2) != MEM_R16(rb + 2);
    }
    return 0;
}

/* entry of 800D688C (a0 = the item): 1 = answer "no copy here" */
int item_picture_from_disc(CPU *c) {
    if (c->r[31] != PICTURE_CALLER) return 0;
    int16_t item = (int16_t)c->r[4];
    return item == 235 || area_picture_mixed(item);
}

/* Bulk food (bulkfood): a thing on a shop's shelf is taken in the hands and 8011C298 asks whether to
 * buy it (its state at 8012D524, the next one at 8012D528; a1 / a2 the question, a3 how it is sold:
 * 0 the shelf's; s1 the item, s3 the price). A yes for food asks how many with
 * the game's own number window (the fodder's: 8011CF40 the number, the text 77; 8012D538: +4 the
 * window's text, +8 the number, +10 the price; 8012D52C = 1 shows it), as many as the rucksack's empty
 * places and the fridge (once the house has the kitchen) can take: one stays in the hands, the rest
 * go to the rucksack, then to the fridge (64 places of 6 bytes at 80070D42: the item, 0, how many;
 * up to 99 in a place). States 15 and 16 are new: the question and the number */
#define SHOP_STATE 0x8012D524u
#define SHOP_NEXT 0x8012D528u
#define SHOP_SHOWN 0x8012D52Cu
#define NUMBER_BOX 0x8012D538u
#define MONEY 0x80071A5Cu
#define BAG_SIZE 0x80071A1Eu        /* the rucksack: 2 << it places, at 80071AC0 */
#define BAG_ITEMS 0x80071AC0u
#define FRIDGE 0x80070D42u
#define HAS_KITCHEN 0x80070D06u
#define ITEMS_TABLE 0x8006C858u     /* -> 40 bytes an item: flags at +24 */
#define FOOD_FLAGS 0xC0000000u      /* food, kept in the fridge */
#define STATE_ASK 15
#define STATE_NUMBER 16
#define TEXT_HOW_MANY 77
#define TEXT_THANKS 187
#define TEXT_NO_MONEY 79
#define TEXT_NOT_BOUGHT 216
#define SET_TEXT 0x8002261Cu
#define TEXT_CLOSE 0x80022738u
#define TEXT_DONE 0x800226D8u
#define TEXT_SOUND 0x80017B18u
#define NUMBER_PICK 0x8011CF40u
#define ITEM_PICTURE 0x800D6A94u

static int bulk_on, bulk_price;
static int16_t bulk_item;
/* the second window's run of the frame (p2view.c) throws its RAM away: so are these */
#define BULK_KEEP int keep_on = bulk_on, keep_price = bulk_price; int16_t keep_item = bulk_item
#define BULK_BACK if (g_view2_run) { bulk_on = keep_on; bulk_price = keep_price; bulk_item = keep_item; }

static void shop_go(uint32_t state, uint32_t next) {
    MEM_W32(SHOP_STATE, state);
    MEM_W32(SHOP_NEXT, next);
}

static int is_food(int16_t item) {
    uint32_t f = MEM_R32(MEM_R32(ITEMS_TABLE) + (uint32_t)item * 40 + 24);
    return (f & 0xF0000000u) == FOOD_FLAGS;
}

/* how many more of the item the rucksack and the fridge take; with put, puts that many there */
static int bulk_room(CPU *c, int16_t item, int put) {
    int room = 0, places = 2 << MEM_R16(BAG_SIZE);
    for (int i = 0; i < places; i++) {
        uint32_t e = BAG_ITEMS + (uint32_t)i * 6;
        if (MEM_R16(e)) continue;
        room++;
        if (put > 0) {
            game_call(c, ITEM_PICTURE, (uint32_t)(int32_t)item, 0, 0, 0);
            MEM_W16(e, (uint16_t)item);
            MEM_W16(e + 2, 0);
            MEM_W16(e + 4, 1);
            put--;
        }
    }
    if (!MEM_R16(HAS_KITCHEN) || !is_food(item)) return room;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < 64; i++) {
            uint32_t e = FRIDGE + (uint32_t)i * 6;
            int16_t id = (int16_t)MEM_R16(e);
            if (pass == 0 ? id != item : id != 0) continue;
            int n = pass == 0 ? 99 - (int16_t)MEM_R16(e + 4) : 99;
            if (n <= 0) continue;
            room += n;
            if (put > 0) {
                if (n > put) n = put;
                if (pass == 1) {
                    MEM_W16(e, (uint16_t)item);
                    MEM_W16(e + 2, 0);
                    MEM_W16(e + 4, 0);
                }
                MEM_W16(e + 4, (uint16_t)(MEM_R16(e + 4) + n));
                put -= n;
            }
        }
    return room;
}

/* right after the yes / no of 8011C298 (8011C470: s0 the answer, 1 yes): a yes for food that more of
 * fit and the money is enough for asks how many; the answer becomes 0 (still asking) */
static uint32_t food_yes(CPU *c, uint32_t answer) {
    int16_t item = (int16_t)c->r[17];
    int price = (int16_t)c->r[19];
    if (!g_cfg.bulkfood || answer != 1 || (int16_t)c->r[18] != 0 || !is_food(item)
        || (int32_t)MEM_R32(MONEY) < price || bulk_room(c, item, 0) < 1)
        return answer;
    bulk_on = 1;
    bulk_item = item;
    bulk_price = price;
    MEM_W32(NUMBER_BOX, 0);
    MEM_W16(NUMBER_BOX + 8, 1);
    MEM_W16(NUMBER_BOX + 10, (uint16_t)price);
    game_call(c, TEXT_CLOSE, 0, 0, 0, 0);
    MEM_W32(NUMBER_BOX + 4, MEM_R32(0x80054BC0u) + 0x8005181Au);
    shop_go(STATE_ASK, 0);
    return 0;
}

/* entry of 8011C298: the new states (1: done here, the routine answers 0, not finished) */
static int food_state(CPU *c) {
    uint32_t state = MEM_R32(SHOP_STATE);
    if (state == 0) bulk_on = 0;
    if (!bulk_on || (state != STATE_ASK && state != STATE_NUMBER)) return 0;
    if (state == STATE_ASK) {
        game_call(c, TEXT_DONE, 0, 0, 0, 0);
        game_call(c, TEXT_SOUND, 0, 0, 0, 0);
        game_call(c, SET_TEXT, 2, TEXT_HOW_MANY, 0, 0);
        shop_go(3, STATE_NUMBER);       /* 3: waits for the text, then the next state */
        return 1;
    }
    MEM_W32(SHOP_SHOWN, 1);
    int most = bulk_room(c, bulk_item, 0) + 1;
    if (most > 99) most = 99;
    uint32_t r = game_call(c, NUMBER_PICK, (uint32_t)most, NUMBER_BOX + 8, 0, 0);
    if (r == 1) {                       /* chosen */
        game_call(c, TEXT_CLOSE, 0, 0, 0, 0);
        if ((int32_t)MEM_R32(MONEY) >= (int16_t)MEM_R16(NUMBER_BOX + 8) * bulk_price) {
            MEM_W32(SHOP_SHOWN, 3);
            game_call(c, SET_TEXT, 2, TEXT_THANKS, 0, 0);
            shop_go(2, 6);              /* 2: waits for the text; 6: pays */
        } else {
            game_call(c, SET_TEXT, 2, TEXT_NO_MONEY, 0, 0);
            shop_go(2, STATE_ASK);
        }
    } else if (r == 3) {                /* cancelled: 7 puts the thing back */
        MEM_W32(SHOP_SHOWN, 3);
        game_call(c, TEXT_CLOSE, 0, 0, 0, 0);
        game_call(c, SET_TEXT, 2, TEXT_NOT_BOUGHT, 0, 0);
        shop_go(2, 7);
        bulk_on = 0;
    }
    return 1;
}

/* 8011C514 (state 6, a0 = minus the price, for the money): the price of all, the rest are put away */
static uint32_t food_pay(CPU *c, uint32_t minus_price) {
    if (!bulk_on) return minus_price;
    bulk_on = 0;
    int n = (int16_t)MEM_R16(NUMBER_BOX + 8);
    if (n < 2) return minus_price;
    bulk_room(c, bulk_item, n - 1);
    MEM_W16(NUMBER_BOX + 8, 1);
    return (uint32_t)(-(n * bulk_price));
}

uint32_t bulk_food_yes(CPU *c, uint32_t answer) {
    BULK_KEEP;
    answer = food_yes(c, answer);
    BULK_BACK;
    return answer;
}

int bulk_food_state(CPU *c) {
    BULK_KEEP;
    int r = food_state(c);
    BULK_BACK;
    return r;
}

uint32_t bulk_food_pay(CPU *c, uint32_t minus_price) {
    BULK_KEEP;
    minus_price = food_pay(c, minus_price);
    BULK_BACK;
    return minus_price;
}

/* Storm-proof greenhouse (sturdygreenhouse): on a typhoon / blizzard night (800DE254, weather
 * 8007AD00 = 3 or 4) 800C05F8 breaks a built greenhouse (80070D0C) when a roll of 100 is under 31:
 * the roll still happens (the same random numbers after it), but it never breaks */
uint32_t greenhouse_storm(uint32_t breaks) {
    return g_cfg.sturdygreenhouse && build_has(1) ? 0 : breaks;
}

/* Bulk feed (bulkfood): the ranch's fodder and the poultry farm's chicken feed are bought through
 * 8011BAFC's "how many?" (the number window, a0 = 99 at 8011BC54); its caller (ra at sp+116) tells
 * which: 80114B2C the fodder (80070D3A), 801157E4 the chicken feed (80070D40). They are kept up to 999
 * (800F397C / 800F3AF0 throw the rest away), so as many as still fit, at least 1 */
uint32_t bulk_feed_most(CPU *c, uint32_t most) {
    if (!g_cfg.bulkfood) return most;
    uint32_t from = MEM_R32(c->r[29] + 116), kept;
    if (from == 0x80114B2Cu) kept = 0x80070D3Au;
    else if (from == 0x801157E4u) kept = 0x80070D40u;
    else return most;
    int room = 999 - (int)MEM_R16(kept);
    return (uint32_t)(room > 1 ? room : 1);
}

/* The 2D drawing room: each of the two frame sets (8005E5E8 and 8005E71C, 308 bytes) has a big
 * room (+304, the 3D) and a small one (+300 its start, +296 12000 bytes: people, animals, shadows,
 * menus, the text), set up by 8001231C / 80012598: set A's small room at 801F1240, set B's after it
 * up to 801F7000. The small room fills up on a busy farm in widescreen (more people drawn at the
 * sides, the seven sprites at work): 92% full, and the bag's menu (Triangle) over it ran past the
 * end, where 80011FD8 waits for ever (break 0x400 at 800129E8). A list is drawn here as soon as the
 * game sends it (dma.c), before the other set is built, so the two sets can share both old rooms:
 * 24000 bytes each. (The end of the big rooms is no place for it: the loading screens keep files
 * there.) */
#define FRAME_SET_A 0x8005E5E8u
#define FRAME_SET_B 0x8005E71Cu
#define SMALL_START 0x801F1240u
#define SMALL_ROOM 24000u

void small_draw_room(void) {
    MEM_W32(FRAME_SET_A + 300, SMALL_START);
    MEM_W32(FRAME_SET_A + 296, SMALL_ROOM);
    MEM_W32(FRAME_SET_B + 300, SMALL_START);
    MEM_W32(FRAME_SET_B + 296, SMALL_ROOM);
}

/* The new game's birthday screen (8002348C): the four seasons over the days, each an icon
 * (80024A10 at x = 30 + 64 i) and its name (8001AA7C into 800A0708, drawn by 80017B3C at x + 12),
 * all at y 60. Names longer than English ones ("Primavera") ran over the next icon: when one
 * does not fit, the four are spread over the box (x 20..296) by their lengths, measured with the
 * game's own 8001AA7C, each frame (season 0 comes first). */
#define SEASON_NAME_AT 0x8001AA7Cu
#define SEASON_NAME_BUF 0x800A0708u
static int16_t season_xs[4];

static void season_layout(CPU *c) {
    uint32_t keep[32];
    int w[4], sum = 0, fits = 1;
    const int adv = g_game_lang == 5 ? 12 : 8;
    memcpy(keep, c->r, sizeof keep);
    for (int k = 0; k < 4; k++) {
        c->r[4] = (uint32_t)k;
        c->r[5] = SEASON_NAME_BUF;
        call_addr(c, SEASON_NAME_AT);
        int n = (int)((c->r[2] - SEASON_NAME_BUF) / 2);
        w[k] = n > 0 ? n * adv - 2 : 0;
        sum += w[k];
        if (12 + w[k] > (k < 3 ? 62 : 295 - 222)) fits = 0;
    }
    memcpy(c->r, keep, sizeof keep);
    memset(season_xs, 0, sizeof season_xs);
    if (fits) return;
    int gap = (272 - 48 - sum) / 3;   /* 2 pixels inside the box on each side */
    if (gap < 2) gap = 2;
    if (gap > 18) gap = 18;
    int x = 22 + (272 - (48 + sum + 3 * gap)) / 2;
    for (int k = 0; k < 4; k++) {
        season_xs[k] = (int16_t)x;
        x += 12 + w[k] + gap;
    }
}

/* a1 of the icon's call (80023504, s1 the season) */
uint32_t season_icon_x(CPU *c, uint32_t season, uint32_t x) {
    if (season == 0) season_layout(c);
    return season < 4 && season_xs[season] ? (uint32_t)season_xs[season] : x;
}

/* a2 of the name's call (80023530) */
uint32_t season_name_x(uint32_t season, uint32_t x) {
    return season < 4 && season_xs[season] ? (uint32_t)(season_xs[season] + 12) : x;
}
