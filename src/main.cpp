// LiteView - tiny photo/video viewer. Engine: libmpv (loaded at runtime). UI/input/overlay/settings: this file.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <windowsx.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <commdlg.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <mpv/client.h>

// ======================= libmpv (dynamic) =======================
#define F(ret, name, args) typedef ret (*name##_t) args; static name##_t p_##name;
F(mpv_handle*, mpv_create, (void))
F(int, mpv_initialize, (mpv_handle*))
F(int, mpv_set_option_string, (mpv_handle*, const char*, const char*))
F(int, mpv_set_property_string, (mpv_handle*, const char*, const char*))
F(char*, mpv_get_property_string, (mpv_handle*, const char*))
F(int, mpv_get_property, (mpv_handle*, const char*, mpv_format, void*))
F(int, mpv_command, (mpv_handle*, const char**))
F(mpv_event*, mpv_wait_event, (mpv_handle*, double))
F(void, mpv_set_wakeup_callback, (mpv_handle*, void (*)(void*), void*))
F(void, mpv_terminate_destroy, (mpv_handle*))
F(void, mpv_free, (void*))

static mpv_handle* h;
static HINSTANCE g_inst;
static HWND g_mw, g_vw, g_set;       // hidden owner window (our thread), mpv's video window, settings window
static HHOOK g_hook;
static std::vector<std::wstring> files;
static CRITICAL_SECTION g_fl;        // guards `files` (preload thread reads it)
static volatile LONG g_flGen;
static int idx;
static bool g_isVid, g_ov, g_fixed;
static int g_shown = -1;
static DWORD g_ui;
static std::string KM[256];          // vk -> action

// ---- user options (edited in the Settings window, stored in %APPDATA%\LiteView\keys.ini) ----
static int o_autonext = 1, o_remvol = 1, o_vol = 100, o_startfs = 0, o_ontop = 0, o_uisec = 2, o_preload = 1;
static int o_inv = 0, o_swipe = 300, o_hq = 1, o_smooth = 1, o_deband = 0, o_hwdec = 1, o_subsize = 40;
static wchar_t o_slang[16], o_alang[16];

static volatile LONG g_preIdx = -1;
static HANDLE g_preEv;

// ======================= helpers =======================
static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, 0, 0);
    s.resize(n - 1);
    return s;
}
static std::wstring Wd(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, 0, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    w.resize(n - 1);
    return w;
}
static void cmd(const char* a, const char* b = 0, const char* c = 0, const char* d = 0,
                const char* e = 0, const char* f = 0, const char* g = 0) {
    const char* v[] = {a, b, c, d, e, f, g, 0};
    p_mpv_command(h, v);
}
static void setp(const char* n, const char* v) { p_mpv_set_property_string(h, n, v); }
static void setd(const char* n, double v) { char b[64]; snprintf(b, 64, "%.4f", v); setp(n, b); }
static double getd(const char* n, double d = 0) {
    double v;
    return p_mpv_get_property(h, n, MPV_FORMAT_DOUBLE, &v) >= 0 ? v : d;
}
static std::string mgets(const char* n) {
    char* s = p_mpv_get_property_string(h, n);
    std::string r = s ? s : "";
    if (s) p_mpv_free(s);
    return r;
}
static bool isYes(const char* n) { return mgets(n) == "yes"; }
static void osd(const char* s) { cmd("show-text", s, "1000"); }
static void showUI() { g_ui = GetTickCount() + std::max(1, o_uisec) * 1000; }

