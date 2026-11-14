/* Hiring the Harvest Sprites through the Chef (spritehire). Hooks emitted by tools/recomp.py.
 *
 * The seven sprites' records (0xF8 bytes each from 8007A638, in the saved part of the RAM): +22
 * friendship (0-255), +40 the hire menu's state, +172 which sprite (0 = the Chef), +174/+176/+178
 * the skill at each job (+1 a day worked, up to 255: the game's own, at night in 8010CB6C), +180
 * the days left (+1: the night takes one off and the sprite works while it is not 0), +184 the job
 * (bits 1, 2, 4; 0x40 hired, 8 talked to once).
 *
 * The talk (GF_FARM 8010DA04) asks to hire only a sprite already talked to (+184 bit 8) with a
 * friendship of 50 or more, and in spring (and the last days of winter) not before the sixth talk
 * (+188). The hire menu (8010E6E8, state +40: 0 the job's answer, 2 shows the days question, 3 its
 * answer, 5 "never mind") asks the job, then how many days.
 *
 * With the option, sprites work in spring too, their skills are kept at the highest (255), they
 * work WORK_SPEED times as fast, and the Chef always asks: after the job he asks how
 * many of the sprites still free (left / right in his menu) and for how long, 1 week (50G a sprite)
 * or 30 days (200G a sprite), takes the money and sends that many to the same job (the others
 * first, the Chef himself last, so he stays home for the rest while any are left). The questions
 * are the game's own texts with new words: a text is written over where it is loaded (80022A94).
 * The rest of the hiring stays the game's own.
 *
 * Talking to a sprite under contract on the farm, or in the barn or the coop (a day or more left: the last day it works too), says what it does and asks whether it should do another job: the answer changes its job,
 * the days stay. The talk picks its text in 8010DC94 / 8010DC98 (bank in a0, number in a1); the text
 * system (80022B98) stores a menu's answer at gp+836 (80022CD8) and closes the window. */
#include "psx.h"
#include "psx_recomp.h"
#include "gpu.h"
#include "text_pt.h"
#include <string.h>

#define SPRITES 0x8007A638u
#define SPRITE_SIZE 0xF8u
#define SPRITE_COUNT 7
#define CHEF 0
#define MONEY 0x80071A5Cu
#define MAP_NOW 0x800A046Cu        /* the map loaded (word): the farm is 20 + the season (the area's
                                    * number, 800A036A, is not always 2 on the farm) */
#define FARM_MAP 20
#define AREA_KIND 0x800A0466u      /* halfword: 5 the barn, 4 the coop (where the animals' sprites
                                    * work), 40 the sprites' house, 1 the farm */
#define ANIMALS_KIND 5
#define COOP_KIND 4
#define FARM_KIND 1
#define GREENHOUSE_KIND 6          /* the greenhouse (the waterers work its beds there too) */
#define HOUR 0x80070CF8u           /* halfword: the hour */
#define TEXT_BUF 0x8006E110u        /* the text being shown (gp+828) */
#define TEXT_MAX 0x800u             /* halfwords there (the 8 KB block's last 2 KB) */
#define TEXT_HEAD 5                 /* the window's codes before the words, kept */
#define PAD_STATE 0x8005E3A8u       /* -> the pad: +0 held */
#define PAD_LEFT 0x8000u
#define PAD_RIGHT 0x2000u
#define START_ACTION 0x8010CE80u   /* a sprite's new action (a0 the sprite, a1 the action) */
#define SPRITE_UPDATE 0x8010CD80u  /* a sprite's frame: its action (a0 the sprite) ... */
#define SPRITE_MOVE 0x8010CAA8u    /* ... then its move (position +56 += speed +72) */
#define WORK_SPEED 3                /* frames a sprite at work runs in one */
#define SOUND 0x8001FABCu           /* a sound effect (5: the number window's) */
#define SPRITE_TEXTS 0x80127CF4u   /* each sprite's hire question (bank 2); +1 its "working" one */
#define FONT_X 896                  /* the font: 4-bit, 8x12 cells, 16 a row, at VRAM 896,0 */
#define ARROW_LEFT 0x9E             /* empty cells of the font: our arrows are drawn there */
#define ARROW_RIGHT 0x9F
#define WEEK_PRICE 50
#define MONTH_PRICE 200

