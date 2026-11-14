/* Experiment: parallel-psx (Themaister's Vulkan PS1 renderer, ISC/MIT, ppsx/) draws the picture.
 * gpu.c hands it every GP0 command it runs (vkr_gp0), the VRAM boxes the CPU uploads (vkr_upload),
 * and asks it for the display area (vkr_display) and, with g_vk_cpu_off, for VRAM the game reads
 * back (vkr_download). Command decoding follows gpu.c (psx-spx); written for this port. */
#include "renderer.hpp"
#include "vk_renderer.h"

#include <chrono>
#include <map>
#include <tuple>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

extern "C" void log_msg(int level, const char *fmt, ...);
extern "C" int g_pgxp_persp, g_pgxp_cracks;
extern "C" double g_depth_margin;

using namespace Vulkan;
using namespace PSX;

/* what the renderer expects from its libretro core */
namespace Granite
{
static void vk_log(enum retro_log_level level, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	size_t n = strlen(buf);
	if (n && buf[n - 1] == '\n')
		buf[n - 1] = 0;
	log_msg(level >= RETRO_LOG_ERROR ? 0 : level == RETRO_LOG_WARN ? 1 : 3, "vk: %s", buf);
}
retro_log_printf_t libretro_log = vk_log;
}
retro_log_printf_t log_cb = Granite::vk_log;
retro_input_state_t dbg_input_state_cb = nullptr;
char retro_cd_base_name[4096];
char retro_cd_base_directory[4096];

int g_vk, g_vk_cpu_off;

static VkInstance instance;
static Context *context;
static Device *device;
static Renderer *renderer;
static int scale = 1;

/* GP0 state the renderer needs per primitive (E1 / polygon texpage, E2 ..) */
static struct
{
	unsigned tp_x, tp_y, abr, tex_mode, flip_x, flip_y;
	int offx, offy, clip_x0, clip_x1;   /* E5 drawing offset, E3/E4 drawing area left and right */
	unsigned e3x, e3y, e4x, e4y;        /* E3/E4 as sent */
	bool mask_set, mask_check;          /* E6 as sent */
	int pl_gouraud, pl_abe, pl_count;
	uint32_t pl_words[2];
	Vertex pl_prev;
} st;

/* what the game does (HMBTN_VK_STATS=1): counts, logged every 600 VBlanks */
static struct
{
	unsigned under, poly, rect, line, fill, copy, upload, upload_px, read, read_px, sync, flip, mask, wrap, cull, precise, vtx;
} cnt;
static int stats, vblanks;
static Rect draw_rect;       /* E3/E4, as the renderer has it */
static bool drawn = true;   /* anything drawn or loaded since the kick */

/* The seam pass (crack fix, gpu.c seam_across / seam_down) needs to know what drew each pixel
 * last: a tag renderer for each picture renderer (2: the game's, 3: the in-between one) is sent
 * the same commands in tag mode, drawing each opaque primitive flat in its tag's colour (0x4000:
 * a moved 3D polygon, low 14 bits: the command's number, as gpu.c's cur_tag); see-through ones
 * (but a textured one's opaque texels: STP clear, tag_stp) and the crack fix's second drawing
 * leave the tags. Textured ones are drawn with the tag renderer's shaders (ppsx/glsl
 * primitive.frag, TAG): their tag where a texel shows, so transparent texels leave the tags as in gpu.c. */
static int cur_inst;        /* the renderer the calls go to (vkr_target) */
static Renderer *renderers[8];  /* all of them: each is flushed at the VBlank (its frame's pools are reused) */
static bool tagging;
static uint32_t tag_now;
static void use(int k);
/* per picture renderer and tag, a moved 3D polygon's plane of 1/w over native pixels (drawing offset
 * included): the seam pass fills a run only where both sides are in front of it (gpu.c seam_plane) */
static float qpl[8][0x4000][4];
/* The depth test (depthtest = 1, gpu.c plot_t), per pixel: the renderers draw in the game's order
 * with a depth buffer of 1/w (Renderer::set_ordered). A solid precise 3D triangle is drawn where
 * nothing drawn before it in this layer is more than the margin nearer and leaves its depth (the
 * farther of a quad's two planes there, as gpu.c); a 2D rectangle at a projected point leaves its
 * anchor's; other solid pixels leave none (0). Depth stored as DT_K / w (in 0..1 for w >= DT_K). */
static const float DT_K = 1.0f / 128.0f;
static float dt_last_w[8] = { 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f, 1e30f };
static bool dt_failed[8];   /* set_ordered failed (no depth/stencil format): the depth test off there */
struct DtPoly
{
	int kind[2];            /* per triangle: 0 none, 1 written only (a flat plane: as a 2D object), 2 tested */
	float z[4], zw[4], zo[4];   /* per corner: tested at, written if flat, the other triangle's plane */
};

static inline bool seams()     /* on a picture renderer (0, 1, 4, 5): its tags are on k + 2 */
{
	return g_pgxp_cracks && !(cur_inst & 2);
}

static uint32_t tag_color(bool d3)
{
	uint32_t t = (d3 ? 0x4000u : 0u) | (tag_now & 0x3FFFu), c = 0;
	for (int k = 0; k < 3; k++)     /* 15 bits as 5-bit channels, each in the middle of its step: they
	                                 * reach the seam pass as 15-bit colour, about the top 5 bits (v * 255
	                                 * / 31 lost 1 at 8, 28 at 233) */
		c |= (((t >> (5 * k)) & 31u) * 8u + 4u) << (8 * k);
	return c;
}

static inline int32_t sext11(uint32_t v)
{
	return (int32_t)((v & 0x7FFu) ^ 0x400u) - 0x400;
}

/* coordinate p (renderer's: without the drawing offset) of a tagged primitive, moved by gpu.c's
 * sprite_coord: about c by s after the anchor's offset d, on the upscaled pixel grid. Less 1/256
 * of a pixel: scaled (the HUD squeezed in widescreen, at upscale 2, 3, 5, 6), a pixel's centre can
 * fall on a texel's edge, where the GPU took one texel or the other depending on where on the
 * screen it drew (the two display buffers: the menu's icons moved a pixel every other frame);
 * this way it is the one gpu.c's draw_sprite_scaled takes (floor). The edges stay between pixels. */
static float moved(float p, int off, float c, float d, float s)
{
	return (rintf((c + d + (p + off - c) * s) * scale) - 1.0f / 256.0f) / scale - off;
}

extern "C" int vkr_init(int s)
{
	if (volkInitialize() != VK_SUCCESS)
	{
#ifdef _WIN32
		log_msg(1, "vk: no Vulkan loader (vulkan-1.dll)");
#else
		log_msg(1, "vk: no Vulkan loader (libvulkan.so.1)");
#endif
		return 0;
	}
	VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
	app.pApplicationName = "HarvestMoonBTN";
	app.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	ici.pApplicationInfo = &app;
	/* windows to show pictures in (vkr_window), when the driver has them */
#ifdef _WIN32
	static const char *surface_ext[] = { "VK_KHR_surface", "VK_KHR_win32_surface" };
#else
	static const char *surface_ext[] = { "VK_KHR_surface", "VK_KHR_xlib_surface", "VK_KHR_wayland_surface" };
#endif
	uint32_t n = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
	std::vector<VkExtensionProperties> exts(n);
	vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
	std::vector<const char *> on;      /* the ones the driver has */
	for (const char *want : surface_ext)
		for (auto &e : exts)
			if (!strcmp(e.extensionName, want))
			{
				on.push_back(want);
				break;
			}
	bool windows = on.size() >= 2 && on[0] == surface_ext[0];   /* VK_KHR_surface and a window kind */
	if (windows)
	{
		ici.enabledExtensionCount = uint32_t(on.size());
		ici.ppEnabledExtensionNames = on.data();
	}
	if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS)
	{
		log_msg(1, "vk: vkCreateInstance failed");
		return 0;
	}
	volkLoadInstance(instance);
	static const VkPhysicalDeviceFeatures none = {};   /* it turns on what it can use */
	static const char *swap_ext[] = { "VK_KHR_swapchain" };
	context = new Context(instance, VK_NULL_HANDLE, VK_NULL_HANDLE, swap_ext, windows, nullptr, 0, &none);
	if (!context->is_valid() && windows)    /* no swapchains: pictures go through the CPU */
	{
		delete context;
		context = new Context(instance, VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr, 0, nullptr, 0, &none);
	}
	if (!context->is_valid())
		return 0;
	device = new Device;
	device->set_context(*context);
	scale = s;
	renderer = renderers[0] = new Renderer(*device, scale, 1, nullptr);
	if (!renderer->is_valid())
		return 0;
	renderer->set_adaptive_smoothing(false);
	renderer->set_filter_mode(Renderer::FilterMode::NearestNeighbor);
	renderer->set_draw_rect({ 0, 0, 0, 0 });
	renderer->set_texture_window({ 0xFF, 0xFF, 0, 0 });
	renderer->toggle_display(true);
	stats = getenv("HMBTN_VK_STATS") != nullptr;
	g_vk_cpu_off = getenv("HMBTN_VK_CPU_OFF") != nullptr;
	log_msg(2, "vk: parallel-psx at x%d on %s%s", scale, device->get_gpu_properties().deviceName,
	        g_vk_cpu_off ? " (CPU drawing off)" : "");
	g_vk = scale;
	return 1;
}

static void set_texpage(uint32_t v)
{
	st.tp_x = (v & 0xFu) * 64u;
	st.tp_y = (v & 0x10u) ? 256u : 0u;
	st.abr = (v >> 5) & 3u;
	st.tex_mode = (v >> 7) & 3u;
}