// ======================= file list =======================
static bool extIn(std::wstring e, const wchar_t* const* L_, int n) {
    for (auto& c : e) c = towlower(c);
    for (int i = 0; i < n; i++) if (e == L_[i]) return true;
    return false;
}
static bool isImg(const std::wstring& e) {
    static const wchar_t* const L_[] = {L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp", L".tif", L".tiff", L".avif", L".jxl", L".ico", L".jfif", L".apng"};
    return extIn(e, L_, sizeof(L_) / sizeof(*L_));
}
static bool isVidExt(const std::wstring& e) {
    static const wchar_t* const L_[] = {L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".flv", L".m4v", L".ts", L".mpg", L".mpeg", L".3gp", L".m2ts", L".ogv", L".vob"};
    return extIn(e, L_, sizeof(L_) / sizeof(*L_));
}
static void scan(const std::wstring& file) {         // fills `files`, sets idx (caller holds g_fl if needed)
    wchar_t dir[MAX_PATH * 2];
    wcsncpy(dir, file.c_str(), MAX_PATH * 2 - 1);
    PathRemoveFileSpecW(dir);
    std::wstring pat = std::wstring(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(pat.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, 0, FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring e = PathFindExtensionW(fd.cFileName);
            if (isImg(e) || isVidExt(e)) files.push_back(std::wstring(dir) + L"\\" + fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    idx = -1;
    for (size_t i = 0; i < files.size(); i++) if (!lstrcmpiW(files[i].c_str(), file.c_str())) idx = (int)i;
    if (idx < 0) { files.push_back(file); idx = (int)files.size() - 1; }
}

// ======================= quality / per-mode settings =======================
static void applyQuality() {
    setp("scale", o_hq ? "ewa_lanczossharp" : "spline36");   // upscaling
    setp("dscale", o_hq ? "mitchell" : "hermite");           // downscaling (no dots / moire)
}
static void applyMode(bool vid) {
    if (vid) {
        setp("interpolation", o_smooth ? "yes" : "no");
        setp("video-sync", o_smooth ? "display-resample" : "audio");
        setp("tscale", "oversample");
        setp("deband", o_deband ? "yes" : "no");
    } else {                                                  // still images: exact pixels, no motion tricks
        setp("interpolation", "no");
        setp("video-sync", "audio");
        setp("deband", "no");
    }
}
static void applyRuntime() {
    applyQuality();
    applyMode(g_isVid);
    setp("hwdec", o_hwdec ? "auto-safe" : "no");
    setp("ontop", o_ontop ? "yes" : "no");
    char b[16]; snprintf(b, 16, "%d", std::max(10, o_subsize)); setp("sub-font-size", b);
}

static void openIdx(int i) {
    idx = i;
    std::wstring p = files[i];
    std::wstring e = PathFindExtensionW(p.c_str());
    for (auto& c : e) c = towlower(c);
    g_isVid = !isImg(e);
    applyMode(g_isVid);
    setp("video-zoom", "0"); setp("video-pan-x", "0"); setp("video-pan-y", "0"); setp("video-rotate", "0");
    setp("loop-file", (e == L".gif" || e == L".webp" || e == L".apng") ? "inf" : "no");
    std::string u = U8(p);
    cmd("loadfile", u.c_str(), "replace");
    setp("pause", "no");
    showUI();
}
static void go(int d) {
    int n = (int)files.size();
    if (n > 1) openIdx(((idx + d) % n + n) % n);
}
static void setFiles(const std::wstring& path) {
    EnterCriticalSection(&g_fl);
    files.clear();
    scan(path);
    InterlockedIncrement(&g_flGen);
    LeaveCriticalSection(&g_fl);
    openIdx(idx);
}

// ======================= neighbour preload (OS file cache warm-up) =======================
static DWORD WINAPI preloadThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    char* buf = (char*)VirtualAlloc(0, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::vector<char> warm;
    LONG gen = -1;
    static const int order[] = {1, -1, 2, -2};
    for (;;) {
        WaitForSingleObject(g_preEv, INFINITE);
    again:
        int base = (int)g_preIdx, ids[4], cnt = 0;
        std::wstring paths[4];
        EnterCriticalSection(&g_fl);
        if (gen != g_flGen) { gen = g_flGen; warm.assign(files.size(), 0); }
        int n = (int)files.size();
        if (base >= 0 && base < n && n > 1)
            for (int k = 0; k < 4; k++) {
                int i = ((base + order[k]) % n + n) % n;
                if (i == base || warm[i]) continue;
                paths[cnt] = files[i]; ids[cnt] = i; cnt++;
            }
        LeaveCriticalSection(&g_fl);
        for (int c = 0; c < cnt; c++) {
            HANDLE f = CreateFileW(paths[c].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
            if (f == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz = {};
            GetFileSizeEx(f, &sz);
            bool vid = !isImg(std::wstring(PathFindExtensionW(paths[c].c_str())));
            long long head = vid ? (4LL << 20) : (32LL << 20), done = 0;
            DWORD got = 0;
            bool abort = false;
            while (done < head && done < sz.QuadPart) {
                if (!ReadFile(f, buf, 1 << 20, &got, 0) || !got) break;
                done += got;
                if (base != (int)g_preIdx) { abort = true; break; }
            }
            if (!abort && vid && sz.QuadPart > head + (2LL << 20)) {
                LARGE_INTEGER p; p.QuadPart = sz.QuadPart - (2LL << 20);
                SetFilePointerEx(f, p, 0, FILE_BEGIN);
                ReadFile(f, buf, 1 << 20, &got, 0);
                ReadFile(f, buf, 1 << 20, &got, 0);
            }
            CloseHandle(f);
            if (abort) goto again;
            if (ids[c] < (int)warm.size()) warm[ids[c]] = 1;
        }
    }
    return 0;
}

// ======================= settings storage =======================
struct Act { const char* id; const wchar_t* label; };
static const Act ACTS[] = {
    {"close", L"Close viewer"}, {"next", L"Next file"}, {"prev", L"Previous file"}, {"first", L"First file"}, {"last", L"Last file"},
    {"pause", L"Play / pause"}, {"volup", L"Volume up"}, {"voldown", L"Volume down"}, {"mute", L"Mute"}, {"fullscreen", L"Fullscreen"},
    {"seekfwd", L"Seek forward 5 s"}, {"seekback", L"Seek back 5 s"}, {"framenext", L"Next frame"}, {"frameback", L"Previous frame"},
    {"zoomin", L"Zoom in"}, {"zoomout", L"Zoom out"}, {"zoomreset", L"Reset zoom"}, {"rotate", L"Rotate"},
    {"brightup", L"Brightness up"}, {"brightdown", L"Brightness down"}, {"speedup", L"Speed up"}, {"speeddown", L"Speed down"},
    {"speedreset", L"Reset speed"}, {"subcycle", L"Next subtitle track"}, {"audiocycle", L"Next audio track"},
    {"subdelayminus", L"Subtitle delay -0.1 s"}, {"subdelayplus", L"Subtitle delay +0.1 s"}, {"loop", L"Loop video"},
    {"screenshot", L"Screenshot"}, {"info", L"File info"}, {"delete", L"Delete file (Recycle Bin)"},
    {"quality", L"Toggle high quality / fast mode"}, {"menu", L"Context menu"}, {"settings", L"Settings"}};
static const int NACT = sizeof(ACTS) / sizeof(*ACTS);

struct KeyName { const wchar_t* n; int v; };
static const KeyName KT[] = {
    {L"Esc", VK_ESCAPE}, {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT}, {L"Up", VK_UP},
    {L"Down", VK_DOWN}, {L"PgUp", VK_PRIOR}, {L"PgDn", VK_NEXT}, {L"Home", VK_HOME}, {L"End", VK_END}, {L"Tab", VK_TAB},
    {L"Backspace", VK_BACK}, {L"Delete", VK_DELETE}, {L"Plus", VK_OEM_PLUS}, {L"Minus", VK_OEM_MINUS}, {L"Comma", VK_OEM_COMMA},
    {L"Period", VK_OEM_PERIOD}, {L"LBracket", VK_OEM_4}, {L"RBracket", VK_OEM_6}};

static int vkOf(const wchar_t* n) {
    if (!n[0]) return 0;
    if (!n[1]) {
        wchar_t c = towupper(n[0]);
        return ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) ? c : 0;
    }
    for (auto& t : KT) if (!lstrcmpiW(n, t.n)) return t.v;
    if ((n[0] == L'F' || n[0] == L'f')) { int k = _wtoi(n + 1); if (k >= 1 && k <= 12) return VK_F1 + k - 1; }
    return 0;
}
static std::wstring nameOf(int vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F12) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    for (auto& t : KT) if (t.v == vk) return t.n;
    return L"";
}

struct DefKey { const wchar_t* k; const char* a; };
static const DefKey DEFK[] = {
    {L"Esc", "close"}, {L"Right", "next"}, {L"Left", "prev"}, {L"PgDn", "next"}, {L"PgUp", "prev"}, {L"Home", "first"}, {L"End", "last"},
    {L"Space", "pause"}, {L"K", "pause"}, {L"Up", "volup"}, {L"Down", "voldown"}, {L"M", "mute"}, {L"F", "fullscreen"},
    {L"Enter", "fullscreen"}, {L"J", "seekback"}, {L"L", "seekfwd"}, {L"Period", "framenext"}, {L"Comma", "frameback"},
    {L"R", "rotate"}, {L"Plus", "zoomin"}, {L"Minus", "zoomout"}, {L"0", "zoomreset"}, {L"B", "brightup"}, {L"V", "brightdown"},
    {L"RBracket", "speedup"}, {L"LBracket", "speeddown"}, {L"Backspace", "speedreset"}, {L"C", "subcycle"}, {L"A", "audiocycle"},
    {L"Z", "subdelayminus"}, {L"X", "subdelayplus"}, {L"O", "loop"}, {L"P", "screenshot"}, {L"I", "info"}, {L"Delete", "delete"},
    {L"Q", "quality"}, {L"S", "settings"}, {L"F1", "settings"}, {L"Tab", "menu"}};

static wchar_t g_ini[MAX_PATH];
static void cfgPath() {
    SHGetFolderPathW(0, CSIDL_APPDATA, 0, 0, g_ini);
    PathAppendW(g_ini, L"LiteView");
    CreateDirectoryW(g_ini, 0);
    PathAppendW(g_ini, L"keys.ini");
}
static void applyDefaultKeys(bool onlyMissing) {
    for (auto& d : DEFK) {
        int vk = vkOf(d.k);
        if (!vk) continue;
        if (onlyMissing) {
            bool has = false;
            for (int i = 1; i < 256; i++) if (KM[i] == d.a) { has = true; break; }
            if (has || !KM[vk].empty()) continue;
        }
        KM[vk] = d.a;
    }
}
static int gi(const wchar_t* k, int d) { return GetPrivateProfileIntW(L"options", k, d, g_ini); }
static void gs(const wchar_t* k, wchar_t* out) { GetPrivateProfileStringW(L"options", k, L"", out, 16, g_ini); }
static void wi(const wchar_t* k, int v) { wchar_t b[32]; _snwprintf(b, 32, L"%d", v); WritePrivateProfileStringW(L"options", k, b, g_ini); }

static void loadSettings() {
    cfgPath();
    o_autonext = gi(L"autonext", 1); o_remvol = gi(L"remember_volume", 1); o_vol = gi(L"volume", 100);
    o_startfs = gi(L"start_fullscreen", 0); o_ontop = gi(L"always_on_top", 0); o_uisec = gi(L"controls_hide_seconds", 2);
    o_preload = gi(L"preload", 1); o_inv = gi(L"invert_swipe", 0); o_swipe = gi(L"swipe_distance", 300);
    o_hq = gi(L"high_quality", 1); o_smooth = gi(L"smooth_motion", 1); o_deband = gi(L"deband_video", 0);
    o_hwdec = gi(L"hardware_decoding", 1); o_subsize = gi(L"subtitle_size", 40);
    gs(L"subtitle_language", o_slang); gs(L"audio_language", o_alang);
    int ver = gi(L"version", 0);
    static wchar_t buf[8192];
    DWORD n = GetPrivateProfileSectionW(L"keys", buf, 8192, g_ini);
    for (auto& s : KM) s.clear();
    if (n == 0) applyDefaultKeys(false);
    else {
        for (wchar_t* s = buf; *s; s += wcslen(s) + 1) {
            wchar_t* eq = wcschr(s, L'=');
            if (!eq) continue;
            *eq = 0;
            wchar_t* k = s; wchar_t* v = eq + 1;
            StrTrimW(k, L" \t"); StrTrimW(v, L" \t");
            int vk = vkOf(k);
            if (vk > 0 && vk < 256) KM[vk] = U8(v);
        }
        if (ver < 2) applyDefaultKeys(true);          // pick up new default shortcuts once
    }
}
static void saveSettings() {
    wi(L"version", 2);
    wi(L"autonext", o_autonext); wi(L"remember_volume", o_remvol); wi(L"volume", o_vol); wi(L"start_fullscreen", o_startfs);
    wi(L"always_on_top", o_ontop); wi(L"controls_hide_seconds", o_uisec); wi(L"preload", o_preload); wi(L"invert_swipe", o_inv);
    wi(L"swipe_distance", o_swipe); wi(L"high_quality", o_hq); wi(L"smooth_motion", o_smooth); wi(L"deband_video", o_deband);
    wi(L"hardware_decoding", o_hwdec); wi(L"subtitle_size", o_subsize);
    WritePrivateProfileStringW(L"options", L"subtitle_language", o_slang, g_ini);
    WritePrivateProfileStringW(L"options", L"audio_language", o_alang, g_ini);
    WritePrivateProfileStringW(L"keys", 0, 0, g_ini);
    for (int vk = 1; vk < 256; vk++) {
        if (KM[vk].empty()) continue;
        std::wstring nm = nameOf(vk);
        if (!nm.empty()) WritePrivateProfileStringW(L"keys", nm.c_str(), Wd(KM[vk]).c_str(), g_ini);
    }
}

// ======================= menus =======================
enum { M_OPEN = 1, M_EXPLORER, M_DELETE, M_PREV, M_NEXT, M_PLAY, M_BACK, M_FWD, M_LOOP, M_ZRESET, M_ROTATE, M_FULL, M_TOP, M_SHOT,
       M_INFO, M_QUALITY, M_SETTINGS, M_EXIT, M_SUBOFF, M_SUBLOAD, M_SUBDM, M_SUBDP, M_SUBSM, M_SUBBG, M_AUDDM, M_AUDDP, M_MUTE };
static const double SPEEDS[] = {0.25, 0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4};
static const char* const ASPECTS[] = {"-1", "16:9", "4:3", "21:9", "1:1"};
static const wchar_t* const ASPECT_L[] = {L"Auto", L"16:9", L"4:3", L"21:9", L"1:1"};

static void addStr(HMENU m, UINT id, const wchar_t* t, bool chk = false, bool dis = false) {
    AppendMenuW(m, MF_STRING | (chk ? MF_CHECKED : 0) | (dis ? MF_GRAYED : 0), id, t);
}
static void addTracks(HMENU m, const char* type, int base) {
    int n = atoi(mgets("track-list/count").c_str()), added = 0;
    char k[64];
    for (int i = 0; i < n; i++) {
        snprintf(k, 64, "track-list/%d/type", i);
        if (mgets(k) != type) continue;
        snprintf(k, 64, "track-list/%d/id", i); int id = atoi(mgets(k).c_str());
        snprintf(k, 64, "track-list/%d/lang", i); std::string lang = mgets(k);
        snprintf(k, 64, "track-list/%d/title", i); std::string ti = mgets(k);
        snprintf(k, 64, "track-list/%d/selected", i); bool sel = mgets(k) == "yes";
        std::wstring lb = std::to_wstring(id) + L": " + Wd(lang.empty() ? std::string("unknown") : lang) + (ti.empty() ? std::wstring() : L"   " + Wd(ti));
        AppendMenuW(m, MF_STRING | (sel ? MF_CHECKED : 0), base + id, lb.c_str());
        added++;
    }
    if (!added) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"(none)");
}
static HMENU menuSubs() {
    HMENU m = CreatePopupMenu();
    addStr(m, M_SUBOFF, L"Off", mgets("sid") == "no");
    addTracks(m, "sub", 2000);
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    addStr(m, M_SUBLOAD, L"Load subtitle file...");
    addStr(m, M_SUBDM, L"Delay -0.1 s"); addStr(m, M_SUBDP, L"Delay +0.1 s");
    addStr(m, M_SUBSM, L"Smaller"); addStr(m, M_SUBBG, L"Bigger");
    return m;
}
static HMENU menuAudio() {
    HMENU m = CreatePopupMenu();
    addTracks(m, "audio", 1000);
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    addStr(m, M_MUTE, L"Mute", isYes("mute"));
    addStr(m, M_AUDDM, L"Audio delay -0.1 s"); addStr(m, M_AUDDP, L"Audio delay +0.1 s");
    return m;
}
static HMENU menuSpeed() {
    HMENU m = CreatePopupMenu();
    double sp = getd("speed", 1);
    for (int i = 0; i < 9; i++) {
        wchar_t b[24]; _snwprintf(b, 24, L"%gx", SPEEDS[i]);
        addStr(m, 200 + i, b, fabs(sp - SPEEDS[i]) < 0.01);
    }
    return m;
}
static HMENU menuAspect() {
    HMENU m = CreatePopupMenu();
    for (int i = 0; i < 5; i++) addStr(m, 100 + i, ASPECT_L[i]);
    return m;
}
static HMENU menuContext() {
    HMENU m = CreatePopupMenu();
    addStr(m, M_OPEN, L"Open file...");
    addStr(m, M_EXPLORER, L"Show in Explorer");
    addStr(m, M_DELETE, L"Delete (Recycle Bin)\tDel");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    addStr(m, M_PREV, L"Previous\tLeft"); addStr(m, M_NEXT, L"Next\tRight");
    if (g_isVid) {
        AppendMenuW(m, MF_SEPARATOR, 0, 0);
        addStr(m, M_PLAY, isYes("pause") ? L"Play\tSpace" : L"Pause\tSpace");
        addStr(m, M_BACK, L"Back 10 s"); addStr(m, M_FWD, L"Forward 10 s");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)menuSpeed(), L"Speed");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)menuAudio(), L"Audio");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)menuSubs(), L"Subtitles");
        AppendMenuW(m, MF_POPUP, (UINT_PTR)menuAspect(), L"Aspect ratio");
        addStr(m, M_LOOP, L"Loop", mgets("loop-file") == "inf");
    }
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    addStr(m, M_ZRESET, L"Reset zoom\t0"); addStr(m, M_ROTATE, L"Rotate\tR");
    addStr(m, M_FULL, L"Fullscreen\tF", isYes("fullscreen"));
    addStr(m, M_TOP, L"Always on top", o_ontop != 0);
    addStr(m, M_SHOT, L"Screenshot\tP"); addStr(m, M_INFO, L"File info\tI");
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    addStr(m, M_QUALITY, L"High quality scaling", o_hq != 0);
    addStr(m, M_SETTINGS, L"Settings...\tS");
    addStr(m, M_EXIT, L"Exit\tEsc");
    return m;
}
static int popup(HMENU m, int sx, int sy, UINT extra = 0) {
    SetForegroundWindow(g_mw);
    int id = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | extra, sx, sy, 0, g_mw, 0);
    PostMessageW(g_mw, WM_NULL, 0, 0);
    DestroyMenu(m);
    if (g_vw) SetForegroundWindow(g_vw);
    return id;
}

