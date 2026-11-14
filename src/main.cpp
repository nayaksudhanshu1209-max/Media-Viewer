// LiteView - tiny photo/video viewer. Engine: libmpv (loaded at runtime). UI/input/overlay: this file.
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
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <mpv/client.h>

// ---------- libmpv, loaded dynamically (no import lib needed) ----------
#define F(ret, name, args) typedef ret (*name##_t) args; static name##_t p_##name;
F(mpv_handle*, mpv_create, (void))
F(int, mpv_initialize, (mpv_handle*))
F(int, mpv_set_option_string, (mpv_handle*, const char*, const char*))
F(int, mpv_set_property_string, (mpv_handle*, const char*, const char*))
F(int, mpv_get_property, (mpv_handle*, const char*, mpv_format, void*))
F(int, mpv_command, (mpv_handle*, const char**))
F(mpv_event*, mpv_wait_event, (mpv_handle*, double))
F(void, mpv_set_wakeup_callback, (mpv_handle*, void (*)(void*), void*))
F(void, mpv_terminate_destroy, (mpv_handle*))

static mpv_handle* h;
static HWND g_mw, g_vw;           // message window (our thread), mpv's video window
static HHOOK g_hook;
static std::vector<std::wstring> files;
static int idx;
static bool g_isVid;
static DWORD g_ui;                // overlay visible until this tick
static int g_shown = -1;          // index of the file actually on screen (title follows this, not idx)
static bool g_fixed;              // window size locked after first file
static double hAcc, sAcc, swipeThr = 300;
static DWORD lastH, navCool, tpUntil;
static int invSwipe;
static volatile LONG g_preIdx = -1;   // file being viewed; preload thread warms its neighbours
static HANDLE g_preEv;
static int g_preload = 1;
static bool g_ov;
static std::string KM[256];       // vk -> action

static std::string U8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, 0, 0, 0, 0);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, 0, 0);
    s.resize(n - 1);
    return s;
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
static void osd(const char* s) { cmd("show-text", s, "1000"); }
static void showUI() { g_ui = GetTickCount() + 2000; }