static void prim_state(int tex, int abe, int raw, uint32_t clut, PrimitiveType type)
{
	static const TextureMode modes[4] = { TextureMode::Palette4bpp, TextureMode::Palette8bpp, TextureMode::ABGR1555,
		                                  TextureMode::ABGR1555 };
	static const SemiTransparentMode blend[4] = { SemiTransparentMode::Average, SemiTransparentMode::Add,
		                                          SemiTransparentMode::Sub, SemiTransparentMode::AddQuarter };
	renderer->set_texture_mode(tex ? modes[st.tex_mode] : TextureMode::None);
	renderer->set_texture_offset(st.tp_x, st.tp_y);
	renderer->set_palette_offset((clut & 0x3Fu) << 4, (clut >> 6) & 0x1FFu);
	renderer->set_semi_transparent(abe ? blend[st.abr] : SemiTransparentMode::None);
	renderer->set_texture_color_modulate(!raw);
	renderer->set_primitive_type(type);
	/* the crack fix: opaque drawing sets the mask bit (unused by this game), so where it is still
	 * clear the screen clear shows (see-through drawing leaves it, as gpu.c's fill marks). Not
	 * rectangles: with it the rain's showed as streaks over water in the game's pictures only (not
	 * in the in-between ones: they flickered), and a polygon's second drawing goes over earlier
	 * rectangles only along its own edge, where it is in front of them anyway */
	renderer->set_force_mask_bit(!tagging && (st.mask_set || (g_pgxp_cracks && !abe && type != PrimitiveType::Sprite)));
	renderer->set_mask_test(!tagging && st.mask_check);
}

static void uv_limits(const Vertex *v, int n)
{
	uint16_t u0 = v[0].u, v0 = v[0].v, u1 = v[0].u, v1 = v[0].v;
	for (int i = 1; i < n; i++)
	{
		u0 = std::min(u0, v[i].u);
		u1 = std::max(u1, v[i].u);
		v0 = std::min(v0, v[i].v);
		v1 = std::max(v1, v[i].v);
	}
	renderer->set_UV_limits(u0, v0, u1, v1);
}

/* the GPU skips a triangle 1024 or more pixels wide or 512 or more high */
static bool culled(const Vertex &a, const Vertex &b, const Vertex &c)
{
	float x0 = std::min(a.x, std::min(b.x, c.x)), x1 = std::max(a.x, std::max(b.x, c.x));
	float y0 = std::min(a.y, std::min(b.y, c.y)), y1 = std::max(a.y, std::max(b.y, c.y));
	return x1 - x0 >= 1024.0f || y1 - y0 >= 512.0f;
}

/* Texture packs: the image a replaced sprite samples instead of its texels (the renderer's HD
 * texture path), made at the output scale over its texel box u_lo..v_hi (widened to whole VRAM
 * halfwords), the pack's pixels picked as gpu.c does (rep_at). Kept for later frames. */
static std::map<std::tuple<uint64_t, unsigned, unsigned, unsigned, unsigned, float, float, float, float>, ImageHandle>
    rep_images;

static void rep_override(const VkRep *r, unsigned u_lo, unsigned v_lo, unsigned u_hi, unsigned v_hi)
{
	unsigned shift = st.tex_mode == 0 ? 2 : st.tex_mode == 1 ? 1 : 0;
	unsigned ua = (u_lo >> shift) << shift, nu = (((u_hi >> shift) + 1) << shift) - ua, nv = v_hi - v_lo + 1;
	unsigned W = nu * scale, H = nv * scale;
	auto key = std::make_tuple(r->key, ua, v_lo, nu, nv, r->u0, r->v0, r->su, r->sv);
	if (rep_images.size() > 1024 && !rep_images.count(key))
		rep_images.clear();     // ponytail: dropped all at once; an LRU if packs get big
	ImageHandle &img = rep_images[key];
	if (!img)
	{
		std::vector<uint32_t> px(size_t(W) * H);
		for (unsigned y = 0; y < H; y++)
		{
			int iy = std::min(std::max(int((v_lo + (y + 0.5f) / scale - r->v0) * r->sv), 0), r->h - 1);
			for (unsigned x = 0; x < W; x++)
			{
				int ix = std::min(std::max(int((ua + (x + 0.5f) / scale - r->u0) * r->su), 0), r->w - 1);
				uint32_t t = r->px[iy * r->w + ix], c = 0;   /* 0: transparent */
				if (t)
				{
					uint32_t cr = t & 31u, cg = (t >> 5) & 31u, cb = (t >> 10) & 31u;
					c = 0xFF000000u | (cb << 3 | cb >> 2) << 16 | (cg << 3 | cg >> 2) << 8 | (cr << 3 | cr >> 2);
					if (!(c & 0xFFFFFFu))
						c |= 1;     /* opaque black is the HD path's "use the texels" mark */
				}
				px[size_t(y) * W + x] = c;
			}
		}
		ImageInitialData init = { px.data(), 0, 0 };
		img = device->create_image(ImageCreateInfo::immutable_2d_image(W, H, VK_FORMAT_R8G8B8A8_UNORM), &init);
	}
	renderer->set_texture_override({ { int(st.tp_x + (ua >> shift)), int(st.tp_y + v_lo), int(nu >> shift), int(nv) },
	                                 { 0, 0, int(W), int(H) }, img });
}

/* The map screen in widescreen (gpu.c edge_wide): a quad at the drawing area's left or right edge
 * gets its side filled out to that edge with its colour on white: nat, the console's corners
 * (drawing offset included), v, where they are drawn */
static void edge_wide(const Vertex *v, const int32_t *nx, const int32_t *ny, uint32_t col)
{
	int32_t lo = std::min(std::min(nx[0], nx[1]), std::min(nx[2], nx[3]));
	int32_t hi = std::max(std::max(nx[0], nx[1]), std::max(nx[2], nx[3]));
	uint32_t white = 0;
	for (int k = 0; k < 24; k += 8)
	{
		uint32_t c = (col >> k) & 0xFFu;
		white |= (c >= 128 ? 255u : c * 2) << k;
	}
	prim_state(0, 0, 0, 0, PrimitiveType::Polygon);
	for (int side = 0; side < 2; side++)
	{
		int32_t at = side ? hi : lo;
		bool ok = hi > lo && (side ? hi >= st.clip_x1 + 1 : lo <= st.clip_x0);
		Vertex e[4];
		for (int i = 0; i < 4 && ok; i++)
		{
			int k = -1;     /* the corner at this edge in i's row */
			for (int j = 0; j < 4; j++)
				if (ny[j] == ny[i] && nx[j] == at)
					k = j;
			ok = k >= 0;
			if (!ok)
				break;
			e[i] = v[k];
			e[i].color = white;
			e[i].w = 1.0f;
			if (i == k)
				e[i].x = float((side ? st.clip_x1 + 1 : st.clip_x0) - st.offx);
		}
		if (ok)
			renderer->draw_quad(e);
	}
}

/* gpu.c dt_layer: a solid polygon at least 4096 farther on average than the one before starts a new
 * layer: what was drawn before hides nothing */
static void dt_layer(float avg_w)
{
	if (avg_w - dt_last_w[cur_inst] >= 4096.0f)
		renderer->depth_clear();
	dt_last_w[cur_inst] = avg_w;
}

/* A precise 3D polygon's depth (prec: x, y, w per corner; triangles 0 1 2 and 1 2 3) */
static void dt_polygon(DtPoly &d, const float *prec, int nv, int abe)
{
	const float inv = float(1.0 / (1.0 + g_depth_margin));
	if (!abe)
	{
		float a = 0.0f;
		for (int i = 0; i < nv; i++)
			a += prec[i * 3 + 2];
		dt_layer(a / (float)nv);
	}
	for (int i = 0; i < nv; i++)
	{
		d.z[i] = d.zo[i] = DT_K / prec[i * 3 + 2];
		d.zw[i] = d.z[i] * inv;
	}
	for (int t = 0; t < nv - 2; t++)    /* flat (gpu.c: pn[1], pn[2] both 0): not tested */
	{
		const float *c = &prec[t * 3];
		d.kind[t] = c[2] == c[5] && c[2] == c[8] ? 1 : 2;
	}
	d.kind[1] = nv == 4 ? d.kind[1] : d.kind[0];
	static const int other[2][4] = { { 1, 2, 3, 0 }, { 0, 1, 2, 3 } };   /* a plane's corners, and the corner off it */
	for (int t = 0; t < 2 && nv == 4; t++)  /* the other triangle's plane at corners 0 and 3 */
	{
		const int *k = other[t];
		const float *a = &prec[k[0] * 3], *b = &prec[k[1] * 3], *c = &prec[k[2] * 3], *o = &prec[k[3] * 3];
		const double x1 = b[0] - a[0], y1 = b[1] - a[1], x2 = c[0] - a[0], y2 = c[1] - a[1], den = x1 * y2 - x2 * y1;
		if (den == 0.0)
			continue;
		const double q0 = 1.0 / a[2], q1 = 1.0 / b[2] - q0, q2 = 1.0 / c[2] - q0;
		const double q = q0 + (q1 * y2 - q2 * y1) / den * (o[0] - a[0]) + (x1 * q2 - x2 * q1) / den * (o[1] - a[1]);
		if (q > 0.0)
			d.zo[k[3]] = std::min(d.z[k[3]], float(DT_K * q));
	}
}

