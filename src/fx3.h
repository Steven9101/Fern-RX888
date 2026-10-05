// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The narrow interface between the module and libusb: vendor requests to
// the FX3 and a stream of bulk transfers from it. The real implementation
// (libusb_backend.cpp) stays a thin wrapper, so that every decision lives
// in code the tests drive with a fake device.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace fern {

// libusb's error codes, repeated so that the module logic does not depend
// on libusb headers.
namespace usb_error {
constexpr int io = -1;
constexpr int invalid_param = -2;
constexpr int access = -3;
constexpr int no_device = -4;
constexpr int not_found = -5;
constexpr int busy = -6;
constexpr int timeout = -7;
constexpr int overflow = -8;
constexpr int pipe = -9;  // the request stalled: the firmware does not know it
constexpr int interrupted = -10;
constexpr int no_mem = -11;
constexpr int not_supported = -12;
}  // namespace usb_error

const char* usb_error_text(int code);

using SampleCallback = void (*)(uint8_t* data, uint32_t len, void* ctx);

// An opened FX3, bootloader or firmware. Destroying it closes it; that must
// not happen while stream() runs in another thread.
class Fx3 {
public:
    virtual ~Fx3() = default;
    // Vendor requests to the device. The byte count transferred, or a
    // negative usb_error code.
    virtual int control_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, uint16_t len,
                            unsigned timeout_ms) = 0;
    virtual int control_in(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len,
                           unsigned timeout_ms) = 0;
    // Keeps `transfers` bulk transfers of `transfer_bytes` in flight on the
    // sample endpoint and calls cb with each one that completes, until
    // cancel_stream() or a failure. 0 when cancelled, otherwise a usb_error
    // code.
    //
    // cb runs on whichever thread handles libusb's events: mostly the one in
    // stream(), but a synchronous control request from another thread, such
    // as a gain change, handles them too while it waits. libusb lets one
    // thread at a time do that, under its own lock, so the calls never
    // overlap and each sees what the one before wrote; cb must still not
    // take a lock that a thread sending control requests may hold.
    virtual int stream(SampleCallback cb, void* ctx, uint32_t transfers, uint32_t transfer_bytes) = 0;
    // From any thread; stream() returns once the transfers in flight are
    // back. Harmless when stream() is not running.
    virtual void cancel_stream() = 0;
};

struct UsbDevice {
    // Bus and port chain, such as "2-1.4". The chain stays across the
    // re-enumeration after the firmware is loaded, the bus may not: see
    // Backend::socket_of.
    std::string port;
    bool bootloader = false;  // no firmware yet: 04b4:00f3
    // The firmware's serial number, sixteen hex digits from the FX3's die;
    // empty for the bootloader, which has none.
    std::string serial;
    std::string product;
    // A negative usb_error code when the device could not be opened to read
    // its strings, typically access or busy.
    int error = 0;
};

// Orders devices by port, numbers as numbers: "2-1.9" before "2-1.10". The
// order index: counts in and --list-devices prints.
bool port_order(const UsbDevice& a, const UsbDevice& b);

struct Enumeration {
    int error = 0;           // 0, or a usb_error code when USB is unusable here
    std::string diagnostic;  // what to do about error, for the operator
    std::vector<UsbDevice> devices;
};

class Backend {
public:
    virtual ~Backend() = default;
    virtual Enumeration enumerate() = 0;
    // 0 with a device, or a negative usb_error code.
    virtual int open(const std::string& port, bool bootloader, std::unique_ptr<Fx3>& out) = 0;
    // For the waits around re-enumeration; the fake backend does not sleep.
    virtual void sleep_ms(unsigned ms) = 0;
    // The physical socket behind a port. An xHCI controller lists each USB 3
    // socket twice, as a port on its USB 2 bus and one on its USB 3 bus, and
    // the FX3 changes bus when it changes speed: its bootloader runs at USB 2
    // and the RX-888's firmware at USB 3. Two ports name the same socket when
    // this gives the same text for both.
    virtual std::string socket_of(const std::string& port) { return port; }
};

}  // namespace fern
