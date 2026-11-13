#pragma once
// The memory the page's own thread (page/bo1_page.js) and the game's threads share: input events the page writes for
// the bridge (web_bridge.cpp) to read, the page's state (the canvas, focus, pointer lock), what the game asks of the
// page (mouse capture, the cursor), the frames the render thread has handed the page, and the controllers' state.
//
// One static block in the WebAssembly memory; its address is Bridge_WebSharedState(). The page reads and writes it as
// Int32Array / Float32Array views at the byte offsets below (bo1_page.js has the same numbers: change both together).

#include <atomic>
#include <stdint.h>

namespace bo1web
{
enum InputEventType : int32_t
{
    IN_KEY_DOWN = 1,   // code: kVK_* (mac_keycodes.h), value: 1 = repeat, mods
    IN_KEY_UP = 2,     // code, mods
    IN_CHAR = 3,       // value: UTF-32 code point
    IN_MOUSE_MOVE = 4, // x, y (canvas pixels, CSS), dx, dy (movement; raw while the pointer is locked)
    IN_MOUSE_DOWN = 5, // code: button (0 left, 1 right, 2 middle, 3, 4), x, y
    IN_MOUSE_UP = 6,
    IN_WHEEL = 7,      // dy: notches (+1 up / away from the user)
    IN_FOCUS = 8,      // value: 1 = the page has the keyboard and is visible, 0 = not
    IN_RESIZE = 9,     // x, y: the canvas's new size (CSS pixels)
    IN_POINTER_LOCK = 10, // value: 1 = locked, 0 = released (by the user: Esc, or the page losing focus)
    IN_QUIT = 11,      // the page is being closed
};

// one event: 8 32-bit words
struct InputRecord
{
    int32_t type;
    int32_t code;
    int32_t x, y;
    float dx, dy;
    int32_t mods; // BRIDGE_MOD_* (platform_bridge.h)
    int32_t value;
};
static_assert(sizeof(InputRecord) == 32, "record layout");

constexpr int kInputRingSize = 1024; // records (a power of two)

struct GamepadState
{
    int32_t connected;   // 1 when a standard-mapping controller is in this slot
    int32_t buttons;     // XINPUT_GAMEPAD_* bits
    int32_t lt, rt;      // 0-255
    int32_t lx, ly, rx, ry; // -32768..32767, y up
    int32_t rumbleLeft, rumbleRight; // the game's request (0-65535), the page applies it
    int32_t packet;      // bumped by the page when the state changes
    int32_t pad;
};
static_assert(sizeof(GamepadState) == 48, "gamepad layout");

// ---- reads of the game's files through the page's fetch worker (web_fs.cpp, page/bo1_fetch_worker.js) ----
// The game's threads never return to the browser's event loop, and a browser frees what a request made only when its
// thread does: a game thread that read the files itself (synchronous requests) would keep every byte it ever read. So a
// game thread puts its request in a slot here and waits; the fetch worker, an ordinary worker of the page, makes the
// request, writes the bytes where the slot says and marks it done.
enum FetchState : int32_t
{
    FETCH_FREE = 0,
    FETCH_FILLING = 1,   // a game thread is writing the request
    FETCH_REQUESTED = 2, // for the fetch worker
    FETCH_RUNNING = 3,   // the fetch worker has it
    FETCH_DONE = 4,      // result and status are in (the game thread frees the slot)
};

struct FetchSlot
{
    std::atomic<int32_t> state;     // FetchState (a futex word: the game thread waits on it)
    int32_t url;                    // address of the URL (UTF-8, urlLength bytes)
    int32_t urlLength;
    int32_t offsetLow, offsetHigh;  // the range's first byte
    int32_t length;                 // and its size
    int32_t dst;                    // where its bytes go
    int32_t result;                 // bytes read, or -1
    int32_t status;                 // the HTTP status (0: no answer)
    int32_t pad[3];
};
static_assert(sizeof(FetchSlot) == 48, "fetch slot layout");

constexpr int kFetchSlots = 16;

struct FetchTable
{
    std::atomic<int32_t> wake;      // bumped and woken when a slot is requested (the fetch worker waits on it)
    std::atomic<int32_t> connected; // 1 once the fetch worker serves the table
    int32_t pad[2];
    FetchSlot slots[kFetchSlots];
};

struct SharedState
{
    // ---- input ring: the page writes records at ringWrite, the game reads from ringRead (both wrap at
    // kInputRingSize) ----
    std::atomic<int32_t> ringWrite;   // 0
    std::atomic<int32_t> ringRead;    // 4
    // a futex word: the page bumps it after writing records, the game's wakers (w32::AddMainThreadWaker) too; the
    // bridge's event wait sleeps on it
    std::atomic<int32_t> wake;        // 8
    int32_t version;                  // 12: kSharedVersion, for the page to check
    // ---- the page's state (written by the page) ----
    std::atomic<int32_t> canvasW;     // 16 CSS pixels
    std::atomic<int32_t> canvasH;     // 20
    std::atomic<int32_t> drawW;       // 24 device pixels (canvas size times devicePixelRatio, capped)
    std::atomic<int32_t> drawH;       // 28
    std::atomic<int32_t> dprX1000;    // 32 devicePixelRatio * 1000
    std::atomic<int32_t> focused;     // 36
    std::atomic<int32_t> pointerLocked; // 40
    std::atomic<int32_t> fullscreen;  // 44
    std::atomic<int32_t> screenW;     // 48 CSS pixels
    std::atomic<int32_t> screenH;     // 52
    std::atomic<int32_t> mouseX;      // 56 last pointer position over the canvas (CSS pixels)
    std::atomic<int32_t> mouseY;      // 60
    // ---- presentation ----
    std::atomic<int32_t> framesPresented; // 64 render thread: frames handed to the page
    std::atomic<int32_t> framesDisplayed; // 68 page: frames it has shown (futex word: the render thread paces on it)
    // ---- what the game asks of the page (written by the game, read by the page every animation frame) ----
    std::atomic<int32_t> wantCapture;    // 72 relative mouse (pointer lock)
    std::atomic<int32_t> cursorVisible;  // 76
    std::atomic<int32_t> wantFullscreen; // 80
    std::atomic<int32_t> titleSerial;    // 84 bumped when title changes
    char title[128];                     // 88
    std::atomic<int32_t> keyState[8];    // 216 kVK_* key down bits (the page's view, for GetAsyncKeyState)
    std::atomic<int32_t> modifiers;      // 248 BRIDGE_MOD_* now
    std::atomic<int32_t> loading;        // 252 game: 1 while it loads (the page shows its own progress until then)
    std::atomic<int32_t> exitCode;       // 256 game: >= 0 when the game has quit
    // ---- the frames: the render thread reads each finished frame back into one of three slots in the game's memory
    // (RGBA, 8 bits a channel, bottom row first) and the page draws the newest on its canvas. Frame n
    // (framesPresented, written after the slot) is in slot n % 3. ----
    int32_t framePixels[3];              // 260 address of the slot's pixels
    int32_t frameWidth[3];               // 272
    int32_t frameHeight[3];              // 284
    int32_t frameAspectW;                // 296 the shape to show the frames at (the game's resolution)
    int32_t frameAspectH;                // 300
    int32_t frameFill;                   // 304 1: stretched over the whole canvas, 0: at frameAspect, black bars
    int32_t reserved[3];                 // 308
    GamepadState pads[4];                // 320
    InputRecord ring[kInputRingSize];    // 512
    FetchTable fetch;                    // 33280
};

constexpr int32_t kSharedVersion = 3;

SharedState &Shared();
} // namespace bo1web

extern "C" bo1web::SharedState *Bridge_WebSharedState(void);