/* the depth for the next draw: triangle t (0, 1; -1: the quad) of polygon d (nullptr: none) */
static void dt_set(const DtPoly *d, int t)
{
	renderer->dt_kind = d ? d->kind[t < 0 ? 0 : t] : 0;
	for (int i = 0, o = t == 1 ? 1 : 0; d && i + o < 4; i++)
	{
		renderer->dt_z[i] = renderer->dt_kind == 2 ? d->z[i + o] : d->zw[i + o];
		renderer->dt_zo[i] = d->zo[i + o];
	}
}

/* a polygon's triangles a (0 1 2) and b (1 2 3), drawn alike when they are */
static void dt_draw(const DtPoly *d, const Vertex *v, bool a, bool b)
{
	if (a && b && (!d || d->kind[0] == d->kind[1]))
	{
		dt_set(d, -1);
		renderer->draw_quad(v);
	}
	else
	{
		if (a)
		{
			dt_set(d, 0);
			renderer->draw_triangle(v);
		}
		if (b)
		{
			dt_set(d, 1);
			renderer->draw_triangle(v + 1);
		}
	}
	dt_set(nullptr, 0);
}

static void polygon(const uint32_t *p, const VkXform *xf, int edge, const VkRep *rep, const float *prec,
                    const int32_t *under)
{
	uint32_t op = p[0] >> 24;
	int gouraud = (op >> 4) & 1, quad = (op >> 3) & 1, tex = (op >> 2) & 1, abe = (op >> 1) & 1, raw = op & 1;
	int nv = quad ? 4 : 3, idx = 0;
	uint32_t clut = 0;
	Vertex v[4];
	int32_t nx[4], ny[4];
	for (int i = 0; i < nv; i++)
	{
		uint32_t col = (i == 0 || gouraud) ? p[idx++] : p[0];
		uint32_t xy = p[idx++];
		nx[i] = sext11(xy);
		ny[i] = sext11(xy >> 16);
		v[i] = { float(nx[i]), float(ny[i]), 0.0f, col & 0xFFFFFFu, 0, 0 };
		if (prec && prec[i * 3 + 2] > 0.0f)     /* PGXP: the precise position (and depth) */
		{
			v[i].x = prec[i * 3];
			v[i].y = prec[i * 3 + 1];
			v[i].w = prec[i * 3 + 2];
			cnt.precise++;
		}
		else if (xf)    /* a 2D sprite quad at a projected point, or HUD */
		{
			v[i].x = moved(v[i].x, st.offx, xf->cx, xf->dx, xf->sx);
			v[i].y = moved(v[i].y, st.offy, xf->cy, xf->dy, xf->sy);
		}
		nx[i] += st.offx;
		ny[i] += st.offy;
		cnt.vtx++;
		if (tex)
		{
			uint32_t uv = p[idx++];
			v[i].u = uv & 0xFFu;
			v[i].v = (uv >> 8) & 0xFFu;
			if (i == 0)
				clut = uv >> 16;
			else if (i == 1)
				set_texpage(uv >> 16);
		}
	}
	bool persp = g_pgxp_persp;     /* perspective-correct only with every corner's depth */
	for (int i = 0; i < nv; i++)
		persp = persp && v[i].w > 0.0f;
	for (int i = 0; i < nv && !persp; i++)
		v[i].w = 1.0f;
	DtPoly dtp = {}, *dt = nullptr;
	if (renderer->ordered && !xf && prec)
	{
		bool d3 = true;
		for (int i = 0; i < nv; i++)
			d3 = d3 && prec[i * 3 + 2] > 0.0f;
		if (d3)
			dt_polygon(*(dt = &dtp), prec, nv, abe);
	}
	if (tagging)    /* its tag where its texels show (the tag renderer's shaders), as the picture maps them */
	{
		if (abe && !tex)
			return;
		bool d3 = !xf && prec;
		for (int i = 0; i < nv && d3; i++)
			d3 = prec[i * 3 + 2] > 0.0f;
		float *pl = qpl[cur_inst & ~2][tag_now & 0x3FFFu];
		pl[0] = pl[1] = pl[2] = 0.0f;
		for (int k = 0; d3 && k <= quad; k++)   /* the first three corners, or the last three */
		{
			const float *a = &prec[k * 3], *b = &prec[k * 3 + 3], *c = &prec[k * 3 + 6];
			double x1 = b[0] - a[0], y1 = b[1] - a[1], x2 = c[0] - a[0], y2 = c[1] - a[1], den = x1 * y2 - x2 * y1;
			if (den == 0.0)
				continue;
			double q0 = 1.0 / a[2], q1 = 1.0 / b[2] - q0, q2 = 1.0 / c[2] - q0;
			double qx = (q1 * y2 - q2 * y1) / den, qy = (x1 * q2 - x2 * q1) / den;
			pl[0] = float(q0 - qx * (a[0] + st.offx) - qy * (a[1] + st.offy));
			pl[1] = float(qx);
			pl[2] = float(qy);
			break;
		}
		for (int i = 0; i < nv; i++)
			v[i].color = tag_color(d3);
		prim_state(tex, 0, 0, clut, PrimitiveType::Polygon);
		if (tex)
			uv_limits(v, nv);
		renderer->tag_stp = abe;    /* see-through: its opaque texels (STP clear) only, as gpu.c */
		dt_draw(dt, v, !culled(v[0], v[1], v[2]), quad && !culled(v[1], v[2], v[3]));
		renderer->tag_stp = false;
		return;
	}
	cnt.poly++;
	prim_state(tex, abe, tex && raw, clut, PrimitiveType::Polygon);
	if (tex)
		uv_limits(v, nv);
	if (edge && quad)
		edge_wide(v, nx, ny, p[0]);
	if (edge && quad)
		prim_state(tex, abe, tex && raw, clut, PrimitiveType::Polygon);
	bool a = !culled(v[0], v[1], v[2]), b = quad && !culled(v[1], v[2], v[3]);
	unsigned u0 = 255, v0 = 255, u1 = 0, v1 = 0;
	for (int i = 0; i < nv && rep; i++)
	{
		u0 = std::min<unsigned>(u0, v[i].u);
		v0 = std::min<unsigned>(v0, v[i].v);
		u1 = std::max<unsigned>(u1, v[i].u);
		v1 = std::max<unsigned>(v1, v[i].v);
	}
	if (dt)     /* a 3D polygon: no texture pack image */
		dt_draw(dt, v, a, b);
	else
	{
		if (rep && tex && a)
			rep_override(rep, u0, v0, u1, v1);
		if (a && b)
			renderer->draw_quad(v);
		else if (a)
			renderer->draw_triangle(v);
		if (rep && tex && b && !a)
			rep_override(rep, u0, v0, u1, v1);
		if (b && !a)
			renderer->draw_triangle(v + 1);
	}
	if (under)
	{
		/* the crack fix (gpu.c under): once more at the console's corners, affine, only where the
		 * mask bit is clear (the screen clear shows), leaving it clear: later ones go over it too */
		Vertex c[4];
		for (int i = 0; i < nv; i++)
		{
			c[i] = v[i];
			c[i].x = float(under[i * 2] - st.offx);
			c[i].y = float(under[i * 2 + 1] - st.offy);
			c[i].w = 1.0f;
		}
		renderer->set_force_mask_bit(st.mask_set);
		renderer->set_mask_test(true);
		bool ca = !culled(c[0], c[1], c[2]), cb = quad && !culled(c[1], c[2], c[3]);
		if (ca && cb)
			renderer->draw_quad(c);
		else if (ca)
			renderer->draw_triangle(c);
		else if (cb)
			renderer->draw_triangle(c + 1);
		cnt.under++;
	}
	if (!a || (quad && !b))
		cnt.cull++;
}

