// The window/input/system bridge (../common/platform_bridge.h) of the browser build: the game's one window is the
// page's canvas.
//
// The page's own thread owns the DOM, and the game never runs there: its main() is a thread of its own
// (PROXY_TO_PTHREAD). The page (page/bo1_page.js) turns DOM events into records in shared memory (web_shared.h) and
// keeps the page's state there (the canvas's size, focus, pointer lock); Bridge_PumpEvents, on the game's main thread,
// turns the records into bridge events, as the SDL bridge does with SDL's queue. What the game asks of the page goes the
// other way: flags the page reads every animation frame (mouse capture, the cursor, the title), or a call proxied to
// the page's thread (message boxes, the clipboard, links).
//
// Win32 screen units are the canvas's CSS pixels, and the canvas is the whole "screen": the window covers it, in full
// screen or not (the page makes the canvas fill the browser window, or the screen in the browser's full screen). The
// renderer draws at the canvas's size in device pixels (Bridge_GetBackingScale).
//
// Keys reach the Win32 layer as macOS virtual key codes (mac_keycodes.h), as with the other bridges: the page maps
// KeyboardEvent.code (the key's place on a US keyboard) to them.

#include "../common/platform_bridge.h"
#include "../win32compat/main_thread.h"
#include "web_shared.h"
#include "web_surface.h"

#include <emscripten.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace bo1web
{
bool MountGameFolder(const char *mountPoint, std::string *error); // web_fs.cpp

namespace
{
SharedState g_shared;
}

SharedState &Shared() { return g_shared; }

static_assert(offsetof(SharedState, canvasW) == 16 && offsetof(SharedState, framesPresented) == 64 &&
                  offsetof(SharedState, title) == 88 && offsetof(SharedState, keyState) == 216 &&
                  offsetof(SharedState, exitCode) == 256 && offsetof(SharedState, framePixels) == 260 &&
                  offsetof(SharedState, frameWidth) == 272 && offsetof(SharedState, frameHeight) == 284 &&
                  offsetof(SharedState, frameAspectW) == 296 && offsetof(SharedState, frameFill) == 304 &&
                  offsetof(SharedState, pads) == 320 &&
                  offsetof(SharedState, ring) == 512 && offsetof(SharedState, fetch) == 33280,
              "bo1_page.js reads these offsets");
} // namespace bo1web

using namespace bo1web;

struct BridgeWindow
{
    void *userData = nullptr;
    WebSurface surface;
    std::atomic<int> visible{0};
    std::atomic<int> minimized{0};
    bool wantFullScreen = false;
};

