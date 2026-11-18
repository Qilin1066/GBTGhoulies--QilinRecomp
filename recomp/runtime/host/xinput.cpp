// XInput (the XPP library: the Xbox's controller ports), replaced natively
// over platform::Input (SDL3's gamepads, and the keyboard on port 1).
//
// The game asks about one device type, gamepads (XDEVICE_TYPE_GAMEPAD, the
// descriptor at 0x1FCDC8); any other type stops the runtime. Its input code
// (0x155540 at start-up, 0x1555B0 each frame) calls XGetDevices once, then
// each frame XGetDeviceChanges, opens and closes ports as they change, reads
// every open port with XInputGetState, and sends rumble with XInputSetState
// when its vibration option is on.
#include <array>
#include <string>

#include "host.hpp"
#include "input.hpp"
#include "strf.hpp"

namespace qilin::host {
namespace {

constexpr uint32_t kGamepad = 0x1FCDC8;      // XDEVICE_TYPE_GAMEPAD
constexpr uint32_t kSetLastError = 0x14B00C;  // XAPI's, lifted
constexpr uint32_t kHandleBase = 0x58490000;  // handles are this plus the port; the game only passes them back
constexpr uint32_t kErrorSuccess = 0, kErrorIoPending = 997, kErrorDeviceNotConnected = 1167;

platform::Input* g_input = nullptr;

struct Port {
    bool open = false;
    uint32_t packet = 0;  // XINPUT_STATE.dwPacketNumber: steps when the state changes
    platform::PadState last;
};
std::array<Port, platform::Input::kPorts> g_ports;
uint32_t g_known = 0;  // the ports the game was last told about (XGetDevices or XGetDeviceChanges)

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }
void st(const rt::Context& c, uint32_t a, uint32_t v) { rt::st32(mem(c), a, v, 0); }

[[noreturn]] void unmodelled(const char* fn, const std::string& what) {
    static std::string why;
    why = strf("%s: %s isn't modelled yet", fn, what.c_str());
    rt::trap(0, why.c_str());
}

void gamepads_only(const char* fn, uint32_t type) {
    if (type != kGamepad) unmodelled(fn, strf("device type %08X (only gamepads, %08X, are)", type, kGamepad));
}

uint32_t present() { return g_input ? g_input->connected_ports() : 0; }

// The port a handle names, or -1.
int port_of(uint32_t handle) {
    const uint32_t n = handle - kHandleBase;
    return n < g_ports.size() && g_ports[n].open ? int(n) : -1;
}

// VOID XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE PreallocTypes):
// starts the USB stack and reserves handles for the device types listed.
void x_init_devices(rt::Context& c) {
    const uint32_t count = arg(c, 0), types = arg(c, 1);
    for (uint32_t i = 0; i < count; ++i) gamepads_only("XInitDevices", rt::ld32(mem(c), types + 8 * i, 0));
    ret(c, c.eax, 8);
}

// DWORD XGetDevices(PXPP_DEVICE_TYPE DeviceType): a bitmap of the ports with
// a device of that type. What it reports is where XGetDeviceChanges counts from.
void x_get_devices(rt::Context& c) {
    gamepads_only("XGetDevices", arg(c, 0));
    g_known = present();
    ret(c, g_known, 4);
}

// BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE DeviceType, PDWORD pdwInsertions,
//     PDWORD pdwRemovals): what was plugged in or removed since the last call.
void x_get_device_changes(rt::Context& c) {
    gamepads_only("XGetDeviceChanges", arg(c, 0));
    const uint32_t now = present();
    const uint32_t insertions = now & ~g_known, removals = g_known & ~now;
    g_known = now;
    st(c, arg(c, 1), insertions);
    st(c, arg(c, 2), removals);
    ret(c, (insertions | removals) ? 1 : 0, 12);
}

// HANDLE XInputOpen(PXPP_DEVICE_TYPE DeviceType, DWORD dwPort, DWORD dwSlot,
//     PXINPUT_POLLING_PARAMETERS pPollingParameters): a handle to the
// controller in a port (dwSlot is for devices plugged into a controller,
// XDEVICE_NO_SLOT here), or NULL with ERROR_DEVICE_NOT_CONNECTED.
void x_input_open(rt::Context& c) {
    gamepads_only("XInputOpen", arg(c, 0));
    const uint32_t port = arg(c, 1), slot = arg(c, 2), polling = arg(c, 3);
    if (slot != 0) unmodelled("XInputOpen", strf("slot %u", slot));
    if (polling) unmodelled("XInputOpen", "polling parameters (the game's own polling rate)");
    if (port >= g_ports.size() || !(present() & (1u << port))) {
        call_guest(c, kSetLastError, {kErrorDeviceNotConnected});
        return ret(c, 0, 16);
    }
    if (g_ports[port].open) unmodelled("XInputOpen", strf("port %u opened twice", port));
    g_ports[port] = Port{true, 0, {}};
    ret(c, kHandleBase + port, 16);
}

// VOID XInputClose(HANDLE hDevice). Rumble stops with it, as the device's
// state is cleared on the console.
void x_input_close(rt::Context& c) {
    const int port = port_of(arg(c, 0));
    if (port < 0) unmodelled("XInputClose", strf("handle %08X, which isn't open", arg(c, 0)));
    if (g_input) g_input->rumble(uint32_t(port), 0, 0);
    g_ports[size_t(port)].open = false;
    ret(c, c.eax, 4);
}

// DWORD XInputGetState(HANDLE hDevice, PXINPUT_STATE pState): the
// controller's state as XINPUT_STATE { dwPacketNumber; XINPUT_GAMEPAD
// { wButtons; bAnalogButtons[8]; sThumbLX, sThumbLY, sThumbRX, sThumbRY } },
// or ERROR_DEVICE_NOT_CONNECTED once it's been removed.
void x_input_get_state(rt::Context& c) {
    const int port = port_of(arg(c, 0));
    if (port < 0) unmodelled("XInputGetState", strf("handle %08X, which isn't open", arg(c, 0)));
    const platform::PadState s = g_input ? g_input->state(uint32_t(port)) : platform::PadState{};
    if (!s.connected) return ret(c, kErrorDeviceNotConnected, 8);
    Port& p = g_ports[size_t(port)];
    if (s != p.last) {
        ++p.packet;
        p.last = s;
    }
    const rt::Mem m = mem(c);
    const uint32_t out = arg(c, 1);
    rt::st32(m, out, p.packet, 0);
    rt::st16(m, out + 4, s.buttons, 0);
    for (uint32_t i = 0; i < 8; ++i) rt::st8(m, out + 6 + i, s.analog[i], 0);
    for (uint32_t i = 0; i < 4; ++i) rt::st16(m, out + 14 + 2 * i, uint16_t(s.thumbs[i]), 0);
    ret(c, kErrorSuccess, 8);
}

// DWORD XInputSetState(HANDLE hDevice, PXINPUT_FEEDBACK pFeedback): rumble,
// sent to the controller asynchronously. XINPUT_FEEDBACK is a header {
// dwStatus, hEvent, Reserved[58] } and XINPUT_RUMBLE { wLeftMotorSpeed,
// wRightMotorSpeed } at +0x42. The original returns ERROR_IO_PENDING and
// later sets dwStatus and signals hEvent; here the rumble is sent at once,
// so the request is complete as it returns.
void x_input_set_state(rt::Context& c) {
    const int port = port_of(arg(c, 0));
    if (port < 0) unmodelled("XInputSetState", strf("handle %08X, which isn't open", arg(c, 0)));
    const uint32_t fb = arg(c, 1);
    if (rt::ld32(mem(c), fb + 4, 0)) unmodelled("XInputSetState", "a completion event (hEvent)");
    if (g_input) g_input->rumble(uint32_t(port), rt::ld16(mem(c), fb + 0x42, 0), rt::ld16(mem(c), fb + 0x44, 0));
    st(c, fb, kErrorSuccess);
    ret(c, kErrorIoPending, 8);
}

}  // namespace

void register_xinput(platform::Input* input) {
    g_input = input;
    register_hle(0x1FDD30, "XInitDevices", x_init_devices);  // a jump to the function itself
    register_hle(xdk_symbol("XInitDevices")->address, "XInitDevices", x_init_devices);
    register_hle(xdk_symbol("XGetDevices")->address, "XGetDevices", x_get_devices);
    register_hle(xdk_symbol("XGetDeviceChanges")->address, "XGetDeviceChanges", x_get_device_changes);
    register_hle(xdk_symbol("XInputOpen")->address, "XInputOpen", x_input_open);
    register_hle(xdk_symbol("XInputClose")->address, "XInputClose", x_input_close);
    register_hle(xdk_symbol("XInputGetState")->address, "XInputGetState", x_input_get_state);
    register_hle(xdk_symbol("XInputSetState")->address, "XInputSetState", x_input_set_state);
}

}  // namespace qilin::host