static bool pickFile(wchar_t* out, const wchar_t* filter) {
    OPENFILENAMEW o = {};
    out[0] = 0;
    o.lStructSize = sizeof o; o.hwndOwner = g_mw; o.lpstrFile = out; o.nMaxFile = MAX_PATH * 2;
    o.lpstrFilter = filter; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) != 0;
}
static void openSettings();

static void doDelete() {
    if (idx < 0 || idx >= (int)files.size()) return;
    std::wstring path = files[idx];
    EnterCriticalSection(&g_fl);
    files.erase(files.begin() + idx);
    InterlockedIncrement(&g_flGen);
    LeaveCriticalSection(&g_fl);
    bool empty = files.empty();
    if (empty) cmd("stop"); else openIdx(std::min(idx, (int)files.size() - 1));
    std::wstring from = path; from.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE; op.pFrom = from.c_str(); op.fFlags = FOF_ALLOWUNDO | FOF_NOERRORUI;
    for (int t = 0; t < 6; t++) {                      // mpv may still hold the old file for a moment
        if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted) break;
        Sleep(150);
    }
    if (empty) cmd("quit"); else osd("Moved to Recycle Bin");
}

// ======================= actions =======================
static void act(const std::string& a) {
    if (a == "close") cmd("quit");
    else if (a == "next") go(1);
    else if (a == "prev") go(-1);
    else if (a == "first") openIdx(0);
    else if (a == "last") openIdx((int)files.size() - 1);
    else if (a == "pause") cmd("cycle", "pause");
    else if (a == "volup") { cmd("add", "volume", "5"); osd("Volume ${volume}%"); }
    else if (a == "voldown") { cmd("add", "volume", "-5"); osd("Volume ${volume}%"); }
    else if (a == "mute") { cmd("cycle", "mute"); osd("Mute: ${mute}"); }
    else if (a == "fullscreen") cmd("cycle", "fullscreen");
    else if (a == "seekfwd") cmd("seek", "5");
    else if (a == "seekback") cmd("seek", "-5");
    else if (a == "framenext") cmd("frame-step");
    else if (a == "frameback") cmd("frame-back-step");
    else if (a == "zoomin") cmd("add", "video-zoom", "0.1");
    else if (a == "zoomout") cmd("add", "video-zoom", "-0.1");
    else if (a == "zoomreset") { setp("video-zoom", "0"); setp("video-pan-x", "0"); setp("video-pan-y", "0"); }
    else if (a == "rotate") cmd("cycle-values", "video-rotate", "90", "180", "270", "0");
    else if (a == "brightup") { cmd("add", "brightness", "3"); osd("Brightness ${brightness}"); }
    else if (a == "brightdown") { cmd("add", "brightness", "-3"); osd("Brightness ${brightness}"); }
    else if (a == "speedup") { cmd("multiply", "speed", "1.1"); osd("Speed ${speed}x"); }
    else if (a == "speeddown") { cmd("multiply", "speed", "0.9091"); osd("Speed ${speed}x"); }
    else if (a == "speedreset") { setp("speed", "1"); osd("Speed 1x"); }
    else if (a == "subcycle") { cmd("cycle", "sub"); osd("Subtitles: ${sid}"); }
    else if (a == "audiocycle") { cmd("cycle", "audio"); osd("Audio track: ${aid}"); }
    else if (a == "subdelayminus") { cmd("add", "sub-delay", "-0.1"); osd("Subtitle delay ${sub-delay}"); }
    else if (a == "subdelayplus") { cmd("add", "sub-delay", "0.1"); osd("Subtitle delay ${sub-delay}"); }
    else if (a == "loop") { cmd("cycle-values", "loop-file", "inf", "no"); osd("Loop: ${loop-file}"); }
    else if (a == "info") cmd("show-text", "${filename}\n${width}x${height}   ${file-size}\n${file-format}", "3500");
    else if (a == "screenshot") {
        wchar_t d[MAX_PATH];
        SHGetFolderPathW(0, CSIDL_MYPICTURES, 0, 0, d);
        wchar_t f[MAX_PATH * 2];
        _snwprintf(f, MAX_PATH * 2, L"%s\\LiteView-%lu.png", d, GetTickCount());
        std::string u = U8(f);
        cmd("screenshot-to-file", u.c_str(), "video");
        osd("Screenshot saved to Pictures");
    }
    else if (a == "delete") doDelete();
    else if (a == "quality") { o_hq = !o_hq; applyQuality(); osd(o_hq ? "Quality: High" : "Quality: Fast"); saveSettings(); }
    else if (a == "settings") openSettings();
    else if (a == "menu") {
        POINT p; GetCursorPos(&p);
        int id = popup(menuContext(), p.x, p.y);
        extern void menuCmd(int);
        if (id) menuCmd(id);
    }
}
void menuCmd(int id) {
    char b[64];
    if (id >= 2000 && id < 3000) { snprintf(b, 64, "%d", id - 2000); setp("sid", b); return; }
    if (id >= 1000 && id < 2000) { snprintf(b, 64, "%d", id - 1000); setp("aid", b); return; }
    if (id >= 200 && id < 300) { setd("speed", SPEEDS[id - 200]); osd("Speed ${speed}x"); return; }
    if (id >= 100 && id < 200) { setp("video-aspect-override", ASPECTS[id - 100]); return; }
    switch (id) {
    case M_OPEN: {
        wchar_t f[MAX_PATH * 2];
        if (pickFile(f, L"Photos and videos\0*.jpg;*.jpeg;*.png;*.gif;*.webp;*.bmp;*.tif;*.tiff;*.avif;*.jxl;*.ico;*.jfif;*.apng;*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.webm;*.flv;*.m4v;*.ts;*.mpg;*.mpeg;*.3gp;*.m2ts;*.ogv;*.vob\0All files\0*.*\0")) setFiles(f);
        break;
    }
    case M_EXPLORER: {
        std::wstring a = L"/select,\"" + files[idx] + L"\"";
        ShellExecuteW(0, L"open", L"explorer.exe", a.c_str(), 0, SW_SHOW);
        break;
    }
    case M_DELETE: act("delete"); break;
    case M_PREV: go(-1); break;
    case M_NEXT: go(1); break;
    case M_PLAY: cmd("cycle", "pause"); break;
    case M_BACK: cmd("seek", "-10"); break;
    case M_FWD: cmd("seek", "10"); break;
    case M_LOOP: act("loop"); break;
    case M_ZRESET: act("zoomreset"); break;
    case M_ROTATE: act("rotate"); break;
    case M_FULL: act("fullscreen"); break;
    case M_TOP: o_ontop = !o_ontop; setp("ontop", o_ontop ? "yes" : "no"); saveSettings(); break;
    case M_SHOT: act("screenshot"); break;
    case M_INFO: act("info"); break;
    case M_QUALITY: act("quality"); break;
    case M_SETTINGS: openSettings(); break;
    case M_EXIT: cmd("quit"); break;
    case M_SUBOFF: setp("sid", "no"); break;
    case M_SUBLOAD: {
        wchar_t f[MAX_PATH * 2];
        if (pickFile(f, L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub\0All files\0*.*\0")) { std::string u = U8(f); cmd("sub-add", u.c_str(), "select"); }
        break;
    }
    case M_SUBDM: act("subdelayminus"); break;
    case M_SUBDP: act("subdelayplus"); break;
    case M_SUBSM: cmd("add", "sub-scale", "-0.1"); osd("Subtitle size ${sub-scale}"); break;
    case M_SUBBG: cmd("add", "sub-scale", "0.1"); osd("Subtitle size ${sub-scale}"); break;
    case M_AUDDM: cmd("add", "audio-delay", "-0.1"); osd("Audio delay ${audio-delay}"); break;
    case M_AUDDP: cmd("add", "audio-delay", "0.1"); osd("Audio delay ${audio-delay}"); break;
    case M_MUTE: act("mute"); break;
    }
}

// ======================= on-screen controls (ASS overlay) =======================
enum { B_PREV = 1, B_BACK, B_PLAY, B_FWD, B_NEXT, B_SUBS, B_AUDIO, B_SPEED, B_MORE };
struct Btn { int id, x, y, w, h; };
static Btn g_b[12];
static int g_nb, g_bx, g_by, g_bw, g_bh, g_rh, g_fs, g_rowY;
static void addB(int id, int x, int y, int w, int h) { g_b[g_nb++] = {id, x, y, w, h}; }
static void layout(int W, int H) {
    g_nb = 0;
    g_fs = std::max(18, H / 40);
    int m = W * 4 / 100, mb = std::max(12, H / 40);
    g_bh = std::max(4, H / 150); g_bx = m; g_bw = W - 2 * m; g_by = H - mb - g_bh;
    g_rh = std::max(40, H / 16); g_rowY = g_by - g_rh - H / 60;
    int bw = g_rh * 5 / 4, x0 = W / 2 - bw * 5 / 2;
    static const int ids[5] = {B_PREV, B_BACK, B_PLAY, B_FWD, B_NEXT};
    for (int i = 0; i < 5; i++) addB(ids[i], x0 + i * bw, g_rowY, bw, g_rh);
    static const int tid[4] = {B_MORE, B_SPEED, B_AUDIO, B_SUBS};
    static const int tl[4] = {3, 5, 5, 9};
    int xr = W - m;
    for (int i = 0; i < 4; i++) {
        int w = (int)(tl[i] * g_fs * 0.62 + g_fs * 1.2);
        xr -= w;
        addB(tid[i], xr, g_rowY, w, g_rh);
        xr -= g_fs / 3;
    }
}
static void fmt(double s, char* o) {
    int t = (int)s;
    if (t >= 3600) sprintf(o, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else sprintf(o, "%d:%02d", t / 60, t % 60);
}
static bool controlsVisible() { return GetTickCount() < g_ui || (g_isVid && isYes("pause")); }

static void overlay() {
    if (!g_vw) return;
    if (!controlsVisible()) {
        if (g_ov) { const char* a[] = {"osd-overlay", "1", "none", "", 0}; p_mpv_command(h, a); g_ov = false; }
        return;
    }
    RECT r; GetClientRect(g_vw, &r);
    int W = r.right, H = r.bottom;
    if (W < 200 || H < 120) return;
    int si = g_shown;
    if (si < 0 || si >= (int)files.size()) return;
    static char buf[7000];
    int n = 0, fs, m = W * 4 / 100;
    layout(W, H);
    fs = g_fs;
    double dur = g_isVid ? getd("duration") : 0;
    #define EMIT(...) do { n += snprintf(buf + n, sizeof(buf) - n, __VA_ARGS__); } while (0)
    if (dur > 0) {                               // dark gradient-ish scrim behind the controls
        int top = g_rowY - g_rh / 2;
        EMIT("{\\an7\\pos(0,%d)\\bord0\\shad0\\1c&H000000&\\1a&H90&\\p1}m 0 0 l %d 0 l %d %d l 0 %d{\\p0}\n", top, W, W, H - top, H - top);
    }
    std::string nm = U8(PathFindFileNameW(files[si].c_str()));
    for (auto& c : nm) if (c == '{' || c == '}') c = '('; else if (c == '\\') c = '/';
    EMIT("{\\an7\\pos(%d,%d)\\fs%d\\bord2\\shad0\\1c&HFFFFFF&\\3c&H000000&\\3a&H60&}%s   %d/%d", m, m / 2, fs, nm.c_str(), si + 1, (int)files.size());
    if (dur > 0) {
        double pos = getd("time-pos"), f = std::clamp(pos / dur, 0.0, 1.0);
        int fw = (int)(g_bw * f);
        char t1[16], t2[16]; fmt(pos, t1); fmt(dur, t2);
        EMIT("\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\1a&HA0&\\p1}m 0 0 l %d 0 l %d %d l 0 %d{\\p0}", g_bx, g_by, g_bw, g_bw, g_bh, g_bh);
        EMIT("\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\p1}m 0 0 l %d 0 l %d %d l 0 %d{\\p0}", g_bx, g_by, fw, fw, g_bh, g_bh);
        EMIT("\n{\\an4\\pos(%d,%d)\\fs%d\\bord2\\shad0\\1c&HFFFFFF&\\3c&H000000&\\3a&H60&}%s / %s", m, g_rowY + g_rh / 2, fs, t1, t2);
        bool paused = isYes("pause");
        char sp[16]; snprintf(sp, 16, "%gx", getd("speed", 1));
        for (int i = 0; i < g_nb; i++) {
            Btn& b = g_b[i];
            int cx = b.x + b.w / 2, cy = b.y + b.h / 2, a = g_rh * 2 / 5, ox = cx - a / 2, oy = cy - a / 2;
            char p[200];
            const char* txt = 0;
            switch (b.id) {
            case B_PLAY:
                if (paused) snprintf(p, 200, "m 0 0 l %d %d l 0 %d", a * 9 / 10, a / 2, a);
                else snprintf(p, 200, "m 0 0 l %d 0 l %d %d l 0 %d m %d 0 l %d 0 l %d %d l %d %d", a * 3 / 10, a * 3 / 10, a, a, a * 7 / 10, a, a, a, a * 7 / 10, a);
                EMIT("\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\p1}%s{\\p0}", ox, oy, p);
                break;
            case B_PREV:
                snprintf(p, 200, "m 0 0 l %d 0 l %d %d l 0 %d m %d %d l %d 0 l %d %d", a * 15 / 100, a * 15 / 100, a, a, a * 25 / 100, a / 2, a, a, a);
                EMIT("\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\p1}%s{\\p0}", ox, oy, p);
                break;
            case B_NEXT:
                snprintf(p, 200, "m 0 0 l %d %d l 0 %d m %d 0 l %d 0 l %d %d l %d %d", a * 75 / 100, a / 2, a, a * 85 / 100, a, a, a, a * 85 / 100, a);
                EMIT("\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\p1}%s{\\p0}", ox, oy, p);
                break;
            case B_BACK: txt = "-10"; break;
            case B_FWD: txt = "+10"; break;
            case B_SUBS: txt = "Subtitles"; break;
            case B_AUDIO: txt = "Audio"; break;
            case B_SPEED: txt = sp; break;
            case B_MORE: txt = "..."; break;
            }
            if (txt) EMIT("\n{\\an5\\pos(%d,%d)\\fs%d\\bord2\\shad0\\1c&HFFFFFF&\\3c&H000000&\\3a&H60&}%s", cx, cy, fs, txt);
        }
    }
    char ws[16], hs[16]; sprintf(ws, "%d", W); sprintf(hs, "%d", H);
    const char* a[] = {"osd-overlay", "1", "ass-events", buf, ws, hs, 0};
    p_mpv_command(h, a);
    g_ov = true;
}

static int hitBtn(int x, int y, int W, int H) {
    if (!g_isVid || !controlsVisible()) return 0;
    layout(W, H);
    for (int i = 0; i < g_nb; i++)
        if (x >= g_b[i].x && x < g_b[i].x + g_b[i].w && y >= g_b[i].y && y < g_b[i].y + g_b[i].h) return g_b[i].id;
    return 0;
}
static void btnClick(int id) {
    Btn* b = 0;
    for (int i = 0; i < g_nb; i++) if (g_b[i].id == id) b = &g_b[i];
    POINT p = {b ? b->x : 0, b ? b->y : 0};
    ClientToScreen(g_vw, &p);
    int r = 0;
    switch (id) {
    case B_PREV: go(-1); break;
    case B_NEXT: go(1); break;
    case B_BACK: cmd("seek", "-10"); break;
    case B_FWD: cmd("seek", "10"); break;
    case B_PLAY: cmd("cycle", "pause"); break;
    case B_SUBS: r = popup(menuSubs(), p.x, p.y, TPM_BOTTOMALIGN); break;
    case B_AUDIO: r = popup(menuAudio(), p.x, p.y, TPM_BOTTOMALIGN); break;
    case B_SPEED: r = popup(menuSpeed(), p.x, p.y, TPM_BOTTOMALIGN); break;
    case B_MORE: r = popup(menuContext(), p.x, p.y, TPM_BOTTOMALIGN); break;
    }
    if (r) menuCmd(r);
}

// ======================= gestures (mouse + touchpad) =======================
static bool ld, rd, md, skipClick;
static int dx0, dy0, rx, ry, rmode, mx, my, downBtn;
static DWORD lastDown, lastH, navCool, tpUntil;
static double sB, sV, sPos, sZ, hAcc, sAcc;

// zoom keeping the point under (x,y) roughly fixed; drives video-zoom / video-pan on the GPU (no window resize)
static double zoomAt(double nz, int x, int y, int W, int H) {
    nz = std::clamp(nz, -1.0, 6.0);
    double z0 = getd("video-zoom");
    if (nz <= 0.001) {
        setd("video-zoom", nz); setp("video-pan-x", "0"); setp("video-pan-y", "0");
        return nz;
    }
    double s = pow(2.0, nz - z0), zf = pow(2.0, z0);
    double kx = (x - W / 2.0) / (W * zf) * (1.0 / s - 1.0);
    double ky = (y - H / 2.0) / (H * zf) * (1.0 / s - 1.0);
    setd("video-pan-x", std::clamp(getd("video-pan-x") + kx, -3.0, 3.0));
    setd("video-pan-y", std::clamp(getd("video-pan-y") + ky, -3.0, 3.0));
    setd("video-zoom", nz);
    return nz;
}

static void onWheel(UINT t, WPARAM wp, LPARAM lp) {
    int raw = GET_WHEEL_DELTA_WPARAM(wp);
    if (!raw || !g_vw) return;
    DWORD now = GetTickCount();
    if (raw % 120) tpUntil = now + 600;          // fractional delta => precision touchpad
    bool tp = now < tpUntil;
    double d = raw / 120.0;
    POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(g_vw, &pt);
    RECT r; GetClientRect(g_vw, &r);
    int W = std::max<int>(1, r.right), H = std::max<int>(1, r.bottom);
    bool ctrl = (LOWORD(wp) & MK_CONTROL) || GetKeyState(VK_CONTROL) < 0;   // pinch arrives as Ctrl+wheel
    bool shift = (LOWORD(wp) & MK_SHIFT) != 0;
    double z = getd("video-zoom"), zf = pow(2.0, z);
    char b[96];

    if (t == WM_MOUSEHWHEEL) {                   // two-finger horizontal swipe
        if (z > 0.01) { setd("video-pan-x", std::clamp(getd("video-pan-x") - d * 100.0 / (W * zf), -3.0, 3.0)); return; }
        if (o_inv) d = -d;
        if (!g_isVid) {
            if (now - lastH > 250) hAcc = 0;
            lastH = now;
            if (now < navCool) { navCool = now + 250; return; }
            hAcc += d * 120;
            if (fabs(hAcc) > std::max(60, o_swipe)) { go(hAcc > 0 ? 1 : -1); hAcc = 0; navCool = now + 450; }
        } else {
            sAcc += d * 5;
            if (fabs(sAcc) >= 1) { snprintf(b, 96, "%.2f", sAcc); cmd("seek", b, "relative+keyframes"); sAcc = 0; }
        }
        return;
    }
    if (ctrl) {
        double nz = zoomAt(z + 0.2 * d, pt.x, pt.y, W, H);
        snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, nz))); osd(b);
        return;
    }
    if (!tp) {
        if (!g_isVid) {
            double nz = zoomAt(z + 0.15 * d, pt.x, pt.y, W, H);
            snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, nz))); osd(b);
        } else if (shift) { snprintf(b, 96, "%d", (int)(d * 5)); cmd("seek", b); }
        else { setd("volume", std::clamp(getd("volume") + d * 5, 0.0, 130.0)); osd("Volume ${volume}%"); }
        return;
    }
    if (z > 0.01) setd("video-pan-y", std::clamp(getd("video-pan-y") + d * 100.0 / (H * zf), -3.0, 3.0));
    else if (g_isVid) { setd("volume", std::clamp(getd("volume") + d * 3, 0.0, 130.0)); osd("Volume ${volume}%"); }
}

