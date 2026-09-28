// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The settings the module declares (printed by --describe and copied into
// the package manifest) and the validation of open and set against them.
// Validation here needs no hardware; what depends on the device happens in
// receiver.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "failure.h"
#include "json.h"

namespace fern {

constexpr const char* module_id = "rx888";
constexpr const char* module_name = "RX-888";
constexpr int module_api = 1;
const char* module_version();

struct DeviceSelector {
    enum class Kind { only, serial, index, port };
    Kind kind = Kind::only;
    std::string serial;  // upper-case hex
    std::string port;
    uint32_t index = 0;
};

struct GainSetting {
    // automatic: the module's own control of the VGA (gain_control.h);
    // manual: db.
    enum class Mode { automatic, manual };
    Mode mode = Mode::automatic;
    double db = 0;

    static GainSetting manual(double gain_db) { return GainSetting{Mode::manual, gain_db}; }
    bool automatic() const { return mode == Mode::automatic; }
};

enum class AdcRange { narrow, wide };  // 1.5 Vpp, 2.25 Vpp
const char* adc_range_name(AdcRange range);

constexpr double default_gain_db = 10.0;  // where gain = auto starts
constexpr uint32_t min_transfers = 4;
// 24 of 512 KiB are 12 MiB, inside the 16 MiB Linux gives all programs'
// USB transfers together unless usbcore.usbfs_memory_mb is raised.
constexpr uint32_t max_transfers = 24;
constexpr uint32_t default_transfers = 16;
constexpr uint32_t transfer_bytes = 512 * 1024;

struct ModuleSettings {
    DeviceSelector device;
    GainSetting gain;
    double attenuation = 0;  // dB
    bool bias_tee = false;
    bool dither = false;
    bool randomizer = false;
    AdcRange adc_range = AdcRange::narrow;
    std::string firmware;  // a file, or empty for the one the module carries
    uint32_t transfers = default_transfers;
};

struct OpenRequest {
    uint32_t sample_rate = 0;
    ModuleSettings settings;
};

// The settings that may change while samples flow.
struct LiveChange {
    std::optional<GainSetting> gain;
    std::optional<double> attenuation;
    std::optional<bool> bias_tee;
    std::optional<bool> dither;
};

// Validates an open message. Fails with ErrorCode::invalid.
std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out);

// Validates the settings object of a set message: only live settings may
// appear. Fails with ErrorCode::invalid.
std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out);

// The settings list, identical in --describe and in the package manifest.
json::Value settings_schema();
// The complete --describe object.
json::Value describe_module();

}  // namespace fern