namespace
{
BridgeEventHandler g_handler;
std::vector<BridgeWindow *> g_windows; // (the engine makes one; others exist for the length of a vid_restart)
std::atomic<int> g_appActive{1};
bool g_keyDown[256];
double g_startTime;

double Now() { return emscripten_get_now() / 1000.0; }

BridgeWindow *MainWindow() { return g_windows.empty() ? nullptr : g_windows.back(); }

void Emit(BridgeEvent &ev)
{
    if (!ev.time)
        ev.time = Now() - g_startTime;
    if (!ev.window)
        ev.window = MainWindow();
    if (ev.window && !ev.userData)
        ev.userData = ev.window->userData;
    if (g_handler)
        g_handler(&ev);
}

void ReleaseHeldKeys()
{
    for (int code = 0; code < 256; ++code)
    {
        if (!g_keyDown[code])
            continue;
        g_keyDown[code] = false;
        BridgeEvent up = {};
        up.type = BRIDGE_EV_KEY_UP;
        up.macKeyCode = (uint16_t)code;
        Emit(up);
    }
}

void SetActive(bool active)
{
    if ((g_appActive.load(std::memory_order_relaxed) != 0) == active)
        return;
    g_appActive.store(active ? 1 : 0, std::memory_order_relaxed);
    if (!active)
        ReleaseHeldKeys();
    BridgeEvent ev = {};
    ev.type = BRIDGE_EV_ACTIVATE;
    ev.value = active ? 1 : 0;
    Emit(ev);
}

// the canvas's size (CSS pixels), at least 1 x 1
void CanvasSize(int *w, int *h)
{
    *w = std::max(1, (int)g_shared.canvasW.load(std::memory_order_relaxed));
    *h = std::max(1, (int)g_shared.canvasH.load(std::memory_order_relaxed));
}

void HandleRecord(const InputRecord &r)
{
    BridgeEvent ev = {};
    switch (r.type)
    {
    case IN_KEY_DOWN:
    case IN_KEY_UP:
    {
        if (r.code < 0 || r.code > 0xFF)
            break;
        const bool down = r.type == IN_KEY_DOWN;
        ev.type = down ? BRIDGE_EV_KEY_DOWN : BRIDGE_EV_KEY_UP;
        ev.macKeyCode = (uint16_t)r.code;
        ev.isRepeat = down && r.value != 0;
        ev.modifiers = (uint32_t)r.mods;
        g_keyDown[r.code] = down;
        Emit(ev);
        break;
    }
    case IN_CHAR:
        ev.type = BRIDGE_EV_CHAR;
        ev.ch = (uint32_t)r.value;
        ev.macKeyCode = (uint16_t)(r.code & 0xFFFF);
        Emit(ev);
        break;
    case IN_MOUSE_MOVE:
        ev.type = BRIDGE_EV_MOUSE_MOVE;
        ev.x = r.x;
        ev.y = r.y;
        ev.dx = r.dx;
        ev.dy = r.dy;
        ev.modifiers = (uint32_t)r.mods;
        Emit(ev);
        break;
    case IN_MOUSE_DOWN:
    case IN_MOUSE_UP:
        if (r.code < 0 || r.code > 4)
            break;
        ev.type = r.type == IN_MOUSE_DOWN ? BRIDGE_EV_MOUSE_DOWN : BRIDGE_EV_MOUSE_UP;
        ev.button = r.code;
        ev.x = r.x;
        ev.y = r.y;
        ev.modifiers = (uint32_t)r.mods;
        Emit(ev);
        break;
    case IN_WHEEL:
        ev.type = BRIDGE_EV_WHEEL;
        ev.dy = r.dy;
        Emit(ev);
        break;
    case IN_FOCUS:
        SetActive(r.value != 0);
        break;
    case IN_RESIZE:
        ev.type = BRIDGE_EV_RESIZE;
        ev.x = std::max(1, r.x);
        ev.y = std::max(1, r.y);
        Emit(ev);
        break;
    case IN_QUIT:
        ev.type = BRIDGE_EV_QUIT;
        Emit(ev);
        break;
    default: break;
    }
}

void DrainRing()
{
    for (;;)
    {
        const int32_t w = g_shared.ringWrite.load(std::memory_order_acquire);
        int32_t r = g_shared.ringRead.load(std::memory_order_relaxed);
        if (r == w)
            return;
        while (r != w)
        {
            const InputRecord rec = g_shared.ring[r & (kInputRingSize - 1)];
            r = (r + 1) & 0x7FFFFFFF;
            g_shared.ringRead.store(r, std::memory_order_release);
            HandleRecord(rec);
        }
    }
}

void WakeMain()
{
    g_shared.wake.fetch_add(1, std::memory_order_release);
    emscripten_futex_wake(&g_shared.wake, 1);
}

// Text from the engine is UTF-8 when it comes from the file system and Windows-1252 / Latin-1 when it is the game's own;
// the page wants UTF-8.
std::string EngineString(const char *s)
{
    std::string out;
    if (!s)
        return out;
    const unsigned char *p = (const unsigned char *)s;
    // valid UTF-8 stays as it is
    bool utf8 = true;
    for (const unsigned char *q = p; *q && utf8;)
    {
        int n = *q < 0x80 ? 0 : (*q & 0xE0) == 0xC0 ? 1 : (*q & 0xF0) == 0xE0 ? 2 : (*q & 0xF8) == 0xF0 ? 3 : -1;
        if (n < 0)
            utf8 = false;
        ++q;
        for (; n > 0 && utf8; --n, ++q)
            if ((*q & 0xC0) != 0x80)
                utf8 = false;
    }
    if (utf8)
        return s;
    for (; *p; ++p)
    {
        const unsigned c = *p;
        if (c < 0x80)
            out += (char)c;
        else
        {
            out += (char)(0xC0 | (c >> 6));
            out += (char)(0x80 | (c & 0x3F));
        }
    }
    return out;
}

void SetString(char *dst, size_t size, const std::string &s)
{
    const size_t n = std::min(s.size(), size - 1);
    memcpy(dst, s.data(), n);
    dst[n] = 0;
}
} // namespace