static void onInput(UINT t, WPARAM wp, LPARAM lp) {
    if (!g_vw) return;
    RECT r; GetClientRect(g_vw, &r);
    int W = std::max<int>(1, r.right), H = std::max<int>(1, r.bottom);
    int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
    char b[96];
    switch (t) {
    case WM_KEYDOWN: {
        int vk = (int)wp;
        if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) break;
        if (vk < 256 && !KM[vk].empty()) act(KM[vk]);
        showUI();
        break;
    }
    case WM_MOUSEMOVE:
        showUI();
        if (md) {
            double zf = pow(2.0, getd("video-zoom"));
            setd("video-pan-x", getd("video-pan-x") + (x - mx) / (W * zf));
            setd("video-pan-y", getd("video-pan-y") + (y - my) / (H * zf));
            mx = x; my = y;
        }
        if (rd) {
            int dx = x - rx, dy = y - ry;
            if (!rmode) {
                if (abs(dx) < 12 && abs(dy) < 12) break;
                rmode = abs(dx) > abs(dy) ? 3 : (rx < W / 2 ? 1 : 2);
                if (rmode == 3 && !g_isVid) rmode = -1;
            }
            if (rmode == 1) {
                setd("brightness", std::clamp(sB - dy * 200.0 / H, -100.0, 100.0));
                osd("Brightness ${brightness}");
            } else if (rmode == 2) {
                if (g_isVid) { setd("volume", std::clamp(sV - dy * 150.0 / H, 0.0, 130.0)); osd("Volume ${volume}%"); }
                else { double z = zoomAt(sZ - dy * 3.0 / H, W / 2, H / 2, W, H); snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, z))); osd(b); }
            } else if (rmode == 3) {
                double d = getd("duration");
                if (d > 0) { snprintf(b, 96, "%.2f", std::clamp(sPos + dx * d / W, 0.0, d)); cmd("seek", b, "absolute+keyframes"); }
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        DWORD now = GetTickCount();
        skipClick = false;
        downBtn = hitBtn(x, y, W, H);
        if (downBtn) { lastDown = 0; skipClick = true; }
        else if (lastDown && now - lastDown < GetDoubleClickTime() && abs(x - dx0) < 8 && abs(y - dy0) < 8) {
            if (g_isVid) cmd("cycle", "pause");   // undo the pause toggle from the first click
            act("fullscreen");
            lastDown = 0; skipClick = true;
        } else lastDown = now;
        dx0 = x; dy0 = y; ld = true;
        break;
    }
    case WM_LBUTTONUP:
        if (ld && downBtn) {
            if (hitBtn(x, y, W, H) == downBtn) btnClick(downBtn);
        } else if (ld && !skipClick && abs(x - dx0) < 5 && abs(y - dy0) < 5 && g_isVid) {
            layout(W, H);
            if (y >= g_by - g_bh * 3 && x >= g_bx - 8 && x <= g_bx + g_bw + 8 && controlsVisible()) {
                double p = std::clamp((x - g_bx) * 100.0 / g_bw, 0.0, 100.0);
                snprintf(b, 96, "%.2f", p); cmd("seek", b, "absolute-percent");
            } else cmd("cycle", "pause");
        }
        ld = false; downBtn = 0; showUI();
        break;
    case WM_RBUTTONDOWN:
        rd = true; rx = x; ry = y; rmode = 0;
        sB = getd("brightness"); sV = getd("volume"); sPos = getd("time-pos"); sZ = getd("video-zoom");
        break;
    case WM_RBUTTONUP: {
        bool click = rd && rmode == 0;
        rd = false;
        if (click) {                              // right click without drag => context menu
            POINT p = {x, y};
            ClientToScreen(g_vw, &p);
            int id = popup(menuContext(), p.x, p.y);
            if (id) menuCmd(id);
        }
        break;
    }
    case WM_MBUTTONDOWN: md = true; mx = x; my = y; break;
    case WM_MBUTTONUP: md = false; break;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
        onWheel(t, wp, lp);
        showUI();
        break;
    }
}

