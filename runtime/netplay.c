/* Online co-op (netplay): two copies of the game, one on each PC, kept in lockstep.
 *
 * The emulation is deterministic (a recorded session replays exactly from the same memory card
 * and the same pad input), so it is enough that both copies start alike and get the same pad
 * input on every frame. The host sends its memory cards and the settings that change what the
 * game does; then each side sends its own buttons for frame f + delay while running frame f, and
 * both play the two players' buttons together, so the two of them control the same farmer.
 * Every second both send a hash of the game's RAM: different hashes mean the copies drifted.
 *
 * One TCP connection; after the handshake every message is 12 bytes (see send_msg). The guest
 * plays on copies of the host's cards in saves/netplay, so its own saves are never touched.
 * When the connection drops, each side goes on alone. */
#include "psx.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define close_sock closesocket
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define close_sock close
#endif

#define NET_VERSION 2
#define RING 256                /* frames of input kept (more than any delay) */
#define HASH_EVERY 60
#define TIMEOUT_MS 15000
#define CARDS_TIMEOUT_MS 300000  /* the host waits this long for the friend to get its cards (slow links) */

enum { MSG_INPUT = 1, MSG_HASH = 2, MSG_BYE = 3 };

static sock_t sock = INVALID_SOCKET;
static int role;                /* 0 off, 1 host, 2 guest */
static int active;              /* connected and in lockstep */
static int delay = 3;
static int port = 7845;
static char host_addr[256];
static uint16_t local_in[RING], remote_in[RING];
static int remote_upto = -1;    /* remote input known for frames <= this */
static uint32_t local_hash[RING / HASH_EVERY + 2], remote_hash[RING / HASH_EVERY + 2];
static int local_hash_f[RING / HASH_EVERY + 2], remote_hash_f[RING / HASH_EVERY + 2];
static char status[96];
static int status_dirty;
static int desynced;
static uint8_t rx[64];
static int rx_n;

static void set_status(const char *s) {
    snprintf(status, sizeof status, "%s", s);
    status_dirty = 1;
    LOGI("netplay: %s", s);
}

const char *netplay_status(int *changed) {
    if (changed) { *changed = status_dirty; status_dirty = 0; }
    return role ? status : NULL;
}

int netplay_role(void) { return role; }
int netplay_active(void) { return active; }

/* --host PORT / --join ADDRESS[:PORT] / --netdelay N */
void netplay_host(int p) { role = 1; if (p > 0) port = p; }
void netplay_join(const char *a) {
    const char *c = strrchr(a, ':');
    role = 2;
    if (c && !strchr(c + 1, ']') && strchr(a, ':') == c) {   /* "addr:port" (not a bare IPv6 address) */
        snprintf(host_addr, sizeof host_addr, "%.*s", (int)(c - a), a);
        port = atoi(c + 1);
    } else {
        snprintf(host_addr, sizeof host_addr, "%s", a);
    }
    if (host_addr[0] == '[') {                                /* "[v6]:port" */
        size_t n = strlen(host_addr);
        memmove(host_addr, host_addr + 1, n);
        if (n > 1 && host_addr[n - 2] == ']') host_addr[n - 2] = 0;
    }
}
void netplay_set_delay(int d) { delay = d < 1 ? 1 : d > 30 ? 30 : d; }

/* ------------------------------------------------------------------ socket helpers */
static int closing(void) {      /* the player closed the window while waiting */
    if (!g_cfg.headless && platform_poll_quit()) g_quit = 1;
    return g_quit;
}

static uint64_t now_ms(void) {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
#endif
}

static int wait_readable(int ms) {
    fd_set r;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&r);
    FD_SET(sock, &r);
    return select((int)sock + 1, &r, NULL, NULL, &tv) > 0;
}

static int wait_writable(int ms) {
    fd_set w;
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    FD_ZERO(&w);
    FD_SET(sock, &w);
    return select((int)sock + 1, NULL, &w, NULL, &tv) > 0;
}

static int would_block(void) {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
#endif
}

/* also on the non-blocking socket of the game: a full send buffer (the cards still on their way
 * over a slow link) is waited out, not taken for a lost connection */
static int send_all(const void *p, int n) {
    const char *c = p;
    while (n > 0) {
        int k = send(sock, c, n, 0);
        if (k < 0 && would_block()) {
            if (!wait_writable(TIMEOUT_MS)) return 0;
            continue;
        }
        if (k <= 0) return 0;
        c += k;
        n -= k;
    }
    return 1;
}