extern "C" {

SharedState *Bridge_WebSharedState(void) { return &g_shared; }

void Bridge_Init(BridgeEventHandler handler)
{
    g_handler = handler;
    g_startTime = Now();
    g_shared.version = kSharedVersion;
    g_shared.cursorVisible.store(1);
    w32::AddMainThreadWaker(WakeMain);
    const int focused = g_shared.focused.load();
    g_appActive.store(focused ? 1 : 0);
}

void Bridge_PumpEvents(int waitMs)
{
    w32::ServiceMainThreadCalls();
    const int32_t seen = g_shared.wake.load(std::memory_order_acquire);
    const bool pending = g_shared.ringRead.load(std::memory_order_relaxed) != g_shared.ringWrite.load(std::memory_order_acquire);
    if (!pending && waitMs > 0 && !w32::MainThreadCallsPending())
        emscripten_futex_wait(&g_shared.wake, (uint32_t)seen, (double)waitMs);
    DrainRing();
    w32::ServiceMainThreadCalls();
}

int Bridge_IsAppActive(void) { return g_appActive.load(std::memory_order_relaxed); }
void Bridge_ActivateApp(void) {}
double Bridge_UnitScale(void) { return 1.0; }

BridgeWindow *Bridge_CreateWindow(const char *title, int, int, int, int, int, int, void *userData)
{
    BridgeWindow *w = new BridgeWindow();
    w->userData = userData;
    g_windows.push_back(w);
    if (title)
        Bridge_SetTitle(w, title);
    return w;
}

void Bridge_DestroyWindow(BridgeWindow *w)
{
    if (!w)
        return;
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), w), g_windows.end());
    // (kept allocated, as the other bridges keep theirs: other threads may still read its state)
    w->visible.store(0);
}

void Bridge_ShowWindow(BridgeWindow *w, int show)
{
    if (w)
        w->visible.store(show ? 1 : 0);
}
void Bridge_MinimizeWindow(BridgeWindow *) {}
void Bridge_RestoreWindow(BridgeWindow *) {}
int Bridge_IsMinimized(BridgeWindow *) { return 0; }
int Bridge_IsVisible(BridgeWindow *w) { return w ? w->visible.load() : 0; }
void Bridge_SetWindowStyle(BridgeWindow *, int, int) {}
// (the window is the canvas: its size is the page's)
void Bridge_SetClientRect(BridgeWindow *, int, int, int, int) {}

void Bridge_GetClientRect(BridgeWindow *, BridgeRect *r)
{
    int w, h;
    CanvasSize(&w, &h);
    *r = {0, 0, w, h};
}

void Bridge_GetFrameRect(BridgeWindow *w, BridgeRect *r) { Bridge_GetClientRect(w, r); }

void Bridge_SetTitle(BridgeWindow *, const char *title)
{
    if (!title)
        return;
    SetString(g_shared.title, sizeof(g_shared.title), EngineString(title));
    g_shared.titleSerial.fetch_add(1);
}

void Bridge_FocusWindow(BridgeWindow *) {}
int Bridge_IsKeyWindow(BridgeWindow *w) { return w && g_appActive.load(std::memory_order_relaxed) ? 1 : 0; }
void Bridge_SetCoversScreen(BridgeWindow *, int) {}

