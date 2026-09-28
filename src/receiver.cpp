// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "receiver.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "log.h"

#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/lsan_interface.h>
#endif

namespace fern {

namespace {

using Clock = std::chrono::steady_clock;

std::string db_text(double db) {
    char text[32];
    std::snprintf(text, sizeof text, "%.1f", db);
    return text;
}

void put_u32(uint8_t* out, uint32_t v) {
    out[0] = static_cast<uint8_t>(v);
    out[1] = static_cast<uint8_t>(v >> 8);
    out[2] = static_cast<uint8_t>(v >> 16);
    out[3] = static_cast<uint8_t>(v >> 24);
}

std::string permission_hint() {
    return " The receiver's user needs access to the RX-888: FernSDR's install.sh adds a udev rule for Cypress "
           "04b4:00f3 and 04b4:00f1; after installing the rule, unplug the RX-888 and plug it in again.";
}

}  // namespace

Receiver::~Receiver() {
    if (device_)
        close_by(Clock::now() + std::chrono::milliseconds(500), true);
}

std::optional<Failure> Receiver::usb_failure(int code, const std::string& what) const {
    std::string message = what + ": " + usb_error_text(code) + ".";
    ErrorCode kind = ErrorCode::usb;
    if (code == usb_error::access) {
        message += permission_hint();
    } else if (code == usb_error::busy) {
        kind = ErrorCode::busy;
        message += " Another program is using the RX-888; stop it, or give this band a different device.";
    } else if (code == usb_error::no_device) {
        message += " The RX-888 was unplugged or its USB connection failed; check the cable and the port, which "
                   "must be USB 3.";
    }
    return Failure{kind, message};
}

std::optional<Failure> Receiver::load_image(const std::string& path, FirmwareImage& image) {
    if (path.empty()) {
        if (auto error = parse_firmware(embedded_firmware, embedded_firmware_size, image))
            return Failure{ErrorCode::internal, "the firmware built into this module is damaged: " + *error};
        return std::nullopt;
    }
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return Failure{ErrorCode::invalid, "module.firmware: cannot open " + path + ": " + std::strerror(errno)};
    std::vector<uint8_t> bytes;
    uint8_t buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            const int err = errno;
            ::close(fd);
            return Failure{ErrorCode::invalid, "module.firmware: cannot read " + path + ": " + std::strerror(err)};
        }
        if (n == 0)
            break;
        bytes.insert(bytes.end(), buf, buf + n);
        if (bytes.size() > max_firmware_bytes)
            break;
    }
    ::close(fd);
    if (auto error = parse_firmware(bytes.data(), bytes.size(), image))
        return Failure{ErrorCode::invalid, "module.firmware: " + path + ": " + *error};
    return std::nullopt;
}

std::optional<Failure> Receiver::select(const DeviceSelector& selector, UsbDevice& chosen) {
    Enumeration e = backend_.enumerate();
    if (e.error != 0)
        return Failure{ErrorCode::usb, "USB is not usable here: " + std::string(usb_error_text(e.error)) + ". " +
                                           e.diagnostic};
    std::sort(e.devices.begin(), e.devices.end(), port_order);
    const auto none = [&](const std::string& why) {
        return Failure{ErrorCode::no_device, why};
    };
    switch (selector.kind) {
    case DeviceSelector::Kind::only:
        if (e.devices.empty())
            return none("no RX-888 is plugged in. Check that it sits in a USB 3 port and that lsusb lists "
                        "Cypress 04b4:00f3 or 04b4:00f1");
        if (e.devices.size() > 1)
            return none(std::to_string(e.devices.size()) +
                        " FX3 devices are plugged in; set module.device to serial:, port: or index: to choose one. "
                        "fern-rx888 --list-devices lists them");
        chosen = e.devices[0];
        return std::nullopt;
    case DeviceSelector::Kind::index:
        if (selector.index >= e.devices.size())
            return none("module.device asks for index " + std::to_string(selector.index) + ", but " +
                        std::to_string(e.devices.size()) + " FX3 device" + (e.devices.size() == 1 ? " is" : "s are") +
                        " plugged in");
        chosen = e.devices[selector.index];
        return std::nullopt;
    case DeviceSelector::Kind::port:
        for (const UsbDevice& d : e.devices)
            if (d.port == selector.port) {
                chosen = d;
                return std::nullopt;
            }
        return none("no FX3 device is plugged in at USB port " + selector.port);
    case DeviceSelector::Kind::serial:
        for (const UsbDevice& d : e.devices)
            if (!d.bootloader && d.serial == selector.serial) {
                chosen = d;
                return std::nullopt;
            }
        // A running RX-888 whose serial could not be read is the likelier
        // explanation than a missing one.
        for (const UsbDevice& d : e.devices)
            if (!d.bootloader && d.error != 0)
                return *usb_failure(d.error, "no RX-888 with serial " + selector.serial +
                                                 " could be found; the one at USB port " + d.port +
                                                 " cannot be opened to read its serial");
        {
            size_t waiting = 0;
            for (const UsbDevice& d : e.devices)
                waiting += d.bootloader;
            if (waiting > 0)
                return none("no RX-888 with serial " + selector.serial + " is running; " + std::to_string(waiting) +
                            " FX3 device" + (waiting == 1 ? " waits" : "s wait") +
                            " for firmware, and a serial is only known once it runs. Select it by port: instead "
                            "(fern-rx888 --list-devices), or start it once with port:");
        }
        return none("no RX-888 with serial " + selector.serial + " is plugged in");
    }
    return none("internal: unhandled device selector");
}