/* blocking read of exactly n bytes (handshake only), giving up after ms of silence */
static int recv_wait(void *p, int n, int ms) {
    char *c = p;
    while (n > 0) {
        int waited = 0;
        while (!wait_readable(50)) {
            if (closing() || (waited += 50) > ms) return 0;
        }
        int k = recv(sock, c, n, 0);
        if (k <= 0) return 0;
        c += k;
        n -= k;
    }
    return 1;
}

static int recv_all(void *p, int n) { return recv_wait(p, n, TIMEOUT_MS); }

static int recv_line_wait(char *out, int n, int ms) {
    int i = 0;
    while (i < n - 1) {
        if (!recv_wait(out + i, 1, ms)) return 0;
        if (out[i] == '\n') break;
        i++;
    }
    out[i] = 0;
    return 1;
}

static int recv_line(char *out, int n) { return recv_line_wait(out, n, TIMEOUT_MS); }

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static uint32_t get32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void drop(const char *why);

static void send_msg(int type, uint16_t buttons, uint32_t frame, uint32_t value) {
    uint8_t m[12] = { (uint8_t)type, 0, (uint8_t)buttons, (uint8_t)(buttons >> 8) };
    put32(m + 4, frame);
    put32(m + 8, value);
    if (active && !send_all(m, 12)) drop("Coop: a conexão caiu (jogando sozinho)");
}

static void drop(const char *why) {
    if (sock != INVALID_SOCKET) close_sock(sock);
    sock = INVALID_SOCKET;
    if (active && role == 1) p2_leave();    /* the friend's farmer goes, its tools back in the chest */
    active = 0;
    set_status(why);
}

/* both sides must run a build of the same sources (another one would drift): tools/compile.py
 * hashes the runtime and the recompiled code into HMBTN_BUILD_ID, the same on any PC */
#ifndef HMBTN_BUILD_ID
#define HMBTN_BUILD_ID 0
#endif
#define BUILD_TEXT2(x) #x
#define BUILD_TEXT(x) BUILD_TEXT2(x)
/* ... also as text in the executable, where the launcher reads it without running it */
__attribute__((used)) static const char build_tag[] = "HMBTN-BUILD " BUILD_TEXT(HMBTN_BUILD_ID);

unsigned netplay_build_id(void) { return HMBTN_BUILD_ID; }

static uint32_t ram_hash(void) {
    const uint32_t *w = (const uint32_t *)ram_ptr(0x80000000u);
    uint32_t h = 2166136261u;
    for (int i = 0; i < 0x200000 / 4; i++) h = (h ^ w[i]) * 16777619u;
    return h;
}

/* ------------------------------------------------------------------ start */
static const char *card_name(char *out, size_t n, const char *dir, int b) {
    if (b == 0) snprintf(out, n, "%s/card1.mcd", dir);
    else snprintf(out, n, "%s/card1_%d.mcd", dir, b + 1);
    return out;
}

/* the settings that change what the game does: both sides must use the host's */
static void settings_line(char *s, size_t n) {
    snprintf(s, n, "fastcd=%d autoslot=%d quickui=%d ws=%d:%d skipscenes=%d textspeed=%d overclock=%d emptybag=%d "
             "fastcard=%d fastload=%d p2=%d cheats=%d:%d:%d:%d:%d:%d\n", g_cfg.fast_cd, g_cfg.autoslot, g_cfg.quickui,
             g_cfg.ws_w, g_cfg.ws_h, g_cfg.skipscenes, g_cfg.textspeed, g_cfg.overclock, g_cfg.emptybag, g_cfg.fastcard,
             g_cfg.fastload, g_p2_enabled, g_cfg.cheat_money, g_cfg.cheat_stamina, g_cfg.cheat_clock, g_cfg.cheat_bag,
             g_cfg.cheat_items, g_cfg.cheat_tools);
}

