/* Runtime-internal declarations shared by the hardware/BIOS reimplementation modules. */
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "psx_recomp.h"

#define PSX_CPU_HZ        33868800u
#define CYCLES_PER_SAMPLE 768u          /* 44100 Hz audio */

extern CPU g_cpu;

/* ---- logging ---- */
enum { LOG_ERR = 0, LOG_WARN = 1, LOG_INFO = 2, LOG_DBG = 3, LOG_TRACE = 4 };
extern int g_log_level;
void log_msg(int level, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
#define LOGE(...) log_msg(LOG_ERR, __VA_ARGS__)
#define LOGW(...) log_msg(LOG_WARN, __VA_ARGS__)
#define LOGI(...) log_msg(LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (g_log_level >= LOG_DBG) log_msg(LOG_DBG, __VA_ARGS__); } while (0)
#define LOGT(...) do { if (g_log_level >= LOG_TRACE) log_msg(LOG_TRACE, __VA_ARGS__); } while (0)
void fatal(const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2), noreturn))
#endif
    ;

/* ---- memory ---- */
static inline uint32_t ram_r32(uint32_t a) { uint32_t v; memcpy(&v, g_ram + (a & 0x1FFFFC), 4); return v; }
static inline uint16_t ram_r16(uint32_t a) { uint16_t v; memcpy(&v, g_ram + (a & 0x1FFFFE), 2); return v; }
static inline uint8_t  ram_r8(uint32_t a)  { return g_ram[a & 0x1FFFFF]; }
static inline void ram_w32(uint32_t a, uint32_t v) { memcpy(g_ram + (a & 0x1FFFFC), &v, 4); }
static inline void ram_w16(uint32_t a, uint16_t v) { memcpy(g_ram + (a & 0x1FFFFE), &v, 2); }
static inline void ram_w8(uint32_t a, uint8_t v)  { g_ram[a & 0x1FFFFF] = v; }
static inline uint8_t *ram_ptr(uint32_t a) { return g_ram + (a & 0x1FFFFF); }
void mem_reset(void);

/* ---- interrupts ---- */
enum {
    IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
    IRQ_TMR2 = 6, IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_SPU = 9, IRQ_PIO = 10
};
extern uint32_t g_istat, g_imask;
void irq_raise(int n);
void irq_write_stat(uint32_t v);
void irq_write_mask(uint32_t v);
void irq_check(CPU *c);           /* dispatch the exception handler if an IRQ is pending & enabled */

/* ---- scheduler ---- */
enum { EV_VBLANK, EV_HBLANK_TIMER, EV_TIMER0, EV_TIMER1, EV_TIMER2, EV_CDROM, EV_CDROM_READ,
       EV_SPU, EV_DMA, EV_SIO, EV_COUNT };
void sched_init(void);
void sched_set(int ev, uint64_t when);
void sched_cancel(int ev);
uint64_t sched_when(int ev);
void sched_update_next(CPU *c);
uint64_t frame_count(void);
void timers_init(void);
uint32_t timer_read(uint32_t off);
void timer_write(uint32_t off, uint32_t v);
void spu_catch_up(void);          /* render SPU output up to the current cycle */
uint32_t sched_line_in_frame(void);

/* ---- DMA ---- */
void dma_init(void);
uint32_t dma_read(uint32_t off);
void dma_write(uint32_t off, uint32_t v);

/* ---- CDROM ---- */
bool cdrom_open_image(const char *path);
void cdrom_init(void);
uint8_t cdrom_read8(uint32_t off);
void cdrom_write8(uint32_t off, uint8_t v);
void cdrom_dma_read(uint32_t *dst, int words);
void cdrom_event(void);
void cdrom_read_event(void);
int  cdrom_read_sector_raw(uint32_t lba, uint8_t *buf2352);
bool iso_find_file(const char *path, uint32_t *lba, uint32_t *size);
bool cdpak_open(const char *path, uint32_t *total_sectors);
int  cdpak_read_sector(uint32_t lba, uint8_t *buf2352);
int  cdpak_verify_all(void);
long zlib_inflate(const uint8_t *in, size_t inlen, uint8_t *out, size_t outlen);

/* ---- SIO0 (joypad / memory card hardware port) ---- */
void sio_init(void);
uint32_t sio_read(uint32_t off, int width);
void sio_write(uint32_t off, uint32_t v, int width);
void sio_event(void);