std::optional<Failure> Receiver::reopen(const std::string& port, bool bootloader, std::unique_ptr<Fx3>& out,
                                         UsbDevice& found) {
    // The device leaves the bus and comes back under a new address. The
    // module's own libusb learns of it from the kernel before udev has given
    // the receiver's user access, so for a while it is listed but cannot be
    // opened: that is waited out like its absence, until the deadline.
    const Clock::time_point deadline = Clock::now() + reenumeration_timeout_;
    int last_error = 0;
    for (;;) {
        const Enumeration e = backend_.enumerate();
        for (const UsbDevice& d : e.devices) {
            if (d.port != port || d.bootloader != bootloader)
                continue;
            if (d.error != 0) {
                last_error = d.error;
                break;
            }
            const int r = backend_.open(port, bootloader, out);
            if (r == 0) {
                found = d;
                return std::nullopt;
            }
            if (r != usb_error::access && r != usb_error::not_found && r != usb_error::no_device)
                return usb_failure(r, std::string("opening the ") + (bootloader ? "FX3 bootloader" : "RX-888") +
                                          " at USB port " + port + " failed");
            last_error = r;
            break;
        }
        if (Clock::now() >= deadline)
            break;
        backend_.sleep_ms(100);
    }
    if (last_error == usb_error::access)
        return usb_failure(last_error, std::string("opening the ") + (bootloader ? "FX3 bootloader" : "RX-888") +
                                           " at USB port " + port + " failed");
    return Failure{ErrorCode::usb, std::string("the RX-888 at USB port ") + port + " did not come back " +
                                       (bootloader ? "to its bootloader" : "with its firmware running") + " within " +
                                       std::to_string((reenumeration_timeout_.count() + 999) / 1000) +
                                       " seconds. Unplug it and plug it in again; if this repeats, try another USB 3 "
                                       "port or cable"};
}

