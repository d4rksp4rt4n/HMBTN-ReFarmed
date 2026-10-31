/* Entry point: configuration, game data discovery (the HarvestMoonBTN.pak data pack, or a
 * .cue/.bin disc image as an alternative), boot (loads the game executable from the game data
 * and jumps into the recompiled code). */
#include "psx.h"
#include "gpu.h"
#include "spu.h"
#include "gte.h"
#include "workers.h"
#include <stdarg.h>
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define mkdir_one(p) _mkdir(p)
#else
#include <unistd.h>
#include <dirent.h>
#include <strings.h>
#define mkdir_one(p) mkdir(p, 0755)
#endif

/* create a directory and any missing parents */
static void mkdir_p(const char *path) {
    char p[1200];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if ((*s == '/' || *s == '\\') && s[-1] != ':') {
            char c = *s;
            *s = 0;
            mkdir_one(p);
            *s = c;
        }
    }
    mkdir_one(p);
}

Config g_cfg;
int g_log_level = LOG_INFO;
volatile int g_quit;
static FILE *log_file;

int platform_init(void);
int platform_3d_scene(void);
void platform_shutdown(void);
void platform_error_box(const char *title, const char *msg);
bool platform_set_binding(const char *key, const char *val);

void log_msg(int level, const char *fmt, ...) {
    static const char *tag[] = { "ERROR", "WARN", "INFO", "DEBUG", "TRACE" };
    if (level > g_log_level) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[%s] %s\n", tag[level], buf);
    if (log_file) { fprintf(log_file, "[%s] f%llu %s\n", tag[level], (unsigned long long)frame_count(), buf); fflush(log_file); }
}

void fatal(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    log_msg(LOG_ERR, "FATAL: %s", buf);
    memcard_flush();
    platform_error_box("Harvest Moon: Back to Nature", buf);
    exit(1);
}

/* ------------------------------------------------------------------ paths */
static char exe_dir[1024];

static void find_exe_dir(const char *argv0) {
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe_dir, sizeof exe_dir);
#else
    ssize_t n = readlink("/proc/self/exe", exe_dir, sizeof exe_dir - 1);
    if (n > 0) exe_dir[n] = 0; else snprintf(exe_dir, sizeof exe_dir, "%s", argv0);
#endif
    char *s1 = strrchr(exe_dir, '/'), *s2 = strrchr(exe_dir, '\\');
    char *s = s1 > s2 ? s1 : s2;
    if (s) s[1] = 0; else snprintf(exe_dir, sizeof exe_dir, "./");
}

static bool file_exists(const char *p) { struct stat st; return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR); }