static void rect(const uint32_t *p, const VkXform *xf, int nxf, const VkRep *rep)
{
	uint32_t op = p[0] >> 24;
	int tex = (op >> 2) & 1, abe = (op >> 1) & 1, raw = op & 1, idx = 1;
	uint32_t xy = p[idx++], u = 0, vv = 0, clut = 0;
	int32_t w, h;
	if (tex)
	{
		uint32_t uv = p[idx++];
		u = uv & 0xFFu;
		vv = (uv >> 8) & 0xFFu;
		clut = uv >> 16;
	}
	switch ((op >> 3) & 3u)
	{
	case 0:
		w = p[idx] & 0x3FFu;
		h = (p[idx] >> 16) & 0x1FFu;
		break;
	case 1: w = h = 1; break;
	case 2: w = h = 8; break;
	default: w = h = 16; break;
	}
	if (!w || !h || nxf < 0 || (tagging && abe && !tex))
		return;
	cnt.rect++;
	/* a flipped sprite reads its texels backwards: start one past u and step down (the shader
	 * samples at pixel centres) */
	if (st.flip_x || st.flip_y)
		cnt.flip++;
	uint16_t ua = st.flip_x ? uint16_t(u + 1) : uint16_t(u), ub = st.flip_x ? uint16_t(u + 1 - w) : uint16_t(u + w);
	uint16_t va = st.flip_y ? uint16_t(vv + 1) : uint16_t(vv), vb = st.flip_y ? uint16_t(vv + 1 - h) : uint16_t(vv + h);
	uint32_t col = tagging ? tag_color(false) : p[0] & 0xFFFFFFu;
	prim_state(tex, tagging ? 0 : abe, tex && raw, clut, PrimitiveType::Sprite);
	if (tagging)
		renderer->tag_stp = abe;    /* see-through: its opaque texels only (see polygon) */
	if (tex)
		renderer->set_UV_limits(uint16_t(st.flip_x ? u - w + 1 : u), uint16_t(st.flip_y ? vv - h + 1 : vv),
		                        uint16_t(st.flip_x ? u : u + w - 1), uint16_t(st.flip_y ? vv : vv + h - 1));
	for (int k = 0; k < std::max(nxf, 1); k++)
	{
		float x0 = float(sext11(xy)), y0 = float(sext11(xy >> 16)), x1 = x0 + w, y1 = y0 + h;
		if (nxf)
		{
			const VkXform &f = xf[k];
			if (f.clip1 > f.clip0 && !renderer->set_scissor_columns(f.clip0, f.clip1))
				continue;   /* a menu tile's copy: only its own columns (see-through tiles blend once) */
			x0 = moved(float(f.x - st.offx), st.offx, f.cx, f.dx, f.sx);
			x1 = moved(float(f.x - st.offx + w), st.offx, f.cx, f.dx, f.sx);
			y0 = moved(float(f.y - st.offy), st.offy, f.cy, f.dy, f.sy);
			y1 = moved(float(f.y - st.offy + h), st.offy, f.cy, f.dy, f.sy);
			/* a copy moved wholly off VRAM (a menu tile's copy left of the edge) shows nothing; the
			 * renderer would take its box (clamped to VRAM, negative width) as a huge one */
			if (x1 + st.offx <= 0.0f || x0 + st.offx >= 1024.0f || y1 + st.offy <= 0.0f || y0 + st.offy >= 512.0f)
			{
				if (f.clip1 > f.clip0)
					renderer->set_draw_rect(draw_rect);
				continue;
			}
		}
		Vertex v[4] = {
			{ x0, y0, 1.0f, col, ua, va },
			{ x1, y0, 1.0f, col, ub, va },
			{ x0, y1, 1.0f, col, ua, vb },
			{ x1, y1, 1.0f, col, ub, vb },
		};
		if (rep && tex)
			rep_override(rep, u, vv, u + w - 1, vv + h - 1);
		if (renderer->ordered && nxf && xf[k].z > 0.0f)     /* at its anchor's depth (gpu.c sprite_depth_tag) */
		{
			renderer->dt_kind = 1;
			for (float &z : renderer->dt_z)
				z = float(DT_K / xf[k].z / (1.0 + g_depth_margin));
		}
		renderer->draw_quad(v);
		renderer->dt_kind = 0;
		if (nxf && xf[k].clip1 > xf[k].clip0)
			renderer->set_draw_rect(draw_rect);
	}
	renderer->tag_stp = false;
}

static Vertex line_pt(uint32_t col, uint32_t xy)
{
	return { float(sext11(xy)), float(sext11(xy >> 16)), 1.0f, col & 0xFFFFFFu, 0, 0 };
}

static void line(const Vertex &a, const Vertex &b, int abe, const VkXform *xf = nullptr)
{
	if (tagging && abe)
		return;
	Vertex v[2] = { a, b };
	if (tagging)
		v[0].color = v[1].color = tag_color(false);
	for (int i = 0; i < 2 && xf; i++)
		v[i].x = moved(v[i].x, st.offx, xf->cx, xf->dx, xf->sx);
	cnt.line++;
	prim_state(0, abe, 0, 0, PrimitiveType::Polygon);
	renderer->draw_line(v);
}

/* VRAM boxes may wrap at the right and bottom edges: the renderer takes them in pieces */
template <typename F> static void pieces(int x, int y, int w, int h, F f)
{
	x &= 1023;
	y &= 511;
	int w0 = std::min(w, 1024 - x), h0 = std::min(h, 512 - y);
	f(x, y, w0, h0);
	if (w0 < w)
		f(0, y, w - w0, h0);
	if (h0 < h)
		f(x, 0, w0, h - h0);
	if (w0 < w && h0 < h)
		f(0, 0, w - w0, h - h0);
}

static void fill(const uint32_t *p)
{
	int x = p[1] & 0x3F0u, y = (p[1] >> 16) & 0x3FFu;
	int w = ((p[2] & 0x3FFu) + 0xFu) & ~0xFu, h = (p[2] >> 16) & 0x1FFu;
	if (!w || !h)
		return;
	cnt.fill++;
	pieces(x, y, w, h, [&](int px, int py, int pw, int ph) {
		renderer->clear_rect({ unsigned(px), unsigned(py), unsigned(pw), unsigned(ph) },
		                     tagging ? tag_color(false) : p[0] & 0xFFFFFFu);
	});
}

static void copy(const uint32_t *p)
{
	int sx = p[1] & 0x3FFu, sy = (p[1] >> 16) & 0x3FFu, dx = p[2] & 0x3FFu, dy = (p[2] >> 16) & 0x3FFu;
	int w = p[3] & 0x3FFu, h = (p[3] >> 16) & 0x1FFu;
	w = w ? w : 1024;
	h = h ? h : 512;
	cnt.copy++;
	sy &= 511;
	dy &= 511;
	if (sx + w > 1024 || dx + w > 1024 || sy + h > 512 || dy + h > 512)
	{
		cnt.wrap++;     // ponytail: wrapping copies are skipped; split them like pieces() if a game needs it
		return;
	}
	renderer->blit_vram({ unsigned(dx), unsigned(dy), unsigned(w), unsigned(h) },
	                    { unsigned(sx), unsigned(sy), unsigned(w), unsigned(h) });
}