void Bridge_SetFullScreen(BridgeWindow *w, int fullScreen)
{
    if (!w)
        return;
    w->wantFullScreen = fullScreen != 0;
    // (the page enters the browser's full screen when the player next clicks: browsers allow it only then)
    g_shared.wantFullscreen.store(fullScreen ? 1 : 0);
}

int Bridge_IsFullScreen(BridgeWindow *w) { return w && w->wantFullScreen ? 1 : 0; }

void Bridge_SetContentScaling(BridgeWindow *w, int logicalW, int logicalH, int fill)
{
    if (!w)
        return;
    w->surface.logicalW.store(logicalW, std::memory_order_relaxed);
    w->surface.logicalH.store(logicalH, std::memory_order_relaxed);
    w->surface.fill.store(fill ? 1 : 0, std::memory_order_relaxed);
}

void *Bridge_GetMetalLayer(BridgeWindow *w) { return w ? (void *)&w->surface : nullptr; }

double Bridge_GetBackingScale(BridgeWindow *)
{
    const int d = g_shared.dprX1000.load(std::memory_order_relaxed);
    return d > 0 ? d / 1000.0 : 1.0;
}

void Bridge_SetCursorVisible(int visible) { g_shared.cursorVisible.store(visible ? 1 : 0); }
void Bridge_SetMouseCaptured(int captured) { g_shared.wantCapture.store(captured ? 1 : 0); }
void Bridge_WarpCursor(int, int) {} // (a page cannot move the pointer; captured, it is locked instead)

void Bridge_GetCursorPos(int *x, int *y)
{
    *x = g_shared.mouseX.load(std::memory_order_relaxed);
    *y = g_shared.mouseY.load(std::memory_order_relaxed);
}

void Bridge_SetArrowCursor(int) {}

int Bridge_IsMacKeyDown(uint16_t code) { return code < 256 && g_keyDown[code] ? 1 : 0; }
uint32_t Bridge_CurrentModifiers(void) { return (uint32_t)g_shared.modifiers.load(std::memory_order_relaxed); }

int Bridge_GetScreenCount(void) { return 1; }

int Bridge_GetScreenRect(int index, BridgeRect *full, BridgeRect *work, double *refreshHz)
{
    if (index != 0)
        return 0;
    int w, h;
    CanvasSize(&w, &h);
    if (full)
        *full = {0, 0, w, h};
    if (work)
        *work = {0, 0, w, h};
    if (refreshHz)
        *refreshHz = 60.0;
    return 1;
}

int Bridge_ScreenForPoint(int, int) { return 0; }

int Bridge_GetScreenModes(int index, int *widths, int *heights, int *rates, int maxModes)
{
    if (index != 0 || maxModes <= 0)
        return 0;
    // The game draws at its own resolution and the renderer scales the picture to the canvas: the resolutions it can
    // draw at, up to the canvas's size in device pixels (or the screen's, when larger: the browser's full screen)
    const double dpr = Bridge_GetBackingScale(nullptr);
    int maxW = std::max((int)g_shared.drawW.load(), (int)std::lround(g_shared.screenW.load() * dpr));
    int maxH = std::max((int)g_shared.drawH.load(), (int)std::lround(g_shared.screenH.load() * dpr));
    maxW = std::max(maxW, 640);
    maxH = std::max(maxH, 480);
    std::vector<std::pair<int, int>> sizes;
    static const int common[][2] = {{640, 480},   {800, 600},   {1024, 640},  {1024, 768},  {1152, 864},
                                    {1280, 720},  {1280, 800},  {1280, 960},  {1280, 1024}, {1366, 768},
                                    {1440, 900},  {1600, 900},  {1600, 1000}, {1600, 1200}, {1680, 1050},
                                    {1920, 1080}, {1920, 1200}, {2048, 1152}, {2560, 1080}, {2560, 1440},
                                    {2560, 1600}, {2880, 1800}, {3440, 1440}, {3840, 2160}};
    for (auto &c : common)
        sizes.push_back({c[0], c[1]});
    sizes.push_back({(int)g_shared.drawW.load(), (int)g_shared.drawH.load()});
    sizes.erase(std::remove_if(sizes.begin(), sizes.end(),
                               [&](const std::pair<int, int> &z) {
                                   return z.first < 640 || z.second < 480 || z.first > maxW || z.second > maxH;
                               }),
                sizes.end());
    std::sort(sizes.begin(), sizes.end());
    sizes.erase(std::unique(sizes.begin(), sizes.end()), sizes.end());
    const size_t first = sizes.size() > (size_t)maxModes ? sizes.size() - (size_t)maxModes : 0;
    int count = 0;
    for (size_t i = first; i < sizes.size(); ++i, ++count)
    {
        widths[count] = sizes[i].first;
        heights[count] = sizes[i].second;
        rates[count] = 60;
    }
    return count;
}

