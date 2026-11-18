/*
 * CarPlay ETS2 v2 - SANS telephone.
 * Plugin de telemetrie SCS : lit les donnees du jeu (vitesse, limite, mission,
 * distance restante, heure) et dessine un ecran style CarPlay en overlay.
 * Compiler avec les en-tetes du SDK de telemetrie SCS (voir LISEZMOI.txt).
 */
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "scssdk_telemetry.h"

#define HK_TOGGLE 1
#define HK_PREV   2
#define HK_PLAY   3
#define HK_NEXT   4

/* ---------- donnees du jeu ---------- */
static CRITICAL_SECTION g_cs;
static float  d_speed = 0, d_limit = 0, d_navdist = 0, d_navtime = 0;
static unsigned int d_gametime = 0;
static char   d_from[64], d_to[64], d_cargo[64];
static float  d_fuel = 0;
static double d_x = 0, d_z = 0, d_heading = 0;

/* ---------- fenetre ---------- */
static HANDLE g_thread = NULL;
static HWND   g_win = NULL;
static char   g_ini[MAX_PATH];
static int    g_x, g_y, g_w, g_h, g_vk, g_visible = 1;
static int    g_port = 8777, g_overlay = 0;
static char   g_bind[64] = "127.0.0.1";
static char   g_dir[MAX_PATH];
static volatile int g_run = 0;
static HANDLE g_http = NULL;
static HFONT  f_big, f_mid, f_small;

static void load_config(void)
{
    g_x  = GetPrivateProfileIntA("carplay", "X", 1100, g_ini);
    g_y  = GetPrivateProfileIntA("carplay", "Y", 600, g_ini);
    g_w  = GetPrivateProfileIntA("carplay", "Width", 720, g_ini);
    g_h  = GetPrivateProfileIntA("carplay", "Height", 300, g_ini);
    g_vk = GetPrivateProfileIntA("carplay", "ToggleKey", VK_F8, g_ini);
    g_port = GetPrivateProfileIntA("carplay", "Port", 8777, g_ini);
    g_overlay = GetPrivateProfileIntA("carplay", "Overlay", 0, g_ini);
    GetPrivateProfileStringA("carplay", "Bind", "127.0.0.1", g_bind, sizeof(g_bind), g_ini);
}

/* ---------- callbacks telemetrie ---------- */
SCSAPI_VOID on_float(const scs_string_t name, const scs_u32_t index,
                     const scs_value_t *value, const scs_context_t context)
{
    float *dst = (float *)context;
    (void)name; (void)index;
    EnterCriticalSection(&g_cs);
    *dst = (value && value->type == SCS_VALUE_TYPE_float) ? value->value_float.value : 0.0f;
    LeaveCriticalSection(&g_cs);
}

SCSAPI_VOID on_time(const scs_string_t name, const scs_u32_t index,
                    const scs_value_t *value, const scs_context_t context)
{
    (void)name; (void)index; (void)context;
    EnterCriticalSection(&g_cs);
    d_gametime = (value && value->type == SCS_VALUE_TYPE_u32) ? value->value_u32.value : 0;
    LeaveCriticalSection(&g_cs);
}

SCSAPI_VOID on_config(const scs_event_t event, const void *event_info, const scs_context_t context)
{
    const struct scs_telemetry_configuration_t *cfg = (const struct scs_telemetry_configuration_t *)event_info;
    const scs_named_value_t *a;
    (void)event; (void)context;
    if (!cfg || !cfg->id || strcmp(cfg->id, "job") != 0) return;

    EnterCriticalSection(&g_cs);
    d_from[0] = d_to[0] = d_cargo[0] = 0;   /* pas de mission = champs vides */
    for (a = cfg->attributes; a && a->name; ++a) {
        if (a->value.type != SCS_VALUE_TYPE_string) continue;
        if (!strcmp(a->name, "source_city"))           strncpy(d_from,  a->value.value_string.value, 63);
        else if (!strcmp(a->name, "destination_city")) strncpy(d_to,    a->value.value_string.value, 63);
        else if (!strcmp(a->name, "cargo"))            strncpy(d_cargo, a->value.value_string.value, 63);
    }
    LeaveCriticalSection(&g_cs);
}


SCSAPI_VOID on_place(const scs_string_t name, const scs_u32_t index,
                     const scs_value_t *value, const scs_context_t context)
{
    (void)name; (void)index; (void)context;
    if (!value || value->type != SCS_VALUE_TYPE_dplacement) return;
    EnterCriticalSection(&g_cs);
    d_x = value->value_dplacement.position.x;
    d_z = value->value_dplacement.position.z;
    d_heading = value->value_dplacement.orientation.heading;   /* 0..1, 0 = nord */
    LeaveCriticalSection(&g_cs);
}

