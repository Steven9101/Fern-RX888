// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Finds the RX-888 the settings select, loads the firmware into it, checks
// that it is an RX-888 MkII, programs the board and starts the converter.
//
// The firmware is loaded on every open, also into a device that already runs
// one: another program may have left its own there, and the module has to
// know which commands it is speaking to. A device that runs firmware is asked
// what it is first, and one that does not answer as an RX-888 MkII is left
// alone. A device still in the FX3 bootloader cannot say what it is: every
// FX3 board without firmware looks the same, so module.device must not
// select one that is not an RX-888.
#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "failure.h"
#include "firmware.h"
#include "fx3.h"
#include "json.h"
#include "rx888.h"
#include "settings.h"

namespace fern {

// What the hardware was actually set to.
struct Effective {
    double sample_rate = 0;
    GainSetting gain;
    uint8_t vga = 0;       // the AD8370 byte in use
    double gain_db = 0;    // what that byte gives
    double attenuation = 0;
    bool bias_tee = false;
    bool dither = false;
    bool randomizer = false;
    AdcRange adc_range = AdcRange::narrow;
    std::string firmware;  // the path, or empty for the built-in image
    uint32_t transfers = default_transfers;
};

struct Identity {
    std::string port;
    std::string serial;
    std::string product;
    uint8_t firmware_major = 0;
    uint8_t firmware_minor = 0;
};

// How long a device may take to leave the bus and come back after a reset or
// a firmware load.
constexpr std::chrono::milliseconds reenumeration_timeout{6000};

class Receiver {
public:
    explicit Receiver(Backend& backend) : backend_(backend) {}
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Everything up to a running converter and a started stream on the FX3
    // side; the caller then collects the samples with device().stream().
    std::optional<Failure> open(const OpenRequest& request);

    // Applies a set. Validation failures change nothing; a failure with
    // ErrorCode::usb may leave the change half done.
    std::optional<Failure> apply(const LiveChange& change);

    // For gain = auto: the VGA steps in tenths of a dB, ascending, and the
    // index in use.
    const std::vector<int>& gain_steps() const { return gain_steps_tenths_; }
    size_t gain_step() const { return gain_step_; }
    std::optional<Failure> set_gain_step(size_t step);

    const Effective& effective() const { return effective_; }
    const Identity& identity() const { return identity_; }
    Fx3& device() { return *device_; }
    bool is_open() const { return device_ != nullptr; }

    json::Value device_json() const;
    // The effective settings; with a change, only the keys it carried.
    json::Value settings_json() const;
    json::Value settings_json(const LiveChange& change) const;

    // Stops the converter and closes the device, unless the deadline has
    // passed. stop_hardware false skips the control requests, for a device
    // that no longer answers. False when it did not get to close.
    bool close_by(std::chrono::steady_clock::time_point deadline, bool stop_hardware);
    // Forgets the device without touching it; the kernel releases it when the
    // process exits.
    void abandon();

    // For tests, whose fake devices never come back.
    void set_reenumeration_timeout(std::chrono::milliseconds t) { reenumeration_timeout_ = t; }

private:
    std::optional<Failure> load_image(const std::string& path, FirmwareImage& image);
    std::optional<Failure> select(const DeviceSelector& selector, UsbDevice& chosen);
    // Waits for the device at a port to come back in the given state and
    // opens it.
    std::optional<Failure> reopen(const std::string& port, bool bootloader, std::unique_ptr<Fx3>& out, UsbDevice& found);
    // Loads the firmware and opens the device running it into device_.
    std::optional<Failure> boot(const UsbDevice& device, const FirmwareImage& image);
    std::optional<Failure> identify();
    std::optional<Failure> write_gpio();
    std::optional<Failure> set_argument(uint16_t argument, uint16_t value, const char* what);
    std::optional<Failure> set_vga(uint8_t byte);
    std::optional<Failure> usb_failure(int code, const std::string& what) const;

    Backend& backend_;
    std::unique_ptr<Fx3> device_;
    Identity identity_;
    Effective effective_;
    rx888::Board board_;
    std::vector<uint8_t> gain_bytes_;
    std::vector<int> gain_steps_tenths_;
    size_t gain_step_ = 0;
    std::chrono::milliseconds reenumeration_timeout_ = reenumeration_timeout;
};

}  // namespace fern