static void env(uint32_t w)
{
	switch (w >> 24)
	{
	case 0xE1:
		set_texpage(w);
		st.flip_x = (w >> 12) & 1u;
		st.flip_y = (w >> 13) & 1u;
		break;
	case 0xE2:
	{
		unsigned tww = w & 0x1Fu, twh = (w >> 5) & 0x1Fu, twx = (w >> 10) & 0x1Fu, twy = (w >> 15) & 0x1Fu;
		renderer->set_texture_window({ uint8_t(~(tww << 3)), uint8_t(~(twh << 3)), uint8_t((twx & tww) << 3),
		                               uint8_t((twy & twh) << 3) });
		break;
	}
	case 0xE3:
	case 0xE4:
	{
		unsigned &x0 = st.e3x, &y0 = st.e3y, &x1 = st.e4x, &y1 = st.e4y;
		if ((w >> 24) == 0xE3)
		{
			x0 = w & 0x3FFu;
			y0 = (w >> 10) & 0x1FFu;
		}
		else
		{
			x1 = w & 0x3FFu;
			y1 = (w >> 10) & 0x1FFu;
		}
		st.clip_x0 = int(x0);
		st.clip_x1 = int(x1);
		if (x1 >= x0 && y1 >= y0)
			draw_rect = { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
		else
			draw_rect = { 0, 0, 0, 0 };
		renderer->set_draw_rect(draw_rect);
		break;
	}
	case 0xE5:
		st.offx = sext11(w);
		st.offy = sext11(w >> 11);
		renderer->set_draw_offset(st.offx, st.offy);
		break;
	case 0xE6:
		st.mask_set = w & 1u;
		st.mask_check = (w >> 1) & 1u;
		renderer->set_force_mask_bit(st.mask_set);
		renderer->set_mask_test(st.mask_check);
		if (w & 3u)
			cnt.mask++;
		break;
	}
}

static void touch(const Rect &r);

static void gp0(const uint32_t *p, const VkXform *xf, int nxf, int edge, const VkRep *rep, const float *prec,
                const int32_t *under)
{
	uint32_t op = p[0] >> 24;
	if (op >= 0x20 && op < 0x80)
		touch(draw_rect);
	else if (op == 0x02)
		touch({ p[1] & 0x3F0u, (p[1] >> 16) & 0x1FFu, ((p[2] & 0x3FFu) + 0xFu) & ~0xFu, (p[2] >> 16) & 0x1FFu });
	else if ((op >> 5) == 4)
		touch({ p[2] & 0x3FFu, (p[2] >> 16) & 0x1FFu, (p[3] & 0x3FFu) ? p[3] & 0x3FFu : 1024u,
		        ((p[3] >> 16) & 0x1FFu) ? (p[3] >> 16) & 0x1FFu : 512u });
	switch (op >> 5)
	{
	case 0:
		if (op == 0x02)
			fill(p);
		break;
	case 1:
		polygon(p, nxf ? xf : nullptr, edge, rep, prec, under);
		break;
	case 2:
		if (op & 0x10)
			line(line_pt(p[0], p[1]), line_pt(p[2], p[3]), (op >> 1) & 1, nxf ? xf : nullptr);
		else
			line(line_pt(p[0], p[1]), line_pt(p[0], p[2]), (op >> 1) & 1, nxf ? xf : nullptr);
		if (op & 0x08)
		{
			st.pl_gouraud = (op >> 4) & 1;
			st.pl_abe = (op >> 1) & 1;
			st.pl_count = 0;
			st.pl_prev = (op & 0x10) ? line_pt(p[2], p[3]) : line_pt(p[0], p[2]);
		}
		break;
	case 3:
		rect(p, xf, nxf, rep);
		break;
	case 4:
		copy(p);
		break;
	case 7:
		env(p[0]);
		break;
	}
}

/* the depth test: on for the renderer in use (its first command) */
static void dt_on()
{
	if (g_depth_margin <= 0.0 || renderer->ordered || dt_failed[cur_inst])
		return;
	dt_failed[cur_inst] = !renderer->set_ordered(float(g_depth_margin));
	if (dt_failed[cur_inst])
		log_msg(1, "vk: no depth test on renderer %d (no D32S8 depth buffer at x%d)", cur_inst, scale);
}

extern "C" void vkr_gp0(const uint32_t *p, const VkXform *xf, int nxf, int edge, const VkRep *rep, const float *prec,
                        const int32_t *under)
{
	tag_now++;      /* the command's number (gpu.c cur_tag) */
	dt_on();
	gp0(p, xf, nxf, edge, rep, prec, under);
	if (seams())
	{
		int k = cur_inst;
		use(k + 2);
		dt_on();
		tagging = true;
		gp0(p, xf, nxf, edge, nullptr, prec, nullptr);
		tagging = false;
		use(k);
	}
}

static void polyline(uint32_t w)
{
	touch(draw_rect);
	if (st.pl_count == 0 && (w & 0xF000F000u) == 0x50005000u)
		return;
	st.pl_words[st.pl_count++] = w;
	if (st.pl_count < (st.pl_gouraud ? 2 : 1))
		return;
	st.pl_count = 0;
	Vertex b = st.pl_gouraud ? line_pt(st.pl_words[0], st.pl_words[1]) : line_pt(st.pl_prev.color, st.pl_words[0]);
	line(st.pl_prev, b, st.pl_abe);
	st.pl_prev = b;
}

extern "C" void vkr_polyline(uint32_t w)
{
	polyline(w);
	if (seams())
	{
		int k = cur_inst;
		use(k + 2);
		tagging = true;
		polyline(w);
		tagging = false;
		use(k);
	}
}

static void upload(int x, int y, int w, int h, const uint16_t *vram)
{
	touch({ unsigned(x & 1023), unsigned(y & 511), unsigned(w), unsigned(h) });
	cnt.upload++;
	renderer->set_force_mask_bit(st.mask_set);
	renderer->set_mask_test(st.mask_check);
	cnt.upload_px += w * h;
	pieces(x, y, w, h, [&](int px, int py, int pw, int ph) {
		Rect r = { unsigned(px), unsigned(py), unsigned(pw), unsigned(ph) };
		BufferHandle buf = renderer->copy_cpu_to_vram(r);
		uint16_t *dst = renderer->begin_copy(buf);
		for (int j = 0; j < ph; j++)
			memcpy(dst + j * pw, vram + (py + j) * 1024 + px, pw * sizeof(uint16_t));
		renderer->end_copy(buf);
	});
}

extern "C" void vkr_upload(int x, int y, int w, int h, const uint16_t *vram)
{
	upload(x, y, w, h, vram);
	if (seams())
	{
		int k = cur_inst;
		use(k + 2);
		upload(x, y, w, h, vram);
		use(k);
	}
}

extern "C" void vkr_sync(int x, int y, int w, int h, uint16_t *vram)
{
	cnt.sync++;
	pieces(x, y, w, h, [&](int px, int py, int pw, int ph) {
		renderer->copy_vram_to_cpu_synchronous({ unsigned(px), unsigned(py), unsigned(pw), unsigned(ph) }, vram);
	});
}

extern "C" void vkr_download(int x, int y, int w, int h, uint16_t *vram)
{
	cnt.read++;
	cnt.read_px += w * h;
	if (g_vk_cpu_off)       /* else gpu.c's own VRAM is exact */
		vkr_sync(x, y, w, h, vram);
}

/* The picture: the display area of the scaled VRAM (15-bit, as the CPU renderer shows it), made at
 * the VBlank (kick), so the GPU works on it while the frontend waits for the VBlank to be due:
 * pend.img, a colour image the window shows (vkr_present). It is read back only when the frontend
 * wants its pixels (screenshots, or no direct display): pend.buf, as 0xAARRGGBB. Drawing after the
 * kick starts it again at display time. */
static struct
{
	ImageHandle img;        /* the picture (transfer source layout) */
	BufferHandle buf;       /* read back, when wanted */
	Fence fence;
	int x, y, w, h;
	bool done;              /* the read-back picture (seam pass included) is in done_px[cur_inst] */
} pend;
static std::vector<uint32_t> done_px[8];
static double wait_ms;      /* stats: time vkr_display waits */
static unsigned waits;
static bool direct;         /* the windows show pictures on the GPU (vkr_window) */
static bool want_pixels = true;
static struct
{
	ImageHandle img;
	int w, h;
	bool fresh;
} made;                     /* the picture vkr_display made last, for vkr_keep */
static struct
{
	ImageHandle img;
	int w, h;
} slots[3];                 /* pictures kept for the windows (vkr_keep) */

/* drawing into r changes the picture if r is in the display area it was made of (the game draws
 * its next frame into the other buffer while one is shown) */
static void touch(const Rect &r)
{
	if (!pend.img || r.intersects({ unsigned(pend.x), unsigned(pend.y), unsigned(pend.w / scale), unsigned(pend.h / scale) }))
		drawn = true;
}

static bool reading()
{
	return want_pixels || !direct;
}

static void read_back(CommandBufferHandle &cmd, const Image &src, unsigned w, unsigned h)
{
	BufferCreateInfo info;
	info.domain = BufferDomain::CachedHost;
	info.size = VkDeviceSize(w) * h * 4;
	info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	pend.buf = device->create_buffer(info, nullptr);
	cmd->copy_image_to_buffer(*pend.buf, src, 0, { 0, 0, 0 }, { w, h, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
	cmd->barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT,
	             VK_ACCESS_HOST_READ_BIT);
}

static void kick_done(CommandBufferHandle &cmd, int x, int y, unsigned w, unsigned h)
{
	pend.fence.reset();
	device->submit(cmd, &pend.fence);
	pend.x = x;
	pend.y = y;
	pend.w = int(w);
	pend.h = int(h);
	pend.done = false;
	drawn = false;
}

static void kick(int x, int y, int dw, int dh)
{
	dw = std::min(dw, 1024 - x);
	dh = std::min(dh, 512 - y);
	unsigned w = dw * scale, h = dh * scale;
	ImageHandle img = renderer->scanout_vram_to_texture(true);
	renderer->flush();

	ImageCreateInfo ii = ImageCreateInfo::render_target(w, h, VK_FORMAT_B8G8R8A8_UNORM);
	ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	ii.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	pend.img = device->create_image(ii, nullptr);
	pend.buf.reset();
	CommandBufferHandle cmd = device->request_command_buffer();
	cmd->image_barrier(*img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	cmd->image_barrier(*pend.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_ACCESS_TRANSFER_WRITE_BIT);
	cmd->blit_image(*pend.img, *img, { 0, 0, 0 }, { int(w), int(h), 1 }, { x * scale, y * scale, 0 },
	                { int(w), int(h), 1 }, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
	cmd->image_barrier(*pend.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_ACCESS_TRANSFER_READ_BIT);
	if (reading())
		read_back(cmd, *pend.img, w, h);
	kick_done(cmd, x, y, w, h);
}

/* the seam pass (seams.comp, gpu.c seam_across / seam_down) on the GPU */
static const uint32_t seams_comp[] =
#include "seams.comp.inc"
    ;

/* The picture with the seam pass: the picture and its tags (renderer k + 2) are copied to images,
 * seams.comp fills the seams into a colour image (shown) and 0xAARRGGBB words (read back) */
static void kick_all(int x, int y, int dw, int dh)
{
	if (!seams())
	{
		kick(x, y, dw, dh);
		return;
	}
	dw = std::min(dw, 1024 - x);
	dh = std::min(dh, 512 - y);
	unsigned w = dw * scale, h = dh * scale;
	ImageHandle pic = renderer->scanout_vram_to_texture(true);
	renderer->flush();
	int k = cur_inst;
	use(k + 2);
	ImageHandle tag = renderer->scanout_vram_to_texture(true);
	renderer->flush();
	drawn = false;
	use(k);

	auto image = [&](VkFormat f) {
		ImageCreateInfo ii = ImageCreateInfo::render_target(w, h, f);
		ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		ii.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		return device->create_image(ii, nullptr);
	};
	ImageHandle P = image(VK_FORMAT_R8G8B8A8_UNORM), T = image(VK_FORMAT_R8G8B8A8_UNORM);
	ImageHandle snap = image(VK_FORMAT_R32_UINT), words = image(VK_FORMAT_R32_UINT);
	/* the planes of this picture's tags, as a 16384 x 1 image */
	ImageCreateInfo qi = ImageCreateInfo::render_target(0x4000, 1, VK_FORMAT_R32G32B32A32_SFLOAT);
	qi.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	qi.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
	ImageHandle Q = device->create_image(qi, nullptr);
	BufferCreateInfo qb;
	qb.domain = BufferDomain::Host;
	qb.size = sizeof qpl[0];
	qb.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
	BufferHandle qbuf = device->create_buffer(qb, qpl[cur_inst & ~2]);
	pend.img = image(VK_FORMAT_R8G8B8A8_UNORM);
	pend.buf.reset();

	CommandBufferHandle cmd = device->request_command_buffer();
	const ImageHandle *src[2] = { &pic, &tag }, *dst[2] = { &P, &T };
	for (int i = 0; i < 2; i++)
	{
		cmd->image_barrier(**src[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
		cmd->image_barrier(**dst[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                   VK_ACCESS_TRANSFER_WRITE_BIT);
		cmd->blit_image(**dst[i], **src[i], { 0, 0, 0 }, { int(w), int(h), 1 }, { x * scale, y * scale, 0 },
		                { int(w), int(h), 1 }, 0, 0, 0, 0, 1, VK_FILTER_NEAREST);
		cmd->image_barrier(**dst[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	}
	for (const ImageHandle *img : { &snap, &words, &pend.img })
		cmd->image_barrier(**img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
		                   0, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
	cmd->image_barrier(*Q, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
	                   0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	cmd->copy_buffer_to_image(*Q, *qbuf, 0, { 0, 0, 0 }, { 0x4000, 1, 1 }, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 });
	cmd->image_barrier(*Q, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
	                   VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
	static Program *prog = device->request_program(seams_comp, sizeof seams_comp);
	cmd->set_program(*prog);
	cmd->set_storage_texture(0, 0, P->get_view());
	cmd->set_storage_texture(0, 1, T->get_view());
	cmd->set_storage_texture(0, 2, snap->get_view());
	cmd->set_storage_texture(0, 3, words->get_view());
	cmd->set_storage_texture(0, 4, pend.img->get_view());
	cmd->set_storage_texture(0, 5, Q->get_view());
	struct
	{
		int32_t w, h, n, pass, ox, oy;
	} push = { int32_t(w), int32_t(h), scale, 0, x, y };
	cmd->push_constants(&push, 0, sizeof push);
	cmd->dispatch((h + 63) / 64, 1, 1);     /* across: a row each */
	cmd->barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
	             VK_ACCESS_SHADER_READ_BIT);
	push.pass = 1;
	cmd->push_constants(&push, 0, sizeof push);
	cmd->dispatch((w + 63) / 64, 1, 1);     /* down: a column each */
	for (const ImageHandle *img : { &words, &pend.img })
		cmd->image_barrier(**img, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	if (reading())
		read_back(cmd, *words, w, h);
	kick_done(cmd, x, y, w, h);
}

extern "C" void vkr_display(uint32_t *out, int *w, int *h, int x, int y, int dw, int dh)
{
	if (drawn || !pend.img || pend.x != x || pend.y != y || pend.w != std::min(dw, 1024 - x) * scale ||
	    pend.h != std::min(dh, 512 - y) * scale || (reading() && !pend.buf && !pend.done))
		kick_all(x, y, dw, dh);
	*w = pend.w;
	*h = pend.h;
	made = { pend.img, pend.w, pend.h, true };
	if (!reading())
		return;             /* the window shows it from the GPU (vkr_keep, vkr_present) */
	std::vector<uint32_t> &done = done_px[cur_inst];
	if (pend.done)      /* nothing drawn since: as it was read back */
	{
		memcpy(out, done.data(), done.size() * sizeof(uint32_t));
		return;
	}
	auto t0 = std::chrono::steady_clock::now();
	pend.fence->wait();
	wait_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	waits++;
	const uint32_t *px = static_cast<const uint32_t *>(device->map_host_buffer(*pend.buf, MEMORY_ACCESS_READ_BIT));
	for (size_t i = 0, n = size_t(*w) * size_t(*h); i < n; i++)
		out[i] = px[i] | 0xFF000000u;     /* alpha is the mask bit */
	device->unmap_host_buffer(*pend.buf, MEMORY_ACCESS_READ_BIT);
	done.assign(out, out + size_t(*w) * *h);
	pend.done = true;
}

extern "C" void vkr_vblank(int shown, int x, int y, int dw, int dh)
{
	if (cur_inst >= 4)      /* the second view's GPU: its picture; the game's GPU flushes them all */
	{
		renderer->flush();
		if (shown && (drawn || !pend.img))
			kick_all(x, y, dw, dh);
		return;
	}
	for (Renderer *r : renderers)
		if (r)
			r->flush();
	if (shown && (drawn || !pend.img))
		kick_all(x, y, dw, dh);
	device->next_frame_context();
	if (stats && ++vblanks % 600 == 0)
	{
		log_msg(2, "vk stats (600 VBlanks): poly %u rect %u line %u fill %u copy %u (wrap %u) upload %u (%u px) "
		           "read %u (%u px) sync %u flip %u mask %u cull %u precise %u/%u under %u; display wait %.3f ms x %u",
		        cnt.poly, cnt.rect, cnt.line, cnt.fill, cnt.copy, cnt.wrap, cnt.upload, cnt.upload_px, cnt.read,
		        cnt.read_px, cnt.sync, cnt.flip, cnt.mask, cnt.cull, cnt.precise, cnt.vtx, cnt.under,
		        waits ? wait_ms / waits : 0.0, waits);
		memset(&cnt, 0, sizeof cnt);
		wait_ms = 0;
		waits = 0;
	}
}

extern "C" void vkr_kick(int x, int y, int dw, int dh)
{
	kick_all(x, y, dw, dh);
}

/* The renderers on the one device (0: the game's, 1: in-between pictures, 2 and 3: their tags):
 * the one in use has its state in the globals, the others here. ponytail: swapped globals, an
 * instance struct when there are more */
static void use(int k)
{
	struct Saved
	{
		Renderer *renderer;
		decltype(st) st;
		Rect draw_rect;
		decltype(pend) pend;
		bool drawn;
	};
	static Saved saved[8];
	if (k == cur_inst)
		return;
	saved[cur_inst] = { renderer, st, draw_rect, pend, drawn };
	if (!saved[k].renderer)
	{
		Renderer *r = renderers[k] = new Renderer(*device, scale, 1, nullptr);
		r->set_adaptive_smoothing(false);
		r->set_filter_mode(Renderer::FilterMode::NearestNeighbor);
		r->set_draw_rect({ 0, 0, 0, 0 });
		r->set_texture_window({ 0xFF, 0xFF, 0, 0 });
		r->toggle_display(true);
		if (k & 2)
			r->use_tag_shaders();
		saved[k] = {};
		saved[k].renderer = r;
		saved[k].drawn = true;
	}
	renderer = saved[k].renderer;
	st = saved[k].st;
	draw_rect = saved[k].draw_rect;
	pend = saved[k].pend;
	drawn = saved[k].drawn;
	saved[k] = {};
	cur_inst = k;
}

extern "C" void vkr_target(int k)
{
	use(k);
}

extern "C" int vkr_enter(int set)
{
	int old = cur_inst;
	if ((cur_inst & 4) != set)
		use(set);
	return old;
}

/* ---- Direct display: the windows show the pictures from the GPU (a swapchain each) ---- */

extern "C" void vkr_want_pixels(int want)
{
	want_pixels = want;
}

extern "C" int vkr_keep(int slot)
{
	bool fresh = made.fresh;
	made.fresh = false;
	if (slot < 0 || !fresh)
		return 0;
	slots[slot] = { made.img, made.w, made.h };
	return 1;
}

/* a window's surface, without windows.h or the X11/Wayland headers: vulkan_win32.h's, _xlib.h's and
 * _wayland.h's structs are the same, two handles (hinstance, hwnd; Display *, Window; wl_display *,
 * wl_surface *) after flags. Window is an unsigned long, a pointer's size on 64-bit Linux. */
struct SurfaceInfo
{
	VkStructureType sType;
	const void *pNext;
	VkFlags flags;
	void *a, *b;
};

static struct Swap
{
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkSwapchainKHR chain = VK_NULL_HANDLE;
	VkExtent2D extent = {};
	VkExtent2D size = {};       /* the window's pixels (vkr_window_size): for Wayland, which leaves it to us */
	std::vector<VkImage> images;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cb[2] = {};
	VkSemaphore acquired[2] = {}, rendered[2] = {};
	VkFence fence[2] = {};
	ImageHandle held[2][2];     /* what a frame's commands read, kept until its fence */
	BufferHandle held_buf[2];
	ImageHandle pre, upload;    /* sharp-bilinear prescale target; CPU pictures */
	ImageHandle hd[2], hd_pre[2];   /* with HD images: the picture at the window's size; its prescale */
	int frame = 0;
	bool stale = false;
	bool vsync = false;         /* wait for the screen's refresh (FIFO; vkr_vsync) */
	double ms = 0;
	unsigned n = 0;
} swaps[2];

extern "C" int vkr_window(int win, int kind, void *a, void *b)
{
	static const struct
	{
		const char *fn;
		VkStructureType type;
	} kinds[] = { { "vkCreateWin32SurfaceKHR", VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR },
		          { "vkCreateXlibSurfaceKHR", VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR },
		          { "vkCreateWaylandSurfaceKHR", VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR } };
	if (kind < 0 || kind > 2)
		return 0;
	Swap &s = swaps[win];
	auto create = reinterpret_cast<VkResult(VKAPI_PTR *)(VkInstance, const SurfaceInfo *, const VkAllocationCallbacks *,
	                                                       VkSurfaceKHR *)>(vkGetInstanceProcAddr(instance, kinds[kind].fn));
	SurfaceInfo info = { kinds[kind].type, nullptr, 0, a, b };
	VkBool32 ok = VK_FALSE;
	if (!create || !vkCreateSwapchainKHR || create(instance, &info, nullptr, &s.surface) != VK_SUCCESS)
		return 0;
	vkGetPhysicalDeviceSurfaceSupportKHR(context->get_gpu(), context->get_graphics_queue_family(), s.surface, &ok);
	if (!ok)
		return 0;
	VkDevice dev = context->get_device();
	VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pi.queueFamilyIndex = context->get_graphics_queue_family();
	vkCreateCommandPool(dev, &pi, nullptr, &s.pool);
	VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
	ai.commandPool = s.pool;
	ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	ai.commandBufferCount = 2;
	vkAllocateCommandBuffers(dev, &ai, s.cb);
	VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
	VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
	fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
	for (int i = 0; i < 2; i++)
	{
		vkCreateSemaphore(dev, &si, nullptr, &s.acquired[i]);
		vkCreateSemaphore(dev, &si, nullptr, &s.rendered[i]);
		vkCreateFence(dev, &fi, nullptr, &s.fence[i]);
	}
	direct = true;
	log_msg(2, "vk: window %d shown directly", win);
	return 1;
}

extern "C" void vkr_vsync(int win, int on)
{
	if (swaps[win].vsync != !!on)
		swaps[win].stale = true;    /* the next vkr_present makes the swapchain again */
	swaps[win].vsync = !!on;
}

extern "C" void vkr_window_size(int win, int w, int h)
{
	swaps[win].size = { uint32_t(w > 0 ? w : 0), uint32_t(h > 0 ? h : 0) };
}

/* (re)makes the swapchain for the window's size; false: nothing to show into (minimised) */
static bool make_chain(Swap &s)
{
	VkPhysicalDevice gpu = context->get_gpu();
	VkDevice dev = context->get_device();
	VkSurfaceCapabilitiesKHR caps;
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, s.surface, &caps);
	if (caps.currentExtent.width == 0xFFFFFFFFu)    /* the window takes the size we make (Wayland) */
		caps.currentExtent = { std::min(std::max(s.size.width, caps.minImageExtent.width), caps.maxImageExtent.width),
		                       std::min(std::max(s.size.height, caps.minImageExtent.height), caps.maxImageExtent.height) };
	if (!caps.currentExtent.width || !caps.currentExtent.height)
		return false;
	if (s.chain && !s.stale && caps.currentExtent.width == s.extent.width && caps.currentExtent.height == s.extent.height)
		return true;
	vkDeviceWaitIdle(dev);
	uint32_t n = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, s.surface, &n, nullptr);
	std::vector<VkSurfaceFormatKHR> formats(n);
	vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, s.surface, &n, formats.data());
	VkSurfaceFormatKHR fmt = formats[0];
	for (auto &f : formats)
		if (f.format == VK_FORMAT_B8G8R8A8_UNORM)
			fmt = f;
	vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, s.surface, &n, nullptr);
	std::vector<VkPresentModeKHR> modes(n);
	vkGetPhysicalDeviceSurfacePresentModesKHR(gpu, s.surface, &n, modes.data());
	/* vsync: FIFO, each picture shown at a refresh (with G-Sync/FreeSync, at once). Else no waiting:
	 * IMMEDIATE first. MAILBOX keeps the screen at its own rate and shows the newest picture at each
	 * refresh, which also turns variable refresh off: uneven with 59.29 pictures a second */
	VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
	if (!s.vsync)
		for (VkPresentModeKHR want : { VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR })
			if (std::find(modes.begin(), modes.end(), want) != modes.end())
			{
				mode = want;
				break;
			}
	VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	ci.surface = s.surface;
	ci.minImageCount = std::max(caps.minImageCount, 2u);
	if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
		ci.minImageCount = caps.maxImageCount;
	ci.imageFormat = fmt.format;
	ci.imageColorSpace = fmt.colorSpace;
	ci.imageExtent = caps.currentExtent;
	ci.imageArrayLayers = 1;
	ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	ci.preTransform = caps.currentTransform;
	ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ?
	                        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR :
	                        VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
	ci.presentMode = mode;
	ci.clipped = VK_TRUE;
	ci.oldSwapchain = s.chain;
	VkSwapchainKHR chain;
	if (vkCreateSwapchainKHR(dev, &ci, nullptr, &chain) != VK_SUCCESS)
		return false;
	if (s.chain)
		vkDestroySwapchainKHR(dev, s.chain, nullptr);
	else
		log_cb(RETRO_LOG_INFO, "window %d: %s", int(&s - swaps), mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (vsync)" :
		       mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX");
	s.chain = chain;
	s.extent = caps.currentExtent;
	vkGetSwapchainImagesKHR(dev, chain, &n, nullptr);
	s.images.resize(n);
	vkGetSwapchainImagesKHR(dev, chain, &n, s.images.data());
	s.stale = false;
	return true;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
	b.srcAccessMask = src_access;
	b.dstAccessMask = dst_access;
	b.oldLayout = from;
	b.newLayout = to;
	b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = img;
	b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static void blit(VkCommandBuffer cb, VkImage src, int sw, int sh, VkImage dst, int dx, int dy, int dw, int dh, VkFilter f)
{
	VkImageBlit r = {};
	r.srcSubresource = r.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
	r.srcOffsets[1] = { sw, sh, 1 };
	r.dstOffsets[0] = { dx, dy, 0 };
	r.dstOffsets[1] = { dx + dw, dy + dh, 1 };
	vkCmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, f);
}

static ImageHandle scratch_image(ImageHandle &keep, int w, int h)
{
	if (!keep || int(keep->get_width()) != w || int(keep->get_height()) != h)
	{
		ImageCreateInfo ii = ImageCreateInfo::render_target(w, h, VK_FORMAT_B8G8R8A8_UNORM);
		ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		keep = device->create_image(ii, nullptr);
	}
	return keep;
}

/* ---- HD images (vkr_overlay): pieces of pack images drawn over the picture at the window's size ---- */
/* the scanout's quad shaders (ppsx/glsl quad.vert, quad.frag SCALED): an image's rectangle
 * (offset, range: normalised) filling the viewport, smoothly sampled, alpha 1. Over the picture
 * twice per piece, with blending: dst *= (1 - a) from an image of 1 - a, then dst += the
 * premultiplied colour: the image drawn with its transparency, no shader of its own. */
static const uint32_t hd_quad_vert[] =
#include "ppsx/glsl/prebuilt/quad.vert.inc"
    ;
static const uint32_t hd_quad_frag[] =
#include "ppsx/glsl/prebuilt/scaled.quad.frag.inc"
    ;
static std::vector<VkHdPiece> hd_pieces[2];
struct HdImages
{
	ImageHandle keep, colour;   /* 1 - alpha; the premultiplied colour */
};
static std::map<uint64_t, HdImages> hd_images;

extern "C" void vkr_overlay(int win, const VkHdPiece *p, int n)
{
	hd_pieces[win].assign(p, p + n);
}

static const HdImages &hd_images_of(const VkHdPiece &p)
{
	auto it = hd_images.find(p.key);
	if (it != hd_images.end())
		return it->second;
	std::vector<uint32_t> keep(size_t(p.w) * p.h), colour(keep.size());
	for (size_t i = 0; i < keep.size(); i++)
	{
		uint32_t t = p.px[i];
		if (!t)
		{
			keep[i] = 0xFFFFFFFFu;
			colour[i] = 0xFF000000u;
			continue;
		}
		uint32_t r = t & 31u, g = (t >> 5) & 31u, b = (t >> 10) & 31u;
		keep[i] = 0xFF000000u;
		colour[i] = 0xFF000000u | (b << 3 | b >> 2) << 16 | (g << 3 | g >> 2) << 8 | (r << 3 | r >> 2);
	}
	HdImages &h = hd_images[p.key];
	ImageInitialData a = { keep.data(), 0, 0 }, c = { colour.data(), 0, 0 };
	h.keep = device->create_image(ImageCreateInfo::immutable_2d_image(p.w, p.h, VK_FORMAT_R8G8B8A8_UNORM), &a);
	h.colour = device->create_image(ImageCreateInfo::immutable_2d_image(p.w, p.h, VK_FORMAT_R8G8B8A8_UNORM), &c);
	return h;
}

/* The picture src (w x h, transfer source layout) scaled to dw x dh as vkr_present does, with the
 * pieces over it, into keep (transfer source layout when the commands have run) */
static void hd_compose(ImageHandle &keep, ImageHandle &pre_keep, const ImageHandle &src, int w, int h, int dw,
                       int dh, int scaling, const std::vector<VkHdPiece> &pieces)
{
	static BufferHandle quad;
	static Program *prog;
	if (!quad)
	{
		static const float corners[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
		BufferCreateInfo bi;
		bi.domain = BufferDomain::Device;
		bi.size = sizeof corners;
		bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
		quad = device->create_buffer(bi, corners);
		prog = device->request_program(hd_quad_vert, sizeof hd_quad_vert, hd_quad_frag, sizeof hd_quad_frag);
	}
	if (hd_images.size() > 32)
		hd_images.clear();     /* ponytail: dropped all at once, like rep_images */
	std::vector<const HdImages *> ims;
	for (const VkHdPiece &p : pieces)       /* made (and uploaded) before the commands below */
		ims.push_back(&hd_images_of(p));
	if (!keep || int(keep->get_width()) != dw || int(keep->get_height()) != dh)
	{
		ImageCreateInfo ii = ImageCreateInfo::render_target(dw, dh, VK_FORMAT_R8G8B8A8_UNORM);
		ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		ii.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		keep = device->create_image(ii, nullptr);
	}
	CommandBufferHandle cmd = device->request_command_buffer();
	/* srcStage transfer: the window's last copy from this frame's image read it */
	cmd->image_barrier(*keep, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
	int kx = (dw + w - 1) / w, ky = (dh + h - 1) / h;
	while (kx > 1 && kx * w > 8192)
		kx--;
	while (ky > 1 && ky * h > 8192)
		ky--;
	if (scaling == 0 && (kx > 1 || ky > 1))     /* sharp-bilinear */
	{
		if (!pre_keep || int(pre_keep->get_width()) != kx * w || int(pre_keep->get_height()) != ky * h)
		{
			ImageCreateInfo ii = ImageCreateInfo::render_target(kx * w, ky * h, VK_FORMAT_R8G8B8A8_UNORM);
			ii.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
			ii.initial_layout = VK_IMAGE_LAYOUT_UNDEFINED;
			pre_keep = device->create_image(ii, nullptr);
		}
		cmd->image_barrier(*pre_keep, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		cmd->blit_image(*pre_keep, *src, { 0, 0, 0 }, { kx * w, ky * h, 1 }, { 0, 0, 0 }, { w, h, 1 }, 0, 0, 0, 0, 1,
		                VK_FILTER_NEAREST);
		cmd->image_barrier(*pre_keep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                   VK_ACCESS_TRANSFER_READ_BIT);
		cmd->blit_image(*keep, *pre_keep, { 0, 0, 0 }, { dw, dh, 1 }, { 0, 0, 0 }, { kx * w, ky * h, 1 }, 0, 0, 0, 0, 1,
		                VK_FILTER_LINEAR);
	}
	else
		cmd->blit_image(*keep, *src, { 0, 0, 0 }, { dw, dh, 1 }, { 0, 0, 0 }, { w, h, 1 }, 0, 0, 0, 0, 1,
		                scaling == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
	cmd->image_barrier(*keep, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
	                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
	                   VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
	RenderPassInfo rp;
	rp.color_attachments[0] = &keep->get_view();
	rp.num_color_attachments = 1;
	rp.load_attachments = 1;
	rp.store_attachments = 1;
	cmd->begin_render_pass(rp);
	cmd->set_quad_state();
	cmd->set_program(*prog);
	cmd->set_vertex_binding(0, *quad, 0, 8);
	cmd->set_vertex_attrib(0, 0, VK_FORMAT_R32G32_SFLOAT, 0);
	cmd->set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
	cmd->set_blend_enable(true);
	cmd->set_blend_op(VK_BLEND_OP_ADD);
	cmd->set_scissor({ { 0, 0 }, { unsigned(dw), unsigned(dh) } });
	const float sx = float(dw) / w, sy = float(dh) / h;
	for (size_t i = 0; i < pieces.size(); i++)
	{
		const VkHdPiece &p = pieces[i];
		float x0 = p.x0 * sx, y0 = p.y0 * sy, x1 = p.x1 * sx, y1 = p.y1 * sy;
		if (x1 <= x0 || y1 <= y0)
			continue;
		cmd->set_viewport({ x0, y0, x1 - x0, y1 - y0, 0.0f, 1.0f });
		struct
		{
			float offset[2], range[2], uv_min[2], uv_max[2], max_bias;
		} push = { { p.u0 / p.w, p.v0 / p.h },
			       { (p.u1 - p.u0) / p.w, (p.v1 - p.v0) / p.h },
			       { 0.5f / p.w, 0.5f / p.h },
			       { 1.0f - 0.5f / p.w, 1.0f - 0.5f / p.h },
			       0.0f };
		cmd->push_constants(&push, 0, sizeof push);
		cmd->set_blend_factors(VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_COLOR);    /* dst *= 1 - a */
		cmd->set_texture(0, 0, ims[i]->keep->get_view(), StockSampler::LinearClamp);
		cmd->draw(4);
		cmd->set_blend_factors(VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE);           /* dst += colour * a */
		cmd->set_texture(0, 0, ims[i]->colour->get_view(), StockSampler::LinearClamp);
		cmd->draw(4);
	}
	cmd->end_render_pass();
	cmd->image_barrier(*keep, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	                   VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	Fence fence;
	device->submit(cmd, &fence);    /* now: the window's commands are submitted after these */
}

/* Shows a picture in window win: slot's (vkr_keep), or px (w x h, 0xAARRGGBB: the CPU's, a video),
 * at aspect (width / height), letterboxed, scaled as the frontend does (scaling: SCALING_*: 0
 * sharp-bilinear: whole-number nearest prescale, then smooth; 1 sharp; 2 smooth) */
extern "C" void vkr_present(int win, int slot, const uint32_t *px, int w, int h, double aspect, int scaling)
{
	Swap &s = swaps[win];
	VkDevice dev = context->get_device();
	auto t0 = std::chrono::steady_clock::now();
	if (!s.surface || !make_chain(s))
		return;
	int f = s.frame;
	vkWaitForFences(dev, 1, &s.fence[f], VK_TRUE, UINT64_MAX);
	s.held[f][0].reset();
	s.held[f][1].reset();
	s.held_buf[f].reset();
	uint32_t idx;
	VkResult r = vkAcquireNextImageKHR(dev, s.chain, UINT64_MAX, s.acquired[f], VK_NULL_HANDLE, &idx);
	if (r == VK_ERROR_OUT_OF_DATE_KHR)
	{
		s.stale = true;
		return;
	}
	vkResetFences(dev, 1, &s.fence[f]);
	VkCommandBuffer cb = s.cb[f];
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
	bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	vkResetCommandBuffer(cb, 0);
	vkBeginCommandBuffer(cb, &bi);

	ImageHandle src;
	if (px && w > 0 && h > 0)     /* the CPU's picture: through a buffer */
	{
		BufferCreateInfo info;
		info.domain = BufferDomain::Host;
		info.size = VkDeviceSize(w) * h * 4;
		info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
		s.held_buf[f] = device->create_buffer(info, px);
		src = scratch_image(s.upload, w, h);
		barrier(cb, src->get_image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
		VkBufferImageCopy c = {};
		c.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		c.imageExtent = { unsigned(w), unsigned(h), 1 };
		vkCmdCopyBufferToImage(cb, s.held_buf[f]->get_buffer(), src->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		                       1, &c);
		barrier(cb, src->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
	}
	else if (slot >= 0 && slots[slot].img)
	{
		src = slots[slot].img;
		w = slots[slot].w;
		h = slots[slot].h;
		/* it was made in transfer source layout by commands submitted before these */
		VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
	}
	VkImage out = s.images[idx];
	barrier(cb, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
	VkClearColorValue black = {};
	VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
	vkCmdClearColorImage(cb, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
	int ww = int(s.extent.width), wh = int(s.extent.height);
	int dw = ww, dh = int(ww / aspect);
	if (dh > wh)
	{
		dh = wh;
		dw = int(wh * aspect);
	}
	bool hd = !px && src && w > 0 && h > 0 && dw > 0 && dh > 0 && !hd_pieces[win].empty();
	if (hd)     /* the picture with HD images over it, made at the window's size: copied 1:1 */
	{
		hd_compose(s.hd[f], s.hd_pre[f], src, w, h, dw, dh, scaling, hd_pieces[win]);
		VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER };
		mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
		barrier(cb, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		blit(cb, s.hd[f]->get_image(), dw, dh, out, (ww - dw) / 2, (wh - dh) / 2, dw, dh, VK_FILTER_NEAREST);
		s.held[f][0] = src;
		s.held[f][1] = s.hd[f];
	}
	else if (src && w > 0 && h > 0 && dw > 0 && dh > 0)
	{
		barrier(cb, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		        VK_PIPELINE_STAGE_TRANSFER_BIT);
		int kx = (dw + w - 1) / w, ky = (dh + h - 1) / h;
		while (kx > 1 && kx * w > 8192)
			kx--;
		while (ky > 1 && ky * h > 8192)
			ky--;
		if (scaling == 0 && (kx > 1 || ky > 1))     /* sharp-bilinear: every pixel as wide, edges blended */
		{
			ImageHandle pre = scratch_image(s.pre, kx * w, ky * h);
			barrier(cb, pre->get_image(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			        VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
			blit(cb, src->get_image(), w, h, pre->get_image(), 0, 0, kx * w, ky * h, VK_FILTER_NEAREST);
			barrier(cb, pre->get_image(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			        VK_PIPELINE_STAGE_TRANSFER_BIT);
			blit(cb, pre->get_image(), kx * w, ky * h, out, (ww - dw) / 2, (wh - dh) / 2, dw, dh, VK_FILTER_LINEAR);
			s.held[f][1] = pre;
		}
		else
			blit(cb, src->get_image(), w, h, out, (ww - dw) / 2, (wh - dh) / 2, dw, dh,
			     scaling == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
		s.held[f][0] = src;
	}
	barrier(cb, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
	        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
	vkEndCommandBuffer(cb);

	VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
	sub.waitSemaphoreCount = 1;
	sub.pWaitSemaphores = &s.acquired[f];
	sub.pWaitDstStageMask = &wait;
	sub.commandBufferCount = 1;
	sub.pCommandBuffers = &cb;
	sub.signalSemaphoreCount = 1;
	sub.pSignalSemaphores = &s.rendered[f];
	vkQueueSubmit(context->get_graphics_queue(), 1, &sub, s.fence[f]);
	VkPresentInfoKHR pres = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	pres.waitSemaphoreCount = 1;
	pres.pWaitSemaphores = &s.rendered[f];
	pres.swapchainCount = 1;
	pres.pSwapchains = &s.chain;
	pres.pImageIndices = &idx;
	r = vkQueuePresentKHR(context->get_graphics_queue(), &pres);
	if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
		s.stale = true;
	s.frame ^= 1;
	s.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	if (++s.n == 120)
	{
		static std::chrono::steady_clock::time_point last[2];
		auto now = std::chrono::steady_clock::now();
		double secs = std::chrono::duration<double>(now - last[win]).count();
		if (last[win].time_since_epoch().count())
			log_msg(3, "present: renderer=vulkan%s present %.2f ms (avg), %.2f fps", win ? " (window 2)" : "",
			        s.ms / s.n, 120.0 / secs);
		last[win] = now;
		s.ms = 0;
		s.n = 0;
	}
}
