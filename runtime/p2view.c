/* The second view (prototype): with a second farmer on one PC (--p2) a second window follows
 * farmer 2, as the game's window follows the player.
 *
 * The farm's frame (800C162C's caller, 800C14A0: the tick, the drawing and the HUD) builds the
 * frame's lists of what to draw in RAM; the main loop has the GPU draw them later. Each time it
 * starts, the frame is first run once more with the view on farmer 2 (p2_aim_view, where the walk
 * aims it at the player), and everything it changed is put back: the RAM and the scratchpad, the
 * CPU and the GTE, the timers and events, the DMA, the state the hooks keep (p2.c, quickui.c). So
 * the game goes on as without it. What that run sends the GPU goes to a second GPU only
 * (gpu_view2.c, g_gpu_route 2), and the RAM it left is kept: when the main loop has the GPU draw
 * the lists, the second GPU draws them from that RAM (dma.c). Everything else the GPU is sent
 * goes to both (g_gpu_route 1), so the second GPU has the same VRAM, display and drawing area.
 *
 * During that run no event or interrupt is run (the time it takes is put back too), the sound
 * chip is not written, and the CD, the pads and memory cards are not touched: a run that does
 * (or runs for too long: a wait for something that does not come) is dropped. Some scripts load a
 * file from the CD in the frame, every other frame for seconds: then the second window keeps
 * farmer 2's last picture (for up to HOLD frames; after that it shows what the first does).
 * The frame's CD loads (8002194C, after what is drawn is built) are left out of that run: at 12:00
 * and after some talks they wait for the disc every frame for seconds, and every run was dropped
 * (the second window stood still till the player went somewhere else). */
#include "psx.h"
#include "gpu.h"
#include "gte.h"
#include <setjmp.h>

int g_view2 = -1;                   /* -1: on with --p2 when not online (main.c); 2: online guest, in the game's window */
int g_view2_run;

#define FRAME 0x800C14A0u
#define BUDGET 2000000u             /* cycles a run may take (the farm's frame takes up to about 330000) */
#define HOLD 120                    /* frames the second window keeps its picture after a dropped run */

static uint8_t *ram0, *built;       /* the RAM before the run; the RAM the run left */
static uint8_t scratch0[sizeof g_scratch];
static uint8_t *gte0, *sched0, *dma0, *p20;
static CPU cpu0;
static int quick0, hud0, held0, item0, menu0;
static int built_ok, last_dropped;
static uint64_t built_frame, deadline;
static uint64_t tried_frame;        /* the farm's frame last tried a run (dropped or not) */
static jmp_buf jb;
static unsigned runs, dropped;

static int p2view_built(void);

void p2view_init(void) {
    if (g_view2 <= 0) return;
    ram0 = malloc(sizeof g_ram);
    built = malloc(sizeof g_ram);
    gte0 = malloc(gte_state_size());
    sched0 = malloc(sched_state_size());
    dma0 = malloc(dma_state_size());
    p20 = malloc(p2_state_size());
    if (!ram0 || !built || !gte0 || !sched0 || !dma0 || !p20) {
        LOGW("second view: not enough memory");
        g_view2 = 0;
        return;
    }
    /* its own internal resolution (p2view_upscale): lower is lighter, for a slower PC; the
     * precise positions need 2 or more, as for the game's */
    int up = gpu_upscale();
    if (g_cfg.p2view_upscale > 0 && g_cfg.p2view_upscale < up) up = g_cfg.p2view_upscale;
    view2_gpu_init();
    view2_g_pgxp_cracks = g_pgxp_cracks;
    view2_gpu_set_upscale(up);
    view2_gpu_set_threads(gpu_threads());   /* as many as the game's GPU draws with */
    view2_g_pgxp = g_pgxp && view2_gpu_upscale() > 1;
    view2_g_pgxp_persp = g_pgxp_persp && view2_g_pgxp;
    view2_g_sprite_depth = g_sprite_depth && view2_g_pgxp;
    view2_g_sprite_ws = g_sprite_ws;
    view2_g_sprite_tags = g_sprite_tags;
    view2_g_scene_3d = g_scene_3d;
    if (g_interp) view2_gpu_interp_enable();   /* 60 pictures a second, as the game's window */
    g_view2_built = p2view_built;
    g_gpu_route = 1;
    LOGI("second view: a window for farmer 2 (internal resolution x%d)", view2_gpu_upscale());
}

static void save(CPU *c) {
    memcpy(ram0, g_ram, sizeof g_ram);
    memcpy(scratch0, g_scratch, sizeof g_scratch);
    cpu0 = *c;
    gte_save_state(gte0);
    sched_save_state(sched0);
    dma_save_state(dma0);
    p2_save_state(p20);
    quick0 = quick_state();
    hud0 = g_hud_call;
    held0 = g_held_call;
    item0 = g_item_call;
    menu0 = g_menu_tile;
}