enum { NONE, COUNT, DAYS, HIRE, NO_MONEY };
enum { T_NONE, T_COUNT, T_DAYS, T_DONE, T_NO_MONEY, T_JOBS };

static int phase, want_text, how_many, most, hire_days, cost, count_at, job_menu;
static uint32_t job_sprite;
static uint32_t pad_was;

/* the arrows (1 the glyphs' ink), loaded into the font's empty cells with a VRAM write (GP0 A0) */
static const char *const arrow_rows[12] = {
    "........", ".....1..", "....11..", "...111..", "..1111..", ".11111..",
    "..1111..", "...111..", "....11..", ".....1..", "........", "........" };

static void put_glyph(int code, int mirror) {
    gpu_write_gp0(0xA0000000u);
    gpu_write_gp0((uint32_t)((code >> 4) * 12) << 16 | (uint32_t)(FONT_X + (code & 15) * 2));
    gpu_write_gp0(12u << 16 | 2u);
    for (int y = 0; y < 12; y++) {
        uint32_t w = 0;
        for (int x = 0; x < 8; x++)
            if (arrow_rows[y][mirror ? 7 - x : x] == '1') w |= 1u << (x * 4);
        gpu_write_gp0(w);
    }
}

static void load_arrows(void) {
    put_glyph(ARROW_LEFT, 0);
    put_glyph(ARROW_RIGHT, 1);
}

static uint32_t sprite(int i) { return SPRITES + (uint32_t)i * SPRITE_SIZE; }

static int is_chef(uint32_t rec) { return g_cfg.spritehire && MEM_R16(rec + 172) == CHEF; }

/* the free sprites in hiring order, the Chef last (he is free while he talks: the game has already
 * stored his days when the hire is made) */
static int free_sprites(int order[SPRITE_COUNT]) {
    int n = 0;
    for (int k = 1; k <= SPRITE_COUNT; k++) {
        int i = k % SPRITE_COUNT;
        if (i == CHEF || MEM_R16(sprite(i) + 180) < 2) order[n++] = i;
    }
    return n;
}

static uint32_t game_call(CPU *c, uint32_t at, uint32_t a0, uint32_t a1) {
    uint32_t regs[32], hi = c->hi, lo = c->lo;
    memcpy(regs, c->r, sizeof regs);
    c->r[29] = regs[29] - 0x80;
    c->r[4] = a0;
    c->r[5] = a1;
    call_addr(c, at);
    uint32_t v = c->r[2];
    memcpy(c->r, regs, sizeof regs);
    c->hi = hi;
    c->lo = lo;
    return v;
}

/* the night (8010CB6C, a0 the sprite): every sprite's skill at the three jobs is the highest (the
 * routine adds a day's and keeps it at 255) */
void sprite_night(CPU *c) {
    if (!g_cfg.spritehire || g_view2_run) return;
    for (int i = 0; i < SPRITE_COUNT; i++)
        for (uint32_t o = 174; o <= 178; o += 2) MEM_W16(sprite(i) + o, 255);
}

/* a sprite's frame (entry of 8010CD80, a0 the sprite; its caller moves it next with 8010CAA8): one
 * at work (actions 8-11: harvest, water, animals, coming to the farm) runs WORK_SPEED frames */
