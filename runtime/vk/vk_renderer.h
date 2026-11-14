/* Experiment (renderer = vulkan): parallel-psx's Vulkan PS1 renderer draws the picture shown, fed
 * from gpu.c's command handling. gpu.c keeps drawing the native VRAM the game reads, unless
 * g_vk_cpu_off (HMBTN_VK_CPU_OFF=1), which skips its drawing and reads the GPU's VRAM back instead. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

extern int g_vk;            /* parallel-psx draws the picture at this scale (0 = off) */
extern int g_vk_cpu_off;    /* gpu.c doesn't draw (polygons, rectangles, lines) */

/* How a tagged primitive moves (widescreen, sprite anchors and depth, gpu.c's sprite_coord): a
 * point p (drawing offset included) goes to c + d + (p - c) * s; x, y: a rectangle's corner;
 * clip0 .. clip1: the upscaled columns a menu tile's copy may draw to (clip1 <= clip0: any);
 * z: the anchor's depth for the depth test (0: none; gpu.c sprite_depth_tag) */
typedef struct { float cx, cy, dx, dy, sx, sy; int32_t x, y, clip0, clip1; float z; } VkXform;

/* A texture pack image drawn instead of the texels (gpu.c texpack_box / texpack_quad): its pixel
 * for texel (u, v) is ((u - u0) * su, (v - v0) * sv); key: the image's key in the pack */
typedef struct { uint64_t key; const uint16_t *px; int w, h; float u0, v0, su, sv; } VkRep;

int  vkr_init(int scale);                   /* 0: no Vulkan */
/* a GP0 command (all of its words) after gpu.c ran it (prec: a polygon's PGXP corners, x, y, depth
 * each, depth 0: none): drawn once per move in xf (0: as it is,
 * -1: not at all: a menu tile none of whose copies is shown);
 * edge: a map screen quad whose sides are filled to the drawing area's edges (edge_wide) */
/* under: a moved 3D polygon's console corners (x, y each, drawing offset included): drawn once more
 * there, only where the screen clear still shows (the crack fix) */
void vkr_gp0(const uint32_t *p, const VkXform *xf, int nxf, int edge, const VkRep *rep, const float *prec,
             const int32_t *under);
void vkr_polyline(uint32_t w);              /* a word of a polyline after its first command */
void vkr_upload(int x, int y, int w, int h, const uint16_t *vram);    /* CPU -> VRAM done: that box of vram */
void vkr_download(int x, int y, int w, int h, uint16_t *vram);        /* the game's VRAM -> CPU (GP0 C0h) */
void vkr_sync(int x, int y, int w, int h, uint16_t *vram);            /* that box of vram from the GPU's */
void vkr_display(uint32_t *out, int *w, int *h, int x, int y, int dw, int dh);   /* the display area, scaled */
void vkr_vblank(int shown, int x, int y, int dw, int dh);   /* shown: that display area (starts its picture) */
void vkr_kick(int x, int y, int dw, int dh);   /* start the picture of that display area now */

/* Direct display: window win (0 the game's, 1 farmer 2's) shows the pictures from the GPU (0: can't).
 * kind: 0 Win32 (a, b: HINSTANCE, HWND), 1 X11 (Display *, Window), 2 Wayland (wl_display *,
 * wl_surface *). vkr_window_size: its size in pixels, before each vkr_present (Wayland needs it). vkr_display then reads pictures back only when pixels are wanted
 * (vkr_want_pixels: screenshots); vkr_keep(slot) keeps the picture it made last for vkr_present
 * (0: there was none, it was the CPU's; slot -1 forgets it), and vkr_present shows slot's picture,
 * or px (w x h, the CPU's: a video), letterboxed at aspect, scaled by scaling (SCALING_*). */
int  vkr_window(int win, int kind, void *a, void *b);
void vkr_window_size(int win, int w, int h);
void vkr_vsync(int win, int on);    /* window win waits for the screen's refresh (FIFO) */
void vkr_want_pixels(int want);
int  vkr_keep(int slot);
void vkr_present(int win, int slot, const uint32_t *px, int w, int h, double aspect, int scaling);
/* HD images (gpu.h HdPiece): pieces of an image (px: as texels, 0 transparent; key: which) shown
 * over the next vkr_present of window win's picture (x0..y1: in its pixels), at the window's
 * resolution; only over a picture of the GPU's (slot) */
typedef struct { uint64_t key; const uint16_t *px; int w, h; float x0, y0, x1, y1, u0, v0, u1, v1; } VkHdPiece;
void vkr_overlay(int win, const VkHdPiece *p, int n);
/* 0: the game's renderer, 1: the one in-between pictures are redrawn into (2, 3: their tags; 4 ..
 * 7: the same for the second view's GPU, gpu_view2.c); the calls above go to it */
void vkr_target(int k);
int  vkr_enter(int set);    /* to set's renderers (0 or 4) if not on one of them already; returns the one before */

#ifdef __cplusplus
}
#endif
