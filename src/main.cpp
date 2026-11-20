// LiteView - tiny photo/video viewer. Engine: libmpv (loaded at runtime). UI/input/overlay/settings: this file.
//
// Performance rules used throughout:
//  * Never ask mpv for a value on a hot path (it blocks while the core is decoding). The app keeps its own copy of the
//    values and mpv pushes time/pause/duration to us through observed properties. All commands are asynchronous.
//  * Zoom/pan/seek/OSD updates are coalesced and rate limited; rapid next/previous only loads the latest target.
//  * The player UI is drawn as vector shapes through mpv's ASS overlay (no extra windows, no extra libraries).
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
#include <memory>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <wincodec.h>
#include <mpv/client.h>
#include <mpv/stream_cb.h>

// ======================= libmpv (dynamic) =======================
#define F(ret, name, args) typedef ret (*name##_t) args; static name##_t p_##name;
F(mpv_handle*, mpv_create, (void))
F(int, mpv_initialize, (mpv_handle*))
F(int, mpv_set_option_string, (mpv_handle*, const char*, const char*))
F(char*, mpv_get_property_string, (mpv_handle*, const char*))
F(int, mpv_get_property, (mpv_handle*, const char*, mpv_format, void*))
F(int, mpv_command_async, (mpv_handle*, uint64_t, const char**))
F(int, mpv_set_property_async, (mpv_handle*, uint64_t, const char*, mpv_format, void*))
F(int, mpv_observe_property, (mpv_handle*, uint64_t, const char*, mpv_format))
F(mpv_event*, mpv_wait_event, (mpv_handle*, double))
F(void, mpv_set_wakeup_callback, (mpv_handle*, void (*)(void*), void*))
F(void, mpv_terminate_destroy, (mpv_handle*))
F(void, mpv_free, (void*))
F(int, mpv_stream_cb_add_ro, (mpv_handle*, const char*, void*, mpv_stream_cb_open_ro_fn))

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
static std::string KMS[2][256];      // [0] photo / [1] video : vk -> action

// ---- user options (edited in the Settings window, stored in %APPDATA%\LiteView\keys.ini) ----
static int o_autonext = 1, o_remvol = 1, o_vol = 100, o_startfs = 0, o_ontop = 0, o_uisec = 2, o_preload = 1;
static int o_inv = 0, o_invv = 0, o_swipe = 300, o_hq = 1, o_smooth = 0, o_deband = 0, o_hwdec = 1;
static int o_ultra = 0, o_icc = 0, o_next = 0, o_stereo = 1, o_resume = 1, o_subpct = 100;
static wchar_t o_slang[16], o_alang[16];

static volatile LONG g_preIdx = -1;
static HANDLE g_preEv;

// ---- state we own / cache (no round trips to mpv) ----
static double c_dur, c_pos;                 // pushed by mpv (observed)
static bool c_pause, c_eof, c_full;         // pushed by mpv (observed)
static double s_vol = 100, s_bright, s_contrast, s_sat, s_zoom, s_px, s_py, s_speed = 1, s_subdelay, s_audiodelay, s_subscale = 1;
static int s_rot;
static bool s_mute, s_loop;
static bool g_viewDirty, g_osdDirty, g_seekDirty;
static double g_tView, g_tSeek, g_tOsd, g_tOv, g_seekTarget;
static volatile LONG g_mmPending;           // coalesced mouse-move from the hook thread
static volatile LPARAM g_mmLp;
static volatile WPARAM g_mmWp;
static char g_osdBuf[96];
static DWORD g_lastLoad;
static int g_pending = -1, g_mode = -1;     // g_mode: 0 photo, 1 video
static bool g_timerOn;

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
// async command (never blocks)
static void cmd(const char* a, const char* b = 0, const char* c = 0, const char* d = 0,
                const char* e = 0, const char* f = 0, const char* g = 0) {
    const char* v[] = {a, b, c, d, e, f, g, 0};
    p_mpv_command_async(h, 0, v);
}
static void setp(const char* n, const char* v) { const char* s = v; p_mpv_set_property_async(h, 0, n, MPV_FORMAT_STRING, &s); }
static void setd(const char* n, double v) { p_mpv_set_property_async(h, 0, n, MPV_FORMAT_DOUBLE, &v); }
// blocking read: ONLY for user-initiated rare things (menus, info, track names)
static std::string mgets(const char* n) {
    char* s = p_mpv_get_property_string(h, n);
    std::string r = s ? s : "";
    if (s) p_mpv_free(s);
    return r;
}
static bool isYes(const char* n) { return mgets(n) == "yes"; }
static void osd(const char* s) { cmd("show-text", s, "1000"); }
static void osdf(const char* fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    osd(b);
}
static void osdLater(const char* fmt, ...) {      // coalesced: only the latest text is sent
    va_list ap; va_start(ap, fmt); vsnprintf(g_osdBuf, sizeof g_osdBuf, fmt, ap); va_end(ap);
    g_osdDirty = true;
}
static void kick() { if (!g_timerOn) { SetTimer(g_mw, 2, 16, 0); g_timerOn = true; } }   // fallback flush timer
static double nowms() {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / f.QuadPart;
}
static void showUI() { g_ui = GetTickCount() + std::max(1, o_uisec) * 1000; }
static void pump();
static void openSettings();
static void menuCmd(int id);
static void act(const std::string& a);
static void openPanel(int p, int anchor);
static void closePanel();

// ======================= file types =======================
static bool extIn(std::wstring e, const wchar_t* const* L_, int n) {
    for (auto& c : e) c = towlower(c);
    for (int i = 0; i < n; i++) if (e == L_[i]) return true;
    return false;
}
#define CNT(a) ((int)(sizeof(a) / sizeof(*(a))))
// Photos handled by mpv/FFmpeg
static const wchar_t* const EXT_IMG[] = {L".jpg", L".jpeg", L".jpe", L".jfif", L".jif", L".png", L".apng", L".gif", L".webp", L".bmp", L".dib",
    L".tif", L".tiff", L".avif", L".jxl", L".ico", L".cur", L".tga", L".pcx", L".pbm", L".pgm", L".ppm", L".pnm", L".pam", L".pfm",
    L".xbm", L".xpm", L".sgi", L".ras", L".xwd", L".exr", L".hdr", L".dds", L".psd", L".jp2", L".j2k", L".jpf", L".jpx", L".qoi",
    L".dpx", L".fits"};
// Photos decoded through Windows' own codecs (WIC): HEIC/HEIF, JPEG XR, camera RAW (needs the matching Windows codec extension)
static const wchar_t* const EXT_WIC[] = {L".heic", L".heif", L".hif", L".jxr", L".wdp", L".hdp", L".dng", L".cr2", L".cr3", L".crw", L".nef",
    L".nrw", L".arw", L".srf", L".sr2", L".raf", L".orf", L".rw2", L".rwl", L".pef", L".srw", L".x3f", L".3fr", L".erf", L".kdc", L".dcr",
    L".mrw", L".mef", L".mos", L".iiq"};
static const wchar_t* const EXT_VID[] = {L".mp4", L".m4v", L".mp4v", L".mkv", L".mk3d", L".webm", L".avi", L".mov", L".qt", L".wmv", L".asf",
    L".flv", L".f4v", L".mts", L".m2ts", L".m2t", L".tp", L".trp", L".mpg", L".mpeg", L".mpe", L".m2v", L".vob", L".ogv", L".ogm",
    L".3gp", L".3g2", L".3gpp", L".divx", L".xvid", L".rm", L".rmvb", L".hevc", L".h265", L".265", L".h264", L".264", L".avc", L".mxf",
    L".dv", L".wtv", L".dvr-ms", L".nsv", L".y4m", L".ivf", L".mjpeg", L".mjpg", L".vc1", L".amv", L".swf", L".evo", L".bik", L".smk"};
static const wchar_t* const EXT_AUD[] = {L".mp3", L".flac", L".wav", L".ogg", L".oga", L".opus", L".m4a", L".aac", L".wma", L".ape", L".wv",
    L".aiff", L".aif", L".alac", L".mka", L".ac3", L".dts", L".mp2", L".mpc", L".tta", L".caf"};
static bool isWicPrimary(const std::wstring& e) { return extIn(e, EXT_WIC, CNT(EXT_WIC)); }
static bool isImg(const std::wstring& e) { return extIn(e, EXT_IMG, CNT(EXT_IMG)) || isWicPrimary(e); }
static bool isVidExt(const std::wstring& e) { return extIn(e, EXT_VID, CNT(EXT_VID)) || extIn(e, EXT_AUD, CNT(EXT_AUD)); }
static const std::wstring& mediaFilter() {            // file-open dialog filter built from the same tables
    static std::wstring f;
    if (f.empty()) {
        std::wstring all;
        auto add = [&](const wchar_t* const* L_, int n) { for (int i = 0; i < n; i++) { all += L"*"; all += L_[i]; all += L";"; } };
        add(EXT_IMG, CNT(EXT_IMG)); add(EXT_WIC, CNT(EXT_WIC)); add(EXT_VID, CNT(EXT_VID)); add(EXT_AUD, CNT(EXT_AUD));
        if (!all.empty()) all.pop_back();
        f = L"Photos, videos and audio"; f.push_back(0); f += all; f.push_back(0);
        f += L"All files"; f.push_back(0); f += L"*.*"; f.push_back(0);
    }
    return f;
}

// ======================= folder scan (off the main thread) =======================
static void scanInto(const std::wstring& file, std::vector<std::wstring>& v, int& ix) {
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
            if (isImg(e) || isVidExt(e)) v.push_back(std::wstring(dir) + L"\\" + fd.cFileName);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(v.begin(), v.end(), [](const std::wstring& a, const std::wstring& b) { return StrCmpLogicalW(a.c_str(), b.c_str()) < 0; });
    ix = -1;
    for (size_t i = 0; i < v.size(); i++) if (!lstrcmpiW(v[i].c_str(), file.c_str())) ix = (int)i;
    if (ix < 0) { v.push_back(file); ix = (int)v.size() - 1; }
}
static void scan(const std::wstring& file) { scanInto(file, files, idx); }
static std::wstring g_full;
static bool g_scanPending;
static volatile LONG g_scanTok;
struct ScanJob { std::wstring file; int token; };
struct ScanRes { std::vector<std::wstring> v; int ix; int token; };
static DWORD WINAPI scanThread(LPVOID p) {
    ScanJob* j = (ScanJob*)p;
    ScanRes* r = new ScanRes;
    r->token = j->token; r->ix = 0;
    scanInto(j->file, r->v, r->ix);
    PostMessageW(g_mw, WM_USER + 3, 0, (LPARAM)r);
    delete j;
    return 0;
}

// ======================= quality / per-mode settings =======================
static void applyQuality() {
    // Fast: bilinear/hermite.  High (default): spline36 up, mitchell down.  Ultra: ewa_lanczossharp + linear light (heavy).
    setp("scale", o_ultra ? "ewa_lanczossharp" : (o_hq ? "spline36" : "bilinear"));
    setp("dscale", (o_ultra || o_hq) ? "mitchell" : "hermite");   // wide kernel when shrinking: no dots / moire
    setp("linear-downscaling", o_ultra ? "yes" : "no");
    setp("sigmoid-upscaling", o_ultra ? "yes" : "no");
}
static void applyMode(bool vid) {
    if (vid) {
        setp("interpolation", o_smooth ? "yes" : "no");
        setp("video-sync", o_smooth ? "display-resample" : "audio");
        setp("tscale", "oversample");
        setp("vd-lavc-threads", "0");
        setp("deband", o_deband ? "yes" : "no");
    } else {                                                  // still images: exact pixels, no motion tricks
        setp("interpolation", "no");
        setp("video-sync", "audio");
        setp("vd-lavc-threads", "1");               // a single still needs no decoder threads (saves memory + start-up)
        setp("deband", "no");
    }
}
static void applyRuntime() {
    applyQuality();
    g_mode = -1; applyMode(g_isVid); g_mode = g_isVid ? 1 : 0;
    setp("hwdec", o_hwdec ? "auto-safe" : "no");
    setp("icc-profile-auto", o_icc ? "yes" : "no");
    setp("audio-channels", o_stereo ? "stereo" : "auto-safe");
    setp("ontop", o_ontop ? "yes" : "no");
}

// ======================= view / audio state =======================
static void sendView() { setd("video-zoom", s_zoom); setd("video-pan-x", s_px); setd("video-pan-y", s_py); }
static void viewChanged() { g_viewDirty = true; kick(); }
static void setVol(double v) { s_vol = std::clamp(v, 0.0, 130.0); setd("volume", s_vol); }
static void setBright(double v) { s_bright = std::clamp(floor(v + 0.5), -100.0, 100.0); setd("brightness", s_bright); }
static void setContrast(double v) { s_contrast = std::clamp(floor(v + 0.5), -100.0, 100.0); setd("contrast", s_contrast); }
static void setSat(double v) { s_sat = std::clamp(floor(v + 0.5), -100.0, 100.0); setd("saturation", s_sat); }
static void setSpeed(double v) { s_speed = std::clamp(floor(v * 20 + 0.5) / 20, 0.1, 8.0); setd("speed", s_speed); }
static void setSubScale(double v) { s_subscale = std::clamp(floor(v * 20 + 0.5) / 20, 0.3, 3.0); setd("sub-scale", s_subscale); }
static void setSubDelay(double v) { s_subdelay = floor(v * 10 + 0.5) / 10; setd("sub-delay", s_subdelay); }
static void setAudDelay(double v) { s_audiodelay = floor(v * 10 + 0.5) / 10; setd("audio-delay", s_audiodelay); }