void sprite_update(CPU *c) {
    static int busy;
    uint32_t rec = c->r[4];
    uint16_t action = MEM_R16(rec + 38);
    if (busy || !g_cfg.spritehire || g_view2_run || action < 8 || action > 11) return;
    busy = 1;
    for (int k = 1; k < WORK_SPEED; k++) {
        game_call(c, SPRITE_UPDATE, rec, 0);
        game_call(c, SPRITE_MOVE, rec, 0);
    }
    busy = 0;
}

/* the talk's checks, each hook after the instruction: v0 = +184 & 8 (talked to once), v0 =
 * friendship < 50, v0 = the spring talks < 6 */
uint32_t sprite_talked(CPU *c, uint32_t talked) {
    return is_chef(c->r[18]) ? 8u : talked;
}

uint32_t sprite_friendship(CPU *c, uint32_t too_low) {
    return is_chef(c->r[18]) ? 0u : too_low;
}

uint32_t sprite_spring(uint32_t too_few) {
    return g_cfg.spritehire ? 0u : too_few;
}

/* entry of the hire menu (a0 the sprite): the days question is about to be shown */
void sprite_menu(CPU *c) {
    uint32_t rec = c->r[4];
    int16_t state = (int16_t)MEM_R16(rec + 40);
    if (g_view2_run || !is_chef(rec)) return;
    if (state == 0) phase = NONE;
    if (state != 2) return;
    if (phase == NONE) {
        int order[SPRITE_COUNT];
        most = free_sprites(order);
        if (most < 1) return;
        how_many = most;
        phase = COUNT;
        want_text = T_COUNT;
    } else if (phase == DAYS) {
        want_text = T_DAYS;
    }
}

/* the days question's answer (v1, copied from v0 by the hook's instruction: 0 none yet, 1.. the
 * line), s0 the sprite */
uint32_t sprite_days_answer(CPU *c, uint32_t answer) {
    if (g_view2_run || !is_chef(c->r[16]) || answer == 0) return answer;
    uint32_t rec = c->r[16];
    if (phase == COUNT) {
        if (answer != 1) { phase = NONE; return 4; }
        phase = DAYS;
        MEM_W16(rec + 40, 2);       /* the days question next */
        return 0;
    }
    if (phase != DAYS) return answer;
    if (answer != 1 && answer != 2) { phase = NONE; return 4; }
    hire_days = answer == 1 ? 8 : 31;
    cost = how_many * (answer == 1 ? WEEK_PRICE : MONTH_PRICE);
    if ((int32_t)MEM_R32(MONEY) < cost) {
        phase = NO_MONEY;
        want_text = T_NO_MONEY;     /* in place of the "never mind" one */
        return 4;
    }
    MEM_W32(MONEY, MEM_R32(MONEY) - (uint32_t)cost);
    phase = HIRE;
    want_text = T_DONE;
    return 1;                       /* the game's own "OK" (it stores 2 days, put right below) */
}

/* after the game stored the Chef's days (8010E98C): the hire */
void sprite_hired(CPU *c) {
    if (g_view2_run || !is_chef(c->r[16]) || phase != HIRE) return;
    int order[SPRITE_COUNT], n = free_sprites(order);
    uint32_t job = MEM_R16(sprite(CHEF) + 184) & 7u;
    int chef_goes = 0;
    if (how_many > n) how_many = n;
    for (int k = 0; k < how_many; k++) {
        uint32_t rec = sprite(order[k]);
        MEM_W16(rec + 180, (uint16_t)hire_days);
        MEM_W16(rec + 184, (uint16_t)((MEM_R16(rec + 184) & ~0x47u) | 0x40u | job));
        if (order[k] == CHEF) chef_goes = 1;
    }
    if (!chef_goes) {
        uint32_t rec = sprite(CHEF);
        MEM_W16(rec + 180, 0);
        MEM_W16(rec + 184, (uint16_t)(MEM_R16(rec + 184) & ~0x47u));
    }
    LOGI("sprites: %d hired for %d days (%dG)", how_many, hire_days - 1, cost);
    phase = NONE;
}