static LRESULT CALLBACK HookProc(int c, WPARAM wp, LPARAM lp) {
    if (c == HC_ACTION && wp == PM_REMOVE) {
        MSG* m = (MSG*)lp;
        UINT t = m->message;
        if (t == WM_KEYDOWN || t == WM_MOUSEMOVE || t == WM_LBUTTONDOWN || t == WM_LBUTTONUP || t == WM_RBUTTONDOWN ||
            t == WM_RBUTTONUP || t == WM_MBUTTONDOWN || t == WM_MBUTTONUP || t == WM_MOUSEWHEEL || t == WM_MOUSEHWHEEL)
            PostMessageW(g_mw, 0x8000 + t, m->wParam, m->lParam);
    }
    return CallNextHookEx(0, c, wp, lp);
}
static void hookIfNeeded() {
    if (g_vw) return;
    int64_t id = 0;
    if (p_mpv_get_property(h, "window-id", MPV_FORMAT_INT64, &id) >= 0 && id) {
        g_vw = (HWND)(intptr_t)id;
        DWORD tid = GetWindowThreadProcessId(g_vw, 0);
        g_hook = SetWindowsHookExW(WH_GETMESSAGE, HookProc, 0, tid);
    }
}
static void wake(void*) { PostMessageW(g_mw, WM_USER + 1, 0, 0); }