// ======================= Windows codec route (HEIC, RAW, JPEG XR, fallback) =======================
// Decoded on a low-priority worker thread to a 24-bit BMP held in memory, then served to mpv via a custom "lvmem://" stream.
struct Blob { std::vector<unsigned char> d; };
static CRITICAL_SECTION g_bl;
static std::vector<std::pair<int, std::shared_ptr<Blob>>> g_blobList;
static int g_blobSeq;
static int addBlob(std::shared_ptr<Blob> b) {
    EnterCriticalSection(&g_bl);
    int id = ++g_blobSeq;
    g_blobList.clear();                               // only the newest picture is kept
    g_blobList.push_back({id, b});
    LeaveCriticalSection(&g_bl);
    return id;
}
struct Cur { std::shared_ptr<Blob> b; int64_t pos; };
static int64_t sc_read(void* c, char* buf, uint64_t n) {
    Cur* k = (Cur*)c;
    int64_t left = (int64_t)k->b->d.size() - k->pos;
    if (left <= 0) return 0;
    int64_t m = std::min<int64_t>(left, (int64_t)n);
    memcpy(buf, k->b->d.data() + k->pos, (size_t)m);
    k->pos += m;
    return m;
}
static int64_t sc_seek(void* c, int64_t off) {
    Cur* k = (Cur*)c;
    if (off < 0 || off > (int64_t)k->b->d.size()) return MPV_ERROR_GENERIC;
    k->pos = off;
    return off;
}
static int64_t sc_size(void* c) { return (int64_t)((Cur*)c)->b->d.size(); }
static void sc_close(void* c) { delete (Cur*)c; }
static int sc_open(void*, char* uri, mpv_stream_cb_info* info) {
    const char* q = strstr(uri, "://");
    int id = q ? atoi(q + 3) : 0;
    std::shared_ptr<Blob> b;
    EnterCriticalSection(&g_bl);
    for (auto& e : g_blobList) if (e.first == id) b = e.second;
    LeaveCriticalSection(&g_bl);
    if (!b) return MPV_ERROR_LOADING_FAILED;
    info->cookie = new Cur{b, 0};
    info->read_fn = sc_read; info->seek_fn = sc_seek; info->size_fn = sc_size; info->close_fn = sc_close; info->cancel_fn = 0;
    return 0;
}
static std::shared_ptr<Blob> wicDecode(const std::wstring& path) {
    IWICImagingFactory* fac = 0; IWICBitmapDecoder* dec = 0; IWICBitmapFrameDecode* fr = 0;
    IWICFormatConverter* cv = 0; IWICBitmapFlipRotator* fl = 0;
    std::shared_ptr<Blob> out;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, 0, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, (void**)&fac))) return out;
    do {
        if (FAILED(fac->CreateDecoderFromFilename(path.c_str(), 0, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) break;
        if (FAILED(dec->GetFrame(0, &fr))) break;
        UINT w = 0, h = 0;
        fr->GetSize(&w, &h);
        if (!w || !h || (unsigned long long)w * h > 100000000ULL) break;          // up to 100 megapixels
        if (FAILED(fac->CreateFormatConverter(&cv))) break;
        if (FAILED(cv->Initialize(fr, GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, 0, 0, WICBitmapPaletteTypeCustom))) break;
        if (FAILED(fac->CreateBitmapFlipRotator(&fl))) break;                      // BMP rows are stored bottom-up
        if (FAILED(fl->Initialize(cv, WICBitmapTransformFlipVertical))) break;
        UINT stride = (w * 3 + 3) & ~3u;
        size_t pix = (size_t)stride * h;
        out = std::make_shared<Blob>();
        out->d.resize(54 + pix);
        unsigned char* b = out->d.data();
        memset(b, 0, 54);
        auto u32 = [&](int o, uint32_t v) { memcpy(b + o, &v, 4); };
        b[0] = 'B'; b[1] = 'M';
        u32(2, (uint32_t)(54 + pix)); u32(10, 54); u32(14, 40); u32(18, w); u32(22, h);
        b[26] = 1; b[28] = 24; u32(34, (uint32_t)pix);
        if (FAILED(fl->CopyPixels(0, stride, (UINT)pix, b + 54))) out.reset();
    } while (0);
    if (fl) fl->Release();
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    fac->Release();
    return out;
}
struct WicJob { std::wstring path; int token, idx; bool fb; };
struct WicRes { std::shared_ptr<Blob> blob; int token, idx; bool fb; };
static volatile LONG g_wicToken;
static int g_wicTried = -1;
static DWORD WINAPI wicThread(LPVOID p) {
    WicJob* j = (WicJob*)p;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // keep the window responsive while decoding
    CoInitializeEx(0, COINIT_MULTITHREADED);
    std::shared_ptr<Blob> b = wicDecode(j->path);
    CoUninitialize();
    PostMessageW(g_mw, WM_USER + 2, 0, (LPARAM) new WicRes{b, j->token, j->idx, j->fb});
    delete j;
    return 0;
}
static void startWic(int i, bool fallback) {
    g_wicTried = i;
    g_isVid = false;
    WicJob* j = new WicJob{files[i], (int)InterlockedIncrement(&g_wicToken), i, fallback};
    HANDLE t = CreateThread(0, 0, wicThread, j, 0, 0);
    if (t) CloseHandle(t); else delete j;
}
static void showCantOpen() {
    cmd("show-text", "Can't open this file (missing codec?)\nHEIC / RAW: install the free HEIF, HEVC and RAW image extensions from the Microsoft Store", "6000");
}

// ======================= resume position (per video) =======================
static wchar_t g_resIni[MAX_PATH];
static std::wstring g_curVid;                   // video currently loaded (for saving its position)
static DWORD g_toastUntil;                      // "Resumed at ..." popup
static double g_toastPos;
static std::wstring resKey(const std::wstring& p) {
    std::wstring l = p;
    for (auto& c : l) c = towlower(c);
    unsigned long long hh = 1469598103934665603ULL;
    for (wchar_t c : l) { hh ^= (unsigned long long)c; hh *= 1099511628211ULL; }
    wchar_t b[40];
    _snwprintf(b, 40, L"%08x%08x", (unsigned)(hh >> 32), (unsigned)(hh & 0xffffffffu));
    return b;
}
static bool resGet(const std::wstring& p, double& pos, double& dur) {
    wchar_t v[64];
    GetPrivateProfileStringW(L"r", resKey(p).c_str(), L"", v, 64, g_resIni);
    if (!v[0]) return false;
    wchar_t* e = 0;
    pos = wcstod(v, &e);
    if (!e || *e != L',') return false;
    dur = wcstod(e + 1, 0);
    return true;
}
static void saveResume() {
    if (!o_resume || g_curVid.empty() || c_dur < 20) return;
    std::wstring k = resKey(g_curVid);
    if (c_pos > c_dur - 8) { WritePrivateProfileStringW(L"r", k.c_str(), 0, g_resIni); return; }    // finished: forget it
    if (c_pos < 8) return;
    wchar_t v[64];
    _snwprintf(v, 64, L"%.1f,%.1f", c_pos, c_dur);
    WritePrivateProfileStringW(L"r", k.c_str(), v, g_resIni);
}

// ======================= loading =======================
static void doLoad() {
    int i = g_pending; g_pending = -1;
    if (i < 0 || i >= (int)files.size()) return;
    g_lastLoad = GetTickCount();
    InterlockedIncrement(&g_wicToken);              // drop any decode still running for a previous file
    saveResume();                                    // remember where the previous video stopped
    g_curVid.clear(); c_pos = 0; c_dur = 0; g_toastUntil = 0;
    closePanel();
    std::wstring p = files[i];
    std::wstring e = PathFindExtensionW(p.c_str());
    for (auto& c : e) c = towlower(c);
    bool vid = !isImg(e);
    g_isVid = vid;
    if (g_mode != (vid ? 1 : 0)) { g_mode = vid ? 1 : 0; applyMode(vid); }
    if (s_zoom != 0 || s_px != 0 || s_py != 0) { s_zoom = s_px = s_py = 0; sendView(); }
    if (s_rot) { s_rot = 0; setd("video-rotate", 0); }
    bool lp = (e == L".gif" || e == L".webp" || e == L".apng");
    if (lp != s_loop) { s_loop = lp; setp("loop-file", lp ? "inf" : "no"); }
    if (isWicPrimary(e)) { startWic(i, false); return; }   // HEIC / RAW / JXR: decode with Windows codecs (old picture stays until ready)
    std::string u = U8(p);
    char st[48] = "";
    double rp = 0, rd = 0;
    if (vid) {
        g_curVid = p;
        if (o_resume && resGet(p, rp, rd) && rp >= 8 && rp < rd - 8) {   // continue where it stopped; a popup lets you start over
            snprintf(st, sizeof st, "start=%.2f", rp);
            g_toastUntil = GetTickCount() + 7000;
            g_toastPos = rp;
        }
    }
    if (st[0]) cmd("loadfile", u.c_str(), "replace", "-1", st);
    else cmd("loadfile", u.c_str(), "replace");
    setp("pause", "no");
}
static void openIdx(int i) {
    idx = i;
    g_pending = i;
    showUI();
    if (GetTickCount() - g_lastLoad >= 90) doLoad();     // first request loads at once; a burst only loads the latest
    else kick();
}
static void go(int d) {
    int n = (int)files.size();
    if (n > 1) openIdx(((idx + d) % n + n) % n);
}
static void setFiles(const std::wstring& path) {
    EnterCriticalSection(&g_fl);
    files.clear();
    InterlockedIncrement(&g_scanTok);
    scan(path);
    InterlockedIncrement(&g_flGen);
    LeaveCriticalSection(&g_fl);
    g_lastLoad = 0;
    openIdx(idx);
}

// ======================= neighbour preload (OS file cache warm-up) =======================
static DWORD WINAPI preloadThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    char* buf = (char*)VirtualAlloc(0, 1 << 20, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::vector<char> warm;
    LONG gen = -1;
    static const int order[] = {1, -1};
    for (;;) {
        WaitForSingleObject(g_preEv, INFINITE);
    again:
        LONG b0 = g_preIdx; Sleep(450); if (b0 != g_preIdx) goto again;     // let the photo/video show first
        int base = (int)g_preIdx, ids[4], cnt = 0;
        std::wstring paths[4];
        EnterCriticalSection(&g_fl);
        if (gen != g_flGen) { gen = g_flGen; warm.assign(files.size(), 0); }
        int n = (int)files.size();
        if (base >= 0 && base < n && n > 1)
            for (int k = 0; k < 2; k++) {
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
            long long head = vid ? (4LL << 20) : (16LL << 20), done = 0;
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

// ======================= settings storage (separate shortcuts for photo and video) =======================
struct Act { const char* id; const wchar_t* label; int mask; };      // mask: 1 photo, 2 video
static const Act ACTS[] = {
    {"close", L"Close viewer", 3}, {"next", L"Next file", 3}, {"prev", L"Previous file", 3}, {"first", L"First file", 3},
    {"last", L"Last file", 3}, {"fullscreen", L"Fullscreen", 3}, {"info", L"File info", 3}, {"delete", L"Delete file (Recycle Bin)", 3},
    {"settings", L"Settings", 3}, {"menu", L"Context menu", 3}, {"brightup", L"Brightness up", 3}, {"brightdown", L"Brightness down", 3},
    {"rotate", L"Rotate", 3}, {"zoomin", L"Zoom in", 3}, {"zoomout", L"Zoom out", 3}, {"zoomreset", L"Reset zoom", 3},
    {"quality", L"Cycle quality (Fast / High / Ultra)", 1},
    {"pause", L"Play / pause", 2}, {"volup", L"Volume up", 2}, {"voldown", L"Volume down", 2}, {"mute", L"Mute", 2},
    {"seekfwd", L"Seek forward 5 s", 2}, {"seekback", L"Seek back 5 s", 2}, {"seekfwd10", L"Seek forward 10 s", 2},
    {"seekback10", L"Seek back 10 s", 2}, {"framenext", L"Next frame", 2}, {"frameback", L"Previous frame", 2},
    {"speedup", L"Speed up", 2}, {"speeddown", L"Speed down", 2}, {"speedreset", L"Reset speed", 2},
    {"subcycle", L"Next subtitle track", 2}, {"audiocycle", L"Next audio track", 2},
    {"subdelayminus", L"Subtitle delay -0.1 s", 2}, {"subdelayplus", L"Subtitle delay +0.1 s", 2},
    {"audiodelayminus", L"Audio delay -0.1 s", 2}, {"audiodelayplus", L"Audio delay +0.1 s", 2},
    {"loop", L"Loop video", 2}, {"screenshot", L"Screenshot", 2}};
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
static const DefKey DEFK_P[] = {                       // photo viewer
    {L"Esc", "close"}, {L"Right", "next"}, {L"Left", "prev"}, {L"PgDn", "next"}, {L"PgUp", "prev"}, {L"Home", "first"}, {L"End", "last"},
    {L"F", "fullscreen"}, {L"Enter", "fullscreen"}, {L"Plus", "zoomin"}, {L"Minus", "zoomout"}, {L"0", "zoomreset"}, {L"R", "rotate"},
    {L"B", "brightup"}, {L"V", "brightdown"}, {L"I", "info"}, {L"Delete", "delete"}, {L"Q", "quality"}, {L"S", "settings"},
    {L"F1", "settings"}, {L"Tab", "menu"}};
static const DefKey DEFK_V[] = {                       // video player: arrows seek / change volume
    {L"Esc", "close"}, {L"Right", "seekfwd"}, {L"Left", "seekback"}, {L"Up", "volup"}, {L"Down", "voldown"}, {L"PgDn", "next"},
    {L"PgUp", "prev"}, {L"Home", "first"}, {L"End", "last"}, {L"Space", "pause"}, {L"K", "pause"}, {L"M", "mute"}, {L"F", "fullscreen"},
    {L"Enter", "fullscreen"}, {L"J", "seekback10"}, {L"L", "seekfwd10"}, {L"Period", "framenext"}, {L"Comma", "frameback"},
    {L"RBracket", "speedup"}, {L"LBracket", "speeddown"}, {L"Backspace", "speedreset"}, {L"C", "subcycle"}, {L"A", "audiocycle"},
    {L"Z", "subdelayminus"}, {L"X", "subdelayplus"}, {L"O", "loop"}, {L"P", "screenshot"}, {L"R", "rotate"}, {L"B", "brightup"},
    {L"V", "brightdown"}, {L"Plus", "zoomin"}, {L"Minus", "zoomout"}, {L"0", "zoomreset"}, {L"I", "info"}, {L"Delete", "delete"},
    {L"S", "settings"}, {L"F1", "settings"}, {L"Tab", "menu"}};

static wchar_t g_ini[MAX_PATH];
static void cfgPath() {
    SHGetFolderPathW(0, CSIDL_APPDATA, 0, 0, g_ini);
    PathAppendW(g_ini, L"LiteView");
    CreateDirectoryW(g_ini, 0);
    wcscpy(g_resIni, g_ini);
    PathAppendW(g_resIni, L"resume.ini");
    PathAppendW(g_ini, L"keys.ini");
}
static void applyDefaultKeys(int m) {
    for (auto& s : KMS[m]) s.clear();
    const DefKey* t = m ? DEFK_V : DEFK_P;
    int n = m ? CNT(DEFK_V) : CNT(DEFK_P);
    for (int i = 0; i < n; i++) { int vk = vkOf(t[i].k); if (vk) KMS[m][vk] = t[i].a; }
}
static int gi(const wchar_t* k, int d) { return GetPrivateProfileIntW(L"options", k, d, g_ini); }
static void gs(const wchar_t* k, wchar_t* out) { GetPrivateProfileStringW(L"options", k, L"", out, 16, g_ini); }
static void wi(const wchar_t* k, int v) { wchar_t b[32]; _snwprintf(b, 32, L"%d", v); WritePrivateProfileStringW(L"options", k, b, g_ini); }

static void loadSettings() {
    cfgPath();
    o_autonext = gi(L"autonext", 1); o_remvol = gi(L"remember_volume", 1); o_vol = gi(L"volume", 100);
    o_startfs = gi(L"start_fullscreen", 0); o_ontop = gi(L"always_on_top", 0); o_uisec = gi(L"controls_hide_seconds", 2);
    o_preload = gi(L"preload", 1); o_inv = gi(L"invert_swipe", 0); o_invv = gi(L"reverse_scroll", 0); o_swipe = gi(L"swipe_distance", 300);
    o_hq = gi(L"high_quality", 1); o_smooth = gi(L"smooth_motion", 0); o_deband = gi(L"deband_video", 0);
    o_ultra = gi(L"ultra_quality", 0); o_icc = gi(L"icc_profile", 0); o_next = gi(L"gpu_next", 0);
    o_hwdec = gi(L"hardware_decoding", 1); o_stereo = gi(L"stereo_downmix", 1); o_resume = gi(L"remember_position", 1);
    o_subpct = std::clamp(gi(L"subtitle_size_percent", 100), 30, 300);
    gs(L"subtitle_language", o_slang); gs(L"audio_language", o_alang);
    static const wchar_t* sec[2] = {L"keys_photo", L"keys_video"};
    static wchar_t buf[8192];
    for (int m = 0; m < 2; m++) {
        for (auto& s : KMS[m]) s.clear();
        DWORD n = GetPrivateProfileSectionW(sec[m], buf, 8192, g_ini);
        if (n == 0) { applyDefaultKeys(m); continue; }
        for (wchar_t* s = buf; *s; s += wcslen(s) + 1) {
            wchar_t* eq = wcschr(s, L'=');
            if (!eq) continue;
            *eq = 0;
            wchar_t* k = s; wchar_t* v = eq + 1;
            StrTrimW(k, L" \t"); StrTrimW(v, L" \t");
            int vk = vkOf(k);
            if (vk > 0 && vk < 256) KMS[m][vk] = U8(v);
        }
    }
}
static void saveSettings() {
    wi(L"version", 4);
    wi(L"autonext", o_autonext); wi(L"remember_volume", o_remvol); wi(L"volume", o_vol); wi(L"start_fullscreen", o_startfs);
    wi(L"always_on_top", o_ontop); wi(L"controls_hide_seconds", o_uisec); wi(L"preload", o_preload); wi(L"invert_swipe", o_inv);
    wi(L"reverse_scroll", o_invv); wi(L"swipe_distance", o_swipe); wi(L"high_quality", o_hq); wi(L"smooth_motion", o_smooth);
    wi(L"deband_video", o_deband); wi(L"ultra_quality", o_ultra); wi(L"icc_profile", o_icc); wi(L"gpu_next", o_next);
    wi(L"hardware_decoding", o_hwdec); wi(L"stereo_downmix", o_stereo); wi(L"remember_position", o_resume);
    wi(L"subtitle_size_percent", o_subpct);
    WritePrivateProfileStringW(L"options", L"subtitle_language", o_slang, g_ini);
    WritePrivateProfileStringW(L"options", L"audio_language", o_alang, g_ini);
    static const wchar_t* sec[2] = {L"keys_photo", L"keys_video"};
    for (int m = 0; m < 2; m++) {
        WritePrivateProfileStringW(sec[m], 0, 0, g_ini);
        for (int vk = 1; vk < 256; vk++) {
            if (KMS[m][vk].empty()) continue;
            std::wstring nm = nameOf(vk);
            if (!nm.empty()) WritePrivateProfileStringW(sec[m], nm.c_str(), Wd(KMS[m][vk]).c_str(), g_ini);
        }
    }
}

// ======================= UI state (panels, drag, hit regions) =======================
enum { B_PREV = 1, B_BACK, B_PLAY, B_FWD, B_NEXT, B_VOL, B_BRI, B_SPEED, B_SUB, B_AUD, B_MORE, B_ZOUT, B_ZIN, B_ZPILL, B_ROT, B_TSTART, B_TOK };
enum { P_NONE = 0, P_VOL, P_BRI, P_SPEED, P_SUB, P_AUD };
enum { H_BTN = 1, H_SEEK, H_SLIDER, H_ROW, H_PANEL, H_RESET };
enum { S_VOL = 1, S_BRI, S_CON, S_SAT, S_SPEED, S_SUBSIZE, S_SUBDELAY, S_AUDDELAY };
enum { A_MUTE = 1, A_RESETCOL, A_RESETSPEED, A_SUBCYCLE, A_SUBLOAD, A_SUBOFF, A_AUDCYCLE, A_STEREO };
struct Hit { int kind, id, x, y, w, h, tx, tw; };
static Hit g_hits[96];
static int g_nh;
static int g_panel, g_panelAnchor;
static int g_dragKind, g_dragId, g_dragX0, g_dragW;       // 1 = seek bar, 2 = slider
static double g_scrub;
static int g_downKind, g_downId;
static std::string g_subTxt = "Off", g_audTxt = "-";
struct Row { int type, id; char label[56]; double mn, mx, v, def; int fmt; };   // type 0 slider, 1 action

static bool toastActive() { return g_toastUntil && GetTickCount() < g_toastUntil; }
static bool controlsVisible() { return GetTickCount() < g_ui || g_panel || g_dragKind || (g_isVid && c_pause); }

static std::string trackTxt(const char* prop) {
    std::string s = mgets(prop);
    if (s.empty() || s == "no") return "Off";
    return "track " + s;
}
static int panelRows(int p, Row* r) {
    int n = 0;
    auto sl = [&](int id, const char* l, double mn, double mx, double v, double def, int fmt) {
        Row& x = r[n++]; x.type = 0; x.id = id; snprintf(x.label, sizeof x.label, "%s", l); x.mn = mn; x.mx = mx; x.v = v; x.def = def; x.fmt = fmt; };
    auto ac = [&](int id, const char* l) {
        Row& x = r[n++]; x.type = 1; x.id = id; snprintf(x.label, sizeof x.label, "%s", l); x.mn = x.mx = x.v = x.def = 0; x.fmt = 0; };
    char b[80];
    switch (p) {
    case P_VOL: sl(S_VOL, "Volume", 0, 130, s_vol, 100, 0); ac(A_MUTE, s_mute ? "Unmute" : "Mute"); break;
    case P_BRI: sl(S_BRI, "Brightness", -100, 100, s_bright, 0, 0); sl(S_CON, "Contrast", -100, 100, s_contrast, 0, 0);
                sl(S_SAT, "Saturation", -100, 100, s_sat, 0, 0); ac(A_RESETCOL, "Reset colours"); break;
    case P_SPEED: sl(S_SPEED, "Speed", 0.25, 4, s_speed, 1, 1); ac(A_RESETSPEED, "Reset to 1x"); break;
    case P_SUB: snprintf(b, sizeof b, "Subtitles: %s   (tap to switch)", g_subTxt.c_str()); ac(A_SUBCYCLE, b);
                sl(S_SUBSIZE, "Size", 30, 300, s_subscale * 100, 100, 3); sl(S_SUBDELAY, "Delay", -10, 10, s_subdelay, 0, 2);
                ac(A_SUBLOAD, "Load subtitle file..."); ac(A_SUBOFF, "Turn subtitles off"); break;
    case P_AUD: snprintf(b, sizeof b, "Audio: %s   (tap to switch)", g_audTxt.c_str()); ac(A_AUDCYCLE, b);
                sl(S_AUDDELAY, "Audio delay", -5, 5, s_audiodelay, 0, 2);
                ac(A_STEREO, o_stereo ? "Stereo downmix: on" : "Stereo downmix: off"); break;
    }
    return n;
}
static void fmtVal(int f, double v, char* o) {
    switch (f) {
    case 1: sprintf(o, "%.2fx", v); break;
    case 2: sprintf(o, "%+.1f s", v); break;
    case 3: sprintf(o, "%d%%", (int)floor(v + 0.5)); break;
    default: sprintf(o, "%d", (int)floor(v + 0.5));
    }
}
static void setSliderId(int id, double v) {
    switch (id) {
    case S_VOL: setVol(v); break;
    case S_BRI: setBright(v); break;
    case S_CON: setContrast(v); break;
    case S_SAT: setSat(v); break;
    case S_SPEED: setSpeed(v); break;
    case S_SUBSIZE: setSubScale(v / 100.0); o_subpct = (int)floor(s_subscale * 100 + 0.5); break;
    case S_SUBDELAY: setSubDelay(v); break;
    case S_AUDDELAY: setAudDelay(v); break;
    }
}
static void openPanel(int p, int anchor) {
    if (p == P_SUB) g_subTxt = trackTxt("sid");
    if (p == P_AUD) g_audTxt = trackTxt("aid");
    g_panel = p; g_panelAnchor = anchor;
    showUI();
}
static void closePanel() { g_panel = P_NONE; g_dragKind = 0; }

static bool pickFile(wchar_t* out, const wchar_t* filter) {
    OPENFILENAMEW o = {};
    out[0] = 0;
    o.lStructSize = sizeof o; o.hwndOwner = g_mw; o.lpstrFile = out; o.nMaxFile = MAX_PATH * 2;
    o.lpstrFilter = filter; o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o) != 0;
}
static void rowAction(int id) {
    switch (id) {
    case A_MUTE: act("mute"); break;
    case A_RESETCOL: setBright(0); setContrast(0); setSat(0); break;
    case A_RESETSPEED: setSpeed(1); break;
    case A_SUBCYCLE: cmd("cycle", "sub"); g_subTxt = trackTxt("sid"); break;
    case A_SUBLOAD: {
        wchar_t f[MAX_PATH * 2];
        if (pickFile(f, L"Subtitles\0*.srt;*.ass;*.ssa;*.vtt;*.sub\0All files\0*.*\0")) { std::string u = U8(f); cmd("sub-add", u.c_str(), "select"); }
        break;
    }
    case A_SUBOFF: setp("sid", "no"); g_subTxt = "Off"; break;
    case A_AUDCYCLE: cmd("cycle", "audio"); g_audTxt = trackTxt("aid"); break;
    case A_STEREO: o_stereo = !o_stereo; setp("audio-channels", o_stereo ? "stereo" : "auto-safe"); saveSettings(); break;
    }
}

// ======================= menus (compact, categorised) =======================
enum { M_OPEN = 1, M_EXPLORER, M_DELETE, M_PREV, M_NEXT, M_PLAY, M_BACK, M_FWD, M_LOOP, M_ZRESET, M_ROTATE, M_FULL, M_TOP, M_SHOT,
       M_INFO, M_QUALITY, M_SETTINGS, M_EXIT, M_SUBOFF, M_SUBLOAD, M_MUTE, M_P_VOL, M_P_BRI, M_P_SPEED, M_P_SUB, M_P_AUD };
static const char* const ASPECTS[] = {"-1", "16:9", "4:3", "21:9", "1:1"};
static const wchar_t* const ASPECT_L[] = {L"Auto", L"16:9", L"4:3", L"21:9", L"1:1"};

static void addStr(HMENU m, UINT id, const wchar_t* t, bool chk = false) {
    AppendMenuW(m, MF_STRING | (chk ? MF_CHECKED : 0), id, t);
}
static HMENU subMenu(HMENU parent, const wchar_t* title) {
    HMENU s = CreatePopupMenu();
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)s, title);
    return s;
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
static HMENU menuContext() {
    HMENU m = CreatePopupMenu();
    addStr(m, M_OPEN, L"Open file...");
    if (!g_isVid) { addStr(m, M_PREV, L"Previous\tLeft"); addStr(m, M_NEXT, L"Next\tRight"); }
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
    HMENU f = subMenu(m, L"File");
    addStr(f, M_EXPLORER, L"Show in Explorer");
    addStr(f, M_INFO, L"File info\tI");
    if (g_isVid) addStr(f, M_SHOT, L"Screenshot\tP");
    addStr(f, M_DELETE, L"Delete (Recycle Bin)\tDel");
    if (g_isVid) {
        HMENU p = subMenu(m, L"Playback");
        addStr(p, M_PLAY, c_pause ? L"Play\tSpace" : L"Pause\tSpace");
        addStr(p, M_BACK, L"Back 10 s"); addStr(p, M_FWD, L"Forward 10 s");
        addStr(p, M_PREV, L"Previous video"); addStr(p, M_NEXT, L"Next video");
        addStr(p, M_LOOP, L"Loop", s_loop);
        addStr(p, M_P_SPEED, L"Speed...");
        HMENU a = subMenu(m, L"Audio");
        addTracks(a, "audio", 1000);
        AppendMenuW(a, MF_SEPARATOR, 0, 0);
        addStr(a, M_MUTE, L"Mute", s_mute);
        addStr(a, M_P_VOL, L"Volume...");
        addStr(a, M_P_AUD, L"Delay and stereo...");
        HMENU s = subMenu(m, L"Subtitles");
        addStr(s, M_SUBOFF, L"Off");
        addTracks(s, "sub", 2000);
        AppendMenuW(s, MF_SEPARATOR, 0, 0);
        addStr(s, M_SUBLOAD, L"Load file...");
        addStr(s, M_P_SUB, L"Size and delay...");
    }
    HMENU v = subMenu(m, L"View");
    addStr(v, M_P_BRI, L"Brightness and colour...");
    addStr(v, M_ZRESET, L"Reset zoom\t0");
    addStr(v, M_ROTATE, L"Rotate\tR");
    if (g_isVid) {
        HMENU ar = subMenu(v, L"Aspect ratio");
        for (int i = 0; i < 5; i++) addStr(ar, 100 + i, ASPECT_L[i]);
    } else addStr(v, M_QUALITY, L"Cycle quality");
    HMENU w = subMenu(m, L"Window");
    addStr(w, M_FULL, L"Fullscreen\tF", c_full);
    addStr(w, M_TOP, L"Always on top", o_ontop != 0);
    AppendMenuW(m, MF_SEPARATOR, 0, 0);
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
static int clientW() { RECT r = {}; if (g_vw) GetClientRect(g_vw, &r); return r.right; }

static void doDelete() {
    if (idx < 0 || idx >= (int)files.size()) return;
    std::wstring path = files[idx];
    EnterCriticalSection(&g_fl);
    files.erase(files.begin() + idx);
    InterlockedIncrement(&g_flGen);
    LeaveCriticalSection(&g_fl);
    bool empty = files.empty();
    g_lastLoad = 0;                                    // load the next file immediately so mpv releases the old one
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
static void showInfo() {
    std::string nm = U8(PathFindFileNameW(files[idx].c_str()));
    std::string w = mgets("video-params/w"), hh = mgets("video-params/h"), fm = mgets("file-format");
    double mb = atof(mgets("file-size").c_str()) / 1048576.0;
    char b[600];
    snprintf(b, sizeof b, "%s\n%sx%s    %.1f MB    %s", nm.c_str(), w.c_str(), hh.c_str(), mb, fm.c_str());
    cmd("show-text", b, "3500");
}

// ======================= actions =======================
static void act(const std::string& a) {
    if (a == "close") cmd("quit");
    else if (a == "next") go(1);
    else if (a == "prev") go(-1);
    else if (a == "first") openIdx(0);
    else if (a == "last") openIdx((int)files.size() - 1);
    else if (a == "pause") cmd("cycle", "pause");
    else if (a == "volup") { setVol(s_vol + 5); osdf("Volume %d%%", (int)floor(s_vol + 0.5)); }
    else if (a == "voldown") { setVol(s_vol - 5); osdf("Volume %d%%", (int)floor(s_vol + 0.5)); }
    else if (a == "mute") { s_mute = !s_mute; setp("mute", s_mute ? "yes" : "no"); osd(s_mute ? "Muted" : "Sound on"); }
    else if (a == "fullscreen") cmd("cycle", "fullscreen");
    else if (a == "seekfwd") cmd("seek", "5");
    else if (a == "seekback") cmd("seek", "-5");
    else if (a == "seekfwd10") cmd("seek", "10");
    else if (a == "seekback10") cmd("seek", "-10");
    else if (a == "framenext") cmd("frame-step");
    else if (a == "frameback") cmd("frame-back-step");
    else if (a == "zoomin") { s_zoom = std::min(6.0, s_zoom + 0.1); viewChanged(); osdLater("Zoom %d%%", (int)(100 * pow(2.0, s_zoom))); }
    else if (a == "zoomout") { s_zoom = std::max(-1.0, s_zoom - 0.1); if (s_zoom <= 0.001) s_px = s_py = 0; viewChanged(); osdLater("Zoom %d%%", (int)(100 * pow(2.0, s_zoom))); }
    else if (a == "zoomreset") { s_zoom = s_px = s_py = 0; viewChanged(); osdLater("Zoom 100%%"); }
    else if (a == "rotate") { s_rot = (s_rot + 90) % 360; setd("video-rotate", s_rot); }
    else if (a == "brightup") { setBright(s_bright + 3); osdf("Brightness %d", (int)s_bright); }
    else if (a == "brightdown") { setBright(s_bright - 3); osdf("Brightness %d", (int)s_bright); }
    else if (a == "speedup") { setSpeed(s_speed * 1.1); osdf("Speed %.2fx", s_speed); }
    else if (a == "speeddown") { setSpeed(s_speed / 1.1); osdf("Speed %.2fx", s_speed); }
    else if (a == "speedreset") { setSpeed(1); osd("Speed 1.00x"); }
    else if (a == "subcycle") { cmd("cycle", "sub"); osdf("Subtitles: %s", trackTxt("sid").c_str()); }
    else if (a == "audiocycle") { cmd("cycle", "audio"); osdf("Audio: %s", trackTxt("aid").c_str()); }
    else if (a == "subdelayminus") { setSubDelay(s_subdelay - 0.1); osdf("Subtitle delay %+.1f s", s_subdelay); }
    else if (a == "subdelayplus") { setSubDelay(s_subdelay + 0.1); osdf("Subtitle delay %+.1f s", s_subdelay); }
    else if (a == "audiodelayminus") { setAudDelay(s_audiodelay - 0.1); osdf("Audio delay %+.1f s", s_audiodelay); }
    else if (a == "audiodelayplus") { setAudDelay(s_audiodelay + 0.1); osdf("Audio delay %+.1f s", s_audiodelay); }
    else if (a == "loop") { s_loop = !s_loop; setp("loop-file", s_loop ? "inf" : "no"); osd(s_loop ? "Loop: on" : "Loop: off"); }
    else if (a == "info") showInfo();
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
    else if (a == "quality") {
        int lv = o_ultra ? 2 : (o_hq ? 1 : 0);
        lv = (lv + 1) % 3;
        o_ultra = (lv == 2); o_hq = (lv >= 1);
        applyQuality();
        osd(lv == 2 ? "Quality: Ultra" : (lv == 1 ? "Quality: High" : "Quality: Fast"));
        saveSettings();
    }
    else if (a == "settings") openSettings();
    else if (a == "menu") {
        POINT p; GetCursorPos(&p);
        int id = popup(menuContext(), p.x, p.y);
        if (id) menuCmd(id);
    }
}
static void menuCmd(int id) {
    char b[64];
    if (id >= 2000 && id < 3000) { snprintf(b, 64, "%d", id - 2000); setp("sid", b); return; }
    if (id >= 1000 && id < 2000) { snprintf(b, 64, "%d", id - 1000); setp("aid", b); return; }
    if (id >= 100 && id < 200) { setp("video-aspect-override", ASPECTS[id - 100]); return; }
    switch (id) {
    case M_OPEN: {
        wchar_t f[MAX_PATH * 2];
        if (pickFile(f, mediaFilter().c_str())) setFiles(f);
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
    case M_SUBLOAD: rowAction(A_SUBLOAD); break;
    case M_MUTE: act("mute"); break;
    case M_P_VOL: openPanel(P_VOL, clientW() / 2); break;
    case M_P_BRI: openPanel(P_BRI, clientW() / 2); break;
    case M_P_SPEED: openPanel(P_SPEED, clientW() / 2); break;
    case M_P_SUB: openPanel(P_SUB, clientW() / 2); break;
    case M_P_AUD: openPanel(P_AUD, clientW() / 2); break;
    }
}

// ======================= modern player UI (vector shapes in an ASS overlay) =======================
static std::string A, A2, *g_cur = &A;
static bool g_emit;
static void ap(const char* f, ...) {
    if (!g_emit) return;
    char b[3500];
    va_list v; va_start(v, f); vsnprintf(b, sizeof b, f, v); va_end(v);
    *g_cur += b;
}
static void shape(int x, int y, const std::string& path, const char* col, const char* al, const char* extra = "") {
    ap("{\\an7\\pos(%d,%d)\\bord0\\shad0\\blur0.6\\1c&H%s&\\1a&H%s&%s\\p1}%s{\\p0}\n", x, y, col, al, extra, path.c_str());
}
static std::string rrPath(int w, int h, int r) {
    r = std::max(1, std::min(r, std::min(w, h) / 2));
    double k = 0.5523 * r;
    char b[640];
    snprintf(b, sizeof b, "m %d 0 l %d 0 b %.1f 0 %d %.1f %d %d l %d %d b %d %.1f %.1f %d %d %d l %d %d b %.1f %d 0 %.1f 0 %d l 0 %d b 0 %.1f %.1f 0 %d 0",
             r, w - r, w - r + k, w, r - k, w, r, w, h - r, w, h - r + k, w - r + k, h, w - r, h, r, h, r - k, h, h - r + k, h - r, r, r - k, r - k, r);
    return b;
}
static std::string circlePath(int d) { return rrPath(d, d, d / 2); }
enum { I_PLAY = 1, I_PAUSE, I_PREV, I_NEXT, I_SPK, I_SUN, I_ROT };
static std::string iconPath(int k, int s) {
    char b[700];
    switch (k) {
    case I_PLAY: snprintf(b, sizeof b, "m %d 0 l %d %d l %d %d", s * 15 / 100, s, s / 2, s * 15 / 100, s); return b;
    case I_PAUSE: snprintf(b, sizeof b, "m %d 0 l %d 0 l %d %d l %d %d m %d 0 l %d 0 l %d %d l %d %d",
                           s * 15 / 100, s * 40 / 100, s * 40 / 100, s, s * 15 / 100, s, s * 60 / 100, s * 85 / 100, s * 85 / 100, s, s * 60 / 100, s); return b;
    case I_PREV: snprintf(b, sizeof b, "m %d 0 l %d 0 l %d %d l %d %d m %d %d l %d 0 l %d %d",
                          s * 8 / 100, s * 22 / 100, s * 22 / 100, s, s * 8 / 100, s, s * 32 / 100, s / 2, s, s, s); return b;
    case I_NEXT: snprintf(b, sizeof b, "m 0 0 l %d %d l 0 %d m %d 0 l %d 0 l %d %d l %d %d",
                          s * 68 / 100, s / 2, s, s * 78 / 100, s * 92 / 100, s * 92 / 100, s, s * 78 / 100, s); return b;
    case I_SPK: snprintf(b, sizeof b, "m 0 %d l %d %d l %d %d l %d %d l %d %d l 0 %d m %d %d l %d %d l %d %d l %d %d m %d %d l %d %d l %d %d l %d %d",
                         s * 36 / 100, s * 22 / 100, s * 36 / 100, s * 50 / 100, s * 10 / 100, s * 50 / 100, s * 90 / 100, s * 22 / 100, s * 64 / 100, s * 64 / 100,
                         s * 62 / 100, s * 38 / 100, s * 68 / 100, s * 38 / 100, s * 68 / 100, s * 62 / 100, s * 62 / 100, s * 62 / 100,
                         s * 78 / 100, s * 24 / 100, s * 84 / 100, s * 24 / 100, s * 84 / 100, s * 76 / 100, s * 78 / 100, s * 76 / 100); return b;
    case I_SUN: {
        std::string p;
        for (int i = 0; i < 16; i++) {                                           // disc
            double a = i * 3.14159265 / 8;
            snprintf(b, sizeof b, "%s %d %d ", i ? "l" : "m", (int)(s * 0.5 + s * 0.2 * cos(a)), (int)(s * 0.5 + s * 0.2 * sin(a)));
            p += b;
        }
        for (int i = 0; i < 8; i++) {                                            // rays
            double a = i * 3.14159265 / 4, c = cos(a), sn = sin(a), r1 = s * 0.31, r2 = s * 0.47, hw = s * 0.045;
            double cx = s * 0.5, cy = s * 0.5;
            snprintf(b, sizeof b, "m %d %d l %d %d l %d %d l %d %d ",
                     (int)(cx + c * r1 - sn * hw), (int)(cy + sn * r1 + c * hw), (int)(cx + c * r2 - sn * hw), (int)(cy + sn * r2 + c * hw),
                     (int)(cx + c * r2 + sn * hw), (int)(cy + sn * r2 - c * hw), (int)(cx + c * r1 + sn * hw), (int)(cy + sn * r1 - c * hw));
            p += b;
        }
        return p;
    }
    }
    return "";
}
static int txtw(const std::string& t, int fs) { return (int)(t.size() * fs * 0.56); }

static const char* const ACC = "A6B814";         // accent (BGR)  teal
static Hit* hitAt(int x, int y) {
    for (int i = g_nh - 1; i >= 0; i--) {
        Hit& k = g_hits[i];
        if (x >= k.x && x < k.x + k.w && y >= k.y && y < k.y + k.h) return &k;
    }
    return 0;
}
static Hit* findBtn(int id) {
    for (int i = 0; i < g_nh; i++) if (g_hits[i].kind == H_BTN && g_hits[i].id == id) return &g_hits[i];
    return 0;
}
static void fmtTime(double s, char* o) {
    int t = s > 0 ? (int)s : 0;
    if (t >= 3600) sprintf(o, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else sprintf(o, "%d:%02d", t / 60, t % 60);
}

// Builds hit regions and (if emit) the ASS text for the main overlay (A) and the resume popup (A2).
static void uiBuild(int W, int H, bool emit) {
    g_nh = 0; g_emit = emit; A.clear(); A2.clear(); g_cur = &A;
    const double u = std::clamp(H / 720.0, 0.8, 2.4);
#define U(v) ((int)((v) * u + 0.5))
    auto addHit = [&](int kind, int id, int x, int y, int w, int h, int tx = 0, int tw = 0) { if (g_nh < 96) g_hits[g_nh++] = {kind, id, x, y, w, h, tx, tw}; };
    auto pill = [&](int x, int y, int w, int hh, const char* col, const char* al) { if (g_emit) shape(x, y, rrPath(w, hh, hh / 2), col, al); };
    auto label = [&](int x, int y, int an, int f, const char* col, const char* t, bool bold) {
        ap("{\\an%d\\pos(%d,%d)\\fs%d\\b%d\\bord0\\shad0\\1c&H%s&}%s\n", an, x, y, f, bold ? 1 : 0, col, t); };
    auto icon = [&](int k, int cx, int cy, int s, const char* col, const char* al = "00") { if (g_emit) shape(cx - s / 2, cy - s / 2, iconPath(k, s), col, al); };
    auto ring = [&](int cx, int cy, int d) { if (g_emit) shape(cx - d / 2, cy - d / 2, circlePath(d), "FFFFFF", "FF", "\\bord2\\3c&HFFFFFF&"); };

    const bool vis = controlsVisible();
    int barTop = H - U(24);
    int si = g_shown;
    if (vis && si >= 0 && si < (int)files.size()) {                      // file name chip (top-left)
        std::string nm = U8(PathFindFileNameW(files[si].c_str()));
        for (auto& c : nm) if (c == '{' || c == '}') c = '('; else if (c == '\\') c = '/';
        if (nm.size() > 56) nm = nm.substr(0, 53) + "...";
        char cnt[32]; snprintf(cnt, 32, "%d / %d", si + 1, (int)files.size());
        int f = U(14), w = txtw(nm, f) + txtw(cnt, f) + U(50), hh = U(34), x = U(20), y = U(18);
        pill(x, y, w, hh, "141414", "40");
        label(x + U(16), y + hh / 2, 4, f, "FFFFFF", nm.c_str(), false);
        label(x + w - U(16), y + hh / 2, 6, f, "B8B8B8", cnt, false);
    }
    if (vis && g_isVid) {                                                // ---------- video control bar ----------
        int bw = std::min(W - U(32), U(880)), bh = U(112), bx = (W - bw) / 2, by = H - U(22) - bh;
        barTop = by;
        if (g_emit) shape(bx, by, rrPath(bw, bh, U(26)), "141414", "38");
        addHit(H_PANEL, 0, bx, by, bw, bh);
        double dur = c_dur, pos = (g_dragKind == 1) ? g_scrub : c_pos;
        double fr = dur > 0 ? std::clamp(pos / dur, 0.0, 1.0) : 0;
        int sy = by + U(28);
        char t1[16], t2[16]; fmtTime(pos, t1); fmtTime(dur, t2);
        label(bx + U(24), sy, 4, U(13), "E0E0E0", t1, false);
        label(bx + bw - U(24), sy, 6, U(13), "E0E0E0", t2, false);
        int tx0 = bx + U(86), tx1 = bx + bw - U(86), tw = tx1 - tx0, th = U(6);
        pill(tx0, sy - th / 2, tw, th, "FFFFFF", "A8");
        int fw = (int)(tw * fr);
        if (fw > 0) pill(tx0, sy - th / 2, std::max(fw, th), th, ACC, "00");
        int kd = (g_dragKind == 1) ? U(19) : U(15);
        if (g_emit) shape(tx0 + fw - kd / 2, sy - kd / 2, circlePath(kd), "FFFFFF", "00");
        addHit(H_SEEK, 0, tx0 - U(10), sy - U(16), tw + U(20), U(32), tx0, tw);

        int cy = by + bh - U(35);
        int bP = U(40), bR = U(46), bPl = U(54), gap = U(10);
        int total = bP * 2 + bR * 2 + bPl + gap * 4, x = W / 2 - total / 2;
        addHit(H_BTN, B_PREV, x, cy - bP / 2, bP, bP); icon(I_PREV, x + bP / 2, cy, U(17), "FFFFFF"); x += bP + gap;
        addHit(H_BTN, B_BACK, x, cy - bR / 2, bR, bR); ring(x + bR / 2, cy, U(34)); label(x + bR / 2, cy, 5, U(12), "FFFFFF", "-10", true); x += bR + gap;
        addHit(H_BTN, B_PLAY, x, cy - bPl / 2, bPl, bPl);
        if (g_emit) shape(x, cy - bPl / 2, circlePath(bPl), "FFFFFF", "00");
        icon(c_pause ? I_PLAY : I_PAUSE, x + bPl / 2 + (c_pause ? U(2) : 0), cy, U(20), "141414"); x += bPl + gap;
        addHit(H_BTN, B_FWD, x, cy - bR / 2, bR, bR); ring(x + bR / 2, cy, U(34)); label(x + bR / 2, cy, 5, U(12), "FFFFFF", "+10", true); x += bR + gap;
        addHit(H_BTN, B_NEXT, x, cy - bP / 2, bP, bP); icon(I_NEXT, x + bP / 2, cy, U(17), "FFFFFF");

        int lx = bx + U(22);                                             // left: volume + brightness
        addHit(H_BTN, B_VOL, lx, cy - U(20), U(40), U(40)); icon(I_SPK, lx + U(20), cy, U(20), s_mute ? "808080" : "FFFFFF");
        addHit(H_BTN, B_BRI, lx + U(46), cy - U(20), U(40), U(40)); icon(I_SUN, lx + U(66), cy, U(21), "FFFFFF");

        int rx = bx + bw - U(22), ph = U(32);                            // right: speed / subtitles / audio / more
        auto rp = [&](int id, const char* txt, int w, bool active) {
            rx -= w;
            addHit(H_BTN, id, rx, cy - ph / 2, w, ph);
            pill(rx, cy - ph / 2, w, ph, "FFFFFF", active ? "98" : "D8");
            label(rx + w / 2, cy, 5, U(13), "FFFFFF", txt, true);
            rx -= U(8);
        };
        rp(B_MORE, "...", U(44), false);
        rp(B_AUD, "Audio", U(64), g_panel == P_AUD);
        rp(B_SUB, "CC", U(46), g_panel == P_SUB);
        char sp[16]; snprintf(sp, 16, "%gx", s_speed);
        rp(B_SPEED, sp, U(58), g_panel == P_SPEED);
    } else if (vis) {                                                    // ---------- photo bar ----------
        static const int ids[8] = {B_PREV, B_NEXT, B_ZOUT, B_ZPILL, B_ZIN, B_ROT, B_BRI, B_MORE};
        int wd[8] = {U(44), U(44), U(40), U(74), U(40), U(72), U(44), U(44)}, gap = U(8), tot = U(36);
        for (int i = 0; i < 8; i++) tot += wd[i] + (i ? gap : 0);
        int bh = U(62), bx = (W - tot) / 2, by = H - U(22) - bh, cy = by + bh / 2, x = bx + U(18);
        barTop = by;
        if (g_emit) shape(bx, by, rrPath(tot, bh, U(28)), "141414", "38");
        addHit(H_PANEL, 0, bx, by, tot, bh);
        for (int i = 0; i < 8; i++) {
            int id = ids[i], w = wd[i], hh = U(36);
            addHit(H_BTN, id, x, cy - hh / 2, w, hh);
            if (id == B_PREV) icon(I_PREV, x + w / 2, cy, U(16), "FFFFFF");
            else if (id == B_NEXT) icon(I_NEXT, x + w / 2, cy, U(16), "FFFFFF");
            else if (id == B_BRI) icon(I_SUN, x + w / 2, cy, U(21), "FFFFFF");
            else {
                pill(x, cy - hh / 2, w, hh, "FFFFFF", (id == B_BRI && g_panel == P_BRI) ? "98" : "D8");
                char t[24];
                if (id == B_ZOUT) snprintf(t, 24, "-");
                else if (id == B_ZIN) snprintf(t, 24, "+");
                else if (id == B_ROT) snprintf(t, 24, "Rotate");
                else if (id == B_MORE) snprintf(t, 24, "...");
                else if (s_zoom <= 0.001) snprintf(t, 24, "Fit");
                else snprintf(t, 24, "%d%%", (int)(100 * pow(2.0, s_zoom)));
                label(x + w / 2, cy, 5, (id == B_ZOUT || id == B_ZIN) ? U(18) : U(13), "FFFFFF", t, true);
            }
            x += w + gap;
        }
    }
    if (g_panel && vis) {                                                // ---------- slider panel ----------
        Row rows[8];
        int nr = panelRows(g_panel, rows);
        int pw = U(340), ph = U(20);
        for (int i = 0; i < nr; i++) ph += rows[i].type == 0 ? U(56) : U(46);
        int px = std::clamp(g_panelAnchor - pw / 2, U(12), std::max(U(12), W - pw - U(12))), py = barTop - U(12) - ph, yy = py + U(12);
        if (g_emit) shape(px, py, rrPath(pw, ph, U(22)), "141414", "30");
        addHit(H_PANEL, 1, px, py, pw, ph);
        for (int i = 0; i < nr; i++) {
            Row& r = rows[i];
            if (r.type == 0) {
                char vb[32]; fmtVal(r.fmt, r.v, vb);
                label(px + U(22), yy + U(14), 4, U(13), "D0D0D0", r.label, false);
                label(px + pw - U(22), yy + U(14), 6, U(13), "FFFFFF", vb, true);
                addHit(H_RESET, r.id, px + pw - U(120), yy, U(110), U(28));
                int tx = px + U(22), tw = pw - U(44), ty = yy + U(40), th = U(6);
                double f = std::clamp((r.v - r.mn) / (r.mx - r.mn), 0.0, 1.0);
                int fw = (int)(tw * f), kd = (g_dragKind == 2 && g_dragId == r.id) ? U(20) : U(16);
                pill(tx, ty - th / 2, tw, th, "FFFFFF", "A8");
                if (fw > 0) pill(tx, ty - th / 2, std::max(fw, th), th, ACC, "00");
                if (g_emit) shape(tx + fw - kd / 2, ty - kd / 2, circlePath(kd), "FFFFFF", "00");
                addHit(H_SLIDER, r.id, tx - U(8), ty - U(16), tw + U(16), U(32), tx, tw);
                yy += U(56);
            } else {
                pill(px + U(16), yy + U(4), pw - U(32), U(36), "FFFFFF", "D8");
                label(px + pw / 2, yy + U(22), 5, U(13), "FFFFFF", r.label, true);
                addHit(H_ROW, r.id, px + U(16), yy + U(4), pw - U(32), U(36));
                yy += U(46);
            }
        }
    }
    if (toastActive() && g_isVid) {                                      // ---------- resume popup (bottom centre) ----------
        g_cur = &A2;
        int tw = U(390), th = U(46), tx = (W - tw) / 2, ty = barTop - U(14) - th;
        if (!vis) ty = H - U(26) - th;
        char t1[16], msg[64]; fmtTime(g_toastPos, t1);
        snprintf(msg, sizeof msg, "Resumed at %s", t1);
        if (g_emit) shape(tx, ty, rrPath(tw, th, th / 2), "141414", "30");
        label(tx + U(22), ty + th / 2, 4, U(14), "FFFFFF", msg, false);
        int bh = U(30), by2 = ty + (th - bh) / 2, wOk = U(48), wSt = U(100), bx2 = tx + tw - U(10) - wOk;
        pill(bx2, by2, wOk, bh, "FFFFFF", "D8");
        label(bx2 + wOk / 2, by2 + bh / 2, 5, U(13), "FFFFFF", "OK", true);
        addHit(H_BTN, B_TOK, bx2, by2, wOk, bh);
        bx2 -= U(8) + wSt;
        pill(bx2, by2, wSt, bh, ACC, "00");
        label(bx2 + wSt / 2, by2 + bh / 2, 5, U(13), "141414", "Start over", true);
        addHit(H_BTN, B_TSTART, bx2, by2, wSt, bh);
        g_cur = &A;
    }
#undef U
}

static void clearOverlays() {
    const char* a1[] = {"osd-overlay", "1", "none", "", 0};
    const char* a2[] = {"osd-overlay", "2", "none", "", 0};
    p_mpv_command_async(h, 0, a1);
    p_mpv_command_async(h, 0, a2);
}
static void overlay() {
    if (!g_vw) return;
    static std::string last1, last2;
    bool vis = controlsVisible(), toast = toastActive();
    if (!vis && !toast) {
        if (g_ov) { clearOverlays(); g_ov = false; last1.clear(); last2.clear(); }
        return;
    }
    RECT r; GetClientRect(g_vw, &r);
    int W = r.right, H = r.bottom;
    if (W < 200 || H < 120 || g_shown < 0 || g_shown >= (int)files.size()) return;
    uiBuild(W, H, true);
    while (!A.empty() && A.back() == '\n') A.pop_back();
    while (!A2.empty() && A2.back() == '\n') A2.pop_back();
    char ws[16], hs[16]; sprintf(ws, "%d", W); sprintf(hs, "%d", H);
    std::string k = std::string(ws) + "x" + hs;
    if (A.empty()) { if (!last1.empty()) { const char* a[] = {"osd-overlay", "1", "none", "", 0}; p_mpv_command_async(h, 0, a); last1.clear(); } }
    else if (A + k != last1) {
        last1 = A + k;
        const char* a[] = {"osd-overlay", "1", "ass-events", A.c_str(), ws, hs, 0};
        p_mpv_command_async(h, 0, a);
    }
    if (A2.empty()) { if (!last2.empty()) { const char* a[] = {"osd-overlay", "2", "none", "", 0}; p_mpv_command_async(h, 0, a); last2.clear(); } }
    else if (A2 + k != last2) {
        last2 = A2 + k;
        const char* a[] = {"osd-overlay", "2", "ass-events", A2.c_str(), ws, hs, 0};
        p_mpv_command_async(h, 0, a);
    }
    g_ov = true;
}

// ======================= input: gestures, buttons, sliders =======================
static bool ld, rd, md, skipClick, winDrag;
static int dx0, dy0, rx, ry, rmode, mx, my, lx, ly;
static DWORD lastDown, lastH, navCool, tpUntil;
static double sB, sV, sPos, sZ, hAcc, sAcc;

// zoom keeping the point under (x,y) roughly fixed; only changes video-zoom/video-pan (GPU), never the window size
static double zoomAt(double nz, int x, int y, int W, int H) {
    nz = std::clamp(nz, -1.0, 6.0);
    if (nz <= 0.001) { s_zoom = nz; s_px = s_py = 0; viewChanged(); return nz; }
    double s = pow(2.0, nz - s_zoom), zf = pow(2.0, s_zoom);
    s_px = std::clamp(s_px + (x - W / 2.0) / (W * zf) * (1.0 / s - 1.0), -3.0, 3.0);
    s_py = std::clamp(s_py + (y - H / 2.0) / (H * zf) * (1.0 / s - 1.0), -3.0, 3.0);
    s_zoom = nz;
    viewChanged();
    return nz;
}
static void dragMove(int x) {
    if (g_dragW <= 0) return;
    double f = std::clamp((x - g_dragX0) / (double)g_dragW, 0.0, 1.0);
    if (g_dragKind == 1) {
        if (c_dur > 0) { g_scrub = f * c_dur; g_seekTarget = g_scrub; g_seekDirty = true; kick(); }
    } else if (g_dragKind == 2) {
        Row rows[8];
        int n = panelRows(g_panel, rows);
        for (int i = 0; i < n; i++) if (rows[i].type == 0 && rows[i].id == g_dragId) setSliderId(g_dragId, rows[i].mn + f * (rows[i].mx - rows[i].mn));
    }
}
static void endDrag() {
    if (g_dragKind == 1 && c_dur > 0) { char b[48]; snprintf(b, 48, "%.2f", g_scrub); cmd("seek", b, "absolute"); g_seekDirty = false; }
    if (g_dragKind == 2) saveSettings();
    g_dragKind = 0;
}
static void togglePanel(int p, int id) {
    Hit* b = findBtn(id);
    int cx = b ? b->x + b->w / 2 : clientW() / 2;
    if (g_panel == p) closePanel(); else openPanel(p, cx);
}
static void btnClick(int id) {
    Hit* b = findBtn(id);
    int W = clientW();
    switch (id) {
    case B_PREV: go(-1); break;
    case B_NEXT: go(1); break;
    case B_BACK: cmd("seek", "-10"); break;
    case B_FWD: cmd("seek", "10"); break;
    case B_PLAY: cmd("cycle", "pause"); break;
    case B_VOL: togglePanel(P_VOL, id); break;
    case B_BRI: togglePanel(P_BRI, id); break;
    case B_SPEED: togglePanel(P_SPEED, id); break;
    case B_SUB: togglePanel(P_SUB, id); break;
    case B_AUD: togglePanel(P_AUD, id); break;
    case B_MORE: {
        POINT p = {b ? b->x : W / 2, b ? b->y : 0};
        ClientToScreen(g_vw, &p);
        int r = popup(menuContext(), p.x, p.y, TPM_BOTTOMALIGN);
        if (r) menuCmd(r);
        break;
    }
    case B_ZOUT: { double z = std::max(-1.0, s_zoom - 0.25); s_zoom = z; if (z <= 0.001) s_px = s_py = 0; viewChanged(); break; }
    case B_ZIN: s_zoom = std::min(6.0, s_zoom + 0.25); viewChanged(); break;
    case B_ZPILL: s_zoom = s_px = s_py = 0; viewChanged(); break;
    case B_ROT: act("rotate"); break;
    case B_TSTART: cmd("seek", "0", "absolute"); g_toastUntil = 0; break;
    case B_TOK: g_toastUntil = 0; break;
    }
}
static void uiClick(int kind, int id) {
    if (kind == H_BTN) btnClick(id);
    else if (kind == H_ROW) rowAction(id);
    else if (kind == H_RESET) {
        Row rows[8];
        int n = panelRows(g_panel, rows);
        for (int i = 0; i < n; i++) if (rows[i].type == 0 && rows[i].id == id) setSliderId(id, rows[i].def);
        saveSettings();
    }
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
    double zf = pow(2.0, s_zoom);
    char b[96];

    if (t == WM_MOUSEHWHEEL) {                   // two-finger horizontal swipe
        if (s_zoom > 0.01) { s_px = std::clamp(s_px - d * 100.0 / (W * zf), -3.0, 3.0); viewChanged(); return; }
        double dh = d;
        if (g_isVid) dh = -dh;                   // video seek: swipe direction matches the timeline
        if (o_inv) dh = -dh;
        if (!g_isVid) {
            if (now - lastH > 250) hAcc = 0;
            lastH = now;
            if (now < navCool) { navCool = now + 250; return; }
            hAcc += dh * 120;
            if (fabs(hAcc) > std::max(60, o_swipe)) { go(hAcc > 0 ? 1 : -1); hAcc = 0; navCool = now + 450; }
        } else {
            sAcc += dh * 5;
            if (fabs(sAcc) >= 1) { snprintf(b, 96, "%.2f", sAcc); cmd("seek", b, "relative+keyframes"); sAcc = 0; }
        }
        return;
    }
    if (ctrl) {                                  // pinch (or Ctrl+wheel)
        double nz = zoomAt(s_zoom + 0.2 * d, pt.x, pt.y, W, H);
        osdLater("Zoom %d%%", (int)(100 * pow(2.0, nz)));
        return;
    }
    if (!g_isVid) {                              // photo
        if (!tp) {
            double nz = zoomAt(s_zoom + 0.15 * d, pt.x, pt.y, W, H);
            osdLater("Zoom %d%%", (int)(100 * pow(2.0, nz)));
        } else if (s_zoom > 0.01) { s_py = std::clamp(s_py + d * 100.0 / (H * zf), -3.0, 3.0); viewChanged(); }
        return;
    }
    if (shift && !tp) { snprintf(b, 96, "%d", (int)(d * 5)); cmd("seek", b); return; }
    // video: right half = volume, left half = brightness (touchpad: swipe up = increase)
    double dv = d;
    if (tp) { dv = -d; if (o_invv) dv = -dv; }
    if (pt.x > W / 2) { setVol(s_vol + dv * (tp ? 3 : 5)); osdLater("Volume %d%%", (int)floor(s_vol + 0.5)); }
    else { setBright(s_bright + dv * (tp ? 3 : 4)); osdLater("Brightness %d", (int)s_bright); }
    kick();
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
        if (vk == VK_ESCAPE && g_panel) { closePanel(); break; }
        std::string* km = KMS[g_isVid ? 1 : 0];
        if (vk < 256 && !km[vk].empty()) act(km[vk]);
        showUI();
        break;
    }
    case WM_MOUSEMOVE:
        showUI();
        if (g_dragKind) { if (!(g_mmWp & MK_LBUTTON)) endDrag(); else dragMove(x); break; }
        if (md) {
            double zf = pow(2.0, s_zoom);
            s_px = std::clamp(s_px + (x - mx) / (W * zf), -3.0, 3.0);
            s_py = std::clamp(s_py + (y - my) / (H * zf), -3.0, 3.0);
            mx = x; my = y;
            viewChanged();
        }
        if (ld && !skipClick && !winDrag && (g_mmWp & MK_LBUTTON)) {
            if (s_zoom > 0.01) {                                   // zoomed in: drag pans
                double zf = pow(2.0, s_zoom);
                s_px = std::clamp(s_px + (x - lx) / (W * zf), -3.0, 3.0);
                s_py = std::clamp(s_py + (y - ly) / (H * zf), -3.0, 3.0);
                lx = x; ly = y;
                viewChanged();
            } else if (!c_full && (abs(x - dx0) > 6 || abs(y - dy0) > 6)) {   // otherwise drag moves the window
                winDrag = true;
                PostMessageW(g_vw, WM_SYSCOMMAND, SC_MOVE | 2, 0);
            }
        }
        if (rd) {
            int dx = x - rx, dy = y - ry;
            if (!rmode) {
                if (abs(dx) < 12 && abs(dy) < 12) break;
                rmode = abs(dx) > abs(dy) ? 3 : (rx < W / 2 ? 1 : 2);
                if (rmode == 3 && !g_isVid) rmode = -1;
            }
            if (rmode == 1) {
                setBright(sB - dy * 200.0 / H);
                osdLater("Brightness %d", (int)s_bright); kick();
            } else if (rmode == 2) {
                if (g_isVid) { setVol(sV - dy * 150.0 / H); osdLater("Volume %d%%", (int)floor(s_vol + 0.5)); kick(); }
                else { double z = zoomAt(sZ - dy * 3.0 / H, W / 2, H / 2, W, H); osdLater("Zoom %d%%", (int)(100 * pow(2.0, z))); }
            } else if (rmode == 3 && c_dur > 0) {
                g_seekTarget = std::clamp(sPos + dx * c_dur / W, 0.0, c_dur);
                g_seekDirty = true; kick();
            }
        }
        break;
    case WM_LBUTTONDOWN: {
        DWORD now = GetTickCount();
        skipClick = false; winDrag = false; ld = true; downBtn_reset:
        g_downKind = 0;
        dx0 = x; dy0 = y; lx = x; ly = y;
        uiBuild(W, H, false);
        Hit* hh = (controlsVisible() || toastActive()) ? hitAt(x, y) : 0;
        if (hh) {
            if (hh->kind == H_SLIDER) {
                g_dragKind = 2; g_dragId = hh->id; g_dragX0 = hh->tx; g_dragW = hh->tw; skipClick = true; dragMove(x);
                break;
            }
            if (hh->kind == H_SEEK) {
                g_dragKind = 1; g_dragX0 = hh->tx; g_dragW = hh->tw; g_scrub = c_pos; skipClick = true; dragMove(x);
                break;
            }
            if (hh->kind == H_BTN || hh->kind == H_ROW || hh->kind == H_RESET) { g_downKind = hh->kind; g_downId = hh->id; skipClick = true; lastDown = 0; break; }
            if (hh->kind == H_PANEL) { if (hh->id == 0 && g_panel) closePanel(); skipClick = true; break; }
        }
        if (g_panel) { closePanel(); skipClick = true; break; }
        if (lastDown && now - lastDown < GetDoubleClickTime() && abs(x - dx0) < 8 && abs(y - dy0) < 8) {
            if (g_isVid) cmd("cycle", "pause");   // undo the pause toggle from the first click
            act("fullscreen");
            lastDown = 0; skipClick = true;
        } else lastDown = now;
        break;
    }
    case WM_LBUTTONUP:
        if (g_dragKind) endDrag();
        else if (g_downKind) {
            uiBuild(W, H, false);
            Hit* hh = hitAt(x, y);
            if (hh && hh->kind == g_downKind && hh->id == g_downId) uiClick(hh->kind, hh->id);
        } else if (ld && !skipClick && !winDrag && abs(x - dx0) < 5 && abs(y - dy0) < 5 && g_isVid) cmd("cycle", "pause");
        ld = false; g_downKind = 0; winDrag = false; showUI();
        break;
    case WM_RBUTTONDOWN:
        rd = true; rx = x; ry = y; rmode = 0;
        sB = s_bright; sV = s_vol; sPos = c_pos; sZ = s_zoom;
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
    pump();                                       // apply the latest zoom/pan/OSD right away (WM_TIMER is starved during input)
}

static LRESULT CALLBACK HookProc(int c, WPARAM wp, LPARAM lp) {
    if (c == HC_ACTION && wp == PM_REMOVE) {
        MSG* m = (MSG*)lp;
        UINT t = m->message;
        if (t == WM_MOUSEMOVE) {                 // keep only the latest position; never build a backlog
            g_mmLp = m->lParam; g_mmWp = m->wParam;
            if (InterlockedExchange(&g_mmPending, 1) == 0) PostMessageW(g_mw, 0x8000 + WM_MOUSEMOVE, 0, 0);
        } else if (t == WM_KEYDOWN || t == WM_LBUTTONDOWN || t == WM_LBUTTONUP || t == WM_RBUTTONDOWN ||
                   t == WM_RBUTTONUP || t == WM_MBUTTONDOWN || t == WM_MBUTTONUP || t == WM_MOUSEWHEEL || t == WM_MOUSEHWHEEL) {
            if (t != WM_KEYDOWN && t != WM_MOUSEWHEEL && t != WM_MOUSEHWHEEL) { g_mmLp = m->lParam; g_mmWp = m->wParam; }
            PostMessageW(g_mw, 0x8000 + t, m->wParam, m->lParam);
        }
    }
    return CallNextHookEx(0, c, wp, lp);
}
static void hookIfNeeded() {
    if (g_vw) return;
    int64_t id = 0;
    if (p_mpv_get_property(h, "window-id", MPV_FORMAT_INT64, &id) >= 0 && id) {
        g_vw = (HWND)(intptr_t)id;
        HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
        HICON sm = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        if (big) PostMessageW(g_vw, WM_SETICON, ICON_BIG, (LPARAM)big);
        if (sm) PostMessageW(g_vw, WM_SETICON, ICON_SMALL, (LPARAM)sm);
        DWORD tid = GetWindowThreadProcessId(g_vw, 0);
        g_hook = SetWindowsHookExW(WH_GETMESSAGE, HookProc, 0, tid);
    }
}
static void wake(void*) { PostMessageW(g_mw, WM_USER + 1, 0, 0); }

// Applies the latest state, rate-limited. Called after every input message AND from a fallback timer
// (WM_TIMER is starved while input messages keep arriving).
static void pump() {
    double t = nowms();
    DWORD now = GetTickCount();
    if (g_pending >= 0 && now - g_lastLoad >= 90) doLoad();
    if (g_viewDirty && t - g_tView >= 10) { sendView(); g_viewDirty = false; g_tView = t; }
    if (g_seekDirty && t - g_tSeek >= 60) { char b[48]; snprintf(b, 48, "%.2f", g_seekTarget); cmd("seek", b, "absolute+keyframes"); g_seekDirty = false; g_tSeek = t; }
    if (g_osdDirty && t - g_tOsd >= 70) { osd(g_osdBuf); g_osdDirty = false; g_tOsd = t; }
    if (t - g_tOv >= (g_dragKind ? 16.0 : 100.0)) { overlay(); g_tOv = t; }
    if (g_pending < 0 && !g_viewDirty && !g_seekDirty && !g_osdDirty && !g_dragKind && g_timerOn) { KillTimer(g_mw, 2); g_timerOn = false; }
    else if (g_dragKind) kick();
}

static LRESULT CALLBACK MW(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == 0x8000 + WM_MOUSEMOVE) { InterlockedExchange(&g_mmPending, 0); onInput(WM_MOUSEMOVE, g_mmWp, g_mmLp); return 0; }
    if (m >= 0x8000 && m < 0xC000) { onInput(m - 0x8000, wp, lp); return 0; }
    if (m == WM_USER + 1) {
        for (;;) {
            mpv_event* e = p_mpv_wait_event(h, 0);
            if (e->event_id == MPV_EVENT_NONE) break;
            if (e->event_id == MPV_EVENT_SHUTDOWN) { PostQuitMessage(0); return 0; }
            if (e->event_id == MPV_EVENT_PROPERTY_CHANGE && e->data) {
                mpv_event_property* p = (mpv_event_property*)e->data;
                if (p->format == MPV_FORMAT_DOUBLE) {
                    double v = *(double*)p->data;
                    if (!strcmp(p->name, "time-pos")) c_pos = v; else if (!strcmp(p->name, "duration")) c_dur = v;
                } else if (p->format == MPV_FORMAT_FLAG) {
                    int v = *(int*)p->data;
                    if (!strcmp(p->name, "pause")) c_pause = v != 0;
                    else if (!strcmp(p->name, "eof-reached")) c_eof = v != 0;
                    else if (!strcmp(p->name, "fullscreen")) c_full = v != 0;
                } else if (p->format == MPV_FORMAT_NONE) {
                    if (!strcmp(p->name, "duration")) c_dur = 0; else if (!strcmp(p->name, "time-pos")) c_pos = 0;
                    else if (!strcmp(p->name, "eof-reached")) c_eof = false;
                }
            }
            if (e->event_id == MPV_EVENT_END_FILE && e->data) {
                mpv_event_end_file* ef = (mpv_event_end_file*)e->data;
                if (ef->reason == MPV_END_FILE_REASON_ERROR) {
                    std::wstring ce = (idx >= 0 && idx < (int)files.size()) ? std::wstring(PathFindExtensionW(files[idx].c_str())) : std::wstring();
                    if (g_wicTried != idx && isImg(ce)) startWic(idx, true);   // mpv could not decode it: try Windows codecs
                    else showCantOpen();
                }
            }
            if (e->event_id == MPV_EVENT_FILE_LOADED || e->event_id == MPV_EVENT_VIDEO_RECONFIG) {
                hookIfNeeded();
                if (g_scanPending) {                        // first frame is on screen: now list the folder in the background
                    g_scanPending = false;
                    ScanJob* sj = new ScanJob{g_full, (int)InterlockedIncrement(&g_scanTok)};
                    HANDLE st = CreateThread(0, 0, scanThread, sj, 0, 0);
                    if (st) CloseHandle(st); else delete sj;
                }
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
    if (m == WM_USER + 2) {                       // Windows-codec decode finished
        WicRes* r = (WicRes*)lp;
        if (r->token == g_wicToken && r->idx == idx && idx < (int)files.size()) {
            if (r->blob) {
                int id = addBlob(r->blob);
                char u[48]; snprintf(u, 48, "lvmem://%d.bmp", id);
                cmd("loadfile", u, "replace");
                setp("pause", "no");
            } else if (!r->fb) {                        // no Windows codec: still let mpv try (works for simple HEIC)
                std::string u = U8(files[r->idx]);
                cmd("loadfile", u.c_str(), "replace");
                setp("pause", "no");
            } else showCantOpen();
        }
        delete r;
        return 0;
    }
    if (m == WM_USER + 3) {                       // folder listing finished
        ScanRes* r = (ScanRes*)lp;
        if (r->token == g_scanTok) {
            EnterCriticalSection(&g_fl);
            files.swap(r->v); idx = r->ix;
            InterlockedIncrement(&g_flGen);
            LeaveCriticalSection(&g_fl);
            g_shown = idx;
        }
        delete r;
        return 0;
    }
    if (m == WM_TIMER) {
        if (wp == 2) { pump(); return 0; }
        hookIfNeeded();
        static int tick;
        if (++tick % 50 == 0) saveResume();       // about every 10 s
        pump();
        if (o_autonext && g_isVid && idx < (int)files.size() - 1 && c_eof) {
            static DWORD lastAuto;
            if (GetTickCount() - lastAuto > 1500) { lastAuto = GetTickCount(); go(1); }
        }
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

// ======================= settings window (photo OR video, never both) =======================
struct Opt { const wchar_t* lbl; int type; void* p; int mask; };   // type 0 checkbox, 1 number, 2 text; mask 1 photo, 2 video
static Opt OPTS[] = {
    // ---- photo viewer ----
    {L"High quality scaling (uncheck = fastest mode for very old GPUs)", 0, &o_hq, 1},
    {L"Ultra quality: heaviest scaler, needs a decent GPU", 0, &o_ultra, 1},
    {L"Colour-managed display (ICC profile, slower start)", 0, &o_icc, 1},
    {L"Touchpad swipe distance for next / previous (smaller = more sensitive)", 1, &o_swipe, 1},
    // ---- video player ----
    {L"Hardware (GPU) decoding - picks the best method for this PC", 0, &o_hwdec, 2},
    {L"Smooth motion (display-resample + interpolation, heavier)", 0, &o_smooth, 2},
    {L"Light debanding", 0, &o_deband, 2},
    {L"Keep stereo audio as stereo (downmix surround sound)", 0, &o_stereo, 2},
    {L"Remember playback position of each video", 0, &o_resume, 2},
    {L"Play the next video automatically", 0, &o_autonext, 2},
    {L"Remember the last volume", 0, &o_remvol, 2},
    {L"Default volume (0-130)", 1, &o_vol, 2},
    {L"Preferred subtitle language (e.g. en)", 2, o_slang, 2},
    {L"Preferred audio language (e.g. en)", 2, o_alang, 2},
    {L"Reverse two-finger scroll direction (volume / brightness)", 0, &o_invv, 2},
    // ---- both ----
    {L"Reverse two-finger swipe direction (seek / next-previous)", 0, &o_inv, 3},
    {L"Preload next / previous files in the background", 0, &o_preload, 3},
    {L"Hide controls after (seconds)", 1, &o_uisec, 3},
    {L"Start in fullscreen", 0, &o_startfs, 3},
    {L"Keep window always on top", 0, &o_ontop, 3},
    {L"Use gpu-next renderer (better HDR, slower; applies on next open)", 0, &o_next, 3}};
static const int NOPT = sizeof(OPTS) / sizeof(*OPTS);
static HWND g_tab, g_lv, g_status;
static std::vector<HWND> g_pg[2];
static HFONT g_font;
static int g_dpi = 96, g_cap = -1, g_setMode = 1;          // g_setMode: 1 photo, 2 video
static int g_la[64], g_nla;                                // list row -> ACTS index
static bool g_building;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static BOOL CALLBACK setFontCb(HWND c, LPARAM) { SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE); return TRUE; }
static std::string* setKM() { return KMS[g_setMode == 2 ? 1 : 0]; }

static void showPage(int p) {
    for (int i = 0; i < 2; i++) for (HWND c : g_pg[i]) ShowWindow(c, i == p ? SW_SHOW : SW_HIDE);
}
static void refreshList() {
    std::string* km = setKM();
    for (int i = 0; i < g_nla; i++) {
        std::wstring keys;
        for (int vk = 1; vk < 256; vk++)
            if (km[vk] == ACTS[g_la[i]].id) { if (!keys.empty()) keys += L", "; keys += nameOf(vk); }
        ListView_SetItemText(g_lv, i, 1, (LPWSTR)keys.c_str());
    }
}
static void startCapture() {
    int sel = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
    if (sel < 0 || sel >= g_nla) { SetWindowTextW(g_status, L"Select an action first."); return; }
    g_cap = sel;
    std::wstring s = std::wstring(L"Press the new key for \"") + ACTS[g_la[sel]].label + L"\"  (Esc = cancel)";
    SetWindowTextW(g_status, s.c_str());
    SetFocus(g_set);
}
static void clearKeysOf(const char* a) { std::string* km = setKM(); for (int vk = 1; vk < 256; vk++) if (km[vk] == a) km[vk].clear(); }

static LRESULT CALLBACK SetProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_CREATE: {
        INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES};
        InitCommonControlsEx(&ic);
        NONCLIENTMETRICSW ncm = {sizeof ncm};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
        if (!g_font) g_font = CreateFontIndirectW(&ncm.lfMessageFont);
        g_building = true;
        g_tab = CreateWindowExW(0, WC_TABCONTROLW, 0, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, S(10), S(10), S(575), S(570), hw, (HMENU)1, g_inst, 0);
        static const wchar_t* tn1[2] = {L"Photo viewer", L"Photo shortcuts"};
        static const wchar_t* tn2[2] = {L"Video player", L"Video shortcuts"};
        for (int i = 0; i < 2; i++) { TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)(g_setMode == 2 ? tn2[i] : tn1[i]); TabCtrl_InsertItem(g_tab, i, &ti); }
        int y = S(48);
        for (int i = 0; i < NOPT; i++) {
            Opt& o = OPTS[i];
            if (!(o.mask & g_setMode)) continue;
            HWND c;
            if (o.type == 0) {
                c = CreateWindowExW(0, L"BUTTON", o.lbl, WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP, S(30), y, S(540), S(24), hw, (HMENU)(INT_PTR)(2000 + i), g_inst, 0);
                SendMessageW(c, BM_SETCHECK, *(int*)o.p ? BST_CHECKED : BST_UNCHECKED, 0);
                g_pg[0].push_back(c);
            } else {
                HWND l = CreateWindowExW(0, L"STATIC", o.lbl, WS_CHILD, S(30), y + S(3), S(410), S(22), hw, 0, g_inst, 0);
                wchar_t v[32];
                if (o.type == 1) _snwprintf(v, 32, L"%d", *(int*)o.p); else { wcsncpy(v, (wchar_t*)o.p, 31); v[31] = 0; }
                c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", v, WS_CHILD | ES_AUTOHSCROLL | WS_TABSTOP | (o.type == 1 ? ES_NUMBER : 0), S(450), y, S(100), S(24), hw, (HMENU)(INT_PTR)(2000 + i), g_inst, 0);
                g_pg[0].push_back(l); g_pg[0].push_back(c);
            }
            y += S(29);
        }
        g_lv = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, 0, WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP, S(25), S(45), S(545), S(440), hw, (HMENU)3000, g_inst, 0);
        ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT);
        LVCOLUMNW col = {}; col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = (LPWSTR)L"Action"; col.cx = S(300); ListView_InsertColumn(g_lv, 0, &col);
        col.pszText = (LPWSTR)L"Key"; col.cx = S(220); ListView_InsertColumn(g_lv, 1, &col);
        g_nla = 0;
        for (int i = 0; i < NACT && g_nla < 64; i++) {
            if (!(ACTS[i].mask & g_setMode)) continue;
            LVITEMW it = {}; it.mask = LVIF_TEXT; it.iItem = g_nla; it.pszText = (LPWSTR)ACTS[i].label;
            ListView_InsertItem(g_lv, &it);
            g_la[g_nla++] = i;
        }
        refreshList();
        g_pg[1].push_back(g_lv);
        static const wchar_t* bt[3] = {L"Change key...", L"Clear", L"Reset all"};
        for (int i = 0; i < 3; i++)
            g_pg[1].push_back(CreateWindowExW(0, L"BUTTON", bt[i], WS_CHILD | BS_PUSHBUTTON | WS_TABSTOP, S(25) + i * S(130), S(495), S(120), S(28), hw, (HMENU)(INT_PTR)(3001 + i), g_inst, 0));
        g_status = CreateWindowExW(0, L"STATIC", L"Select an action, then press \"Change key...\" (or double-click it).", WS_CHILD, S(25), S(535), S(545), S(22), hw, 0, g_inst, 0);
        g_pg[1].push_back(g_status);
        CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, S(485), S(592), S(100), S(30), hw, (HMENU)IDCANCEL, g_inst, 0);
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
            if (sel >= 0 && sel < g_nla) { clearKeysOf(ACTS[g_la[sel]].id); refreshList(); saveSettings(); }
            return 0;
        }
        if (id == 3003) {
            applyDefaultKeys(g_setMode == 2 ? 1 : 0); refreshList(); saveSettings();
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
            saveSettings();
        }
        return 0;
    }
    case WM_KEYDOWN:
        if (g_cap >= 0) {
            int vk = (int)wp;
            if (vk == VK_ESCAPE) { g_cap = -1; SetWindowTextW(g_status, L"Cancelled."); return 0; }
            if (nameOf(vk).empty()) return 0;
            const Act& a = ACTS[g_la[g_cap]];
            clearKeysOf(a.id);
            setKM()[vk] = a.id;
            std::wstring s = std::wstring(L"\"") + a.label + L"\" = " + nameOf(vk);
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
    int mode = g_isVid ? 2 : 1;
    if (g_set) {
        if (g_setMode == mode) { ShowWindow(g_set, SW_SHOW); SetForegroundWindow(g_set); return; }
        DestroyWindow(g_set);                                   // other mode: rebuild with that mode's settings
    }
    g_setMode = mode;
    static bool reg;
    if (!reg) {
        WNDCLASSW wc = {};
        wc.lpfnWndProc = SetProc; wc.hInstance = g_inst; wc.lpszClassName = L"LiteViewSettings";
        wc.hCursor = LoadCursorW(0, IDC_ARROW); wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
        RegisterClassW(&wc);
        reg = true;
    }
    HDC dc = GetDC(0); g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(0, dc);
    RECT rc = {0, 0, S(600), S(635)};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    int w = rc.right - rc.left, hgt = rc.bottom - rc.top;
    g_set = CreateWindowExW(WS_EX_APPWINDOW, L"LiteViewSettings", mode == 2 ? L"LiteView - Video settings" : L"LiteView - Photo settings",
                            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                            (GetSystemMetrics(SM_CXSCREEN) - w) / 2, (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2, w, hgt, 0, 0, g_inst, 0);
    SetForegroundWindow(g_set);
}

// ======================= main =======================
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDPIAware();
    g_inst = hi;
    InitializeCriticalSection(&g_fl);
    InitializeCriticalSection(&g_bl);
    CoInitializeEx(0, COINIT_APARTMENTTHREADED);
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
    L(mpv_create); L(mpv_initialize); L(mpv_set_option_string); L(mpv_get_property_string); L(mpv_get_property);
    L(mpv_command_async); L(mpv_set_property_async); L(mpv_observe_property); L(mpv_wait_event);
    L(mpv_set_wakeup_callback); L(mpv_terminate_destroy); L(mpv_free); L(mpv_stream_cb_add_ro);

    loadSettings();
    s_vol = std::clamp(o_vol, 0, 130);
    s_subscale = o_subpct / 100.0;
    wchar_t full[MAX_PATH * 2];
    GetFullPathNameW(argv[1], MAX_PATH * 2, full, 0);
    files.push_back(full); idx = 0; g_full = full; g_scanPending = true;   // folder is listed after the first frame

    WNDCLASSW wc = {};
    wc.lpfnWndProc = MW; wc.hInstance = hi; wc.lpszClassName = L"LiteViewMsg";
    RegisterClassW(&wc);
    g_mw = CreateWindowExW(WS_EX_TOOLWINDOW, L"LiteViewMsg", L"", WS_POPUP, 0, 0, 0, 0, 0, 0, hi, 0);   // hidden; owns menus

    h = p_mpv_create();
    char vol[16], sub[16];
    snprintf(vol, 16, "%d", std::clamp(o_vol, 0, 130));
    snprintf(sub, 16, "%.2f", s_subscale);
    wchar_t cd[MAX_PATH];
    SHGetFolderPathW(0, CSIDL_LOCAL_APPDATA, 0, 0, cd);
    PathAppendW(cd, L"LiteView"); CreateDirectoryW(cd, 0);
    PathAppendW(cd, L"cache"); CreateDirectoryW(cd, 0);
    std::string cache = U8(cd);                              // compiled shaders are cached here: no recompiling on every launch
    const char* o[][2] = {
        {"config", "no"}, {"load-scripts", "no"}, {"ytdl", "no"}, {"terminal", "no"}, {"msg-level", "all=no"},
        // --- renderer / quality ---
        {"vo", o_next ? "gpu-next,gpu" : "gpu"}, {"gpu-api", "d3d11"}, {"gpu-context", "d3d11"},
        {"gpu-shader-cache-dir", cache.c_str()}, {"icc-cache-dir", cache.c_str()},
        {"scale", o_ultra ? "ewa_lanczossharp" : (o_hq ? "spline36" : "bilinear")},
        {"dscale", (o_ultra || o_hq) ? "mitchell" : "hermite"},
        {"correct-downscaling", "yes"}, {"linear-downscaling", o_ultra ? "yes" : "no"}, {"sigmoid-upscaling", o_ultra ? "yes" : "no"},
        {"icc-profile-auto", o_icc ? "yes" : "no"},
        {"deband", "no"}, {"deband-iterations", "1"},
        {"interpolation", "no"}, {"tscale", "oversample"}, {"video-sync", "audio"},
        // --- video decoding: GPU decoding chosen per system, safe software fallback ---
        {"hwdec", o_hwdec ? "auto-safe" : "no"}, {"hwdec-codecs", "all"}, {"framedrop", "vo"}, {"hdr-compute-peak", "no"},
        // --- audio ---
        {"audio-channels", o_stereo ? "stereo" : "auto-safe"}, {"audio-normalize-downmix", "yes"},
        // --- window / ui ---
        {"osc", "no"}, {"osd-bar", "no"}, {"input-default-bindings", "no"}, {"input-builtin-bindings", "no"},
        {"border", "no"}, {"title", ""}, {"force-window", "yes"}, {"keep-open", "yes"}, {"idle", "no"},
        {"autofit-larger", "85%x85%"}, {"geometry", "50%:50%"}, {"window-dragging", "no"}, {"cursor-autohide", "1500"},
        {"image-display-duration", "inf"}, {"background-color", "#000000"},
        {"osd-font-size", "30"}, {"osd-border-size", "2"}, {"osd-margin-y", "40"},
        {"fullscreen", o_startfs ? "yes" : "no"}, {"ontop", o_ontop ? "yes" : "no"},
        // --- audio files / subtitles / memory ---
        {"volume", vol}, {"volume-max", "130"}, {"audio-display", "embedded-first"}, {"save-position-on-quit", "no"},
        {"sub-auto", "fuzzy"}, {"sub-font-size", "40"}, {"sub-border-size", "2"}, {"sub-scale", sub},
        {"demuxer-max-bytes", "48MiB"}, {"demuxer-max-back-bytes", "8MiB"}, {"taskbar-progress", "no"}};
    for (auto& kv : o) p_mpv_set_option_string(h, kv[0], kv[1]);
    if (o_slang[0]) p_mpv_set_option_string(h, "slang", U8(o_slang).c_str());
    if (o_alang[0]) p_mpv_set_option_string(h, "alang", U8(o_alang).c_str());
    p_mpv_set_wakeup_callback(h, wake, 0);
    p_mpv_initialize(h);
    if (p_mpv_stream_cb_add_ro) p_mpv_stream_cb_add_ro(h, "lvmem", 0, sc_open);
    p_mpv_observe_property(h, 0, "pause", MPV_FORMAT_FLAG);
    p_mpv_observe_property(h, 0, "eof-reached", MPV_FORMAT_FLAG);
    p_mpv_observe_property(h, 0, "fullscreen", MPV_FORMAT_FLAG);
    p_mpv_observe_property(h, 0, "duration", MPV_FORMAT_DOUBLE);
    p_mpv_observe_property(h, 0, "time-pos", MPV_FORMAT_DOUBLE);
    if (o_preload) {
        g_preEv = CreateEventW(0, FALSE, FALSE, 0);
        CreateThread(0, 0, preloadThread, 0, 0, 0);
    }
    openIdx(idx);
    SetTimer(g_mw, 1, 200, 0);

    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0) > 0) {
        if (g_set && g_cap < 0 && IsDialogMessageW(g_set, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) UnhookWindowsHookEx(g_hook);
    saveResume();
    if (o_remvol) o_vol = (int)floor(s_vol + 0.5);
    saveSettings();
    p_mpv_terminate_destroy(h);
    return 0;
}