static void apply_settings(const char *s) {
    sscanf(s, "fastcd=%d autoslot=%d quickui=%d ws=%d:%d skipscenes=%d textspeed=%d overclock=%d emptybag=%d "
           "fastcard=%d fastload=%d p2=%d cheats=%d:%d:%d:%d:%d:%d", &g_cfg.fast_cd, &g_cfg.autoslot, &g_cfg.quickui,
           &g_cfg.ws_w, &g_cfg.ws_h, &g_cfg.skipscenes, &g_cfg.textspeed, &g_cfg.overclock, &g_cfg.emptybag,
           &g_cfg.fastcard, &g_cfg.fastload, &g_p2_enabled, &g_cfg.cheat_money, &g_cfg.cheat_stamina,
           &g_cfg.cheat_clock, &g_cfg.cheat_bag, &g_cfg.cheat_items, &g_cfg.cheat_tools);
    g_p2_demo = 0;
}

static int host_accept(void) {
    char t[96];
    struct sockaddr_in6 a;
    sock_t ls = socket(AF_INET6, SOCK_STREAM, 0);
    int no = 0, yes = 1;
    if (ls == INVALID_SOCKET) return 0;
    setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&no, sizeof no);   /* IPv4 and IPv6 */
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    memset(&a, 0, sizeof a);
    a.sin6_family = AF_INET6;
    a.sin6_port = htons((uint16_t)port);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) != 0 || listen(ls, 1) != 0) {
        close_sock(ls);
        snprintf(t, sizeof t, "Coop: a porta %d está em uso", port);
        set_status(t);
        return 0;
    }
    snprintf(t, sizeof t, "Coop: esperando o amigo entrar (porta %d)...", port);
    set_status(t);
    for (;;) {
        fd_set r;
        struct timeval tv = { 0, 50000 };
        FD_ZERO(&r);
        FD_SET(ls, &r);
        if (select((int)ls + 1, &r, NULL, NULL, &tv) > 0) {
            sock = accept(ls, NULL, NULL);
            if (sock != INVALID_SOCKET) break;
        }
        if (closing()) { close_sock(ls); return 0; }
    }
    close_sock(ls);
    return 1;
}

