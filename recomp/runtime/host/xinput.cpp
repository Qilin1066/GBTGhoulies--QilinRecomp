// XInput (the XPP library: the Xbox's controller ports), replaced natively.
//
// There's no input backend yet, so this is a console with nothing plugged
// in: no device of any type is present and none arrives. A title then never
// opens a port; XInputOpen and the calls on an open port aren't registered,
// so the runtime stops if one is reached.
#include "host.hpp"

namespace qilin::host {
namespace {

rt::Mem mem(const rt::Context& c) { return rt::Mem{c.mem, c.mem_size}; }

// VOID XInitDevices(DWORD dwPreallocTypeCount, PXDEVICE_PREALLOC_TYPE PreallocTypes):
// starts the USB stack and reserves handles for the device types listed.
void x_init_devices(rt::Context& c) { ret(c, c.eax, 8); }

// DWORD XGetDevices(PXPP_DEVICE_TYPE DeviceType): a bitmap of the ports with
// a device of that type.
void x_get_devices(rt::Context& c) { ret(c, 0, 4); }

// BOOL XGetDeviceChanges(PXPP_DEVICE_TYPE DeviceType, PDWORD pdwInsertions,
//     PDWORD pdwRemovals): what was plugged in or removed since the last call.
void x_get_device_changes(rt::Context& c) {
    rt::st32(mem(c), arg(c, 1), 0, 0);
    rt::st32(mem(c), arg(c, 2), 0, 0);
    ret(c, 0, 12);
}

}  // namespace

void register_xinput() {
    register_hle(0x1FDD30, "XInitDevices", x_init_devices);  // a jump to the function itself
    register_hle(xdk_symbol("XInitDevices")->address, "XInitDevices", x_init_devices);
    register_hle(xdk_symbol("XGetDevices")->address, "XGetDevices", x_get_devices);
    register_hle(xdk_symbol("XGetDeviceChanges")->address, "XGetDeviceChanges", x_get_device_changes);
}

}  // namespace qilin::host