std::optional<Failure> Receiver::boot(const UsbDevice& chosen, const FirmwareImage& image) {
    UsbDevice boot_device = chosen;
    if (!chosen.bootloader) {
        // It runs firmware already. Only an RX-888 MkII's is sent back to the
        // bootloader: asked under the firmware it runs now, since the one
        // this module loads reports an MkII on any board. A device that does
        // not answer as SDDC firmware does, or names another board, is left
        // as it is.
        std::unique_ptr<Fx3> old;
        if (int r = backend_.open(chosen.port, false, old); r != 0)
            return usb_failure(r, "opening the RX-888 at USB port " + chosen.port + " failed");
        uint8_t reply[4] = {};
        const int asked = old->control_in(rx888::request::identify, 0, 0, reply, sizeof reply, rx888::control_timeout_ms);
        if (asked < 0 && asked != usb_error::pipe)
            return usb_failure(asked, "asking the device at USB port " + chosen.port + " what it is failed");
        if (asked != sizeof reply || reply[0] != rx888::board_rx888_mk2)
            return Failure{ErrorCode::no_device,
                           "the FX3 device at USB port " + chosen.port + " runs firmware that " +
                               (asked == sizeof reply ? "reports board type " + std::to_string(reply[0]) +
                                                            ", not an RX-888 MkII"
                                                      : std::string("does not answer as an RX-888's does")) +
                               ". It was left as it is; set module.device to the RX-888's port or serial"};
        const int r = old->control_out(rx888::request::reset, 0, 0, nullptr, 0, rx888::control_timeout_ms);
        old.reset();
        if (r < 0 && r != usb_error::io && r != usb_error::no_device && r != usb_error::pipe)
            return usb_failure(r, "resetting the RX-888 at USB port " + chosen.port + " failed");
    }
    {
        std::unique_ptr<Fx3> loader;
        if (chosen.bootloader) {
            if (int r = backend_.open(chosen.port, true, loader); r != 0)
                return usb_failure(r, "opening the FX3 bootloader at USB port " + chosen.port + " failed");
        } else if (auto f = reopen(chosen.port, true, loader, boot_device)) {
            return f;
        }
        if (auto error = load_firmware(*loader, image))
            return Failure{ErrorCode::usb, *error};
    }
    UsbDevice running;
    if (auto f = reopen(boot_device.port, false, device_, running))
        return f;
    identity_.port = running.port;
    identity_.serial = running.serial;
    identity_.product = running.product;
    return std::nullopt;
}

std::optional<Failure> Receiver::identify() {
    uint8_t reply[4] = {};
    const int r = device_->control_in(rx888::request::identify, 0, 0, reply, sizeof reply, rx888::control_timeout_ms);
    if (r < 0)
        return usb_failure(r, "asking the RX-888 what it is failed");
    if (r != sizeof reply)
        return Failure{ErrorCode::usb, "the RX-888's firmware answered its identify request with " + std::to_string(r) +
                                           " bytes instead of 4"};
    identity_.firmware_major = reply[1];
    identity_.firmware_minor = reply[2];
    if (reply[0] != rx888::board_rx888_mk2) {
        // The built-in firmware reports an MkII on every board, so this only
        // catches an image given with module.firmware that tells them apart.
        (void)device_->control_out(rx888::request::reset, 0, 0, nullptr, 0, rx888::control_timeout_ms);
        device_.reset();
        return Failure{ErrorCode::no_device,
                       "the FX3 device at USB port " + identity_.port + " is not an RX-888 MkII (its firmware reports "
                       "board type " + std::to_string(reply[0]) +
                       "); this module drives the MkII only. It was sent back to its bootloader"};
    }
    return std::nullopt;
}

std::optional<Failure> Receiver::write_gpio() {
    uint8_t word[4];
    put_u32(word, rx888::gpio_word(board_));
    const int r = device_->control_out(rx888::request::gpio, 0, 0, word, sizeof word, rx888::control_timeout_ms);
    if (r < 0)
        return usb_failure(r, "setting the RX-888's switches failed");
    return std::nullopt;
}

std::optional<Failure> Receiver::set_argument(uint16_t argument, uint16_t value, const char* what) {
    // The firmware reads the value from wValue; the one byte of data is there
    // because it expects a data stage.
    uint8_t pad = 0;
    const int r = device_->control_out(rx888::request::set_argument, value, argument, &pad, 1, rx888::control_timeout_ms);
    if (r < 0)
        return usb_failure(r, std::string("setting the RX-888's ") + what + " failed");
    return std::nullopt;
}

std::optional<Failure> Receiver::set_vga(uint8_t byte) {
    if (auto f = set_argument(rx888::argument::vga, byte, "gain"))
        return f;
    effective_.vga = byte;
    effective_.gain_db = rx888::vga_db(byte);
    return std::nullopt;
}

