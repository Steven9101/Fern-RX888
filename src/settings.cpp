// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "settings.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <string_view>
#include <vector>

#include "rx888.h"

#ifndef FERN_RX888_VERSION
#error "FERN_RX888_VERSION must be defined by the build"
#endif

namespace fern {

const char* module_version() { return FERN_RX888_VERSION; }

const char* adc_range_name(AdcRange range) { return range == AdcRange::wide ? "2.25" : "1.5"; }

namespace {

enum class Key { device, gain, attenuation, bias_tee, dither, randomizer, adc_range, firmware, transfers };

struct KeyInfo {
    Key key;
    const char* name;
    bool live;
};

// The order here is the order of --describe.
constexpr KeyInfo all_keys[] = {
    {Key::device, "device", false},
    {Key::gain, "gain", true},
    {Key::attenuation, "attenuation", true},
    {Key::bias_tee, "bias_tee", true},
    {Key::dither, "dither", true},
    {Key::randomizer, "randomizer", false},
    {Key::adc_range, "adc_range", false},
    {Key::firmware, "firmware", false},
    {Key::transfers, "transfers", false},
};

const KeyInfo* find_key(std::string_view name) {
    for (const KeyInfo& k : all_keys)
        if (name == k.name)
            return &k;
    return nullptr;
}

std::string known_keys() {
    std::string s;
    const size_t n = sizeof all_keys / sizeof all_keys[0];
    for (size_t i = 0; i < n; ++i) {
        if (i > 0)
            s += i + 1 == n ? " and " : ", ";
        s += all_keys[i].name;
    }
    return s;
}

// A value as the operator would recognise it in a message, kept short.
std::string shown(const json::Value& v) {
    std::string s = json::serialize(v);
    if (s.size() > 60)
        s = s.substr(0, 57) + "...";
    return s;
}

std::optional<Failure> invalid(std::string message) {
    return Failure{ErrorCode::invalid, std::move(message)};
}

bool whole_in_range(const json::Value& v, double lo, double hi, double& out) {
    if (!v.is_number() || !json::is_whole(v.as_number()))
        return false;
    const double d = v.as_number();
    if (d < lo || d > hi)
        return false;
    out = d;
    return true;
}

bool hex_digits(std::string_view s) {
    if (s.empty() || s.size() > 32)
        return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

bool port_path(std::string_view s) {
    // "2-1.4": a bus number, a dash, then port numbers separated by dots.
    if (s.empty() || s.size() > 32)
        return false;
    bool dash = false;
    char previous = '-';
    for (char c : s) {
        if (c == '-') {
            if (dash || previous == '-')
                return false;
            dash = true;
        } else if (c == '.') {
            if (!dash || previous == '.' || previous == '-')
                return false;
        } else if (c < '0' || c > '9') {
            return false;
        }
        previous = c;
    }
    return dash && previous != '-' && previous != '.';
}

std::optional<Failure> parse_device(const json::Value& v, DeviceSelector& out) {
    const std::string wrong = "module.device must be serial:<serial>, index:<n>, port:<bus-port> or empty, not ";
    if (!v.is_string())
        return invalid(wrong + shown(v));
    const std::string& s = v.as_string();
    DeviceSelector sel;
    if (s.empty()) {
        sel.kind = DeviceSelector::Kind::only;
    } else if (s.rfind("serial:", 0) == 0 && hex_digits(std::string_view(s).substr(7))) {
        sel.kind = DeviceSelector::Kind::serial;
        sel.serial = s.substr(7);
        for (char& c : sel.serial)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    } else if (s.rfind("port:", 0) == 0 && port_path(std::string_view(s).substr(5))) {
        sel.kind = DeviceSelector::Kind::port;
        sel.port = s.substr(5);
    } else if (s.rfind("index:", 0) == 0 && s.size() > 6 && s.size() <= 16) {
        uint64_t n = 0;
        for (char c : std::string_view(s).substr(6)) {
            if (c < '0' || c > '9')
                return invalid(wrong + shown(v));
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        if (n > UINT32_MAX)
            return invalid(wrong + shown(v));
        sel.kind = DeviceSelector::Kind::index;
        sel.index = static_cast<uint32_t>(n);
    } else {
        return invalid(wrong + shown(v));
    }
    out = sel;
    return std::nullopt;
}

bool same_word(const std::string& s, const char* word) {
    size_t i = 0;
    for (; word[i] != '\0'; ++i)
        if (i >= s.size() || std::tolower(static_cast<unsigned char>(s[i])) != word[i])
            return false;
    return i == s.size();
}

// A number of dB given as a JSON number or as text, as the schema's string
// type delivers it from the configuration.
bool decibels(const json::Value& v, double& out) {
    if (v.is_number()) {
        out = v.as_number();
        return std::isfinite(out);
    }
    if (!v.is_string() || v.as_string().empty())
        return false;
    const std::string& s = v.as_string();
    const char* last = s.data() + s.size();
    const auto res = std::from_chars(s.data(), last, out, std::chars_format::fixed);
    return res.ec == std::errc() && res.ptr == last && std::isfinite(out);
}

std::optional<Failure> parse_gain(const json::Value& v, GainSetting& out) {
    char range[160];
    std::snprintf(range, sizeof range, "module.gain must be auto or a gain in dB from %.0f to %.0f, such as 12, not ",
                  rx888::min_vga_db, rx888::max_vga_db);
    if (v.is_string() && same_word(v.as_string(), "auto")) {
        out = GainSetting();
        return std::nullopt;
    }
    double db;
    if (!decibels(v, db) || db < rx888::min_vga_db || db > rx888::max_vga_db)
        return invalid(range + shown(v));
    out = GainSetting::manual(db);
    return std::nullopt;
}

std::optional<Failure> parse_bool(const char* key, const json::Value& v, bool& out) {
    if (!v.is_bool())
        return invalid(std::string("module.") + key + " must be yes or no (a JSON boolean), not " + shown(v));
    out = v.as_bool();
    return std::nullopt;
}

std::optional<Failure> parse_value(const KeyInfo& k, const json::Value& v, ModuleSettings& s) {
    double d;
    switch (k.key) {
    case Key::device:
        return parse_device(v, s.device);
    case Key::gain:
        return parse_gain(v, s.gain);
    case Key::attenuation:
        if (!v.is_number() || !std::isfinite(v.as_number()) || v.as_number() < 0 ||
            v.as_number() > rx888::max_attenuation_db ||
            std::fabs(v.as_number() * 2 - std::round(v.as_number() * 2)) > 1e-9)
            return invalid("module.attenuation must be 0 to 31.5 dB in steps of 0.5, not " + shown(v));
        s.attenuation = v.as_number();
        return std::nullopt;
    case Key::bias_tee:
        return parse_bool(k.name, v, s.bias_tee);
    case Key::dither:
        return parse_bool(k.name, v, s.dither);
    case Key::randomizer:
        return parse_bool(k.name, v, s.randomizer);
    case Key::adc_range:
        if (v.is_string() && v.as_string() == "1.5")
            s.adc_range = AdcRange::narrow;
        else if (v.is_string() && v.as_string() == "2.25")
            s.adc_range = AdcRange::wide;
        else
            return invalid("module.adc_range must be 1.5 or 2.25, not " + shown(v));
        return std::nullopt;
    case Key::firmware:
        if (!v.is_string() || (!v.as_string().empty() && v.as_string()[0] != '/') ||
            v.as_string().find('\0') != std::string::npos || v.as_string().size() > 4096)
            return invalid("module.firmware must be empty, for the firmware this module carries, or the full "
                           "path of an FX3 image, not " +
                           shown(v));
        s.firmware = v.as_string();
        return std::nullopt;
    case Key::transfers:
        if (!whole_in_range(v, min_transfers, max_transfers, d))
            return invalid("module.transfers must be a whole number from " + std::to_string(min_transfers) + " to " +
                           std::to_string(max_transfers) + ", not " + shown(v));
        s.transfers = static_cast<uint32_t>(d);
        return std::nullopt;
    }
    return invalid("internal: unhandled setting");
}

// Unknown keys are refused all at once, so that the operator can fix every
// one of them in one go.
std::optional<Failure> check_keys(const json::Value& settings) {
    std::vector<std::string> unknown;
    for (const json::Member& m : settings.members())
        if (!find_key(m.key))
            unknown.push_back(m.key);
    if (unknown.empty())
        return std::nullopt;
    std::string names;
    for (size_t i = 0; i < unknown.size() && i < 8; ++i) {
        if (i > 0)
            names += ", ";
        const std::string& key = unknown[i];
        names += "module." + (key.size() > 40 ? key.substr(0, 37) + "..." : key);
    }
    if (unknown.size() > 8)
        names += " and " + std::to_string(unknown.size() - 8) + " more";
    return invalid(std::string(unknown.size() == 1 ? "unknown setting " : "unknown settings ") + names +
                   "; this module takes " + known_keys());
}

}  // namespace

std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out) {
    OpenRequest req;
    double d;

    const json::Value* signal = message.find("signal");
    if (!signal)
        return invalid("open carries no signal");
    if (!signal->is_string() || (signal->as_string() != "iq" && signal->as_string() != "real"))
        return invalid("signal must be iq or real, not " + shown(*signal));
    if (signal->as_string() != "real")
        return invalid("an RX-888 samples the antenna directly and delivers real samples; set signal = real "
                       "for this band");

    const json::Value* rate = message.find("sample_rate");
    if (!rate)
        return invalid("open carries no sample_rate");
    if (!whole_in_range(*rate, rx888::min_sample_rate, rx888::max_sample_rate, d))
        return invalid("sample_rate " + shown(*rate) +
                       " Hz is not possible with an RX-888 MkII; use a whole number of Hz from 10000000 to "
                       "130000000, such as 64800000 for 0 to 32 MHz or 129600000 for 0 to 64 MHz");
    req.sample_rate = static_cast<uint32_t>(d);

    const json::Value* center = message.find("center");
    if (!center)
        return invalid("open carries no center");
    if (!center->is_number() || center->as_number() != 0)
        return invalid("an RX-888 samples from 0 Hz up: set center = 0 for this band (the band's own offset, "
                       "not the module, moves it), not " +
                       shown(*center));

    const json::Value* settings = message.find("settings");
    if (settings) {
        if (!settings->is_object())
            return invalid("settings in open must be an object, not " + shown(*settings));
        if (auto f = check_keys(*settings))
            return f;
        for (const json::Member& m : settings->members())
            if (auto f = parse_value(*find_key(m.key), m.value, req.settings))
                return f;
    }
    out = req;
    return std::nullopt;
}

std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out) {
    if (!settings.is_object())
        return invalid("settings in set must be an object, not " + shown(settings));
    if (auto f = check_keys(settings))
        return f;
    LiveChange change;
    ModuleSettings scratch;
    for (const json::Member& m : settings.members()) {
        const KeyInfo& k = *find_key(m.key);
        if (!k.live)
            return invalid("module." + m.key + " cannot change while the band runs; restart the band to apply it");
        if (auto f = parse_value(k, m.value, scratch))
            return f;
        switch (k.key) {
        case Key::gain: change.gain = scratch.gain; break;
        case Key::attenuation: change.attenuation = scratch.attenuation; break;
        case Key::bias_tee: change.bias_tee = scratch.bias_tee; break;
        case Key::dither: change.dither = scratch.dither; break;
        default: break;
        }
    }
    out = change;
    return std::nullopt;
}

json::Value settings_schema() {
    json::Value list = json::Value::array();
    for (const KeyInfo& k : all_keys) {
        json::Value s = json::Value::object();
        s.set("key", k.name);
        switch (k.key) {
        case Key::device:
            s.set("type", "string");
            s.set("label", "Device");
            s.set("default", "");
            s.set("help",
                  "Which RX-888 to use: serial:<serial>, port:<bus-port> or index:<n>. Leave empty when exactly "
                  "one is plugged in. With several, use port:, which names the USB socket and is the one "
                  "choice known before the firmware runs: a device without firmware has no serial, and every "
                  "FX3 board looks the same then, so make sure the port holds an RX-888 MkII. "
                  "fern-rx888 --list-devices shows them.");
            break;
        case Key::gain:
            s.set("type", "string");
            s.set("label", "Gain");
            s.set("default", "auto");
            s.set("help",
                  "The VGA in front of the converter. auto chooses the highest gain that keeps the converter "
                  "out of clipping with 6 dB to spare, and lowers it at once when something clips; or a gain "
                  "in dB from -25 to 34, of which the nearest the VGA supports is used and reported.");
            break;
        case Key::attenuation:
            s.set("type", "number");
            s.set("label", "Attenuation");
            s.set("default", 0);
            s.set("min", 0);
            s.set("max", rx888::max_attenuation_db);
            s.set("unit", "dB");
            s.set("help",
                  "The step attenuator at the HF input, 0 to 31.5 dB in 0.5 dB steps. For signals so strong "
                  "that even the lowest gain clips.");
            break;
        case Key::bias_tee:
            s.set("type", "boolean");
            s.set("label", "Bias tee");
            s.set("default", false);
            s.set("help", "Puts DC on the HF antenna input to power an active antenna or LNA.");
            break;
        case Key::dither:
            s.set("type", "boolean");
            s.set("label", "Dither");
            s.set("default", false);
            s.set("help",
                  "The converter's internal dither: fewer spurs from a quiet band at the cost of a slightly "
                  "higher noise floor.");
            break;
        case Key::randomizer:
            s.set("type", "boolean");
            s.set("label", "Randomizer");
            s.set("default", false);
            s.set("advanced", true);
            s.set("help",
                  "Scrambles the converter's output bits on the board and unscrambles them in the module, "
                  "which keeps the data lines from coupling back into the input.");
            break;
        case Key::adc_range:
            s.set("type", "choice");
            s.set("label", "Converter input range");
            s.set("choices", json::Value::array().push("1.5").push("2.25"));
            s.set("default", "1.5");
            s.set("unit", "Vpp");
            s.set("advanced", true);
            s.set("help", "1.5 Vpp gives 3.5 dB more gain; 2.25 Vpp more headroom.");
            break;
        case Key::firmware:
            s.set("type", "string");
            s.set("label", "Firmware");
            s.set("default", "");
            s.set("advanced", true);
            s.set("help",
                  "Full path of an FX3 firmware image to load instead of the one this module carries "
                  "(ringof/rx888-firmware 0.1.0). It must speak the same commands.");
            break;
        case Key::transfers:
            s.set("type", "number");
            s.set("label", "USB transfers");
            s.set("default", default_transfers);
            s.set("min", min_transfers);
            s.set("max", max_transfers);
            s.set("advanced", true);
            s.set("help",
                  "Number of 512 KiB USB transfers kept in flight, 4 ms of samples each at 64.8 MHz. More of "
                  "them ride out longer pauses of a busy host. At most 24, so that they stay inside the 16 MB Linux "
                  "gives USB transfers by default.");
            break;
        }
        s.set("live", k.live);
        list.push(std::move(s));
    }
    return list;
}

json::Value describe_module() {
    json::Value d = json::Value::object();
    d.set("api", module_api);
    d.set("id", module_id);
    d.set("name", module_name);
    d.set("version", module_version());
    d.set("kind", "input");
    d.set("settings", settings_schema());
    return d;
}

}  // namespace fern
