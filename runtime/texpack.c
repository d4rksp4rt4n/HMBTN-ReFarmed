/* Texture packs: replacement images for the game's textures in the upscaled picture, found by a
 * key of the texture's content (see "Texture packs" in docs/GRAPHICS.md). The images are PNG
 * files named <key>.png in a folder; textures can be dumped to PNG for drawing them anew. */
#include "psx.h"
#include "gpu.h"
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#define mkdir_one(p) mkdir(p, 0755)
#endif

int g_texpack, g_texdump, g_artfix;
static TexImage *imgs;              /* sorted by key */
static int nimgs;
static char dump_dir[1024];
#define DUMPED_N (1 << 16)
static uint64_t dumped[DUMPED_N];   /* keys written this session (open addressing, 0 = free) */
static int ndumped;

/* FNV-1a over the size and the 16-bit texels */
uint64_t texpack_key(int w, int h, const uint16_t *px)
{
    uint64_t k = 0xCBF29CE484222325ull;
    int i;
    k = (k ^ (uint32_t)w) * 0x100000001B3ull;
    k = (k ^ (uint32_t)h) * 0x100000001B3ull;
    for (i = 0; i < w * h; i++)
        k = (k ^ px[i]) * 0x100000001B3ull;
    return k;
}

static int key_cmp(const void *a, const void *b)
{
    uint64_t x = ((const TexImage *)a)->key, y = ((const TexImage *)b)->key;
    return x < y ? -1 : x > y;
}

const TexImage *texpack_find(uint64_t key)
{
    TexImage k;
    k.key = key;
    return nimgs ? bsearch(&k, imgs, (size_t)nimgs, sizeof *imgs, key_cmp) : NULL;
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* An 8-bit RGB or RGBA PNG as texels: 15-bit colour, alpha below 128 transparent (0), black as 0421h
 * (0 would be transparent). NULL when it can't be read. */
static uint16_t *png_load(const char *path, int *pw, int *ph)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    uint8_t *f = NULL, *idat = NULL, *raw = NULL;
    uint16_t *px = NULL;
    size_t n = 0, ni = 0, pos = 8, stride = 0;
    int w = 0, h = 0, bpp = 0, x, y;
    long len;
    FILE *fp = fopen(path, "rb");
    if (!fp)
        return NULL;
    if (fseek(fp, 0, SEEK_END) == 0 && (len = ftell(fp)) > 8 && len < (64 << 20) && fseek(fp, 0, SEEK_SET) == 0 &&
        (f = malloc((size_t)len)) && fread(f, 1, (size_t)len, fp) == (size_t)len)
        n = (size_t)len;
    fclose(fp);
    if (!n || memcmp(f, sig, 8) || !(idat = malloc(n)))
        goto done;
    while (pos + 12 <= n) {
        uint32_t cl = be32(f + pos);
        const uint8_t *t = f + pos + 4, *d = f + pos + 8;
        if (cl > n - pos - 12)
            goto done;
        if (!memcmp(t, "IHDR", 4) && cl >= 13) {
            w = (int)be32(d);
            h = (int)be32(d + 4);
            bpp = d[9] == 6 ? 4 : d[9] == 2 ? 3 : 0;
            if (d[8] != 8 || d[12] || !bpp || w < 1 || h < 1 || w > 4096 || h > 4096)
                goto done;
        } else if (!memcmp(t, "IDAT", 4)) {
            memcpy(idat + ni, d, cl);
            ni += cl;
        } else if (!memcmp(t, "IEND", 4))
            break;
        pos += 12 + cl;
    }
    stride = (size_t)w * (size_t)bpp;
    if (!bpp || !(raw = malloc((stride + 1) * (size_t)h)) ||
        zlib_inflate(idat, ni, raw, (stride + 1) * (size_t)h) != (long)((stride + 1) * (size_t)h) ||
        !(px = malloc((size_t)w * (size_t)h * sizeof *px)))
        goto done;
    for (y = 0; y < h; y++) {             /* undo the row filters, in place */
        uint8_t *r = raw + (size_t)y * (stride + 1), ft = r[0];
        const uint8_t *up = y ? r - stride : NULL;
        size_t i;
        r++;
        for (i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? r[i - bpp] : 0, b = up ? up[i] : 0, c = up && i >= (size_t)bpp ? up[i - bpp] : 0;
            r[i] = (uint8_t)(r[i] + (ft == 1 ? a : ft == 2 ? b : ft == 3 ? (a + b) / 2 : ft == 4 ? paeth(a, b, c) : 0));
        }
        for (x = 0; x < w; x++) {
            const uint8_t *p = r + x * bpp;
            uint16_t c = (uint16_t)((p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
            px[y * w + x] = bpp == 4 && p[3] < 128 ? 0 : c ? c : 0x0421;
        }
    }
    *pw = w;
    *ph = h;
done:
    if (!px)
        LOGW("texture pack: cannot read %s (an 8-bit RGB or RGBA PNG)", path);
    free(f);
    free(idat);
    free(raw);
    return px;
}

/* Reads every <key>.png (key: 16 hex digits) in dir. Returns the number of images there are. */
int texpack_load(const char *dir)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return nimgs;
    while ((e = readdir(d))) {
        const char *s = e->d_name;
        char path[1200];
        TexImage im;
        if (strlen(s) != 20 || strspn(s, "0123456789abcdefABCDEF") != 16 || (strcmp(s + 16, ".png") && strcmp(s + 16, ".PNG")))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, s);
        if (!(im.px = png_load(path, &im.w, &im.h)))
            continue;
        im.key = strtoull(s, NULL, 16);
        im.thin = NULL;
        if (texpack_find(im.key)) {         /* the same key again (e.g. .png and .PNG): keep the first */
            free(im.px);
            continue;
        }
        imgs = realloc(imgs, (size_t)(nimgs + 1) * sizeof *imgs);
        imgs[nimgs++] = im;
        qsort(imgs, (size_t)nimgs, sizeof *imgs, key_cmp);
    }
    closedir(d);
    g_texpack = nimgs > 0;
    return nimgs;
}

