// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A fake RX-888 behind a fake USB. It keeps the FX3's two lives apart, the
// bootloader and the firmware, takes the firmware the way the bootloader
// does, re-enumerates after the jump, answers the firmware's requests as
// ringof/rx888-firmware 0.1.0 does, and streams a known signal: a ramp of
// 16-bit samples, scrambled while the randomizer bit is set, so that tests
// can check every byte that reaches fd 1.
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "fx3.h"

namespace fake {

// Sample i of the stream every fake RX-888 sends, before scrambling: a ramp
// that visits every 16-bit value, so that clipped samples appear too.
int16_t ramp_sample(uint64_t i);

struct Request {
    bool in = false;
    uint8_t request = 0;
    uint16_t value = 0;
    uint16_t index = 0;
    std::vector<uint8_t> data;
};

struct Spec {
    std::string port = "2-1";
    std::string serial = "0123456789ABCDEF";
    bool firmware_running = false;  // plugged in with firmware already loaded
    // What identify reports under the firmware the device was plugged in
    // with: 4 is the MkII. Once the module has loaded ringof's, it reports 4
    // on any board, as the real one does.
    uint8_t board = 4;
    int open_error = 0;             // what Backend::open returns for this device
    int list_error = 0;             // what enumeration reports for its strings
    int stream_error = 0;           // stream() fails at once with this
    uint64_t unplug_after = 0;      // bytes streamed before the device vanishes; 0 never
    uint64_t stall_after = 0;       // bytes streamed before it goes quiet; 0 never
    // A request that fails with this code the first time it is sent; 0 none.
    uint8_t fail_request = 0;
    int fail_code = 0;
    // Enumerations the device stays away for after a reset or a jump.
    int gone_for = 2;
    // Opens refused with access after each re-enumeration, while udev has not
    // yet given the receiver's user the new device node; the listing cannot
    // read its strings either in that time.
    int denied_for = 0;
    bool realtime = false;          // pace the stream at the sample rate
};

class Device;

class Backend : public fern::Backend {
public:
    Device& add(const Spec& spec);
    fern::Enumeration enumerate() override;
    int open(const std::string& port, bool bootloader, std::unique_ptr<fern::Fx3>& out) override;
    void sleep_ms(unsigned) override {}

    int enumerate_error = 0;
    std::vector<std::unique_ptr<Device>> devices;
};

class Device {
public:
    explicit Device(const Spec& s) : spec(s), firmware_running(s.firmware_running) {}

    Spec spec;
    std::mutex mutex;
    // What the device has been sent, bootloader and firmware alike, in order.
    std::vector<Request> requests;
    bool firmware_running;
    int gone = 0;    // enumerations left before it is back
    int denied = 0;  // opens left that fail with access
    bool loaded_by_module = false;
    bool unplugged = false;
    // The bootloader's RAM, as the 0xA0 writes left it.
    std::map<uint32_t, uint8_t> ram;
    uint32_t gpio = 0;
    uint32_t adc_hz = 0;
    bool streaming = false;
    uint16_t attenuator = 0;
    uint16_t vga = 0;
    std::atomic<uint64_t> streamed{0};
    std::atomic<bool> cancelled{false};
    std::atomic<int> open_handles{0};

    std::vector<Request> taken();  // a copy of requests under the lock
    uint32_t loaded_entry = 0;     // where the last jump went
};

}  // namespace fake
