// XInput (win32compat/include/xinput.h) for the browser build: the page polls the Gamepad API every animation frame
// (navigator.getGamepads(), the page's thread only) and writes each controller with the "standard" mapping into the
// shared state as XInput would report it (web_shared.h, page/bo1_page.js). Rumble requests go back the same way; the
// page plays them with the controller's vibration actuator where the browser has one. The counterpart of
// linux/sdl_gamepad.cpp; the XInput structures are declared here again with the same layout.

#include "web_shared.h"

#include <cstdint>
#include <cstring>

namespace
{
struct XGamepad
{
    uint16_t wButtons;
    uint8_t bLeftTrigger;
    uint8_t bRightTrigger;
    int16_t sThumbLX;
    int16_t sThumbLY;
    int16_t sThumbRX;
    int16_t sThumbRY;
};
struct XState
{
    uint32_t dwPacketNumber;
    XGamepad Gamepad;
};
struct XVibration
{
    uint16_t wLeftMotorSpeed;
    uint16_t wRightMotorSpeed;
};
struct XCapabilities
{
    uint8_t Type;
    uint8_t SubType;
    uint16_t Flags;
    XGamepad Gamepad;
    XVibration Vibration;
};
struct XBattery
{
    uint8_t BatteryType;
    uint8_t BatteryLevel;
};
static_assert(sizeof(XGamepad) == 12 && sizeof(XState) == 16 && sizeof(XCapabilities) == 20, "XInput layout");

enum : uint32_t
{
    kErrorSuccess = 0,
    kErrorBadArguments = 160,
    kErrorDeviceNotConnected = 1167,
    kErrorEmpty = 4306,
};
constexpr int kMaxUsers = 4;
bool g_enabled = true;

int16_t Clamp16(int32_t v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }
uint8_t Clamp8(int32_t v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
} // namespace

extern "C" {

uint32_t XInputGetState(uint32_t user, XState *state)
{
    if (user >= kMaxUsers || !state)
        return kErrorBadArguments;
    const bo1web::GamepadState &p = bo1web::Shared().pads[user];
    memset(state, 0, sizeof(*state));
    if (!__atomic_load_n(&p.connected, __ATOMIC_ACQUIRE))
        return kErrorDeviceNotConnected;
    state->dwPacketNumber = (uint32_t)__atomic_load_n(&p.packet, __ATOMIC_RELAXED);
    if (!g_enabled)
        return kErrorSuccess;
    state->Gamepad.wButtons = (uint16_t)__atomic_load_n(&p.buttons, __ATOMIC_RELAXED);
    state->Gamepad.bLeftTrigger = Clamp8(__atomic_load_n(&p.lt, __ATOMIC_RELAXED));
    state->Gamepad.bRightTrigger = Clamp8(__atomic_load_n(&p.rt, __ATOMIC_RELAXED));
    state->Gamepad.sThumbLX = Clamp16(__atomic_load_n(&p.lx, __ATOMIC_RELAXED));
    state->Gamepad.sThumbLY = Clamp16(__atomic_load_n(&p.ly, __ATOMIC_RELAXED));
    state->Gamepad.sThumbRX = Clamp16(__atomic_load_n(&p.rx, __ATOMIC_RELAXED));
    state->Gamepad.sThumbRY = Clamp16(__atomic_load_n(&p.ry, __ATOMIC_RELAXED));
    return kErrorSuccess;
}

uint32_t XInputSetState(uint32_t user, XVibration *vib)
{
    if (user >= kMaxUsers || !vib)
        return kErrorBadArguments;
    bo1web::GamepadState &p = bo1web::Shared().pads[user];
    if (!__atomic_load_n(&p.connected, __ATOMIC_ACQUIRE))
        return kErrorDeviceNotConnected;
    __atomic_store_n(&p.rumbleLeft, g_enabled ? (int32_t)vib->wLeftMotorSpeed : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&p.rumbleRight, g_enabled ? (int32_t)vib->wRightMotorSpeed : 0, __ATOMIC_RELAXED);
    return kErrorSuccess;
}

uint32_t XInputGetCapabilities(uint32_t user, uint32_t, XCapabilities *caps)
{
    if (user >= kMaxUsers || !caps)
        return kErrorBadArguments;
    if (!__atomic_load_n(&bo1web::Shared().pads[user].connected, __ATOMIC_ACQUIRE))
        return kErrorDeviceNotConnected;
    memset(caps, 0, sizeof(*caps));
    caps->Type = 0x01;    // XINPUT_DEVTYPE_GAMEPAD
    caps->SubType = 0x01; // XINPUT_DEVSUBTYPE_GAMEPAD
    caps->Gamepad.wButtons = 0xF3FF;
    caps->Gamepad.bLeftTrigger = 0xFF;
    caps->Gamepad.bRightTrigger = 0xFF;
    caps->Gamepad.sThumbLX = (int16_t)0xFFC0;
    caps->Gamepad.sThumbLY = (int16_t)0xFFC0;
    caps->Gamepad.sThumbRX = (int16_t)0xFFC0;
    caps->Gamepad.sThumbRY = (int16_t)0xFFC0;
    caps->Vibration.wLeftMotorSpeed = 0xFFFF;
    caps->Vibration.wRightMotorSpeed = 0xFFFF;
    return kErrorSuccess;
}

uint32_t XInputGetBatteryInformation(uint32_t user, uint8_t, XBattery *info)
{
    if (user >= kMaxUsers || !info)
        return kErrorBadArguments;
    const bool connected = __atomic_load_n(&bo1web::Shared().pads[user].connected, __ATOMIC_ACQUIRE) != 0;
    info->BatteryType = connected ? 0x01 : 0x00; // BATTERY_TYPE_WIRED / DISCONNECTED (browsers don't tell)
    info->BatteryLevel = connected ? 0x03 : 0;
    return connected ? kErrorSuccess : kErrorDeviceNotConnected;
}

uint32_t XInputGetKeystroke(uint32_t user, uint32_t, void *)
{
    if (user >= kMaxUsers && user != 0xFF)
        return kErrorBadArguments;
    return kErrorEmpty;
}

void XInputEnable(int enable)
{
    g_enabled = enable != 0;
    if (!g_enabled)
        for (bo1web::GamepadState &p : bo1web::Shared().pads)
        {
            __atomic_store_n(&p.rumbleLeft, 0, __ATOMIC_RELAXED);
            __atomic_store_n(&p.rumbleRight, 0, __ATOMIC_RELAXED);
        }
}

} // extern "C"