char *Bridge_GetClipboardText(void)
{
    // (the page keeps the text of the last paste into it: browsers read the clipboard only in a paste event)
    char *s = (char *)MAIN_THREAD_EM_ASM_PTR({
        var t = Module['bo1ClipboardText'] || "";
        if (!t) return 0;
        var n = lengthBytesUTF8(t) + 1;
        var p = _malloc(n);
        stringToUTF8(t, p, n);
        return p;
    });
    return s;
}

void Bridge_SetClipboardText(const char *utf8)
{
    const std::string t = EngineString(utf8 ? utf8 : "");
    char *copy = strdup(t.c_str());
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var t = UTF8ToString($0);
            _free($0);
            Module['bo1ClipboardText'] = t;
            try {
                if (navigator.clipboard) navigator.clipboard.writeText(t).catch(function() {});
            } catch (e) {
            }
        },
        copy);
}

int Bridge_MessageBox(const char *title, const char *text, int buttons, int icon)
{
    const std::string t = EngineString(title), m = EngineString(text);
    fprintf(stderr, "\n*** %s ***\n%s\n", t.c_str(), m.c_str());
    // buttons: 0 OK, 1 OK/Cancel, 4 Yes/No, 3 Yes/No/Cancel; returns the index of the button pressed. The page shows
    // it over the canvas (its own dialog: the pointer is released first) and answers when one is clicked.
    return MAIN_THREAD_EM_ASM_INT(
        {
            var title = UTF8ToString($0);
            var text = UTF8ToString($1);
            var buttons = $2;
            var icon = $3;
            if (Module['bo1MessageBox']) return Module['bo1MessageBox'](title, text, buttons, icon);
            if (buttons == 0) {
                alert(title + '\n\n' + text);
                return 0;
            }
            return confirm(title + '\n\n' + text) ? 0 : 1;
        },
        t.c_str(), m.c_str(), buttons, icon);
}

void Bridge_OpenURL(const char *url)
{
    if (!url || !*url || url[0] == '/')
        return; // (a folder of the game's: nothing to show in a browser)
    char *copy = strdup(url);
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            var u = UTF8ToString($0);
            _free($0);
            try {
                window.open(u, '_blank', 'noopener');
            } catch (e) {
            }
        },
        copy);
}

void Bridge_PreventDisplaySleep(int prevent)
{
    MAIN_THREAD_ASYNC_EM_ASM(
        {
            if (Module['bo1WakeLock']) Module['bo1WakeLock']($0);
        },
        prevent);
}

const char *Bridge_AppSupportPath(void)
{
    // the game folder, mounted from the server (web_fs.cpp)
    static bool mounted = false, tried = false;
    if (!tried)
    {
        tried = true;
        std::string err;
        mounted = MountGameFolder("/game", &err);
        if (!mounted)
            fprintf(stderr, "[web] the game folder could not be read from the server: %s\n", err.c_str());
    }
    return mounted ? "/game" : "";
}

const char *Bridge_BundleResourcePath(void)
{
    return ""; // (the build's own game files are laid over the game folder by the server: tools/web-server.py)
}

} // extern "C"
