// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "fx3.h"
#include "json.h"

namespace fern {

// FernSDR reads at most this much from --describe and --list-devices.
constexpr size_t max_report_bytes = 64 * 1024;

// Lists the FX3 devices without opening them for anything but their strings
// and without loading firmware: listing must not change what is plugged in.
// A device still waiting for firmware has no serial yet, only its port.
json::Value list_devices(Backend& backend);

// Serializes a report, dropping devices until it fits into max_report_bytes
// including the newline.
std::string report_text(const json::Value& report);

}  // namespace fern
