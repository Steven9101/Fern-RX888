// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// The FX3 firmware image and the bootloader's way of taking one.
//
// An image starts with 'C', 'Y', a control byte and the type 0xB0 (a normal
// executable image). Sections follow, each a length in 32-bit words, a RAM
// address and that many words; a section of length 0 ends the list and its
// address is the entry point. A last word holds the sum of all data words,
// wrapping. Everything is little-endian.
//
// The bootloader takes a section with vendor request 0xA0 carrying at most
// 4096 bytes, the address split over wValue (low half) and wIndex (high
// half), and starts the firmware on a 0xA0 without data at the entry point.
// It then leaves the bus and the firmware enumerates in its place.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "fx3.h"

namespace fern {

struct FirmwareSection {
    uint32_t address = 0;
    std::vector<uint8_t> data;
};

struct FirmwareImage {
    std::vector<FirmwareSection> sections;
    uint32_t entry = 0;
};

// The FX3's RAM is 512 KiB; an image bigger than this is not one.
constexpr size_t max_firmware_bytes = 1u << 20;
constexpr uint16_t firmware_chunk_bytes = 4096;

// Checks and parses an image. An error text for the operator on failure.
std::optional<std::string> parse_firmware(const uint8_t* data, size_t size, FirmwareImage& out);

// Writes every section to the bootloader and starts the firmware. An error
// text on failure; the jump itself may be cut short by the device leaving
// the bus, which is not one.
std::optional<std::string> load_firmware(Fx3& bootloader, const FirmwareImage& image);

// The image this module carries (firmware/SDDC_FX3.img), its SHA-256 and
// the notices that go with it; generated at build time.
extern const uint8_t embedded_firmware[];
extern const size_t embedded_firmware_size;
extern const char embedded_firmware_sha256[];
extern const char embedded_firmware_notices[];

}  // namespace fern