std::optional<Failure> Receiver::open(const OpenRequest& request) {
    const ModuleSettings& s = request.settings;
    FirmwareImage image;
    if (auto f = load_image(s.firmware, image))
        return f;

    UsbDevice chosen;
    if (auto f = select(s.device, chosen))
        return f;
    if (chosen.error != 0 && !chosen.bootloader)
        return usb_failure(chosen.error, "the RX-888 at USB port " + chosen.port + " cannot be opened");
    log_line("loading %s firmware into the FX3 at USB port %s", s.firmware.empty() ? "the built-in" : s.firmware.c_str(),
             chosen.port.c_str());
    if (auto f = boot(chosen, image))
        return f;
    if (auto f = identify())
        return f;

    board_ = rx888::Board{};
    board_.adc_on = true;
    board_.bias_tee = s.bias_tee;
    board_.dither = s.dither;
    board_.randomizer = s.randomizer;
    board_.wide_range = s.adc_range == AdcRange::wide;
    if (auto f = write_gpio())
        return f;

    const uint16_t att = rx888::attenuator_code(s.attenuation);
    if (auto f = set_argument(rx888::argument::attenuator, att, "attenuator"))
        return f;
    effective_.attenuation = rx888::attenuator_db(att);

    gain_bytes_ = rx888::vga_steps();
    gain_steps_tenths_.clear();
    for (uint8_t b : gain_bytes_)
        gain_steps_tenths_.push_back(static_cast<int>(std::lround(rx888::vga_db(b) * 10)));
    effective_.gain = s.gain;
    if (s.gain.automatic()) {
        gain_step_ = 0;
        for (size_t i = 0; i < gain_bytes_.size(); ++i)
            if (std::fabs(rx888::vga_db(gain_bytes_[i]) - default_gain_db) <
                std::fabs(rx888::vga_db(gain_bytes_[gain_step_]) - default_gain_db))
                gain_step_ = i;
        if (auto f = set_vga(gain_bytes_[gain_step_]))
            return f;
    } else if (auto f = set_vga(rx888::vga_byte(s.gain.db))) {
        return f;
    }

    uint8_t rate[4];
    put_u32(rate, request.sample_rate);
    if (int r = device_->control_out(rx888::request::start_adc, 0, 0, rate, sizeof rate, rx888::control_timeout_ms);
        r < 0)
        return usb_failure(r, "starting the RX-888's converter clock at " + std::to_string(request.sample_rate) +
                                  " Hz failed");
    effective_.sample_rate = rx888::achieved_sample_rate(request.sample_rate);

    uint8_t start[4] = {};
    if (int r = device_->control_out(rx888::request::start_stream, 0, 0, start, sizeof start, rx888::control_timeout_ms);
        r < 0)
        return usb_failure(r, "starting the RX-888's sample stream failed");

    effective_.bias_tee = s.bias_tee;
    effective_.dither = s.dither;
    effective_.randomizer = s.randomizer;
    effective_.adc_range = s.adc_range;
    effective_.firmware = s.firmware;
    effective_.transfers = s.transfers;
    log_line("RX-888 MkII %s at USB port %s, firmware %u.%u, %.0f Hz, gain %s dB%s, attenuation %s dB",
             identity_.serial.c_str(), identity_.port.c_str(), identity_.firmware_major, identity_.firmware_minor,
             effective_.sample_rate, db_text(effective_.gain_db).c_str(), s.gain.automatic() ? " (auto)" : "",
             db_text(effective_.attenuation).c_str());
    return std::nullopt;
}

std::optional<Failure> Receiver::apply(const LiveChange& change) {
    if (!device_)
        return Failure{ErrorCode::internal, "the RX-888 is not open"};
    if (change.gain) {
        effective_.gain = *change.gain;
        if (change.gain->automatic()) {
            // The control starts from the step nearest to the gain in use.
            size_t nearest = 0;
            for (size_t i = 0; i < gain_bytes_.size(); ++i)
                if (std::fabs(rx888::vga_db(gain_bytes_[i]) - effective_.gain_db) <
                    std::fabs(rx888::vga_db(gain_bytes_[nearest]) - effective_.gain_db))
                    nearest = i;
            gain_step_ = nearest;
            if (auto f = set_vga(gain_bytes_[gain_step_]))
                return f;
        } else if (auto f = set_vga(rx888::vga_byte(change.gain->db))) {
            return f;
        }
    }
    if (change.attenuation) {
        const uint16_t att = rx888::attenuator_code(*change.attenuation);
        if (auto f = set_argument(rx888::argument::attenuator, att, "attenuator"))
            return f;
        effective_.attenuation = rx888::attenuator_db(att);
    }
    if (change.bias_tee || change.dither) {
        rx888::Board next = board_;
        if (change.bias_tee)
            next.bias_tee = *change.bias_tee;
        if (change.dither)
            next.dither = *change.dither;
        const rx888::Board before = board_;
        board_ = next;
        if (auto f = write_gpio()) {
            board_ = before;
            return f;
        }
        effective_.bias_tee = board_.bias_tee;
        effective_.dither = board_.dither;
    }
    return std::nullopt;
}

