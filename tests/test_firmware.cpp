// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstring>
#include <string>
#include <vector>

#include "fake_fx3.h"
#include "firmware.h"
#include "rx888.h"
#include "test.h"

using fern::FirmwareImage;

namespace {

void put(std::vector<uint8_t>& v, uint32_t w) {
    for (int i = 0; i < 4; ++i)
        v.push_back(static_cast<uint8_t>(w >> (8 * i)));
}

// An image with the given sections (address, words) and entry point.
std::vector<uint8_t> image(const std::vector<std::pair<uint32_t, std::vector<uint32_t>>>& sections, uint32_t entry,
                           int checksum_error = 0) {
    std::vector<uint8_t> v = {'C', 'Y', 0x1c, 0xb0};
    uint32_t sum = 0;
    for (const auto& s : sections) {
        put(v, static_cast<uint32_t>(s.second.size()));
        put(v, s.first);
        for (uint32_t w : s.second) {
            put(v, w);
            sum += w;
        }
    }
    put(v, 0);
    put(v, entry);
    put(v, sum + static_cast<uint32_t>(checksum_error));
    return v;
}

}  // namespace

TEST(the_embedded_firmware_is_ringof_0_1_0_and_parses) {
    CHECK_EQ(std::string(fern::embedded_firmware_sha256),
             std::string("f1c682293c5cb1714b75e8b8cfad0e6cfe86b5f83b987082d425404dc56e4a06"));
    FirmwareImage img;
    const auto error = fern::parse_firmware(fern::embedded_firmware, fern::embedded_firmware_size, img);
    REQUIRE(!error);
    CHECK_EQ(img.sections.size(), size_t{4});
    CHECK(img.entry != 0);
    size_t bytes = 0;
    for (const auto& s : img.sections)
        bytes += s.data.size();
    CHECK(bytes > 100000 && bytes < fern::embedded_firmware_size);
    const std::string notices = fern::embedded_firmware_notices;
    CHECK_HAS(notices, "MIT License");
    CHECK_HAS(notices, "Cypress");
    CHECK_HAS(notices, "ringof/rx888-firmware");
}

TEST(parse_firmware_reads_sections_entry_and_checksum) {
    const auto bytes = image({{0x40000000, {1, 2, 3}}, {0x40003000, {0xffffffff}}}, 0x40001234);
    FirmwareImage img;
    REQUIRE(!fern::parse_firmware(bytes.data(), bytes.size(), img));
    REQUIRE(img.sections.size() == 2);
    CHECK_EQ(img.sections[0].address, 0x40000000u);
    CHECK_EQ(img.sections[0].data.size(), size_t{12});
    CHECK_EQ(img.sections[1].address, 0x40003000u);
    CHECK_EQ(img.entry, 0x40001234u);
}

TEST(parse_firmware_refuses_damaged_or_foreign_files) {
    FirmwareImage img;
    const auto good = image({{0x40000000, {1, 2}}}, 0x40000000);
    auto check_refused = [&](std::vector<uint8_t> b, const char* why) {
        const auto error = fern::parse_firmware(b.data(), b.size(), img);
        CHECK(error.has_value());
        if (error)
            CHECK_HAS(*error, why);
    };
    auto b = good;
    b[0] = 'X';
    check_refused(b, "does not start with CY");
    b = good;
    b[3] = 0xb1;
    check_refused(b, "0xB0");
    check_refused(image({{0x40000000, {1, 2}}}, 0x40000000, 1), "checksum");
    b = good;
    b.pop_back();
    check_refused(b, "before its checksum");
    b = good;
    b.push_back(0);
    check_refused(b, "after its checksum");
    check_refused(image({{0x40000002, {1}}}, 0x40000000), "word-aligned");
    check_refused(std::vector<uint8_t>(good.begin(), good.begin() + 14), "middle of a section");
    check_refused(image({}, 0x40000000), "no sections");
    check_refused(std::vector<uint8_t>{'C', 'Y'}, "does not start with CY");
}

TEST(load_firmware_writes_4096_byte_blocks_then_jumps_to_the_entry) {
    std::vector<uint32_t> words(2500);  // 10000 bytes: blocks of 4096, 4096 and 1808
    for (size_t i = 0; i < words.size(); ++i)
        words[i] = static_cast<uint32_t>(i * 2654435761u);
    const auto bytes = image({{0x40010000, words}}, 0x40010040);
    FirmwareImage img;
    REQUIRE(!fern::parse_firmware(bytes.data(), bytes.size(), img));

    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    std::unique_ptr<fern::Fx3> loader;
    REQUIRE(usb.open("2-1", true, loader) == 0);
    CHECK(!fern::load_firmware(*loader, img));

    const auto sent = dev.taken();
    REQUIRE(sent.size() == 4);
    const uint32_t addresses[] = {0x40010000, 0x40011000, 0x40012000};
    const size_t lengths[] = {4096, 4096, 1808};
    for (int i = 0; i < 3; ++i) {
        CHECK_EQ(sent[i].request, fern::rx888::request::firmware_load);
        CHECK_EQ(sent[i].value, static_cast<uint16_t>(addresses[i] & 0xffff));
        CHECK_EQ(sent[i].index, static_cast<uint16_t>(addresses[i] >> 16));
        CHECK_EQ(sent[i].data.size(), lengths[i]);
    }
    CHECK_EQ(sent[3].value, 0x0040);
    CHECK_EQ(sent[3].index, 0x4001);
    CHECK(sent[3].data.empty());
    // What the bootloader's RAM holds is the section, byte for byte.
    bool same = true;
    for (size_t i = 0; i < img.sections[0].data.size(); ++i)
        same = same && dev.ram[0x40010000 + static_cast<uint32_t>(i)] == img.sections[0].data[i];
    CHECK(same);
    CHECK(dev.firmware_running);
    CHECK_EQ(dev.loaded_entry, 0x40010040u);
}

TEST(load_firmware_reports_a_failed_block_but_not_a_device_leaving_on_the_jump) {
    const auto bytes = image({{0x40000000, {1, 2, 3, 4}}}, 0x40000000);
    FirmwareImage img;
    REQUIRE(!fern::parse_firmware(bytes.data(), bytes.size(), img));
    fake::Backend usb;
    fake::Spec spec;
    spec.fail_request = fern::rx888::request::firmware_load;
    spec.fail_code = fern::usb_error::timeout;
    usb.add(spec);
    std::unique_ptr<fern::Fx3> loader;
    REQUIRE(usb.open("2-1", true, loader) == 0);
    const auto error = fern::load_firmware(*loader, img);
    REQUIRE(error.has_value());
    CHECK_HAS(*error, "timeout");
    // The fake answers the jump with an I/O error, as a real FX3 leaving the
    // bus may: that is success.
    CHECK(!fern::load_firmware(*loader, img));
}