static int guest_connect(void) {
    char t[320], ps[16];
    struct addrinfo hints, *res = NULL, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(ps, sizeof ps, "%d", port);
    snprintf(t, sizeof t, "Coop: conectando a %s:%d...", host_addr, port);
    set_status(t);
    for (int tries = 0; tries < 20 && !closing(); tries++) {   /* the host may still be starting */
        if (getaddrinfo(host_addr, ps, &hints, &res) != 0) { set_status("Coop: endereço não encontrado"); return 0; }
        for (ai = res; ai; ai = ai->ai_next) {
            sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (sock == INVALID_SOCKET) continue;
            if (connect(sock, ai->ai_addr, (int)ai->ai_addrlen) == 0) break;
            close_sock(sock);
            sock = INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (sock != INVALID_SOCKET) return 1;
        for (int w = 0; w < 10 && !closing(); w++) {   /* half a second */
#ifdef _WIN32
            Sleep(50);
#else
            usleep(50000);
#endif
        }
    }
    set_status("Coop: não deu para conectar ao anfitrião");
    return 0;
}

/* Connects and makes both sides start alike. The guest calls it before opening its memory cards
 * (it gets the host's), the host after (it sends them). *bank is the 1-based card in slot 1.
 * Returns 0 when there is no co-op (then the game runs alone as usual). */
int netplay_start(int *bank, const char *guest_dir) {
    char line[512];
    int yes = 1;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { set_status("Coop: rede indisponível"); return 0; }
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    if (!(role == 1 ? host_accept() : guest_connect())) { role = role && !g_quit ? role : 0; return 0; }
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&yes, sizeof yes);
    uint32_t me = HMBTN_BUILD_ID;
    if (role == 2) {
        snprintf(line, sizeof line, "HMBTN-NETPLAY %d %08x\n", NET_VERSION, me);
        if (!send_all(line, (int)strlen(line)) || !recv_line(line, sizeof line)) { drop("Coop: o anfitrião não respondeu"); return 0; }
        if (strncmp(line, "OK ", 3)) {
            unsigned theirs = 0;
            if (sscanf(line, "ERR version %x", &theirs) == 1) {     /* the host's version (older hosts: none) */
                char why[160];
                snprintf(why, sizeof why, "Coop: versões diferentes do jogo (a sua: %08x, a do anfitrião: %08x)", me, theirs);
                drop(why);
            } else drop(!strcmp(line, "ERR version") ? "Coop: versões diferentes do jogo (os dois precisam do mesmo .exe)"
                                                     : "Coop: o anfitrião recusou a conexão");
            return 0;
        }
        int d = 0, b = 1;
        sscanf(line + 3, "%d %d", &d, &b);
        netplay_set_delay(d);
        *bank = b;
        if (!recv_line(line, sizeof line)) { drop("Coop: conexão perdida"); return 0; }
        apply_settings(line);
        set_status("Coop: recebendo os saves do anfitrião...");
        for (int i = 0; i < CARD_BANKS; i++) {      /* the host's cards, into saves/netplay */
            uint8_t n4[4];
            char p[1200];
            if (!recv_all(n4, 4)) { drop("Coop: conexão perdida"); return 0; }
            uint32_t n = get32(n4);
            card_name(p, sizeof p, guest_dir, i);
            if (n == 0) { remove(p); continue; }
            if (n > (1u << 20)) { drop("Coop: dados inválidos"); return 0; }
            uint8_t *data = malloc(n);
            if (!recv_all(data, (int)n)) { free(data); drop("Coop: conexão perdida"); return 0; }
            FILE *f = fopen(p, "wb");
            if (f) { fwrite(data, 1, n, f); fclose(f); }
            free(data);
        }
        if (!send_all("READY\n", 6)) { drop("Coop: conexão perdida"); return 0; }
        snprintf(g_cfg.save_dir, sizeof g_cfg.save_dir, "%s", guest_dir);
    } else {
        unsigned v = 0, h = 0;
        if (!recv_line(line, sizeof line) || sscanf(line, "HMBTN-NETPLAY %u %x", &v, &h) != 2) { drop("Coop: conexão inválida"); return 0; }
        if (v != NET_VERSION || h != me) {
            char why[160];
            snprintf(line, sizeof line, "ERR version %08x\n", me);
            send_all(line, (int)strlen(line));
            if (v != NET_VERSION) h = 0;    /* another co-op protocol: its build id means nothing here */
            snprintf(why, sizeof why, "Coop: o amigo tem outra versão do jogo (a sua: %08x, a do amigo: %08x)", me, h);
            drop(why);
            return 0;
        }
        snprintf(line, sizeof line, "OK %d %d\n", delay, *bank);
        send_all(line, (int)strlen(line));
        settings_line(line, sizeof line);
        send_all(line, (int)strlen(line));
        set_status("Coop: mandando os saves para o amigo...");
        for (int i = 0; i < CARD_BANKS; i++) {
            char p[1200];
            uint8_t n4[4];
            uint8_t *data = NULL;
            long n = 0;
            FILE *f = fopen(card_name(p, sizeof p, g_cfg.save_dir, i), "rb");
            if (i == *bank - 1) {                   /* the card in slot 1 as it is now (maybe not written yet) */
                data = memcard_data(0);
                n = 128 * 1024;
            } else if (f) {
                fseek(f, 0, SEEK_END);
                n = ftell(f);
                fseek(f, 0, SEEK_SET);
                data = malloc((size_t)n);
                if (fread(data, 1, (size_t)n, f) != (size_t)n) n = 0;
            }
            if (f) fclose(f);
            put32(n4, (uint32_t)n);
            if (!send_all(n4, 4) || (n && !send_all(data, (int)n))) { drop("Coop: conexão perdida"); return 0; }
            if (data && i != *bank - 1) free(data);
        }
        /* the game starts once the friend has all of them: with several saves that is half a
         * megabyte, which a slow link takes longer than TIMEOUT_MS to carry */
        if (!recv_line_wait(line, sizeof line, CARDS_TIMEOUT_MS) || strcmp(line, "READY")) {
            drop("Coop: o amigo não recebeu os saves (conexão perdida)");
            return 0;
        }
    }
#ifdef _WIN32
    { u_long nb = 1; ioctlsocket(sock, FIONBIO, &nb); }
#else
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
#endif
    g_cfg.allow_fast_forward = 0;   /* Tab would only make the other side wait */
    active = 1;
    atexit(netplay_quit);           /* the game quits with exit(): tell the other side */
    snprintf(line, sizeof line, "Coop: %s (atraso %d)", role == 1 ? "o amigo entrou" : "conectado ao anfitrião", delay);
    set_status(line);
    return 1;
}

