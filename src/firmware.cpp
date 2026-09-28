// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "firmware.h"

#include <algorithm>

#include "rx888.h"

namespace fern {

namespace {

uint32_t word_at(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

}  // namespace

std::optional<std::string> parse_firmware(const uint8_t* data, size_t size, FirmwareImage& out) {
    if (size > max_firmware_bytes)
        return std::string("the firmware file is larger than any FX3 image can be");
    if (size < 4 || data[0] != 'C' || data[1] != 'Y')
        return std::string("the firmware file is not an FX3 image: it does not start with CY");
    if (data[3] != 0xB0)
        return std::string("the firmware file is not a plain FX3 executable image (type byte is not 0xB0)");
    FirmwareImage image;
    uint32_t sum = 0;
    size_t pos = 4;
    for (;;) {
        if (size - pos < 8)
            return std::string("the firmware file ends in the middle of a section header");
        const uint32_t words = word_at(data + pos);
        const uint32_t address = word_at(data + pos + 4);
        pos += 8;
        if (words == 0) {
            image.entry = address;
            break;
        }
        if (address % 4 != 0)
            return std::string("the firmware file has a section at an address that is not word-aligned");
        if (words > (size - pos) / 4)
            return std::string("the firmware file ends in the middle of a section");
        const size_t bytes = static_cast<size_t>(words) * 4;
        if (static_cast<uint64_t>(address) + bytes > 0x100000000ull)
            return std::string("the firmware file has a section past the end of the address space");
        FirmwareSection section;
        section.address = address;
        section.data.assign(data + pos, data + pos + bytes);
        for (size_t i = 0; i < bytes; i += 4)
            sum += word_at(data + pos + i);
        pos += bytes;
        image.sections.push_back(std::move(section));
    }
    if (image.sections.empty())
        return std::string("the firmware file holds no sections");
    if (size - pos != 4)
        return std::string(size - pos < 4 ? "the firmware file ends before its checksum"
                                          : "the firmware file carries data after its checksum");
    if (word_at(data + pos) != sum)
        return std::string("the firmware file's checksum does not match its contents: it is damaged");
    out = std::move(image);
    return std::nullopt;
}

std::optional<std::string> load_firmware(Fx3& bootloader, const FirmwareImage& image) {
    for (const FirmwareSection& section : image.sections) {
        for (size_t offset = 0; offset < section.data.size(); offset += firmware_chunk_bytes) {
            const size_t len = std::min<size_t>(firmware_chunk_bytes, section.data.size() - offset);
            const uint32_t address = section.address + static_cast<uint32_t>(offset);
            const int r = bootloader.control_out(rx888::request::firmware_load, static_cast<uint16_t>(address & 0xffff),
                                                 static_cast<uint16_t>(address >> 16), section.data.data() + offset,
                                                 static_cast<uint16_t>(len), rx888::control_timeout_ms);
            if (r != static_cast<int>(len))
                return std::string("loading the firmware into the RX-888 failed: ") +
                       (r < 0 ? usb_error_text(r) : "the bootloader took only part of a block");
        }
    }
    // The bootloader may leave the bus before it acknowledges the jump: an
    // I/O error or a vanished device here is the firmware starting.
    const int r = bootloader.control_out(rx888::request::firmware_load, static_cast<uint16_t>(image.entry & 0xffff),
                                         static_cast<uint16_t>(image.entry >> 16), nullptr, 0,
                                         rx888::control_timeout_ms);
    if (r < 0 && r != usb_error::io && r != usb_error::no_device && r != usb_error::pipe)
        return std::string("starting the RX-888 firmware failed: ") + usb_error_text(r);
    return std::nullopt;
}

}  // namespace fern
