/* DMA controller.  Transfers complete instantly (the CPU is not stalled), then the channel
 * completion interrupt is raised exactly as the hardware would at the end of the transfer. */
#include "psx.h"
#include "gpu.h"
#include "spu.h"

typedef struct { uint32_t madr, bcr, chcr; } DmaChan;
static DmaChan ch[7];
static uint32_t dpcr = 0x07654321, dicr;

void dma_init(void) {
    memset(ch, 0, sizeof ch);
    dpcr = 0x07654321;
    dicr = 0;
}

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    bool master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    uint32_t old = dicr & 0x80000000u;
    dicr = (dicr & 0x7FFFFFFFu) | (master ? 0x80000000u : 0);
    if (master && !old) irq_raise(IRQ_DMA);
}

static void finish(int n) {
    ch[n].chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + n))) {
        dicr |= 1u << (24 + n);
    }
    dicr_update();
}

static uint32_t tmpbuf[0x10000 * 2];

static void gpu_list(uint32_t addr, const uint8_t *ram) {
    int guard = 0;
    for (;;) {
        uint32_t hdr, w;
        memcpy(&hdr, ram + (addr & 0x1FFFFC), 4);
        uint32_t cnt = hdr >> 24;
        for (uint32_t k = 0; k < cnt; k++) {
            g_gp0_addr = (addr + 4 + k * 4) & 0x1FFFFC;   /* for PGXP */
            memcpy(&w, ram + g_gp0_addr, 4);
            gpu_write_gp0(w);
        }
        if (hdr & 0x800000) break;
        addr = hdr & 0x1FFFFC;
        if (++guard > 0x100000) { LOGE("GPU DMA linked list loop"); break; }
    }
    g_gp0_addr = 0xFFFFFFFFu;
}

static void run_channel(int n) {
    DmaChan *d = &ch[n];
    uint32_t chcr = d->chcr;
    uint32_t sync = (chcr >> 9) & 3;
    bool from_ram = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d->madr & 0x1FFFFC;

    if (n == 2 && sync == 2) {
        /* GPU linked list (ordering table); the second view's GPU walks it in the RAM its own
         * frame was built in (p2view.c) */
        int route = g_gpu_route;
        if (route == 1) g_gpu_route = 0;
        gpu_list(addr, g_ram);
        const uint8_t *ram2 = route == 1 ? p2view_ram() : NULL;
        if (ram2) {
            g_gpu_route = 2;
            gpu_list(addr, ram2);
        } else if (route == 1) {
            /* the second window keeps its picture (a menu, a talk): the list's loads still reach
             * its VRAM (the tool chest loads the tools' pictures in its list: farmer 2's window
             * drew farmer 2's new tool with the old picture in its place) */
            extern int g_view2_uploads_only;
            g_gpu_route = 2;
            g_view2_uploads_only = 1;
            gpu_list(addr, g_ram);
            g_view2_uploads_only = 0;
        }
        if (route == 1) g_gpu_route = 1;
        d->madr = 0x00FFFFFF;
        finish(n);
        return;
    }

    uint32_t words;
    if (sync == 0) {
        words = d->bcr & 0xFFFF;
        if (!words) words = 0x10000;
    } else {
        uint32_t bs = d->bcr & 0xFFFF, ba = d->bcr >> 16;
        if (!bs) bs = 0x10000;
        if (!ba) ba = 0x10000;
        words = bs * ba;
        if (words > 0x20000) words = 0x20000;
    }

    switch (n) {
    case 6: /* OTC: reverse clear ordering table */
        for (uint32_t k = 0; k < words; k++) {
            uint32_t a = (addr - k * 4) & 0x1FFFFC;
            ram_w32(a, (k == words - 1) ? 0x00FFFFFF : ((a - 4) & 0x1FFFFF));
        }
        break;
    case 2: /* GPU block */
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            gpu_dma_write(tmpbuf, (int)words);
        } else {
            gpu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    case 3: /* CDROM -> RAM */
        if (g_view2_run) p2view_abort();
        cdrom_dma_read(tmpbuf, (int)words);
        for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        break;
    case 4: /* SPU */
        if (g_view2_run) break;         /* the farm's frame run again for the second view: no sound */
        spu_catch_up();
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            spu_dma_write(tmpbuf, (int)words);
        } else {
            spu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    default:
        LOGW("DMA channel %d transfer ignored (chcr=%08X)", n, chcr);
        break;
    }
    if (sync != 0) {
        d->madr = (addr + (uint32_t)step * words) & 0xFFFFFF;
        if (sync == 1) d->bcr &= 0xFFFF;   /* block count reaches zero */
    }
    finish(n);
}

uint32_t dma_read(uint32_t off) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: return ch[n].madr;
        case 4: return ch[n].bcr;
        case 8: return ch[n].chcr;
        default: return 0;
        }
    }
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    return 0;
}

/* the second view (p2view.c) runs the farm's frame once more and puts everything back */
size_t dma_state_size(void) { return sizeof ch + sizeof dpcr + sizeof dicr; }
void dma_save_state(void *dst) {
    uint8_t *o = dst;
    memcpy(o, ch, sizeof ch);
    memcpy(o + sizeof ch, &dpcr, sizeof dpcr);
    memcpy(o + sizeof ch + sizeof dpcr, &dicr, sizeof dicr);
}
void dma_load_state(const void *src) {
    const uint8_t *i = src;
    memcpy(ch, i, sizeof ch);
    memcpy(&dpcr, i + sizeof ch, sizeof dpcr);
    memcpy(&dicr, i + sizeof ch + sizeof dpcr, sizeof dicr);
}

void dma_write(uint32_t off, uint32_t v) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: ch[n].madr = v & 0xFFFFFF; break;
        case 4: ch[n].bcr = v; break;
        case 8:
            if (n == 6) v = (v & 0x51000000u) | 2;   /* OTC: only start/trigger bits writable, step -4 */
            ch[n].chcr = v;
            if ((v & 0x01000000u) && (((v >> 9) & 3) != 0 || (v & 0x10000000u))) {
                if (!(dpcr & (8u << (n * 4))))
                    LOGD("DMA%d started while disabled in DPCR", n);
                run_channel(n);
            }
            break;
        }
        return;
    }
    if (off == 0x70) { dpcr = v; return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7F000000u;
        dicr = (dicr & 0xFF000000u & ~ack) | (v & 0x00FF803Fu);
        dicr_update();
        return;
    }
}