/* look for a .cue (preferred) or .bin in a directory */
static bool scan_dir_for_disc(const char *dir, char *out, size_t n) {
#ifdef _WIN32
    char pat[1200];
    WIN32_FIND_DATAA fd;
    const char *exts[2] = { ".cue", ".bin" };
    for (int k = 0; k < 2; k++) {
        snprintf(pat, sizeof pat, "%s*%s", dir, exts[k]);
        HANDLE h = FindFirstFileA(pat, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            snprintf(out, n, "%s%s", dir, fd.cFileName);
            FindClose(h);
            return true;
        }
    }
    return false;
#else
    const char *exts[2] = { ".cue", ".bin" };
    for (int k = 0; k < 2; k++) {
        DIR *d = opendir(dir[0] ? dir : ".");
        if (!d) return false;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t l = strlen(e->d_name);
            if (l > 4 && !strcasecmp(e->d_name + l - 4, exts[k])) {
                snprintf(out, n, "%s%s", dir, e->d_name);
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    return false;
#endif
}

static bool locate_disc(void) {
    if (g_cfg.disc_path[0]) {
        if (file_exists(g_cfg.disc_path)) return true;
        LOGW("configured game data not found: %s - searching the default locations", g_cfg.disc_path);
        g_cfg.disc_path[0] = 0;
    }
    /* a data pack made from the player's disc (tools/makepak.py) */
    const char *pakdirs[] = { "", "data/", "../" };
    for (size_t i = 0; i < sizeof pakdirs / sizeof *pakdirs; i++) {
        char p[1400];
        snprintf(p, sizeof p, "%s%sHarvestMoonBTN.pak", exe_dir, pakdirs[i]);
        if (file_exists(p)) { snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", p); return true; }
    }
    /* otherwise the player's disc image ("../../disc/" = the repository's disc folder for
     * development builds in build/<config>/) */
    const char *subdirs[] = { "", "disc/", "Disc/", "../../disc/" };
    for (size_t i = 0; i < sizeof subdirs / sizeof *subdirs; i++) {
        char dir[1200];
        snprintf(dir, sizeof dir, "%s%s", exe_dir, subdirs[i]);
        char p[1400];
        snprintf(p, sizeof p, "%sHarvest Moon - Back to Nature.cue", dir);
        if (file_exists(p)) { snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", p); return true; }
        if (scan_dir_for_disc(dir, g_cfg.disc_path, sizeof g_cfg.disc_path)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ config */
static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    char *b = s;
    while (isspace((unsigned char)*b)) b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}

static void set_scaling(const char *val) {
    if (!strcmp(val, "sharp-bilinear")) g_cfg.scaling = SCALING_SHARP_BILINEAR;
    else if (!strcmp(val, "sharp")) g_cfg.scaling = SCALING_SHARP;
    else if (!strcmp(val, "smooth")) g_cfg.scaling = SCALING_SMOOTH;
    else LOGW("unknown scaling '%s' (sharp-bilinear, sharp or smooth)", val);
}

/* "off" / "0", "1" (= 16:9) or an aspect ratio W:H wider than 4:3 */
static void set_widescreen(const char *val) {
    int w = 0, h = 0;
    if (sscanf(val, "%d:%d", &w, &h) == 2 && w > 0 && h > 0 && w * 3 > h * 4) { g_cfg.ws_w = w; g_cfg.ws_h = h; }
    else if (!strcmp(val, "1")) { g_cfg.ws_w = 16; g_cfg.ws_h = 9; }
    else if (!strcmp(val, "0") || !strcmp(val, "off")) g_cfg.ws_w = g_cfg.ws_h = 0;
    else LOGW("unknown widescreen '%s' (off, 16:9, 16:10, 21:9, ...)", val);
}

static void set_option(const char *key, const char *val) {
    if (!strcmp(key, "disc")) snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", val);
    else if (!strcmp(key, "savedir")) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", val);
    else if (!strcmp(key, "scale")) g_cfg.scale = atoi(val);
    else if (!strcmp(key, "upscale")) g_cfg.upscale = atoi(val);
    else if (!strcmp(key, "drawthreads")) g_cfg.draw_threads = !strcmp(val, "auto") ? -1 : atoi(val);
    else if (!strcmp(key, "p2view_upscale")) g_cfg.p2view_upscale = atoi(val);
    else if (!strcmp(key, "pgxp")) g_cfg.pgxp = atoi(val);
    else if (!strcmp(key, "perspective")) g_cfg.perspective = atoi(val);
    else if (!strcmp(key, "sprite_depth")) {}   /* older ini files: always off now (--sprite-depth for tests) */
    else if (!strcmp(key, "crackfix")) g_cfg.crackfix = atoi(val);
    else if (!strcmp(key, "interp")) g_cfg.interp = atoi(val);
    else if (!strcmp(key, "texturepack")) g_cfg.texturepack = atoi(val);
    else if (!strcmp(key, "dumptextures")) g_cfg.dumptextures = atoi(val);
    else if (!strcmp(key, "widescreen")) set_widescreen(val);
    else if (!strcmp(key, "fullscreen")) g_cfg.fullscreen = atoi(val);
    else if (!strcmp(key, "fastcd")) g_cfg.fast_cd = atoi(val);
    else if (!strcmp(key, "record")) g_cfg.record = atoi(val);
    else if (!strcmp(key, "autoslot")) g_cfg.autoslot = atoi(val);
    else if (!strcmp(key, "fastcard")) g_cfg.fastcard = atoi(val);
    else if (!strcmp(key, "quickui")) g_cfg.quickui = atoi(val);
    else if (!strcmp(key, "fastload")) g_cfg.fastload = atoi(val);
    else if (!strcmp(key, "skipscenes")) g_cfg.skipscenes = atoi(val);
    else if (!strcmp(key, "textspeed")) g_cfg.textspeed = atoi(val);
    else if (!strcmp(key, "overclock")) g_cfg.overclock = atoi(val);
    else if (!strcmp(key, "emptybag")) g_cfg.emptybag = atoi(val);
    else if (!strcmp(key, "cheat_money")) g_cfg.cheat_money = atoi(val);
    else if (!strcmp(key, "cheat_stamina")) g_cfg.cheat_stamina = atoi(val);
    else if (!strcmp(key, "cheat_clock")) g_cfg.cheat_clock = atoi(val);
    else if (!strcmp(key, "cheat_bag")) g_cfg.cheat_bag = atoi(val);
    else if (!strcmp(key, "cheat_items")) g_cfg.cheat_items = atoi(val);
    else if (!strcmp(key, "cheat_tools")) g_cfg.cheat_tools = atoi(val);
    else if (!strcmp(key, "smooth")) { if (atoi(val)) g_cfg.scaling = SCALING_SMOOTH; }   /* older ini files */
    else if (!strcmp(key, "scaling")) set_scaling(val);
    else if (!strcmp(key, "aspect43")) g_cfg.aspect_4_3 = atoi(val);
    else if (!strcmp(key, "fastforward")) g_cfg.allow_fast_forward = atoi(val);
    else if (!strcmp(key, "loglevel")) g_log_level = atoi(val);
    else if (!strncmp(key, "coop_", 5)) {}   /* the launcher's co-op page: it passes them as --host / --join */
    else if (!strcmp(key, "language")) {}    /* the launcher's own */
    else if (!platform_set_binding(key, val)) LOGW("unknown config option '%s'", key);
}

static void load_ini(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1200];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = line, *v = eq + 1;
        trim(k);
        trim(v);
        set_option(k, v);
    }
    fclose(f);
}

/* the cheats, changed while the game runs (the launcher opened by F5 saves the ini at each change):
 * 1 = the ini changed since last asked, with v = the cheat_ keys below */
static char ini_path[1200];
static time_t ini_time;

static time_t ini_changed(void) {
    struct stat st;
    return stat(ini_path, &st) == 0 ? st.st_mtime : 0;
}

int config_cheats(int v[CHEATS]) {
    time_t t = ini_changed();
    if (!t || t == ini_time) return 0;
    ini_time = t;
    FILE *f = fopen(ini_path, "r");
    if (!f) return 0;
    char line[1200];
    memset(v, 0, CHEATS * sizeof *v);
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        trim(line);
        static const char *const keys[CHEATS] = { "cheat_money", "cheat_stamina", "cheat_clock", "cheat_bag",
                                                  "cheat_items", "cheat_tools" };
        for (int k = 0; k < CHEATS; k++) if (!strcmp(line, keys[k])) v[k] = atoi(eq + 1);
    }
    fclose(f);
    return 1;
}

/* F5: the launcher on its Cheats page, over the game (the exe, or launcher.pyw with Python) */
void open_cheats_window(void) {
#ifdef _WIN32
    char cmd[2400], exe[1100];
    snprintf(exe, sizeof exe, "%sHarvestMoonBTN-Launcher.exe", exe_dir);
    if (GetFileAttributesA(exe) != INVALID_FILE_ATTRIBUTES)
        snprintf(cmd, sizeof cmd, "\"%s\" --page cheats --live", exe);
    else
        snprintf(cmd, sizeof cmd, "pythonw \"%slauncher.pyw\" --page cheats --live", exe_dir);
    /* the game may have been started by the launcher (a PyInstaller one-file exe), whose variables it
     * inherited: a new launcher took them for its own and looked for the old one's files, gone ("Failed
     * to load Python DLL"). A fresh start for it */
    static const char *const pyi[] = { "_MEIPASS2", "_PYI_ARCHIVE_FILE", "_PYI_APPLICATION_HOME_DIR",
                                       "_PYI_PARENT_PROCESS_LEVEL", "_PYI_SPLASH_IPC" };
    for (size_t i = 0; i < sizeof pyi / sizeof *pyi; i++) SetEnvironmentVariableA(pyi[i], NULL);
    SetEnvironmentVariableA("PYINSTALLER_RESET_ENVIRONMENT", "1");
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        LOGW("cannot open the launcher (%s)", cmd);
    }
#endif
}

static void usage(void) {
    printf("Harvest Moon: Back to Nature - native port\n"
           "usage: hmbtn [options] [HarvestMoonBTN.pak|disc.cue|disc.bin]\n"
           "  --fullscreen          start in fullscreen (F11 toggles)\n"
           "  --scale N             initial window scale (default 3)\n"
           "  --upscale N           internal resolution 1..4 (default 1 = original 320x240)\n"
           "  --drawthreads N       threads drawing the upscaled 3D, 0..4 (default: by processor)\n"
           "  --pgxp                precise 3D vertex positions, less wobble (needs --upscale 2+)\n"
           "  --perspective         perspective-correct textures (needs --pgxp)\n"
           "  --sprite-depth        scale 2D objects with their depth (needs --pgxp)\n"
           "  --no-crackfix         leave the cracks between models open (faster)\n"
           "  --widescreen W:H      wider 3D view, e.g. 16:9 (2D objects keep their shape with --upscale 2+)\n"
           "  --interp              in-between pictures: camera and 3D scenery at 60 fps (needs --pgxp)\n"
           "  --texturepack         draw 2D sprites with the images in textures/ (needs --upscale 2+)\n"
           "  --dumptextures        write the 2D sprites' textures to textures/dump/ (needs --upscale 2+)\n"
           "  --savedir DIR         memory card directory\n"
           "  --fastcd              faster disc loading (music streaming unaffected)\n"
           "  --no-autoslot         ask for the MEMORY CARD slot when saving/loading\n"
           "  --card N              start with memory card N (1-4) in slot 1 (F1-F4 switch)\n"
           "  --scaling MODE        sharp-bilinear (default), sharp or smooth\n"
           "  --log N               log level 0..4\n"
           "  --host [PORT]         online co-op: wait for a friend (default port 7845)\n"
           "  --join ADDRESS[:PORT] online co-op: play on a friend's game\n"
           "  --netdelay N          online co-op: input delay in frames (the host's counts, default 3)\n"
           "  --verify-data         check the game data pack against its checksums and exit\n"
           "testing options:\n"
           "  --headless --frames N --unthrottled --shots N --shotstart F --shotdir DIR --input SCRIPT|@FILE\n"
           "  --record FILE (pad input, replays with --input @FILE) --trace\n");
}

/* ------------------------------------------------------------------ boot */
static void load_exe(void) {
    uint32_t lba, size;
    if (!iso_find_file("SYSTEM.CNF", &lba, &size)) fatal("SYSTEM.CNF not found on disc");
    uint8_t raw[2352];
    cdrom_read_sector_raw(lba, raw);
    char cnf[2049];
    memcpy(cnf, raw + 24, 2048);
    cnf[size < 2048 ? size : 2048] = 0;
    char *boot = strstr(cnf, "BOOT");
    if (!boot) fatal("SYSTEM.CNF has no BOOT line");
    char *p = strchr(boot, ':');
    if (!p) fatal("bad BOOT line");
    p++;
    while (*p == '\\' || *p == '/') p++;
    char path[128];
    int n = 0;
    while (*p && *p != '\r' && *p != '\n' && *p != ' ' && n < 127) path[n++] = *p++;
    path[n] = 0;
    if (!iso_find_file(path, &lba, &size)) fatal("boot file %s not found", path);
    LOGI("boot executable: %s (%u bytes)", path, size);
    uint8_t *exe = malloc(size + 2048);
    for (uint32_t i = 0; i * 2048 < size; i++) {
        cdrom_read_sector_raw(lba + i, raw);
        memcpy(exe + i * 2048, raw + 24, 2048);
    }
    if (memcmp(exe, "PS-X EXE", 8)) fatal("boot file is not a PS-X EXE");
    uint32_t pc, gp, taddr, tsize, saddr, ssize;
    memcpy(&pc, exe + 0x10, 4);
    memcpy(&gp, exe + 0x14, 4);
    memcpy(&taddr, exe + 0x18, 4);
    memcpy(&tsize, exe + 0x1C, 4);
    memcpy(&saddr, exe + 0x30, 4);
    memcpy(&ssize, exe + 0x34, 4);
    if (tsize > size - 0x800) tsize = size - 0x800;
    memcpy(ram_ptr(taddr), exe + 0x800, tsize);
    free(exe);

    /* verify this disc matches the recompiled executable */
    const ModuleDesc *m = &g_modules[0];
    int bad = 0;
    for (int i = 0; i < m->nfuncs; i++)
        if (ram_r32(m->funcs[i].addr) != m->funcs[i].w0 || ram_r32(m->funcs[i].addr + 4) != m->funcs[i].w1) bad++;
    if (bad) fatal("This disc image does not match Harvest Moon: Back to Nature (USA, SLUS-01115).\n"
                   "%d of %d code checks failed.", bad, m->nfuncs);

    g_cpu.r[28] = gp;
    uint32_t sp = saddr ? saddr + ssize : 0x801FFF00u;
    g_cpu.r[29] = sp;
    g_cpu.r[30] = sp;
    g_cpu.r[31] = 0x80000080u;
    LOGI("entry %08X gp %08X sp %08X", pc, gp, sp);
    call_addr(&g_cpu, pc);
    LOGW("game returned from main()");
}

int main(int argc, char **argv) {
    int verify_data = 0;
    int card_bank = 0;   /* 1-4 from --card, 0 = the one last used */
    find_exe_dir(argv[0]);
    g_cfg.scale = 3;
    g_cfg.aspect_4_3 = 1;
    g_cfg.allow_fast_forward = 1;
    g_cfg.autoslot = 1;
    g_cfg.fastcard = 1;
    g_cfg.draw_threads = -1;
    g_cfg.crackfix = 1;
    g_cfg.quickui = 1;
    g_cfg.fastload = 1;
    g_cfg.skipscenes = 1;
    g_cfg.textspeed = 3;
    g_cfg.overclock = 200;
    g_cfg.emptybag = 1;
    char ini[1200];
    snprintf(ini, sizeof ini, "%shmbtn.ini", exe_dir);
    load_ini(ini);
    snprintf(ini_path, sizeof ini_path, "%s", ini);
    ini_time = ini_changed();           /* what is in it now is in use */
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *nx = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(); return 0; }
        else if (!strcmp(a, "--fullscreen")) g_cfg.fullscreen = 1;
        else if (!strcmp(a, "--scale")) { g_cfg.scale = atoi(nx); i++; }
        else if (!strcmp(a, "--upscale")) { g_cfg.upscale = atoi(nx); i++; }
        else if (!strcmp(a, "--drawthreads")) { g_cfg.draw_threads = atoi(nx); i++; }
        else if (!strcmp(a, "--pgxp")) g_cfg.pgxp = 1;
        else if (!strcmp(a, "--perspective")) g_cfg.perspective = 1;
        else if (!strcmp(a, "--sprite-depth")) g_cfg.sprite_depth = 1;
        else if (!strcmp(a, "--no-crackfix")) g_cfg.crackfix = 0;
        else if (!strcmp(a, "--interp")) g_cfg.interp = 1;
        else if (!strcmp(a, "--no-interp")) g_cfg.interp = 0;
        else if (!strcmp(a, "--texturepack")) g_cfg.texturepack = 1;
        else if (!strcmp(a, "--no-texturepack")) g_cfg.texturepack = 0;
        else if (!strcmp(a, "--dumptextures")) g_cfg.dumptextures = 1;
        else if (!strcmp(a, "--widescreen")) { set_widescreen(nx); i++; }
        else if (!strcmp(a, "--savedir")) { snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", nx); i++; }
        else if (!strcmp(a, "--fastcd")) g_cfg.fast_cd = 1;
        else if (!strcmp(a, "--no-autoslot")) g_cfg.autoslot = 0;
        else if (!strcmp(a, "--card")) { card_bank = atoi(nx); i++; }
        else if (!strcmp(a, "--overclock")) { g_cfg.overclock = atoi(nx); i++; }
        else if (!strcmp(a, "--smooth")) g_cfg.scaling = SCALING_SMOOTH;
        else if (!strcmp(a, "--scaling")) { set_scaling(nx); i++; }
        else if (!strcmp(a, "--log")) { g_log_level = atoi(nx); i++; }
        else if (!strcmp(a, "--host")) { if (isdigit((unsigned char)nx[0])) { netplay_host(atoi(nx)); i++; } else netplay_host(0); }
        else if (!strcmp(a, "--join")) { netplay_join(nx); i++; }
        else if (!strcmp(a, "--netdelay")) { netplay_set_delay(atoi(nx)); i++; }
        else if (!strcmp(a, "--p2")) g_p2_enabled = 1;
        else if (!strcmp(a, "--p2demo")) g_p2_enabled = g_p2_demo = 1;
        else if (!strcmp(a, "--p2view")) { g_view2 = atoi(nx); i++; }
        else if (!strcmp(a, "--p2view-upscale")) { g_cfg.p2view_upscale = atoi(nx); i++; }
        else if (!strcmp(a, "--headless")) g_cfg.headless = 1;
        else if (!strcmp(a, "--verify-data")) { verify_data = 1; g_cfg.headless = 1; }
        else if (!strcmp(a, "--frames")) { g_cfg.max_frames = atoi(nx); i++; }
        else if (!strcmp(a, "--unthrottled")) g_cfg.unthrottled = 1;
        else if (!strcmp(a, "--shots")) { g_cfg.screenshot_every = atoi(nx); i++; }
        else if (!strcmp(a, "--shotstart")) { g_cfg.screenshot_start = atoi(nx); i++; }
        else if (!strcmp(a, "--shotdir")) { snprintf(g_cfg.screenshot_dir, sizeof g_cfg.screenshot_dir, "%s", nx); i++; }
        else if (!strcmp(a, "--input")) { snprintf(g_cfg.input_script, sizeof g_cfg.input_script, "%s", nx); i++; }
        else if (!strcmp(a, "--record")) { snprintf(g_cfg.record_path, sizeof g_cfg.record_path, "%s", nx); i++; }
        else if (!strcmp(a, "--dumpram")) {
            for (int k = 0; k < 8; k++) if (!g_cfg.dump_frame[k]) { g_cfg.dump_frame[k] = atoi(nx); break; }
            i++;
        }
        else if (!strcmp(a, "--calltest")) {
            for (int k = 0; k < 4; k++) if (!g_cfg.call_frame[k]) {
                sscanf(nx, "%d:%x:%x", &g_cfg.call_frame[k], &g_cfg.call_addr[k], &g_cfg.call_a0[k]);
                break;
            }
            i++;
        }
        else if (!strcmp(a, "--poke")) {
            for (int k = 0; k < 4; k++) if (!g_cfg.poke_frame[k]) {
                sscanf(nx, "%d:%x:%x", &g_cfg.poke_frame[k], &g_cfg.poke_addr[k], &g_cfg.poke_val[k]);
                break;
            }
            i++;
        }
        else if (!strcmp(a, "--fuzz")) { g_cfg.fuzz_seed = atoi(nx); i++; }
        else if (!strcmp(a, "--fuzzstart")) { g_cfg.fuzz_start = atoi(nx); i++; }
        else if (!strcmp(a, "--wav")) { snprintf(g_cfg.wav_path, sizeof g_cfg.wav_path, "%s", nx); i++; }
        else if (!strcmp(a, "--trace")) g_trace_calls = 1;
        else if (!strcmp(a, "--tracecalls")) g_trace_calls = 2;
        else if (!strcmp(a, "--logfile")) { log_file = fopen(nx, "w"); i++; }
        else if (a[0] != '-') snprintf(g_cfg.disc_path, sizeof g_cfg.disc_path, "%s", a);
        else { usage(); return 1; }
    }
    if (g_cfg.upscale < 1) g_cfg.upscale = 1;
    if (g_cfg.upscale > 4) g_cfg.upscale = 4;
    if (!g_cfg.save_dir[0]) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%ssaves", exe_dir);
    if (!log_file && !g_cfg.headless) {
        char lp[1200];
        snprintf(lp, sizeof lp, "%sHarvestMoonBTN.log", exe_dir);
        log_file = fopen(lp, "w");
    }
    if (!g_cfg.screenshot_dir[0]) snprintf(g_cfg.screenshot_dir, sizeof g_cfg.screenshot_dir, "%sscreenshots", exe_dir);
    if (netplay_role() == 2) { g_cfg.record = 0; g_cfg.record_path[0] = 0; }   /* the host's session is the one */
    if (g_cfg.record && !g_cfg.record_path[0] && !g_cfg.headless) {   /* recordings/session_<date>_<time>.txt */
        char d[1100];
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        snprintf(d, sizeof d, "%srecordings", exe_dir);
        mkdir_p(d);
        snprintf(g_cfg.record_path, sizeof g_cfg.record_path, "%s/session_%04d%02d%02d_%02d%02d%02d.txt", d,
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    }

    if (g_view2 < 0 && netplay_role() != 2) g_view2 = g_p2_enabled && !netplay_role();   /* a window for farmer 2 */
    if (platform_init() != 0) return 1;

    if (!locate_disc())
        fatal("Game data not found.\n\nThis port needs your own copy of Harvest Moon: Back to Nature (USA).\n"
              "Put your disc image (.cue + .bin) into the \"disc\" folder next to the executable,\n"
              "set \"disc =\" in hmbtn.ini, or pass the path on the command line.\n"
              "(A HarvestMoonBTN.pak data pack made from your disc also works.)");
    if (!cdrom_open_image(g_cfg.disc_path)) fatal("Cannot open game data:\n%s", g_cfg.disc_path);
    if (verify_data) {
        cdpak_verify_all();
        printf("game data OK: %s\n", g_cfg.disc_path);
        return 0;
    }

    mkdir_p(g_cfg.save_dir);
    {
        /* fall back to a per-user folder when the game directory is not writable */
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/.write_test", g_cfg.save_dir);
        FILE *t = fopen(probe, "wb");
        if (t) { fclose(t); remove(probe); }
        else {
#ifdef _WIN32
            const char *base = getenv("APPDATA");
            if (base) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/HarvestMoonBTN", base);
#else
            const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
            if (xdg) snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/HarvestMoonBTN", xdg);
            else if (home) {
                char d[1100];
                snprintf(d, sizeof d, "%s/.local", home); mkdir_p(d);
                snprintf(d, sizeof d, "%s/.local/share", home); mkdir_p(d);
                snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s/.local/share/HarvestMoonBTN", home);
            }
#endif
            mkdir_p(g_cfg.save_dir);
            LOGW("game folder is read-only; saving memory cards to %s", g_cfg.save_dir);
        }
    }
    if (netplay_role() == 2) {          /* co-op guest: plays on copies of the host's cards */
        char netdir[1200];
        snprintf(netdir, sizeof netdir, "%s/netplay", g_cfg.save_dir);
        mkdir_p(netdir);
        if (!netplay_start(&card_bank, netdir)) {
            if (!g_quit) fatal("%s", netplay_status(NULL));
            return 0;
        }
        if (g_view2 < 0) g_view2 = g_p2_enabled ? 2 : 0;   /* two farmers: the guest's window follows farmer 2 */
    }
    memcard_open_banks(card_bank);
    if (netplay_role() == 1) {          /* co-op host: sends its cards once the friend is in */
        int b = memcard_bank();
        if (!netplay_start(&b, NULL)) {
            if (!g_quit) fatal("%s", netplay_status(NULL));
            return 0;
        }
    }
    if (g_cfg.overclock < 100) g_cfg.overclock = 100;
    if (g_cfg.overclock > 400) g_cfg.overclock = 400;
    g_cycle_scale = 256u * 100u / (uint32_t)g_cfg.overclock;
    if (g_cfg.overclock != 100) LOGI("CPU overclock %d%%", g_cfg.overclock);
    LOGI("build %08x (online co-op connects only to the same)", netplay_build_id());
    char card1[1200];
    memcard_bank_file(card1, sizeof card1);
    if (g_cfg.record_path[0]) {         /* the replay starts from the same memory card (the one in slot 1): <recording>_card1.mcd */
        char copy[1200];
        size_t k = strlen(g_cfg.record_path);
        FILE *in = fopen(card1, "rb"), *out;
        snprintf(copy, sizeof copy, "%.*s_card1.mcd", (int)(k > 4 && !strcmp(g_cfg.record_path + k - 4, ".txt") ? k - 4 : k), g_cfg.record_path);
        if (in && (out = fopen(copy, "wb")) != NULL) {
            char buf[8192];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, in)) > 0)
                fwrite(buf, 1, n, out);
            fclose(out);
        }
        if (in) fclose(in);
        LOGI("recording the session to %s", g_cfg.record_path);
    }

    mem_reset();
    gte_reset();
    gpu_init();
    g_pgxp_cracks = g_cfg.pgxp && g_cfg.crackfix;   /* before: the upscaled copy gets what it needs */
    gpu_set_upscale(g_cfg.upscale);
    if (gpu_upscale() != g_cfg.upscale) LOGW("upscale %d: not enough memory, using 1", g_cfg.upscale);
    if (gpu_upscale() > 1) {
        /* by processor: one thread per logical processor but the game's, up to 4 */
        int n = g_cfg.draw_threads >= 0 ? g_cfg.draw_threads : wk_cpu_count() - 1;
        gpu_set_threads(n);
        if (gpu_threads()) LOGI("upscaled 3D drawn by %d threads", gpu_threads());
    }
    g_pgxp = g_cfg.pgxp && gpu_upscale() > 1;
    g_pgxp_persp = g_pgxp && g_cfg.perspective;
    g_sprite_depth = g_pgxp && g_cfg.sprite_depth;
    if (g_cfg.pgxp && !g_pgxp) LOGW("pgxp has no effect without upscale 2 or more");
    if (g_cfg.ws_w) {
        /* the game's projection is made for a 4:3 TV: squeeze x by (4/3) / (W/H) */
        int num = 4 * g_cfg.ws_h, den = 3 * g_cfg.ws_w;
        gte_set_widescreen(num, den);
        if (gpu_upscale() > 1) g_sprite_ws = (float)num / (float)den;
        else LOGW("widescreen: 2D objects and people look wider without upscale 2 or more");
        g_scene_3d = platform_3d_scene;
        LOGI("widescreen %d:%d", g_cfg.ws_w, g_cfg.ws_h);
    }
    if ((g_cfg.texturepack || g_cfg.dumptextures) && gpu_upscale() < 2)
        LOGW("texture packs need upscale 2 or more");
    else if (g_cfg.texturepack || g_cfg.dumptextures) {
        char dir[1200];
        snprintf(dir, sizeof dir, "%stextures", exe_dir);
        if (g_cfg.texturepack) LOGI("texture pack: %d images in %s", texpack_load(dir), dir);
        if (g_cfg.dumptextures) {
            snprintf(dir, sizeof dir, "%stextures/dump", exe_dir);
            texpack_dump_to(dir);
            LOGI("dumping textures to %s", dir);
        }
    }
    /* sprite quads at projected points are found by their tags, so texture packs record tags too
     * (without pgxp and widescreen they don't move or scale anything) */
    g_sprite_tags = gpu_upscale() > 1 && (g_pgxp || g_cfg.ws_w || g_texpack || g_texdump);
    if (g_cfg.interp && g_pgxp) gpu_interp_enable();
    else if (g_cfg.interp) LOGW("interp needs upscale 2 or more and pgxp");
    p2view_init();
    spu_init();
    dma_init();
    cdrom_init();
    sio_init();
    timers_init();
    sched_init();
    bios_init();
    g_cpu.sr = 0x40000401u;   /* COP2 enabled, IRQs enabled (I_MASK is still 0) */
    g_pad_connected[0] = true;

    load_exe();
    netplay_quit();
    memcard_flush();
    platform_shutdown();
    return 0;
}