static LRESULT CALLBACK MW(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m >= 0x8000 && m < 0xC000) { onInput(m - 0x8000, wp, lp); return 0; }
    if (m == WM_USER + 1) {
        for (;;) {
            mpv_event* e = p_mpv_wait_event(h, 0);
            if (e->event_id == MPV_EVENT_NONE) break;
            if (e->event_id == MPV_EVENT_SHUTDOWN) { PostQuitMessage(0); return 0; }
            if (e->event_id == MPV_EVENT_FILE_LOADED || e->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                hookIfNeeded();
                g_shown = idx;
                if (o_preload && g_preEv && g_preIdx != idx) { g_preIdx = idx; SetEvent(g_preEv); }
                if (!g_fixed && e->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                    g_fixed = true;                       // stop mpv resizing the window for every new file
                    setp("auto-window-resize", "no");
                }
            }
        }
        return 0;
    }
    if (m == WM_TIMER) {
        hookIfNeeded();
        overlay();
        static DWORD lastAuto;
        if (o_autonext && g_isVid && idx < (int)files.size() - 1 && GetTickCount() - lastAuto > 1500 && isYes("eof-reached")) {
            lastAuto = GetTickCount();
            go(1);
        }
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ======================= settings window =======================
struct Opt { const wchar_t* lbl; int type; void* p; int page; };   // type 0 checkbox, 1 number, 2 text
static Opt OPTS[] = {
    {L"Play the next file automatically when a video ends", 0, &o_autonext, 0},
    {L"Remember the last volume", 0, &o_remvol, 0},
    {L"Default volume (0-130)", 1, &o_vol, 0},
    {L"Start in fullscreen", 0, &o_startfs, 0},
    {L"Keep window always on top", 0, &o_ontop, 0},
    {L"Hide controls after (seconds)", 1, &o_uisec, 0},
    {L"Preload next / previous files in the background", 0, &o_preload, 0},
    {L"Invert touchpad swipe direction", 0, &o_inv, 0},
    {L"Touchpad swipe distance (smaller = more sensitive)", 1, &o_swipe, 0},
    {L"High quality scaling (uncheck = fast mode for old GPUs)", 0, &o_hq, 1},
    {L"Smooth motion for video (display-resample + interpolation)", 0, &o_smooth, 1},
    {L"Light debanding for video", 0, &o_deband, 1},
    {L"Hardware video decoding", 0, &o_hwdec, 1},
    {L"Subtitle size (default 40)", 1, &o_subsize, 1},
    {L"Preferred subtitle language (e.g. en)", 2, o_slang, 1},
    {L"Preferred audio language (e.g. en)", 2, o_alang, 1}};
static const int NOPT = sizeof(OPTS) / sizeof(*OPTS);
static HWND g_tab, g_lv, g_status;
static std::vector<HWND> g_pg[3];
static HFONT g_font;
static int g_dpi = 96, g_cap = -1;
static bool g_building;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static BOOL CALLBACK setFontCb(HWND c, LPARAM) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return TRUE; }