static void load(CPU *c) {
    memcpy(g_ram, ram0, sizeof g_ram);
    memcpy(g_scratch, scratch0, sizeof g_scratch);
    *c = cpu0;
    gte_load_state(gte0);
    sched_load_state(sched0);
    dma_load_state(dma0);
    p2_load_state(p20);
    quick_set_state(quick0);
    g_hud_call = hud0;
    g_held_call = held0;
    g_item_call = item0;
    g_menu_tile = menu0;
}

/* entry of the farm's frame */
void p2view_frame(CPU *c) {
    if (g_view2 <= 0 || g_view2_run) return;
    if (p2_menu() >= 0) return;     /* a talk or menu: the game stands still, the windows are held */
    tried_frame = frame_count();
    save(c);
    g_view2_run = 1;
    g_gpu_route = 2;
    deadline = c->cycles + BUDGET;
    c->next_event = deadline;
    if (!setjmp(jb)) {
        call_addr(c, FRAME);
        memcpy(built, g_ram, sizeof g_ram);
        built_ok = 1;
        built_frame = frame_count();
        last_dropped = 0;
    } else {
        last_dropped = 1;
        dropped++;
    }
    g_view2_run = 0;
    g_gpu_route = 1;
    load(c);
    if (++runs % 1800 == 0) LOGD("second view: %u runs, %u dropped", runs, dropped);
}

void p2view_abort(void) {
    LOGD("second view: run dropped (func %08X)", g_cpu.cur_func);
    longjmp(jb, 1);
}

void p2view_events(CPU *c) {
    if (c->cycles >= deadline) p2view_abort();
    c->next_event = deadline;
}

/* The game's picture lags the game: when farmer 2's menu opens, the view moves to farmer 2 a tick
 * later and the picture shows it two frames after that; when it closes, the picture keeps the menu
 * for a few frames and then shows the player's view. So the windows swap SWAP_IN frames after the
 * menu opens (till then the second window keeps its picture) and swap back SWAP_OUT frames after it
 * closes (the picture jumped to the player at each open and close) */
#define SWAP_IN 5
#define SWAP_OUT 4
static uint64_t tracked, menu_seen, mine_since, swap_end;
static int swapped;
static void track(void) {
    uint64_t now = frame_count();
    if (tracked == now) return;
    tracked = now;
    p2_scene();                     /* followed every frame (it notes when a talk or menu began) */
    int m = p2_menu();
    if (m >= 0) menu_seen = now;
    if (m != 1) mine_since = 0;
    else if (!mine_since) mine_since = now;
    if (mine_since && now - mine_since >= SWAP_IN) {
        swapped = 1;
        swap_end = now + SWAP_OUT;
    } else if (now >= swap_end) {
        swapped = 0;
    }
}

/* the lists the main loop draws were built by the farm's frame; when it ran last, not long ago.
 * NULL: the last run was dropped, the second window keeps its picture */
const uint8_t *p2view_ram(void) {
    track();
    if (p2_menu() >= 0) return built_ok ? NULL : g_ram;    /* a talk or menu: held */
    if (!built_ok) return g_ram;
    /* While the farm's frame keeps trying runs and they are dropped (after a talk with an animal,
     * in a shop: a script waits for the disc every frame, for seconds), the picture is held as
     * long as it takes: showing the game's lists instead showed the player's view (the camera
     * jumped to the player and back). Only when the farm's frame is not run (another screen) is
     * the game's picture shown after HOLD frames. The first frames after a talk or menu, before
     * the next run, are held too */
    uint64_t now = frame_count(), age = now - built_frame;
    int trying = now - tried_frame <= 4;
    if (built_frame < menu_seen) return trying || now - menu_seen <= HOLD ? NULL : g_ram;
    if (last_dropped) return trying || age <= HOLD ? NULL : g_ram;
    return age <= 4 ? built : g_ram;
}

static int p2view_built(void) {
    return p2view_ram() != g_ram;
}

int p2view_held(void) {
    return g_view2 > 0 && !p2view_ram();
}

/* Each window shows its own farmer's talks and menus. The game stands still while one is open, so
 * the other farmer's window keeps its last picture: in the player's, the second window is held
 * (no run); in farmer 2's, the game's picture (its view on farmer 2 then) goes to farmer 2's window
 * and the game's window keeps its own (platform_sdl.c). */
/* a scene no farmer started (p2.c): farmer 2's window shows the game's picture too */
int p2view_scene(void) {
    return g_view2 > 0 && p2_scene();
}

int p2view_swapped(void) {
    track();
    return g_view2 > 0 && swapped;
}