/* ---- BIOS (HLE kernel) ---- */
void bios_init(void);
void bios_call(CPU *c, uint32_t vec, uint32_t fn);   /* vec = 0xA0/0xB0/0xC0 */
void bios_syscall(CPU *c);
void bios_exception_irq(CPU *c);
void bios_vblank(void);           /* hardware vblank happened (pad polling) */
extern int g_in_exception;

/* ---- controllers ---- */
enum {
    PAD_SELECT = 0x0001, PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_START = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000
};
extern uint16_t g_pad_buttons[2];   /* active-high button masks set by the platform layer */
extern bool g_pad_connected[2];

/* ---- memory card storage ---- */
bool memcard_load(int slot, const char *path);
void memcard_flush(void);
uint8_t *memcard_data(int slot);  /* 128KB raw image */
void memcard_mark_dirty(int slot);
#define CARD_BANKS 4
void memcard_open_banks(int bank);  /* load slot 1 from bank 1-4; 0 = the bank last used */
bool memcard_switch_bank(int bank, bool force); /* 0-based; false if refused (card in use) or the same */
int memcard_bank(void);
bool memcard_select_bank(int bank);  /* F1-F4: on a diary list the list swaps it and shows it */
void memcard_bank_file(char *out, size_t n);             /* 1-based bank in slot 1 */
void sio_card_changed(int port);    /* the card in this port was swapped */
void memcard_touch(void);           /* the card's contents were read or written now */
bool bios_card_files_open(void);    /* a file on a memory card is open */
#define PORT_TEXT_ADDR 0x1F000400u
extern uint16_t g_port_text[128];  /* text made by the port, read by the game at 9F000400 */
bool memcard_busy(void);           /* a save/load screen is waiting for the card (fastcard) */
bool quick_loading(bool black);    /* "Now Loading" or a black loading pause (fastload) */
void quick_hold_music(bool hold);  /* the music sequencer waits (a loading pause run at full speed) */
uint16_t quick_skip(uint16_t pad);  /* START held in a cutscene: the pad the game sees (skipscenes) */
bool quick_skipping(void);         /* ... and whether it is skipping now */

/* ---- dispatcher ---- */
void dispatch_init(void);
RecompFunc dispatch_lookup(uint32_t addr, const char **modname);
void call_guest(CPU *c, uint32_t addr);   /* run a guest function to completion (sets ra sentinel) */
void dump_backtrace(void);

/* ---- platform (frontend) ---- */
void platform_frame(void);        /* present + input + throttle; called once per vblank */
void platform_audio_push(const int16_t *stereo, int frames);
int platform_poll_quit(void);     /* handles window events; nonzero when the window was closed */
extern volatile int g_quit;

/* ---- second farmer (p2.c, prototype) ---- */
extern int g_p2_enabled, g_p2_demo;
extern uint16_t g_p2_buttons;
void p2_return_tools(void);     /* the save / load screens: farmer 2's things into the storages */
void p2_take_back(void);        /* ... and takes them back if no diary was loaded */
void cheats_tick(void);         /* cheats.c: each farm tick */
#define CHEATS 6                /* money, stamina, clock, rucksack, materials, tools */
void cheats_set(const int v[CHEATS]);   /* ... changed while the game runs */
int config_cheats(int v[CHEATS]);       /* main.c: the ini's cheats, if it changed */
void open_cheats_window(void);  /* main.c: F5, the launcher's Cheats page */
void p2_cheats(int rest, int bag, int tools);   /* ... farmer 2's stamina, rucksack and tools */
void p2_leave(void);            /* farmer 2's player left: its things into the storages, farmer 2 gone */
size_t p2_state_size(void);
void p2_save_state(void *dst);
void p2_load_state(const void *src);
void p2_aim_view(void);         /* the second view: the view on the other farmer */
int p2_owner(void);             /* farmer 2 has the game: the windows are swapped */
int p2_menu(void);              /* whose talk or menu is open: 0 the player's, 1 farmer 2's, -1 none */
int p2view_swapped(void);       /* farmer 2's menu: the game's picture goes to farmer 2's window */

/* ---- the second view (p2view.c): a second window that follows farmer 2 ---- */
extern int g_view2;               /* on (--p2 on this PC, or --p2view 1) */
extern int g_view2_run;           /* the farm's frame is being run once more for it */
void p2view_init(void);
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void p2view_abort(void);          /* that run did what it can't (CD, pad, ...): drop it */
void p2view_events(CPU *c);       /* events_run during that run: none, and a time limit */
const uint8_t *p2view_ram(void);  /* the RAM the second view's frame was built in (NULL: held) */
int p2view_held(void);             /* the last run was dropped: the second window keeps its picture */
size_t dma_state_size(void);
void dma_save_state(void *dst);
void dma_load_state(const void *src);
size_t sched_state_size(void);
void sched_save_state(void *dst);
void sched_load_state(const void *src);
int quick_state(void);
void quick_set_state(int s);