static void showPage(int p) {
    for (int i = 0; i < 3; i++) for (HWND c : g_pg[i]) ShowWindow(c, i == p ? SW_SHOW : SW_HIDE);
}
static void refreshList() {
    for (int i = 0; i < NACT; i++) {
        std::wstring keys;
        for (int vk = 1; vk < 256; vk++)
            if (KM[vk] == ACTS[i].id) { if (!keys.empty()) keys += L", "; keys += nameOf(vk); }
        ListView_SetItemText(g_lv, i, 1, (LPWSTR)keys.c_str());
    }
}
static void startCapture() {
    int sel = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
    if (sel < 0) { SetWindowTextW(g_status, L"Select an action first."); return; }
    g_cap = sel;
    std::wstring s = std::wstring(L"Press the new key for \"") + ACTS[sel].label + L"\"  (Esc = cancel)";
    SetWindowTextW(g_status, s.c_str());
    SetFocus(g_set);
}
static void clearKeysOf(const char* a) { for (int vk = 1; vk < 256; vk++) if (KM[vk] == a) KM[vk].clear(); }

static LRESULT CALLBACK SetProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&ic);
        NONCLIENTMETRICSW ncm = {sizeof ncm};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        if (!g_font) g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        g_building = true;
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, 0, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, S(10), S(10), S(565), S(450), hw, (HMENU)1, g_inst, 0);
        static const wchar_t* tn[3] = {L"General", L"Video && Image", L"Shortcuts"};
        for (int i = 0; i < 3; i++) { TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)tn[i]; TabCtrl_InsertItem(g_tab, i, &ti); }
        int y[2] = {S(50), S(50)};
        for (int i = 0; i < NOPT; i++) {
            Opt& o = OPTS[i];
            int pg = o.page; HWND c;
            if (o.type == 0) {
                c = CreateWindowExW(0, L"BUTTON", o.lbl, WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP, S(30), y[pg], S(530), S(24), hw, (HMENU)(INT_PTR)(2000 + i), g_inst, 0);
                SendMessageW(c, BM_SETCHECK, *(int*)o.p ? BST_CHECKED : BST_UNCHECKED, 0);
                g_pg[pg].push_back(c);
            } else {
                HWND l = CreateWindowExW(0, L"STATIC", o.lbl, WS_CHILD, S(30), y[pg] + S(3), S(400), S(22), hw, 0, g_inst, 0);
                wchar_t v[32];
                if (o.type == 1) _snwprintf(v, 32, L"%d", *(int*)o.p); else wcsncpy(v, (wchar_t*)o.p, 31), v[31] = 0;
                c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", v, WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | (o.type == 1 ? ES_NUMBER : 0), S(440), y[pg], S(110), S(24), hw, (HMENU)(INT_PTR)(2000 + i), g_inst, 0);
                g_pg[pg].push_back(l); g_pg[pg].push_back(c);
            }
            y[pg] += S(34);
        }
        g_lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, 0, WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, S(25), S(45), S(535), S(340), hw, (HMENU)3000, g_inst, 0);
        ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Action"; col.cx = S(290); ListView_InsertColumn(g_lv, 0, &col);
        col.pszText = (LPWSTR)L"Key"; col.cx = S(220); ListView_InsertColumn(g_lv, 1, &col);
        for (int i = 0; i < NACT; i++) {
            LVITEMW it = {}; it.mask = LVIF_TEXT; it.iItem = i; it.pszText = (LPWSTR)ACTS[i].label;
            ListView_InsertItem(g_lv, &it);
        }
        refreshList();
        g_pg[2].push_back(g_lv);
        static const wchar_t* bt[3] = {L"Change key...", L"Clear", L"Reset all"};
        for (int i = 0; i < 3; i++)
            g_pg[2].push_back(CreateWindowExW(0, L"BUTTON", bt[i], WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, S(25) + i * S(130), S(395), S(120), S(28), hw, (HMENU)(INT_PTR)(3001 + i), g_inst, 0));
        g_status = CreateWindowExW(0, L"STATIC", L"Select an action, then press \"Change key...\" (or double-click it).", WS_CHILD, S(25), S(430), S(540), S(22), hw, 0, g_inst, 0);
        g_pg[2].push_back(g_status);
        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, S(475), S(472), S(100), S(30), hw, (HMENU)IDCANCEL, g_inst, 0);
        EnumChildWindows(hw, setFontCb, 0);
        showPage(0);
        g_building = false;
        return 0;
    }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->hwndFrom == g_tab && nh->code == TCN_SELCHANGE) showPage(TabCtrl_GetCurSel(g_tab));
        if (nh->hwndFrom == g_lv && nh->code == NM_DBLCLK) startCapture();
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDCANCEL) { PostMessageW(hw, WM_CLOSE, 0, 0); return 0; }
        if (id == 3001) { startCapture(); return 0; }
        if (id == 3002) {
            int sel = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
            if (sel >= 0) { clearKeysOf(ACTS[sel].id); refreshList(); saveSettings(); }
            return 0;
        }
        if (id == 3003) {
            for (auto& s : KM) s.clear();
            applyDefaultKeys(false); refreshList(); saveSettings();
            SetWindowTextW(g_status, L"Shortcuts reset to defaults.");
            return 0;
        }
        if (id >= 2000 && id < 2000 + NOPT && !g_building) {
            Opt& o = OPTS[id - 2000];
            if (o.type == 0 && code == BN_CLICKED) *(int*)o.p = (SendMessageW((HWND)lp, BM_GETCHECK, 0, 0) == BST_CHECKED);
            else if (o.type != 0 && code == EN_CHANGE) {
                wchar_t v[32]; GetWindowTextW((HWND)lp, v, 32);
                if (o.type == 1) *(int*)o.p = _wtoi(v); else { wcsncpy((wchar_t*)o.p, v, 15); ((wchar_t*)o.p)[15] = 0; }
            } else return 0;
            applyRuntime();
            if (g_vw) {
                if (o_ontop) setp("ontop", "yes"); else setp("ontop", "no");
            }
            saveSettings();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (g_cap >= 0) {
            int vk = (int)wp;
            if (vk == VK_ESCAPE) { g_cap = -1; SetWindowTextW(g_status, L"Cancelled."); return 0; }
            if (nameOf(vk).empty()) return 0;
            clearKeysOf(ACTS[g_cap].id);
            KM[vk] = ACTS[g_cap].id;
            std::wstring s = std::wstring(L"\"") + ACTS[g_cap].label + L"\" = " + nameOf(vk);
            SetWindowTextW(g_status, s.c_str());
            g_cap = -1; refreshList(); saveSettings();
            return 0;
        }
        if (wp == VK_ESCAPE) PostMessageW(hw, WM_CLOSE, 0, 0);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wp, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_CLOSE:
        saveSettings();
        DestroyWindow(hw);
        return 0;
    case WM_DESTROY:
        g_set = 0; g_cap = -1;
        for (auto& v : g_pg) v.clear();
        if (g_vw) SetForegroundWindow(g_vw);
        return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}
