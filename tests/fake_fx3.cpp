// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fake_fx3.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include "rx888.h"

namespace fake {

int16_t ramp_sample(uint64_t i) { return static_cast<int16_t>(static_cast<uint16_t>(i * 37)); }

namespace {

uint32_t u32(const std::vector<uint8_t>& d) {
    if (d.size() < 4)
        return 0;
    return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 | static_cast<uint32_t>(d[2]) << 16 |
           static_cast<uint32_t>(d[3]) << 24;
}

class Handle : public fern::Fx3 {
public:
    Handle(Device& device, bool bootloader) : device_(device), bootloader_(bootloader) { ++device_.open_handles; }
    ~Handle() override { --device_.open_handles; }

    int control_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, uint16_t len,
                    unsigned) override {
        std::lock_guard<std::mutex> lock(device_.mutex);
        Request r{false, request, value, index, std::vector<uint8_t>(data, data + len)};
        device_.requests.push_back(r);
        if (device_.unplugged || device_.gone > 0)
            return fern::usb_error::no_device;
        if (device_.spec.fail_request == request && device_.spec.fail_code != 0) {
            const int code = device_.spec.fail_code;
            device_.spec.fail_request = 0;
            return code;
        }
        if (bootloader_) {
            if (request != fern::rx888::request::firmware_load)
                return fern::usb_error::pipe;
            const uint32_t address = static_cast<uint32_t>(value) | static_cast<uint32_t>(index) << 16;
            if (len == 0) {
                // The jump: the bootloader leaves and the firmware comes back.
                device_.loaded_entry = address;
                device_.firmware_running = true;
                device_.loaded_by_module = true;
                device_.gone = device_.spec.gone_for;
                device_.denied = device_.spec.denied_for;
                return fern::usb_error::io;
            }
            if (len > 4096)
                return fern::usb_error::invalid_param;
            for (uint16_t i = 0; i < len; ++i)
                device_.ram[address + i] = data[i];
            return len;
        }
        switch (request) {
        case fern::rx888::request::reset:
            device_.firmware_running = false;
            device_.streaming = false;
            device_.adc_hz = 0;
            device_.loaded_by_module = false;
            device_.gone = device_.spec.gone_for;
            device_.denied = device_.spec.denied_for;
            return fern::usb_error::io;
        case fern::rx888::request::gpio:
            if (len != 4)
                return fern::usb_error::pipe;
            device_.gpio = u32(r.data);
            return len;
        case fern::rx888::request::set_argument:
            if (index == fern::rx888::argument::attenuator && value <= 63)
                device_.attenuator = value;
            else if (index == fern::rx888::argument::vga && value <= 255)
                device_.vga = value;
            else
                return fern::usb_error::pipe;
            return len;
        case fern::rx888::request::start_adc:
            if (len != 4)
                return fern::usb_error::pipe;
            device_.adc_hz = u32(r.data);
            return len;
        case fern::rx888::request::start_stream:
            // ringof refuses to stream without a running clock.
            if (device_.adc_hz == 0)
                return fern::usb_error::pipe;
            device_.streaming = true;
            return len;
        case fern::rx888::request::stop_stream:
            device_.streaming = false;
            return len;
        default:
            return fern::usb_error::pipe;
        }
    }

    int control_in(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len,
                   unsigned) override {
        std::lock_guard<std::mutex> lock(device_.mutex);
        device_.requests.push_back(Request{true, request, value, index, {}});
        if (device_.unplugged || device_.gone > 0)
            return fern::usb_error::no_device;
        if (device_.spec.fail_request == request && device_.spec.fail_code != 0) {
            const int code = device_.spec.fail_code;
            device_.spec.fail_request = 0;
            return code;
        }
        if (bootloader_ || request != fern::rx888::request::identify || len < 4)
            return fern::usb_error::pipe;
        data[0] = device_.loaded_by_module ? fern::rx888::board_rx888_mk2 : device_.spec.board;
        data[1] = 2;
        data[2] = 6;
        data[3] = 0;
        return 4;
    }