std::optional<Failure> Receiver::set_gain_step(size_t step) {
    if (step >= gain_bytes_.size())
        return Failure{ErrorCode::internal, "gain step out of range"};
    if (auto f = set_vga(gain_bytes_[step]))
        return f;
    gain_step_ = step;
    return std::nullopt;
}

json::Value Receiver::device_json() const {
    json::Value d = json::Value::object();
    d.set("name", "RX-888 MkII");
    d.set("serial", identity_.serial);
    d.set("port", identity_.port);
    char version[16];
    std::snprintf(version, sizeof version, "%u.%u", identity_.firmware_major, identity_.firmware_minor);
    d.set("firmware", effective_.firmware.empty() ? std::string("built in (ringof/rx888-firmware 0.1.0)")
                                                  : effective_.firmware);
    d.set("firmware_version", version);
    return d;
}

json::Value Receiver::settings_json() const {
    json::Value s = json::Value::object();
    s.set("gain", effective_.gain.automatic() ? json::Value("auto") : json::Value(db_text(effective_.gain_db)));
    s.set("gain_db", std::round(effective_.gain_db * 10) / 10);
    s.set("attenuation", effective_.attenuation);
    s.set("bias_tee", effective_.bias_tee);
    s.set("dither", effective_.dither);
    s.set("randomizer", effective_.randomizer);
    s.set("adc_range", adc_range_name(effective_.adc_range));
    s.set("firmware", effective_.firmware);
    s.set("transfers", effective_.transfers);
    return s;
}

json::Value Receiver::settings_json(const LiveChange& change) const {
    json::Value s = json::Value::object();
    if (change.gain) {
        s.set("gain", effective_.gain.automatic() ? json::Value("auto") : json::Value(db_text(effective_.gain_db)));
        s.set("gain_db", std::round(effective_.gain_db * 10) / 10);
    }
    if (change.attenuation)
        s.set("attenuation", effective_.attenuation);
    if (change.bias_tee)
        s.set("bias_tee", effective_.bias_tee);
    if (change.dither)
        s.set("dither", effective_.dither);
    return s;
}

bool Receiver::close_by(Clock::time_point deadline, bool stop_hardware) {
    if (!device_)
        return true;
    if (stop_hardware) {
        // Stream off, clock off, converter shut down and the LED out: an idle
        // RX-888 left running gets hot.
        const auto left = [&] {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            return static_cast<unsigned>(std::max<long long>(ms, 0));
        };
        uint8_t zero[4] = {};
        if (left() > 0)
            (void)device_->control_out(rx888::request::stop_stream, 0, 0, zero, sizeof zero, std::min(left(), 1000u));
        if (left() > 0)
            (void)device_->control_out(rx888::request::start_adc, 0, 0, zero, sizeof zero, std::min(left(), 1000u));
        board_.adc_on = false;
        uint8_t word[4];
        put_u32(word, rx888::gpio_word(board_));
        if (left() > 0)
            (void)device_->control_out(rx888::request::gpio, 0, 0, word, sizeof word, std::min(left(), 1000u));
    }
    if (Clock::now() >= deadline && stop_hardware) {
        abandon();
        return false;
    }
    device_.reset();
    return true;
}

void Receiver::abandon() {
    Fx3* left = device_.release();
#if defined(__SANITIZE_ADDRESS__)
    // Never freed, on purpose: closing a device that stopped answering can
    // hang. The leak checker is told so, and only about this object.
    if (left)
        __lsan_ignore_object(left);
#endif
    (void)left;
}

}  // namespace fern