/* ---------- mini serveur HTTP (page web + API JSON) ---------- */
static void jesc(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    for (; *src && i + 3 < n; ++src) {
        if (*src == '"' || *src == '\\') { dst[i++] = '\\'; dst[i++] = *src; }
        else if ((unsigned char)*src >= 32) dst[i++] = *src;
    }
    dst[i] = 0;
}

static int build_json(char *out, int n)
{
    float speed, limit, dist, ntime, fuel; unsigned int gt; double x, z, hd;
    char from[64], to[64], cargo[64], ef[140], et[140], ec[140];
    EnterCriticalSection(&g_cs);
    speed = d_speed; limit = d_limit; dist = d_navdist; ntime = d_navtime; fuel = d_fuel; gt = d_gametime;
    x = d_x; z = d_z; hd = d_heading;
    strcpy(from, d_from); strcpy(to, d_to); strcpy(cargo, d_cargo);
    LeaveCriticalSection(&g_cs);
    jesc(ef, sizeof(ef), from); jesc(et, sizeof(et), to); jesc(ec, sizeof(ec), cargo);
    return snprintf(out, n,
        "{\"speed\":%.1f,\"limit\":%.1f,\"navDist\":%.0f,\"navTime\":%.0f,\"gameTime\":%u,"
        "\"fuelRange\":%.0f,\"x\":%.1f,\"z\":%.1f,\"heading\":%.4f,"
        "\"from\":\"%s\",\"to\":\"%s\",\"cargo\":\"%s\"}",
        speed * 3.6f, limit * 3.6f, dist, ntime, gt, fuel, x, z, hd, ef, et, ec);
}

static void send_all(SOCKET s, const char *b, int n)
{
    while (n > 0) { int k = send(s, b, n, 0); if (k <= 0) return; b += k; n -= k; }
}

static void respond(SOCKET s, const char *status, const char *ctype, const char *body, int len)
{
    char hdr[300];
    int n = sprintf(hdr,
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
        status, ctype, len);
    send_all(s, hdr, n);
    if (len > 0) send_all(s, body, len);
}

static void serve_page(SOCKET s)
{
    char path[MAX_PATH]; HANDLE f; DWORD size, got = 0; char *buf;
    strcpy(path, g_dir); strcat(path, "carplay.html");
    f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        const char *m = "carplay.html introuvable a cote de la DLL.";
        respond(s, "404 Not Found", "text/plain; charset=utf-8", m, (int)strlen(m));
        return;
    }
    size = GetFileSize(f, NULL);
    buf = (char *)malloc(size + 1);
    if (buf && ReadFile(f, buf, size, &got, NULL))
        respond(s, "200 OK", "text/html; charset=utf-8", buf, (int)got);
    free(buf); CloseHandle(f);
}

static void media_key(WORD vk);

static void handle_client(SOCKET c)
{
    char req[2048], method[8], path[256], json[1024];
    int n = recv(c, req, sizeof(req) - 1, 0);
    if (n <= 0) return;
    req[n] = 0;
    if (sscanf(req, "%7s %255s", method, path) != 2) return;

    if (!strcmp(method, "OPTIONS")) { respond(c, "204 No Content", "text/plain", "", 0); return; }
    if (!strncmp(path, "/api", 4)) {
        int len = build_json(json, sizeof(json));
        respond(c, "200 OK", "application/json; charset=utf-8", json, len);
    } else if (!strncmp(path, "/media", 6)) {
        if (strstr(path, "cmd=prev")) media_key(VK_MEDIA_PREV_TRACK);
        else if (strstr(path, "cmd=play")) media_key(VK_MEDIA_PLAY_PAUSE);
        else if (strstr(path, "cmd=next")) media_key(VK_MEDIA_NEXT_TRACK);
        respond(c, "200 OK", "text/plain", "ok", 2);
    } else if (!strcmp(path, "/") || !strncmp(path, "/index", 6) || !strncmp(path, "/carplay", 8) || path[1] == '?') {
        serve_page(c);
    } else {
        respond(c, "404 Not Found", "text/plain", "404", 3);
    }
}