/* the words: A-Z 0.., a-z 0x20.., 0-9 0x40..; '\n' a new line, '|' the start of the answers,
 * '#' the number of sprites (its place kept in count_at) */
static uint32_t put(uint32_t at, uint16_t h) {
    if (at < TEXT_MAX) MEM_W16(TEXT_BUF + at * 2, h);
    return at + 1;
}

static uint32_t put_number(uint32_t at, int v) {
    char d[12];
    int n = 0;
    do { d[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) at = put(at, (uint16_t)(0x40 + d[--n] - '0'));
    return at;
}

static uint32_t put_words(uint32_t at, const char *s) {
    for (; *s; s++) {
        char ch = *s;
        int g = pt_glyph(&s);
        if (g >= 0) at = put(at, (uint16_t)g);
        else if (ch >= 'A' && ch <= 'Z') at = put(at, (uint16_t)(ch - 'A'));
        else if (ch >= 'a' && ch <= 'z') at = put(at, (uint16_t)(0x20 + ch - 'a'));
        else if (ch >= '0' && ch <= '9') at = put(at, (uint16_t)(0x40 + ch - '0'));
        else if (ch == ' ') at = put(at, 0x0FFD);
        else if (ch == ',') at = put(at, 0x8C);
        else if (ch == '.') at = put(at, 0x6C);
        else if (ch == '?') at = put(at, 0x62);
        else if (ch == '!') at = put(at, 0x63);
        else if (ch == '<') at = put(at, ARROW_LEFT);
        else if (ch == '>') at = put(at, ARROW_RIGHT);
        else if (ch == '\'' && (s[1] == 'm' || s[1] == 'l' || s[1] == 's')) {
            /* 'm 'l 's are glyphs of their own (the game writes I'll as I, 'l, l) */
            s++;
            at = put(at, *s == 'm' ? 0x54 : *s == 'l' ? 0x52 : 0x51);
        } else if (ch == '\'') at = put(at, 0x8D);
        else if (ch == '\n') at = put(at, 0xFFFE);
        else if (ch == '|') {
            at = put(at, 0x0FFC);
            at = put(at, 0x0FF8);
            at = put(at, 0xFFFE);
            at = put(at, 0x0FF6);
        } else if (ch == '#') {
            count_at = (int)at;
            at = put(at, (uint16_t)(0x40 + how_many));
        }
    }
    return at;
}

/* a text was loaded (80022A94): written over when one of ours is wanted */
void sprite_text_loaded(void) {
    if (g_view2_run) return;
    if (want_text != T_JOBS) job_menu = 0;     /* another text: the job menu was left */
    if (!want_text) return;
    int which = want_text;
    want_text = T_NONE;
    uint32_t at = TEXT_HEAD;
    if (GAME_PT) switch (which) {   /* the Portuguese translation's game: the same texts in Portuguese */
    case T_COUNT:
        load_arrows();
        at = put_words(at, "Quantos de nós devem ir?\nUse esquerda e direita.| Mandar < # > de nós\n Ah, deixa para lá.");
        break;
    case T_DAYS:
        at = put_words(at, "Por quanto tempo vamos\ntrabalhar, budum?| 1 semana, ");
        at = put_number(at, how_many * WEEK_PRICE);
        at = put_words(at, "G\n 30 dias, ");
        at = put_number(at, how_many * MONTH_PRICE);
        at = put_words(at, "G\n Ah, deixa para lá.");
        break;
    case T_DONE:
        at = put_words(at, "Certo. ");
        at = put_number(at, how_many);
        at = put_words(at, how_many == 1 ? " de nós vai ajudar a\npartir de amanhã às 6h, budum."
                                         : " de nós vão ajudar a\npartir de amanhã às 6h, budum.");
        break;
    case T_NO_MONEY:
        at = put_words(at, "Vai custar ");
        at = put_number(at, cost);
        at = put_words(at, "G, budum.\nVolte quando tiver o dinheiro.");
        phase = NONE;
        break;
    case T_JOBS: {
        uint32_t job = MEM_R16(job_sprite + 184);
        at = put_words(at, job & 1u ? "Estou colhendo agora, budum." : job & 2u ? "Estou regando agora, budum."
                                                                         : "Estou com os animais, budum.");
        at = put_words(at, "\nQuer que eu troque de serviço?| Colher a plantação\n Regar a plantação\n Cuidar dos animais\n Ah, deixa para lá.");
        job_menu = 2;
        break;
    }
    } else switch (which) {
    case T_COUNT:
        load_arrows();
        at = put_words(at, "How many of us should go?\nPress left or right to pick.| Send < # > of us\n Oh, never mind.");
        break;
    case T_DAYS:
        at = put_words(at, "How long should we work,\nbudum?| 1 week, ");
        at = put_number(at, how_many * WEEK_PRICE);
        at = put_words(at, "G\n 30 days, ");
        at = put_number(at, how_many * MONTH_PRICE);
        at = put_words(at, "G\n Oh, never mind.");
        break;
    case T_DONE:
        at = put_words(at, "OK. ");
        at = put_number(at, how_many);
        at = put_words(at, " of us will help from\n6AM tomorrow, budum.");
        break;
    case T_NO_MONEY:
        at = put_words(at, "That would be ");
        at = put_number(at, cost);
        at = put_words(at, "G, budum.\nCome back when you have it.");
        phase = NONE;
        break;
    case T_JOBS: {
        uint32_t job = MEM_R16(job_sprite + 184);
        at = put_words(at, job & 1u ? "I'm harvesting now, budum." : job & 2u ? "I'm watering now, budum."
                                                                      : "I'm with the animals, budum.");
        at = put_words(at, "\nShould I change jobs?| Harvest the fields\n Water the fields\n Take care of the animals\n Oh, never mind.");
        job_menu = 2;
        break;
    }
    }
    put(at, 0xFFFF);
}

/* the farm: the kind of area (the map's number is not always 20 + the season there: 19 and 0
 * were seen on the farm) */
static int on_farm(void) {
    return MEM_R16(AREA_KIND) == FARM_KIND || MEM_R32(MAP_NOW) - FARM_MAP <= 3u;
}

/* the barn or the coop (a sprite's +48 there is the same kind) */
static int animals_kind(uint32_t kind) {
    return kind == ANIMALS_KIND || kind == COOP_KIND;
}

static int in_greenhouse(void) {
    return MEM_R16(AREA_KIND) == GREENHOUSE_KIND;
}

/* the talk's text (after 8010DC98: a0 bank, a1 number; s2 the sprite): one under contract asks
 * whether to do another job */
void sprite_talk_text(CPU *c) {
    uint32_t rec = c->r[18];
    if (g_view2_run || !g_cfg.spritehire || (!on_farm() && !in_greenhouse() && !animals_kind(MEM_R16(AREA_KIND))) || MEM_R16(rec + 180) < 1
        || !(MEM_R16(rec + 184) & 7u)) return;
    job_sprite = rec;
    job_menu = 1;
    c->r[4] = 2;
    c->r[5] = (uint32_t)(int32_t)(int16_t)MEM_R16(SPRITE_TEXTS + MEM_R16(rec + 172) * 2u);
    want_text = T_JOBS;
}

/* a menu answered (80022CDC, a0 = 1.. the line): the job menu's (1-3 the jobs, as the hire menu's:
 * bits 1, 2, 4; 4 never mind) changes the sprite's job */
void sprite_menu_answered(CPU *c, uint32_t answer) {
    if (g_view2_run || job_menu != 2) return;
    job_menu = 0;
    if (answer < 1 || answer > 3) return;
    uint32_t rec = job_sprite;
    MEM_W16(rec + 184, (uint16_t)((MEM_R16(rec + 184) & ~7u) | (1u << (answer - 1))));
    /* the job is chosen once, when the sprite reaches the farm: action 11 starts action 8, 9 or 10
     * (harvest, water, animals; 8010F324) with 8010CE80, and a talk on the farm doesn't stop it. A
     * sprite at work starts the new job's action the same way (the action it would go back to,
     * +190, too). In the barn or the coop (a job of the fields) the sprite leaves at once for the
     * farm: a sprite is where its +48 (the kind of area, as 800A0466) says, and is moved and drawn
     * only there (8010C800); on the farm it arrives (action 11) and starts the new job; with 0x40
     * clear it works the fields meanwhile, as one out of sight (8010CAD4) */
    uint16_t saved = MEM_R16(rec + 38);
    int work_hours = saved != 12 && MEM_R16(HOUR) - 6u < 15u;
    if (in_greenhouse() && MEM_R16(rec + 48) == GREENHOUSE_KIND) {
        /* in the greenhouse: watering goes on there (action 9 works the greenhouse's beds with +48
         * 6); another job sends the sprite to the farm at once, as from the barn (action 11) */
        if (answer == 2 && saved != 9 && work_hours) {
            MEM_W16(rec + 184, (uint16_t)(MEM_R16(rec + 184) & ~0x40u));
            MEM_W16(rec + 190, 0);
            game_call(c, START_ACTION, rec, 9);
        } else if (answer != 2 && work_hours) {
            MEM_W16(rec + 48, FARM_KIND);
            MEM_W16(rec + 184, (uint16_t)(MEM_R16(rec + 184) & ~0x40u));
            MEM_W16(rec + 190, 0);
            game_call(c, START_ACTION, rec, 11);
        }
    } else if (!on_farm() && answer != 3 && animals_kind(MEM_R16(rec + 48))) {
        MEM_W16(rec + 48, FARM_KIND);
        MEM_W16(rec + 184, (uint16_t)(MEM_R16(rec + 184) & ~0x40u));
        MEM_W16(rec + 190, 0);
        game_call(c, START_ACTION, rec, 11);
    } else if (on_farm() && saved >= 8 && saved <= 10) {
        if (answer == 3) MEM_W16(rec + 34, 3);
        game_call(c, START_ACTION, rec, 7 + answer);
        if (MEM_R16(rec + 190) >= 8 && MEM_R16(rec + 190) <= 10) MEM_W16(rec + 190, (uint16_t)(7 + answer));
    } else if (on_farm() && work_hours) {
        /* done for the day (0x40, walking about): back to work at the new job, in work hours
         * (from 21h the sprites go home: action 12, 8010CE00) */
        MEM_W16(rec + 184, (uint16_t)(MEM_R16(rec + 184) & ~0x40u));
        MEM_W16(rec + 190, 0);
        if (answer == 3) MEM_W16(rec + 34, 3);
        game_call(c, START_ACTION, rec, 7 + answer);
    }
    LOGI("sprites: sprite %u now on job %u (action %u -> %u)", MEM_R16(rec + 172), answer, saved,
         MEM_R16(rec + 38));
}

/* each tick the text window is drawn (80022B98): left / right change how many */
void sprite_text_tick(CPU *c) {
    if (g_view2_run || phase != COUNT || !count_at) return;
    uint32_t held = MEM_R32(MEM_R32(PAD_STATE));
    uint32_t pressed = held & ~pad_was;
    pad_was = held;
    int was = how_many;
    if (pressed & PAD_RIGHT) how_many = how_many >= most ? 1 : how_many + 1;
    if (pressed & PAD_LEFT) how_many = how_many <= 1 ? most : how_many - 1;
    if (how_many == was) return;
    MEM_W16(TEXT_BUF + (uint32_t)count_at * 2, (uint16_t)(0x40 + how_many));
    game_call(c, SOUND, 5, 0);
}