// ---------- file list ----------
static bool isImg(std::wstring e) {
    for (auto& c : e) c = towlower(c);
    static const wchar_t* L_[] = {L".jpg", L".jpeg", L".png", L".gif", L".webp", L".bmp", L".tif", L".tiff", L".avif", L".jxl", L".ico", L".jfif", L".apng"};
    for (auto x : L_) if (e == x) return true;
    return false;
}
static bool isVidExt(std::wstring e) {
    for (auto& c : e) c = towlower(c);
    static const wchar_t* L_[] = {L".mp4", L".mkv", L".avi", L".mov", L".wmv", L".webm", L".flv", L".m4v", L".ts", L".mpg", L".mpeg", L".3gp", L".m2ts", L".ogv", L".vob"};
    for (auto x : L_) if (e == x) return true;
    return false;
}
static void scan(const std::wstring& file) {
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
static void openIdx(int i) {
    idx = i;
    std::wstring p = files[i];
    std::wstring e = PathFindExtensionW(p.c_str());
    for (auto& c : e) c = towlower(c);
    g_isVid = !isImg(e);
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

// ---------- neighbour preload ----------
// Low-priority background thread that reads the next/previous files into the Windows file cache
// (photos: first 32 MB; videos: first 4 MB + last 2 MB for MP4 index). The data lives in the OS cache,
// not in this process, so Windows can drop it any time memory is needed.
static DWORD WINAPI preloadThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);   // also lowers disk priority
    char* buf = (char*)VirtualAlloc(0, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::vector<char> warm(files.size(), 0);
    static const int order[] = {1, -1, 2, -2};
    for (;;) {
        WaitForSingleObject(g_preEv, INFINITE);
    again:
        int base = (int)g_preIdx, n = (int)files.size();
        if (base < 0 || n < 2) continue;
        for (int k = 0; k < 4; k++) {
            int i = ((base + order[k]) % n + n) % n;
            if (i == base || warm[i]) continue;
            HANDLE f = CreateFileW(files[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, 0);
            if (f == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz = {};
            GetFileSizeEx(f, &sz);
            bool vid = !isImg(std::wstring(PathFindExtensionW(files[i].c_str())));
            long long head = vid ? (4LL << 20) : (32LL << 20), done = 0;
            DWORD got = 0;
            bool abort = false;
            while (done < head && done < sz.QuadPart) {
                if (!ReadFile(f, buf, 1 << 20, &got, 0) || !got) break;
                done += got;
                if (base != (int)g_preIdx) { abort = true; break; }     // user moved on: restart from new position
            }
            if (!abort && vid && sz.QuadPart > head + (2LL << 20)) {
                LARGE_INTEGER p; p.QuadPart = sz.QuadPart - (2LL << 20);
                SetFilePointerEx(f, p, 0, FILE_BEGIN);
                ReadFile(f, buf, 1 << 20, &got, 0);
                ReadFile(f, buf, 1 << 20, &got, 0);
            }
            CloseHandle(f);
            if (abort) goto again;
            warm[i] = 1;
        }
    }
    return 0;
}

// ---------- shortcuts (%APPDATA%\LiteView\keys.ini) ----------
static const char* DEF =
    "; LiteView shortcuts.  Format:  Key=action   (save the file, then reopen the viewer)\r\n"
    "; Keys: A-Z 0-9 F1-F12 Esc Space Enter Left Right Up Down PgUp PgDn Home End Tab Backspace Delete\r\n"
    ";       Plus Minus Comma Period LBracket RBracket\r\n"
    "; Actions: close next prev first last pause volup voldown mute fullscreen seekfwd seekback\r\n"
    ";          framenext frameback zoomin zoomout zoomreset rotate brightup brightdown\r\n"
    ";          contrastup contrastdown speedup speeddown speedreset info settings\r\n"
    "[keys]\r\nEsc=close\r\nRight=next\r\nLeft=prev\r\nPgDn=next\r\nPgUp=prev\r\nHome=first\r\nEnd=last\r\n"
    "Space=pause\r\nK=pause\r\nUp=volup\r\nDown=voldown\r\nM=mute\r\nF=fullscreen\r\nEnter=fullscreen\r\n"
    "J=seekback\r\nL=seekfwd\r\nPeriod=framenext\r\nComma=frameback\r\nR=rotate\r\n"
    "Plus=zoomin\r\nMinus=zoomout\r\n0=zoomreset\r\nB=brightup\r\nV=brightdown\r\n"
    "RBracket=speedup\r\nLBracket=speeddown\r\nBackspace=speedreset\r\nI=info\r\nS=settings\r\nF1=settings\r\n"
    "\r\n; Touchpad: two-finger horizontal swipe = next/prev photo (seek in video), pinch = zoom,\r\n"
    "; two-finger scroll = pan when zoomed. invert_swipe=1 flips swipe direction.\r\n"
    "; preload=1 warms the next/previous files in the background (0 = off).\r\n"
    "[options]\r\ninvert_swipe=0\r\nswipe_distance=300\r\npreload=1\r\n";

static void cfgPath(wchar_t* p) {
    SHGetFolderPathW(0, CSIDL_APPDATA, 0, 0, p);
    PathAppendW(p, L"LiteView");
    CreateDirectoryW(p, 0);
    PathAppendW(p, L"keys.ini");
}
static int vkOf(const wchar_t* n) {
    if (!n[1]) {
        wchar_t c = towupper(n[0]);
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
        return 0;
    }
    struct { const wchar_t* n; int v; } T[] = {
        {L"Esc", VK_ESCAPE}, {L"Space", VK_SPACE}, {L"Enter", VK_RETURN}, {L"Left", VK_LEFT}, {L"Right", VK_RIGHT},
        {L"Up", VK_UP}, {L"Down", VK_DOWN}, {L"PgUp", VK_PRIOR}, {L"PgDn", VK_NEXT}, {L"Home", VK_HOME}, {L"End", VK_END},
        {L"Tab", VK_TAB}, {L"Backspace", VK_BACK}, {L"Delete", VK_DELETE}, {L"Plus", VK_OEM_PLUS}, {L"Minus", VK_OEM_MINUS},
        {L"Comma", VK_OEM_COMMA}, {L"Period", VK_OEM_PERIOD}, {L"LBracket", VK_OEM_4}, {L"RBracket", VK_OEM_6}};
    for (auto& t : T) if (!lstrcmpiW(n, t.n)) return t.v;
    if ((n[0] == L'F' || n[0] == L'f') && n[1]) { int k = _wtoi(n + 1); if (k >= 1 && k <= 12) return VK_F1 + k - 1; }
    return 0;
}
static void loadKeys() {
    wchar_t p[MAX_PATH];
    cfgPath(p);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = _wfopen(p, L"wb");
        if (f) { fputs(DEF, f); fclose(f); }
    }
    static wchar_t buf[8192];
    invSwipe = GetPrivateProfileIntW(L"options", L"invert_swipe", 0, p);
    g_preload = GetPrivateProfileIntW(L"options", L"preload", 1, p);
    swipeThr = std::max(60, (int)GetPrivateProfileIntW(L"options", L"swipe_distance", 300, p));
    GetPrivateProfileSectionW(L"keys", buf, 8192, p);
    for (wchar_t* s = buf; *s; s += wcslen(s) + 1) {
        wchar_t* eq = wcschr(s, L'=');
        if (!eq) continue;
        *eq = 0;
        wchar_t* k = s; wchar_t* v = eq + 1;
        StrTrimW(k, L" \t"); StrTrimW(v, L" \t");
        int vk = vkOf(k);
        if (vk > 0 && vk < 256) KM[vk] = U8(v);
    }
}

// ---------- actions ----------
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
    else if (a == "zoomin") { cmd("add", "video-zoom", "0.1"); }
    else if (a == "zoomout") { cmd("add", "video-zoom", "-0.1"); }
    else if (a == "zoomreset") { setp("video-zoom", "0"); setp("video-pan-x", "0"); setp("video-pan-y", "0"); }
    else if (a == "rotate") cmd("cycle-values", "video-rotate", "90", "180", "270", "0");
    else if (a == "brightup") { cmd("add", "brightness", "3"); osd("Brightness ${brightness}"); }
    else if (a == "brightdown") { cmd("add", "brightness", "-3"); osd("Brightness ${brightness}"); }
    else if (a == "contrastup") { cmd("add", "contrast", "3"); osd("Contrast ${contrast}"); }
    else if (a == "contrastdown") { cmd("add", "contrast", "-3"); osd("Contrast ${contrast}"); }
    else if (a == "speedup") { cmd("multiply", "speed", "1.1"); osd("Speed ${speed}x"); }
    else if (a == "speeddown") { cmd("multiply", "speed", "0.9091"); osd("Speed ${speed}x"); }
    else if (a == "speedreset") { setp("speed", "1"); osd("Speed 1x"); }
    else if (a == "info") cmd("show-text", "${filename}\n${width}x${height}   ${file-size}\n${file-format}", "3500");
    else if (a == "settings") { wchar_t p[MAX_PATH]; cfgPath(p); ShellExecuteW(0, L"open", L"notepad.exe", p, 0, SW_SHOW); }
}

// ---------- overlay (seek bar + file name), drawn with mpv's ASS overlay ----------
static void fmt(double s, char* o) {
    int t = (int)s;
    if (t >= 3600) sprintf(o, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else sprintf(o, "%d:%02d", t / 60, t % 60);
}
static void overlay() {
    if (!g_vw) return;
    if (GetTickCount() >= g_ui) {
        if (g_ov) { const char* a[] = {"osd-overlay", "1", "none", "", 0}; p_mpv_command(h, a); g_ov = false; }
        return;
    }
    RECT r; GetClientRect(g_vw, &r);
    int W = r.right, H = r.bottom;
    if (W < 80 || H < 80) return;
    static char buf[2048];
    int n = 0, fs = std::max(18, H / 38), m = W * 4 / 100;
    int si = g_shown;
    if (si < 0 || si >= (int)files.size()) return;
    std::string nm = U8(PathFindFileNameW(files[si].c_str()));
    for (auto& c : nm) if (c == '{' || c == '}') c = '('; else if (c == '\\') c = '/';
    n += snprintf(buf + n, sizeof(buf) - n, "{\\an7\\pos(%d,%d)\\fs%d\\bord2\\shad0\\1c&HFFFFFF&\\3c&H000000&\\3a&H60&}%s   %d/%d",
                  m, m / 2, fs, nm.c_str(), si + 1, (int)files.size());
    double dur = g_isVid ? getd("duration") : 0;
    if (dur > 0) {
        double pos = getd("time-pos"), f = std::clamp(pos / dur, 0.0, 1.0);
        int bw = W - 2 * m, by = H - m, bh = std::max(4, H / 150), fw = (int)(bw * f);
        char t1[16], t2[16]; fmt(pos, t1); fmt(dur, t2);
        n += snprintf(buf + n, sizeof(buf) - n, "\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\1a&HA0&\\p1}m 0 0 l %d 0 l %d %d l 0 %d{\\p0}", m, by, bw, bw, bh, bh);
        n += snprintf(buf + n, sizeof(buf) - n, "\n{\\an7\\pos(%d,%d)\\bord0\\shad0\\1c&HFFFFFF&\\p1}m 0 0 l %d 0 l %d %d l 0 %d{\\p0}", m, by, fw, fw, bh, bh);
        n += snprintf(buf + n, sizeof(buf) - n, "\n{\\an1\\pos(%d,%d)\\fs%d\\bord2\\shad0\\1c&HFFFFFF&\\3c&H000000&\\3a&H60&}%s / %s", m, by - bh, fs, t1, t2);
    }
    char ws[16], hs[16]; sprintf(ws, "%d", W); sprintf(hs, "%d", H);
    const char* a[] = {"osd-overlay", "1", "ass-events", buf, ws, hs, 0};
    p_mpv_command(h, a);
    g_ov = true;
}

// ---------- input (captured from mpv's window thread, handled on our thread) ----------
static bool ld, rd, md, skipClick;
static int dx0, dy0, rx, ry, rmode, mx, my;
static DWORD lastDown;
static double sB, sV, sPos, sZ;

// zoom keeping the point under (x,y) roughly fixed; returns the new zoom (log2)
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

// Mouse wheel AND precision-touchpad gestures. Touchpads send small fractional deltas
// (not multiples of 120) and horizontal scroll; everything is accumulated, nothing is rounded away.
static void onWheel(UINT t, WPARAM wp, LPARAM lp) {
    int raw = GET_WHEEL_DELTA_WPARAM(wp);
    if (!raw || !g_vw) return;
    DWORD now = GetTickCount();
    if (raw % 120) tpUntil = now + 600;          // fractional delta => touchpad
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
        if (z > 0.01) {                          // zoomed in: pan
            setd("video-pan-x", std::clamp(getd("video-pan-x") - d * 100.0 / (W * zf), -3.0, 3.0));
            return;
        }
        if (invSwipe) d = -d;
        if (!g_isVid) {                          // photo: swipe to next / previous
            if (now - lastH > 250) hAcc = 0;
            lastH = now;
            if (now < navCool) { navCool = now + 250; return; }   // swallow inertia after a swipe
            hAcc += d * 120;
            if (fabs(hAcc) > swipeThr) { go(hAcc > 0 ? 1 : -1); hAcc = 0; navCool = now + 450; }
        } else {                                 // video: swipe to scrub
            sAcc += d * 5;
            if (fabs(sAcc) >= 1) { snprintf(b, 96, "%.2f", sAcc); cmd("seek", b, "relative+keyframes"); sAcc = 0; }
        }
        return;
    }
    if (ctrl) {                                  // pinch (or Ctrl+wheel): smooth zoom toward the cursor
        double nz = zoomAt(z + 0.2 * d, pt.x, pt.y, W, H);
        snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, nz))); osd(b);
        return;
    }
    if (!tp) {                                   // classic mouse wheel
        if (!g_isVid) {
            double nz = zoomAt(z + 0.15 * d, pt.x, pt.y, W, H);
            snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, nz))); osd(b);
        } else if (shift) { snprintf(b, 96, "%d", (int)(d * 5)); cmd("seek", b); }
        else { setd("volume", std::clamp(getd("volume") + d * 5, 0.0, 130.0)); osd("Volume ${volume}%"); }
        return;
    }
    // two-finger vertical scroll on a touchpad
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
                else { double z = std::clamp(sZ - dy * 3.0 / H, -1.0, 6.0); setd("video-zoom", z); snprintf(b, 96, "Zoom %d%%", (int)(100 * pow(2.0, z))); osd(b); }
            } else if (rmode == 3) {
                double d = getd("duration");
                if (d > 0) { snprintf(b, 96, "%.2f", std::clamp(sPos + dx * d / W, 0.0, d)); cmd("seek", b, "absolute+keyframes"); }
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        DWORD now = GetTickCount();
        skipClick = false;
        if (lastDown && now - lastDown < GetDoubleClickTime() && abs(x - dx0) < 8 && abs(y - dy0) < 8) {
            if (g_isVid) cmd("cycle", "pause");   // undo the pause toggle from the first click
            act("fullscreen");
            lastDown = 0; skipClick = true;
        } else lastDown = now;
        dx0 = x; dy0 = y; ld = true;
        break;
    }
    case WM_LBUTTONUP:
        if (ld && !skipClick && abs(x - dx0) < 5 && abs(y - dy0) < 5 && g_isVid) {
            if (y > H * 0.9) {
                double p = std::clamp((x - W * 0.04) / (W * 0.92) * 100.0, 0.0, 100.0);
                snprintf(b, 96, "%.2f", p); cmd("seek", b, "absolute-percent");
            } else cmd("cycle", "pause");
        }
        ld = false; showUI();
        break;
    case WM_RBUTTONDOWN:
        rd = true; rx = x; ry = y; rmode = 0;
        sB = getd("brightness"); sV = getd("volume"); sPos = getd("time-pos"); sZ = getd("video-zoom");
        break;
    case WM_RBUTTONUP: rd = false; break;
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
                if (g_preload && g_preEv && g_preIdx != idx) { g_preIdx = idx; SetEvent(g_preEv); }
                if (!g_fixed && e->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                    g_fixed = true;                       // stop mpv resizing the window for every new file
                    setp("auto-window-resize", "no");     // -> no stretch/jump, new file just fills the same window
                }
            }
        }
        return 0;
    }
    if (m == WM_TIMER) { hookIfNeeded(); overlay(); return 0; }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
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
    L(mpv_create); L(mpv_initialize); L(mpv_set_option_string); L(mpv_set_property_string); L(mpv_get_property);
    L(mpv_command); L(mpv_wait_event); L(mpv_set_wakeup_callback); L(mpv_terminate_destroy);

    loadKeys();
    wchar_t full[MAX_PATH * 2];
    GetFullPathNameW(argv[1], MAX_PATH * 2, full, 0);
    scan(full);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MW; wc.hInstance = hi; wc.lpszClassName = L"LiteViewMsg";
    RegisterClassW(&wc);
    g_mw = CreateWindowExW(0, L"LiteViewMsg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, 0, hi, 0);

    h = p_mpv_create();
    const char* o[][2] = {
        {"config", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
        {"profile", "fast"}, {"vo", "gpu"}, {"hwdec", "auto-safe"}, {"osc", "no"}, {"osd-bar", "no"},
        {"input-default-bindings", "no"}, {"input-builtin-bindings", "no"}, {"border", "no"}, {"title", ""},
        {"force-window", "yes"}, {"keep-open", "yes"}, {"idle", "no"}, {"autofit-larger", "85%x85%"}, {"geometry", "50%:50%"},
        {"window-dragging", "yes"}, {"cursor-autohide", "1500"}, {"image-display-duration", "inf"},
        {"background-color", "#000000"}, {"osd-font-size", "30"}, {"osd-border-size", "2"}, {"osd-margin-y", "40"},
        {"volume-max", "130"}, {"demuxer-max-bytes", "24MiB"}, {"demuxer-max-back-bytes", "4MiB"},
        {"audio-display", "no"}, {"save-position-on-quit", "no"}, {"taskbar-progress", "no"}};
    for (auto& kv : o) p_mpv_set_option_string(h, kv[0], kv[1]);
    p_mpv_set_wakeup_callback(h, wake, 0);
    p_mpv_initialize(h);
    if (g_preload && files.size() > 1) {
        g_preEv = CreateEventW(0, FALSE, FALSE, 0);
        CreateThread(0, 0, preloadThread, 0, 0, 0);
    }
    openIdx(idx);
    SetTimer(g_mw, 1, 120, 0);

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    p_mpv_terminate_destroy(h);
    return 0;
}