static DWORD WINAPI http_thread(LPVOID unused)
{
    WSADATA w; SOCKET ls; struct sockaddr_in a; int one = 1; (void)unused;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 1;
    ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) { WSACleanup(); return 1; }
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = inet_addr(g_bind);
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 8) != 0) {
        closesocket(ls); WSACleanup(); return 1;
    }
    while (g_run) {
        fd_set fs; struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 200000;
        FD_ZERO(&fs); FD_SET(ls, &fs);
        if (select(0, &fs, NULL, NULL, &tv) > 0) {
            SOCKET c = accept(ls, NULL, NULL);
            if (c != INVALID_SOCKET) {
                DWORD to = 2000;
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
                handle_client(c);
                closesocket(c);
            }
        }
    }
    closesocket(ls); WSACleanup();
    return 0;
}

/* ---------- dessin ---------- */
static void txt(HDC dc, HFONT f, COLORREF c, int x, int y, const char *s)
{
    SelectObject(dc, f);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
}

static void panel(HDC dc, int x, int y, int w, int h, COLORREF fill)
{
    HBRUSH b = CreateSolidBrush(fill);
    HPEN   p = CreatePen(PS_NULL, 0, 0);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, x, y, x + w, y + h, 24, 24);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

static void paint(HDC dc, int w, int h)
{
    HDC m = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ old = SelectObject(m, bmp);
    RECT r = {0, 0, w, h};
    HBRUSH bg = CreateSolidBrush(RGB(12, 12, 18));
    float speed, limit, dist, ntime; unsigned int gt;
    char from[64], to[64], cargo[64], buf[128];
    int hh, mm, rh, rm, half = w / 2;

    EnterCriticalSection(&g_cs);
    speed = d_speed; limit = d_limit; dist = d_navdist; ntime = d_navtime; gt = d_gametime;
    strcpy(from, d_from); strcpy(to, d_to); strcpy(cargo, d_cargo);
    LeaveCriticalSection(&g_cs);

    FillRect(m, &r, bg); DeleteObject(bg);
    SetBkMode(m, TRANSPARENT);

    /* panneau gauche : vitesse + limite + heure */
    panel(m, 12, 12, half - 18, h - 70, RGB(28, 28, 38));
    sprintf(buf, "%d", (int)(speed * 3.6f + 0.5f));
    txt(m, f_big, RGB(255, 255, 255), 36, 24, buf);
    txt(m, f_small, RGB(150, 150, 165), 40, 112, "km/h");
    if (limit > 0.5f) {
        sprintf(buf, "Limite %d", (int)(limit * 3.6f + 0.5f));
        txt(m, f_mid, RGB(255, 120, 60), 36, 150, buf);
    }
    hh = (int)((gt / 60) % 24); mm = (int)(gt % 60);
    sprintf(buf, "%02d:%02d", hh, mm);
    txt(m, f_mid, RGB(200, 200, 215), 36, h - 118, buf);

    /* panneau droit : mission */
    panel(m, half + 6, 12, half - 18, h - 70, RGB(28, 28, 38));
    if (to[0]) {
        txt(m, f_small, RGB(150, 150, 165), half + 28, 26, "MISSION");
        sprintf(buf, "%s", from);
        txt(m, f_mid, RGB(255, 255, 255), half + 28, 52, buf);
        txt(m, f_small, RGB(255, 120, 60), half + 28, 90, "vers");
        sprintf(buf, "%s", to);
        txt(m, f_mid, RGB(255, 255, 255), half + 28, 112, buf);
        sprintf(buf, "Cargaison : %s", cargo);
        txt(m, f_small, RGB(200, 200, 215), half + 28, 156, buf);
        rh = (int)(ntime / 3600.0f); rm = (int)((ntime - rh * 3600.0f) / 60.0f);
        sprintf(buf, "%d km  -  %dh%02d", (int)(dist / 1000.0f + 0.5f), rh, rm);
        txt(m, f_mid, RGB(255, 120, 60), half + 28, 184, buf);
    } else {
        txt(m, f_mid, RGB(150, 150, 165), half + 28, 100, "Aucune mission");
    }

    /* barre du bas : musique (touches multimedia) */
    panel(m, 12, h - 52, w - 24, 40, RGB(28, 28, 38));
    txt(m, f_small, RGB(230, 230, 240), 36, h - 42, "F9  <<     F10  Lecture / Pause     F11  >>     F8  Masquer");

    BitBlt(dc, 0, 0, w, h, m, 0, 0, SRCCOPY);
    SelectObject(m, old); DeleteObject(bmp); DeleteDC(m);
}