    int stream(fern::SampleCallback cb, void* ctx, uint32_t transfers, uint32_t transfer_bytes) override {
        if (device_.spec.stream_error != 0)
            return device_.spec.stream_error;
        std::vector<uint8_t> buf(transfer_bytes);
        const auto start = std::chrono::steady_clock::now();
        uint64_t sample = 0;
        (void)transfers;
        while (!device_.cancelled.load()) {
            uint32_t rate;
            bool on;
            uint32_t gpio;
            {
                std::lock_guard<std::mutex> lock(device_.mutex);
                rate = device_.adc_hz;
                on = device_.streaming && !device_.unplugged;
                gpio = device_.gpio;
            }
            const uint64_t done = device_.streamed.load();
            if (device_.spec.unplug_after && done >= device_.spec.unplug_after) {
                std::lock_guard<std::mutex> lock(device_.mutex);
                device_.unplugged = true;
                return fern::usb_error::no_device;
            }
            if (!on || (device_.spec.stall_after && done >= device_.spec.stall_after)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            for (uint32_t i = 0; i < transfer_bytes / 2; ++i, ++sample) {
                uint16_t s = static_cast<uint16_t>(ramp_sample(sample));
                if (gpio & fern::rx888::gpio::randomizer)
                    s ^= static_cast<uint16_t>(0u - (s & 1u)) & 0xFFFEu;
                buf[2 * i] = static_cast<uint8_t>(s);
                buf[2 * i + 1] = static_cast<uint8_t>(s >> 8);
            }
            cb(buf.data(), transfer_bytes, ctx);
            device_.streamed.fetch_add(transfer_bytes);
            if (device_.spec.realtime && rate > 0) {
                const auto due = start + std::chrono::nanoseconds(static_cast<int64_t>(1e9 * sample / rate));
                std::this_thread::sleep_until(due);
            } else {
                std::this_thread::yield();
            }
        }
        device_.cancelled.store(false);
        return 0;
    }

    void cancel_stream() override { device_.cancelled.store(true); }

private:
    Device& device_;
    bool bootloader_;
};

}  // namespace

std::vector<Request> Device::taken() {
    std::lock_guard<std::mutex> lock(mutex);
    return requests;
}

Device& Backend::add(const Spec& spec) {
    devices.push_back(std::make_unique<Device>(spec));
    return *devices.back();
}

fern::Enumeration Backend::enumerate() {
    fern::Enumeration e;
    e.error = enumerate_error;
    if (e.error != 0) {
        e.diagnostic = "the fake USB is broken";
        return e;
    }
    for (auto& d : devices) {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->unplugged)
            continue;
        if (d->gone > 0) {
            --d->gone;
            continue;
        }
        fern::UsbDevice u;
        u.port = d->spec.port;
        u.bootloader = !d->firmware_running;
        if (!u.bootloader) {
            u.serial = d->spec.serial;
            u.product = "RX888mk2";
        }
        if (!u.bootloader && d->denied > 0) {
            // Time passes for udev between enumerations too.
            --d->denied;
            u.error = fern::usb_error::access;
        } else if (!u.bootloader) {
            u.error = d->spec.list_error;
        }
        if (u.error != 0) {
            // A device that cannot be opened cannot tell its strings.
            u.serial.clear();
            u.product.clear();
        }
        e.devices.push_back(u);
    }
    return e;
}

int Backend::open(const std::string& port, bool bootloader, std::unique_ptr<fern::Fx3>& out) {
    for (auto& d : devices) {
        std::lock_guard<std::mutex> lock(d->mutex);
        if (d->spec.port != port || d->unplugged || d->gone > 0 || d->firmware_running == bootloader)
            continue;
        if (d->denied > 0) {
            --d->denied;
            return fern::usb_error::access;
        }
        if (d->spec.open_error != 0)
            return d->spec.open_error;
        out = std::make_unique<Handle>(*d, bootloader);
        return 0;
    }
    return fern::usb_error::not_found;
}

}  // namespace fake