/* ---- online co-op (netplay.c) ---- */
void netplay_host(int port);
void netplay_join(const char *address);
void netplay_set_delay(int frames);
int netplay_role(void);           /* 0 off, 1 host, 2 guest */
int netplay_active(void);
int netplay_start(int *bank, const char *guest_dir);
uint16_t netplay_frame(int frame, uint16_t local);
const char *netplay_status(int *changed);
unsigned netplay_build_id(void);  /* the co-op version: the same on any PC built from the same sources */
void netplay_quit(void);

/* ---- config ---- */
typedef struct {
    char disc_path[1024];
    char save_dir[1024];
    int  scale;                 /* window scale */
    int  upscale;               /* internal resolution multiplier 1..4 (1 = original) */
    int  draw_threads;          /* threads drawing the upscaled polygons: -1 = by processor, 0 = none */
    int  p2view_upscale;        /* farmer 2's window: internal resolution, lower = lighter (0 = the game's) */
    int  pgxp;                  /* precise 3D vertex positions (needs upscale >= 2) */
    int  perspective;           /* perspective-correct textures (needs pgxp) */
    int  sprite_depth;          /* scale 2D objects with depth (needs pgxp) */
    int  crackfix;              /* close the cracks between models (needs pgxp) */
    int  interp;                /* frame interpolation prototype: in-between pictures (needs pgxp) */
    int  texturepack;           /* replacement images from textures/ (needs upscale >= 2) */
    int  dumptextures;          /* write 2D sprites' textures to textures/dump/ (needs upscale >= 2) */
    int  ws_w, ws_h;            /* widescreen aspect W:H (e.g. 16:9), 0 = off */
    int  fullscreen;
    int  headless;              /* no window / audio (testing) */
    int  max_frames;            /* exit after N frames (testing) */
    int  unthrottled;
    int  screenshot_every;      /* dump frames to disk every N frames (testing) */
    int  screenshot_start;      /* ... from this frame on (testing) */
    char screenshot_dir[1024];
    char input_script[65536];    /* testing: scripted input "frame:BUTTONS,frame:..." or "@FILE" */
    char record_path[1024];      /* testing: write the pad input of the session here */
    int  record;                /* record every session to recordings/ (bug reports) */
    int  fast_cd;               /* faster CD data reads (not XA streaming): 1 = 4x, N >= 2 = N times */
    int  autoslot;              /* save/load screens use MEMORY CARD slot 1 without asking */
    int  fastcard;              /* full speed while a save/load screen waits for the card */
    int  quickui;               /* windows open and close at once, shorter fades */
    int  fastload;              /* full speed while "Now Loading" is shown */
    int  skipscenes;            /* holding START in a cutscene runs it at full speed */
    int  textspeed;             /* characters typed per step of the text printer (1 = original) */
    int  overclock;             /* CPU speed in percent of the console's (100 = original) */
    int  emptybag;              /* R1 / R2 do nothing with no tool / item to switch to */
    int  cheat_money;           /* cheats (cheats.c): the money on the farm the first time (0 = off) */
    int  cheat_stamina;         /* the farmers never tire */
    int  cheat_clock;           /* the clock stands still */
    int  cheat_bag;             /* the biggest rucksack */
    int  cheat_items;           /* lumber, fodder, fish food and chicken feed at 999 */
    int  cheat_tools;           /* the tools' levels at their most */
    int  scaling;               /* SCALING_*: how the picture is scaled to the window */
    int  aspect_4_3;
    int  allow_fast_forward;     /* hold Tab to run unthrottled */
    int  dump_frame[8];          /* testing: dump RAM at these frames */
    char wav_path[1024];         /* testing: write audio output to a WAV file */
    int  fuzz_seed;              /* testing: random input (0 = off) */
    int  poke_frame[4]; uint32_t poke_addr[4], poke_val[4];   /* testing: 16-bit I/O writes */
    int  call_frame[4]; uint32_t call_addr[4], call_a0[4];   /* testing: call guest functions at frames */
    int  fuzz_start;             /* testing: frame at which random input starts */
} Config;
enum { SCALING_SHARP_BILINEAR, SCALING_SHARP, SCALING_SMOOTH };   /* Config.scaling; 0 = default */
extern Config g_cfg;