static void openSettings() {
    if (g_set) { ShowWindow(g_set, SW_SHOW); SetForegroundWindow(g_set); return; }
    static bool reg;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = SetProc; wc.hInstance = g_inst; wc.lpszClassName = L"LiteViewSettings";
        wc.hCursor = LoadCursorW(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        RegisterClassW(&wc);
        reg = true;
    }
    HDC dc = GetDC(0); g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(0, dc);
    RECT rc = {0, 0, S(590), S(520)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    g_set = CreateWindowExW(WS_EX_APPWINDOW, L"LiteViewSettings", L"LiteView Settings", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt, 0, 0, g_inst, 0);
    SetForegroundWindow(g_set);
}

// ======================= main =======================
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    g_inst = hi;
    InitializeCriticalSection(&g_fl);
    int argc;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc < 2) {
        MessageBoxW(0, L"Open a photo or video with LiteView (set it as the default app, or drag a file onto LiteView.exe).", L"LiteView", MB_OK);
        return 0;
    }
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(0, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    PathAppendW(exe, L"libmpv-2.dll");
    HMODULE dll = LoadLibraryExW(exe, 0, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll) { MessageBoxW(0, L"libmpv-2.dll not found next to LiteView.exe", L"LiteView", MB_ICONERROR); return 1; }
#define L(n) p_##n = (n##_t)GetProcAddress(dll, #n)
    L(mpv_create); L(mpv_initialize); L(mpv_set_option_string); L(mpv_set_property_string); L(mpv_get_property_string);
    L(mpv_get_property); L(mpv_command); L(mpv_wait_event); L(mpv_set_wakeup_callback); L(mpv_terminate_destroy); L(mpv_free);

    loadSettings();
    wchar_t full[MAX_PATH * 2];
    GetFullPathNameW(argv[1], MAX_PATH * 2, full, 0);
    scan(full);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MW; wc.hInstance = hi; wc.lpszClassName = L"LiteViewMsg";
    RegisterClassW(&wc);
    g_mw = CreateWindowExW(WS_EX_TOOLWINDOW, L"LiteViewMsg", L"", WS_POPUP, 0, 0, 0, 0, 0, 0, hi, 0);   // hidden; owns menus

    h = p_mpv_create();
    char vol[16], sub[16];
    snprintf(vol, 16, "%d", std::clamp(o_vol, 0, 130));
    snprintf(sub, 16, "%d", std::max(10, o_subsize));
    const char* o[][2] = {
        {"config", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
        // --- renderer / quality ---
        {"vo", "gpu-next,gpu"},                                  // libplacebo renderer, falls back to classic gpu
        {"hwdec", o_hwdec ? "auto-safe" : "no"},
        {"scale", o_hq ? "ewa_lanczossharp" : "spline36"},       // upscaling
        {"dscale", o_hq ? "mitchell" : "hermite"},               // downscaling
        {"correct-downscaling", "yes"}, {"linear-downscaling", "yes"}, {"sigmoid-upscaling", "yes"},
        {"icc-profile-auto", "yes"},
        {"deband", "no"}, {"deband-iterations", "1"},            // per-file mode switching sets these again
        {"interpolation", "no"}, {"tscale", "oversample"}, {"video-sync", "audio"},
        // --- window / ui ---
        {"osc", "no"}, {"osd-bar", "no"}, {"input-default-bindings", "no"}, {"input-builtin-bindings", "no"},
        {"border", "no"}, {"title", ""}, {"force-window", "yes"}, {"keep-open", "yes"}, {"idle", "no"},
        {"autofit-larger", "85%x85%"}, {"geometry", "50%:50%"}, {"window-dragging", "yes"}, {"cursor-autohide", "1500"},
        {"image-display-duration", "inf"}, {"background-color", "#000000"},
        {"osd-font-size", "30"}, {"osd-border-size", "2"}, {"osd-margin-y", "40"},
        {"fullscreen", o_startfs ? "yes" : "no"}, {"ontop", o_ontop ? "yes" : "no"},
        // --- audio / subtitles / memory ---
        {"volume", vol}, {"volume-max", "130"}, {"audio-display", "no"}, {"save-position-on-quit", "no"},
        {"sub-auto", "fuzzy"}, {"sub-font-size", sub}, {"sub-border-size", "2"},
        {"demuxer-max-bytes", "32MiB"}, {"demuxer-max-back-bytes", "8MiB"}, {"taskbar-progress", "no"}};
    for (auto& kv : o) p_mpv_set_option_string(h, kv[0], kv[1]);
    if (o_slang[0]) p_mpv_set_option_string(h, "slang", U8(o_slang).c_str());
    if (o_alang[0]) p_mpv_set_option_string(h, "alang", U8(o_alang).c_str());
    p_mpv_set_wakeup_callback(h, wake, 0);
    p_mpv_initialize(h);
    if (o_preload && files.size() > 1) {
        g_preEv = CreateEventW(0, FALSE, FALSE, 0);
        CreateThread(0, 0, preloadThread, 0, 0, 0);
    }
    openIdx(idx);
    SetTimer(g_mw, 1, 120, 0);

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if (g_set && g_cap < 0 && IsDialogMessageW(g_set, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    if (o_remvol) o_vol = (int)getd("volume", o_vol);
    saveSettings();
    p_mpv_terminate_destroy(h);
    return 0;
}