/* ------------------------------------------------------------------ per frame */
static void got_msg(const uint8_t *m) {
    uint32_t f = get32(m + 4), v = get32(m + 8);
    if (m[0] == MSG_INPUT) {
        remote_in[f % RING] = (uint16_t)(m[2] | m[3] << 8);
        if ((int)f > remote_upto) remote_upto = (int)f;
    } else if (m[0] == MSG_HASH) {
        int k = (int)(f / HASH_EVERY) % (RING / HASH_EVERY + 2);
        remote_hash[k] = v;
        remote_hash_f[k] = (int)f;
    } else if (m[0] == MSG_BYE) {
        drop("Coop: o amigo saiu (jogando sozinho)");
    }
}

static void pump(void) {
    while (active) {
        int k = recv(sock, (char *)rx + rx_n, (int)sizeof rx - rx_n, 0);
        if (k == 0) { drop("Coop: o amigo saiu (jogando sozinho)"); return; }
        if (k < 0) {
#ifdef _WIN32
            if (WSAGetLastError() == WSAEWOULDBLOCK) return;
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) return;
#endif
            /* also when the other side quit while our buttons were still on the way (a reset) */
            drop("Coop: o amigo saiu ou a conexão caiu (jogando sozinho)");
            return;
        }
        rx_n += k;
        int i = 0;
        for (; i + 12 <= rx_n && active; i += 12) got_msg(rx + i);
        memmove(rx, rx + i, (size_t)(rx_n - i));
        rx_n -= i;
    }
}

static void check_hash(int f) {
    int k = (f / HASH_EVERY) % (RING / HASH_EVERY + 2);
    if (local_hash_f[k] != f || remote_hash_f[k] != f) return;
    if (local_hash[k] != remote_hash[k] && !desynced) {
        char t[96];
        desynced = 1;
        snprintf(t, sizeof t, "Coop: os dois jogos ficaram diferentes (quadro %d)", f);
        set_status(t);
    }
    remote_hash_f[k] = -1;
}

/* The pad input for this frame: this player's buttons go out for frame + delay, and the frame
 * plays both players' buttons for it once the other side's have arrived. */
uint16_t netplay_frame(int frame, uint16_t local) {
    if (!active) return local;
    if (frame % HASH_EVERY == 0) {     /* the state at the start of this frame, the same on both sides */
        int k = (frame / HASH_EVERY) % (RING / HASH_EVERY + 2);
        local_hash[k] = ram_hash();
        local_hash_f[k] = frame;
        send_msg(MSG_HASH, 0, (uint32_t)frame, local_hash[k]);
    }
    if (frame == 0) for (int f = 0; f < delay; f++) local_in[f] = remote_in[f] = 0;   /* the first frames: nobody */
    local_in[(frame + delay) % RING] = local;
    send_msg(MSG_INPUT, local, (uint32_t)(frame + delay), 0);
    int waited = 0;
    static int waiting_shown;
    uint64_t t0 = now_ms();
    while (active && remote_upto < frame && frame >= delay) {
        pump();
        if (remote_upto >= frame || !active) break;
        wait_readable(2);
        if (closing()) { send_msg(MSG_BYE, 0, 0, 0); drop("Coop: saindo"); break; }
        waited = (int)(now_ms() - t0);
        if (waited > TIMEOUT_MS) { drop("Coop: o amigo parou de responder (jogando sozinho)"); break; }
        if (waited > 1000 && !waiting_shown) { waiting_shown = 1; set_status("Coop: esperando o amigo..."); }
    }
    pump();
    if (waiting_shown && active) {
        waiting_shown = 0;
        char t[96];
        snprintf(t, sizeof t, "Coop: %s (atraso %d)", role == 1 ? "o amigo voltou" : "conectado ao anfitrião", delay);
        set_status(t);
    }
    if (frame >= HASH_EVERY) check_hash((frame / HASH_EVERY) * HASH_EVERY);
    if (!active) return local;
    if (frame < delay) return 0;
    if (g_p2_enabled) {             /* two farmers: the host plays the game's farmer, the guest farmer 2 */
        uint16_t host = role == 1 ? local_in[frame % RING] : remote_in[frame % RING];
        g_p2_buttons = role == 1 ? remote_in[frame % RING] : local_in[frame % RING];
        return host;
    }
    return (uint16_t)(local_in[frame % RING] | remote_in[frame % RING]);
}

void netplay_quit(void) {
    if (active) send_msg(MSG_BYE, 0, 0, 0);
    if (sock != INVALID_SOCKET) {
        shutdown(sock, 1);          /* the BYE reaches the other side before the connection closes */
        close_sock(sock);
    }
    sock = INVALID_SOCKET;
    active = 0;
}