int g_hd_images;

/* r's image with its opaque part shrunk by rad pixels (a pixel stays only if every pixel up to rad
 * away across and down is opaque), made once (the GPU and its in-between redraws on other threads
 * may ask at once: the first one stored is kept) */
const uint16_t *texpack_thin(const TexImage *r, int rad)
{
    TexImage *m = (TexImage *)r;
    uint16_t *have = __atomic_load_n(&m->thin, __ATOMIC_ACQUIRE), *out;
    uint8_t *a, *b;
    int x, y, k, w = r->w, h = r->h;
    if (have)
        return have;
    out = malloc((size_t)w * h * sizeof *out);
    a = malloc((size_t)w * h);
    b = malloc((size_t)w * h);
    if (!out || !a || !b) {
        free(out);
        free(a);
        free(b);
        return r->px;
    }
    for (y = 0; y < h; y++)         /* across: opaque if rad pixels each side are */
        for (x = 0; x < w; x++) {
            int ok = 1;
            for (k = -rad; k <= rad && ok; k++)
                ok = x + k >= 0 && x + k < w && r->px[y * w + x + k] != 0;
            a[y * w + x] = (uint8_t)ok;
        }
    for (y = 0; y < h; y++)         /* then down */
        for (x = 0; x < w; x++) {
            int ok = 1;
            for (k = -rad; k <= rad && ok; k++)
                ok = y + k >= 0 && y + k < h && a[(y + k) * w + x];
            b[y * w + x] = (uint8_t)ok;
        }
    for (k = 0; k < w * h; k++)
        out[k] = b[k] ? r->px[k] : 0;
    free(a);
    free(b);
    if (!__atomic_compare_exchange_n(&m->thin, &have, out, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        free(out);          /* another thread made it first */
        return have;
    }
    return out;
}

void texpack_dump_to(const char *dir)
{
    char parent[1024], *s;
    snprintf(dump_dir, sizeof dump_dir, "%s", dir);
    snprintf(parent, sizeof parent, "%s", dir);
    if ((s = strrchr(parent, '/')) != NULL || (s = strrchr(parent, '\\')) != NULL) {
        *s = 0;
        mkdir_one(parent);
    }
    mkdir_one(dir);
    g_texdump = 1;
}

static uint32_t crc_png(uint32_t c, const uint8_t *p, size_t n)
{
    int k;
    c = ~c;
    while (n--)
        for (c ^= *p++, k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    return ~c;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void png_chunk(FILE *fp, const char *type, const uint8_t *d, size_t n)
{
    uint8_t b[8];
    put32(b, (uint32_t)n);
    memcpy(b + 4, type, 4);
    fwrite(b, 1, 8, fp);
    fwrite(d, 1, n, fp);
    put32(b, crc_png(crc_png(0, b + 4, 4), d, n));
    fwrite(b, 1, 4, fp);
}

/* Writes texels as <dump folder>/<key>.png (RGBA; texel 0 transparent, bit 15 not kept), once per key
 * and session, and not over a file that is there already. */
void texpack_dump(uint64_t key, int w, int h, const uint16_t *px)
{
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    char path[1200];
    size_t stride = (size_t)w * 4 + 1, n = stride * (size_t)h, nb = (n + 65534) / 65535, i, o = 2;
    uint8_t ihdr[13] = { 0 }, *raw, *z;
    uint32_t a = 1, b = 0, s = (uint32_t)key & (DUMPED_N - 1);
    FILE *fp;
    int k;
    while (dumped[s] && dumped[s] != key)
        s = (s + 1) & (DUMPED_N - 1);
    if (dumped[s] == key || ndumped >= DUMPED_N / 2)
        return;
    dumped[s] = key;
    ndumped++;
    snprintf(path, sizeof path, "%s/%016llx.png", dump_dir, (unsigned long long)key);
    if ((fp = fopen(path, "rb")) != NULL) {
        fclose(fp);
        return;
    }
    raw = malloc(n);
    z = malloc(n + nb * 5 + 6);
    if (!raw || !z || !(fp = fopen(path, "wb"))) {
        free(raw);
        free(z);
        return;
    }
    for (k = 0; k < w * h; k++) {
        uint8_t *p = raw + (size_t)(k / w) * stride + 1 + (size_t)(k % w) * 4;
        uint32_t t = px[k], r = t & 31u, g = (t >> 5) & 31u, bl = (t >> 10) & 31u;
        if (k % w == 0)
            p[-1] = 0;                      /* filter: none */
        p[0] = (uint8_t)((r << 3) | (r >> 2));
        p[1] = (uint8_t)((g << 3) | (g >> 2));
        p[2] = (uint8_t)((bl << 3) | (bl >> 2));
        p[3] = t ? 255 : 0;
    }
    z[0] = 0x78;                            /* zlib, stored (uncompressed) deflate blocks */
    z[1] = 0x01;
    for (i = 0; i < n; i += 65535) {
        size_t m = n - i < 65535 ? n - i : 65535;
        z[o] = i + m == n;
        z[o + 1] = (uint8_t)m;
        z[o + 2] = (uint8_t)(m >> 8);
        z[o + 3] = (uint8_t)~m;
        z[o + 4] = (uint8_t)(~m >> 8);
        memcpy(z + o + 5, raw + i, m);
        o += 5 + m;
    }
    for (i = 0; i < n; i++) {
        a = (a + raw[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    put32(z + o, (b << 16) | a);
    put32(ihdr, (uint32_t)w);
    put32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;                            /* 8 bits, RGBA */
    ihdr[9] = 6;
    fwrite(sig, 1, 8, fp);
    png_chunk(fp, "IHDR", ihdr, 13);
    png_chunk(fp, "IDAT", z, o + 4);
    png_chunk(fp, "IEND", NULL, 0);
    fclose(fp);
    free(raw);
    free(z);
    LOGI("texture dumped: %s (%dx%d)", path, w, h);
}