static void media_key(WORD vk)
{
    keybd_event((BYTE)vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; RECT rc;
        HDC dc = BeginPaint(h, &ps);
        GetClientRect(h, &rc);
        paint(dc, rc.right, rc.bottom);
        EndPaint(h, &ps);
        return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_TIMER:
        if (g_visible) {
            SetWindowPos(h, HWND_TOPMOST, g_x, g_y, g_w, g_h, SWP_NOACTIVATE);
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) { g_visible = !g_visible; ShowWindow(h, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE); }
        else if (wp == HK_PREV) media_key(VK_MEDIA_PREV_TRACK);
        else if (wp == HK_PLAY) media_key(VK_MEDIA_PLAY_PAUSE);
        else if (wp == HK_NEXT) media_key(VK_MEDIA_NEXT_TRACK);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, msg, wp, lp);
}

static DWORD WINAPI ui_thread(LPVOID unused)
{
    WNDCLASSA wc; MSG msg; (void)unused;
    ZeroMemory(&wc, sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CarPlayETS2v2";
    RegisterClassA(&wc);

    f_big   = CreateFontA(96, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    f_mid   = CreateFontA(32, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    f_small = CreateFontA(20, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");

    g_win = CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                            wc.lpszClassName, "CarPlay ETS2", WS_POPUP | WS_VISIBLE,
                            g_x, g_y, g_w, g_h, NULL, NULL, wc.hInstance, NULL);
    if (!g_win) return 1;

    RegisterHotKey(g_win, HK_TOGGLE, 0, (UINT)g_vk);
    RegisterHotKey(g_win, HK_PREV, 0, VK_F9);
    RegisterHotKey(g_win, HK_PLAY, 0, VK_F10);
    RegisterHotKey(g_win, HK_NEXT, 0, VK_F11);
    SetTimer(g_win, 1, 150, NULL);

    while (GetMessageA(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }

    UnregisterHotKey(g_win, HK_TOGGLE); UnregisterHotKey(g_win, HK_PREV);
    UnregisterHotKey(g_win, HK_PLAY);   UnregisterHotKey(g_win, HK_NEXT);
    DeleteObject(f_big); DeleteObject(f_mid); DeleteObject(f_small);
    return 0;
}

/* ---------- points d'entree SCS ---------- */
SCSAPI_RESULT scs_telemetry_init(const scs_u32_t version, const scs_telemetry_init_params_t *const params)
{
    const scs_telemetry_init_params_v101_t *p = (const scs_telemetry_init_params_v101_t *)params;
    HMODULE self = NULL; char *slash;

    if (version < SCS_TELEMETRY_VERSION_1_01) return SCS_RESULT_unsupported;

    InitializeCriticalSection(&g_cs);
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&scs_telemetry_init, &self);
    GetModuleFileNameA(self, g_ini, sizeof(g_ini));
    slash = strrchr(g_ini, '\\');
    if (slash) *(slash + 1) = 0;
    strcat(g_ini, "carplay_plugin.ini");
    load_config();
    strcpy(g_dir, g_ini);
    slash = strrchr(g_dir, '\\');
    if (slash) *(slash + 1) = 0;

    p->register_for_event(SCS_TELEMETRY_EVENT_configuration, on_config, NULL);
    p->register_for_channel("truck.speed", SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, on_float, &d_speed);
    p->register_for_channel("truck.navigation.speed.limit", SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, on_float, &d_limit);
    p->register_for_channel("truck.navigation.distance", SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, on_float, &d_navdist);
    p->register_for_channel("truck.navigation.time", SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, on_float, &d_navtime);
    p->register_for_channel("truck.world.placement", SCS_U32_NIL, SCS_VALUE_TYPE_dplacement, SCS_TELEMETRY_CHANNEL_FLAG_none, on_place, NULL);
    p->register_for_channel("truck.fuel.range", SCS_U32_NIL, SCS_VALUE_TYPE_float, SCS_TELEMETRY_CHANNEL_FLAG_none, on_float, &d_fuel);
    p->register_for_channel("game.time", SCS_U32_NIL, SCS_VALUE_TYPE_u32, SCS_TELEMETRY_CHANNEL_FLAG_none, on_time, NULL);

    g_run = 1;
    g_http = CreateThread(NULL, 0, http_thread, NULL, 0, NULL);
    if (g_overlay) g_thread = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    return SCS_RESULT_ok;
}

SCSAPI_VOID scs_telemetry_shutdown(void)
{
    g_run = 0;
    if (g_http) { WaitForSingleObject(g_http, 2000); CloseHandle(g_http); g_http = NULL; }
    if (g_win) PostMessageA(g_win, WM_CLOSE, 0, 0);
    if (g_thread) { WaitForSingleObject(g_thread, 2000); CloseHandle(g_thread); g_thread = NULL; }
    g_win = NULL;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
