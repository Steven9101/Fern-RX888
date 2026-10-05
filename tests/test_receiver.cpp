// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "fake_fx3.h"
#include "receiver.h"
#include "rx888.h"
#include "test.h"

namespace rq = fern::rx888::request;

namespace {

fern::OpenRequest request(uint32_t rate = 64800000) {
    fern::OpenRequest r;
    r.sample_rate = rate;
    return r;
}

uint32_t u32(const std::vector<uint8_t>& d) {
    return d.size() < 4 ? 0
                        : static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8 |
                              static_cast<uint32_t>(d[2]) << 16 | static_cast<uint32_t>(d[3]) << 24;
}

// The firmware's requests, without the bootloader's 0xA0 blocks.
std::vector<fake::Request> firmware_requests(fake::Device& d) {
    std::vector<fake::Request> out;
    for (const auto& r : d.taken())
        if (r.request != rq::firmware_load)
            out.push_back(r);
    return out;
}

}  // namespace

TEST(open_loads_the_firmware_and_programs_the_board_in_order) {
    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    fern::Receiver rx(usb);
    auto r = request();
    r.settings.attenuation = 6;
    r.settings.gain = fern::GainSetting::manual(12);
    r.settings.bias_tee = true;
    REQUIRE(!rx.open(r));

    // The whole built-in image went in, then the jump.
    size_t loaded = 0;
    for (const auto& q : dev.taken())
        if (q.request == rq::firmware_load)
            loaded += q.data.size();
    fern::FirmwareImage img;
    REQUIRE(!fern::parse_firmware(fern::embedded_firmware, fern::embedded_firmware_size, img));
    size_t expected = 0;
    for (const auto& s : img.sections)
        expected += s.data.size();
    CHECK_EQ(loaded, expected);
    CHECK_EQ(dev.loaded_entry, img.entry);

    const auto sent = firmware_requests(dev);
    REQUIRE(sent.size() == 6);
    CHECK(sent[0].in && sent[0].request == rq::identify);
    CHECK_EQ(sent[1].request, rq::gpio);
    CHECK_EQ(u32(sent[1].data), fern::rx888::gpio::led | fern::rx888::gpio::bias_hf);
    CHECK_EQ(sent[2].request, rq::set_argument);
    CHECK_EQ(sent[2].index, fern::rx888::argument::attenuator);
    CHECK_EQ(sent[2].value, 12);
    CHECK_EQ(sent[3].index, fern::rx888::argument::vga);
    CHECK_EQ(sent[3].value, fern::rx888::vga_byte(12));
    CHECK_EQ(sent[4].request, rq::start_adc);
    CHECK_EQ(u32(sent[4].data), 64800000u);
    CHECK_EQ(sent[5].request, rq::start_stream);
    CHECK(dev.streaming);

    const auto& e = rx.effective();
    CHECK_EQ(e.sample_rate, 64800000.0);
    CHECK_EQ(e.attenuation, 6.0);
    CHECK(std::fabs(e.gain_db - 12) < 0.4);
    CHECK_EQ(rx.identity().serial, std::string("0123456789ABCDEF"));
    CHECK_EQ(rx.identity().port, std::string("2-1"));
    CHECK_EQ(rx.device_json().find("firmware_version")->as_string(), std::string("2.6"));
}

TEST(open_resets_a_device_that_already_runs_firmware_and_loads_its_own) {
    fake::Backend usb;
    fake::Spec spec;
    spec.firmware_running = true;
    fake::Device& dev = usb.add(spec);
    fern::Receiver rx(usb);
    REQUIRE(!rx.open(request()));
    const auto sent = dev.taken();
    REQUIRE(sent.size() > 2);
    CHECK(sent[0].in && sent[0].request == rq::identify);
    CHECK_EQ(sent[1].request, rq::reset);
    size_t blocks = 0;
    for (const auto& q : sent)
        blocks += q.request == rq::firmware_load;
    CHECK(blocks > 10);
    CHECK(dev.streaming);
}

// On an xHCI controller every USB 3 socket is two ports, one on the USB 2
// bus and one on the USB 3 bus. The bootloader is a USB 2 device and the
// firmware connects at SuperSpeed, so the RX-888 leaves from 1-4 and comes
// back at 2-4, and the other way round after a reset.
TEST(the_rx888_is_found_again_on_the_other_bus_of_its_socket) {
    for (const bool running : {false, true}) {
        fake::Backend usb;
        usb.controllers = {{"1", "pci-0000:00:14.0"}, {"2", "pci-0000:00:14.0"}};
        fake::Spec spec;
        spec.port = "1-4";
        spec.firmware_port = "2-4";
        spec.firmware_running = running;
        fake::Device& dev = usb.add(spec);
        fern::Receiver rx(usb);
        rx.set_reenumeration_timeout(std::chrono::milliseconds(200));
        const auto failure = rx.open(request());
        if (failure)
            std::fprintf(stderr, "%s\n", failure->message.c_str());
        REQUIRE(!failure);
        CHECK(dev.streaming);
        CHECK_EQ(rx.identity().port, std::string("2-4"));
    }
}

TEST(port_selects_a_socket_on_either_bus) {
    fake::Backend usb;
    usb.controllers = {{"1", "pci-a"}, {"2", "pci-a"}, {"3", "pci-b"}, {"4", "pci-b"}};
    fake::Spec spec;
    spec.port = "1-4";
    spec.firmware_port = "2-4";
    fake::Device& dev = usb.add(spec);
    fake::Spec other;
    other.port = "3-4";
    other.firmware_port = "4-4";
    usb.add(other);
    for (const char* written : {"1-4", "2-4"}) {
        fern::OpenRequest r = request();
        r.settings.device.kind = fern::DeviceSelector::Kind::port;
        r.settings.device.port = written;
        fern::Receiver rx(usb);
        rx.set_reenumeration_timeout(std::chrono::milliseconds(200));
        REQUIRE(!rx.open(r));
        CHECK_EQ(rx.identity().port, std::string("2-4"));
        CHECK(dev.streaming);
    }
}

// Two controllers each with something at port 4: the device must not be
// taken for the one on the other controller.
TEST(a_socket_on_another_controller_is_not_the_same_socket) {
    fake::Backend usb;
    usb.controllers = {{"1", "pci-a"}, {"2", "pci-a"}, {"3", "pci-b"}};
    fake::Spec spec;
    spec.port = "1-4";
    spec.firmware_port = "3-4";  // cannot happen on real hardware; a stranger
    usb.add(spec);
    fern::Receiver rx(usb);
    rx.set_reenumeration_timeout(std::chrono::milliseconds(200));
    const auto failure = rx.open(request());
    REQUIRE(failure);
    CHECK_HAS(failure->message, "did not come back with its firmware running");
}

TEST(gain_auto_starts_at_ten_decibels_on_the_step_ladder) {
    fake::Backend usb;
    usb.add(fake::Spec{});
    fern::Receiver rx(usb);
    REQUIRE(!rx.open(request()));
    CHECK(rx.effective().gain.automatic());
    CHECK(std::fabs(rx.effective().gain_db - fern::default_gain_db) < 0.6);
    REQUIRE(rx.gain_steps().size() > 40);
    CHECK_EQ(rx.gain_steps()[rx.gain_step()], static_cast<int>(std::lround(rx.effective().gain_db * 10)));
    CHECK(!rx.set_gain_step(0));
    CHECK(rx.effective().gain_db < -20);
    CHECK(rx.set_gain_step(rx.gain_steps().size()).has_value());
}

TEST(a_running_board_that_is_not_an_mkii_is_refused_and_left_alone) {
    fake::Backend usb;
    fake::Spec spec;
    spec.firmware_running = true;
    spec.board = 7;  // RX-888 r3
    fake::Device& dev = usb.add(spec);
    fern::Receiver rx(usb);
    const auto f = rx.open(request());
    REQUIRE(f.has_value());
    CHECK(f->code == fern::ErrorCode::no_device);
    CHECK_HAS(f->message, "not an RX-888 MkII");
    CHECK_HAS(f->message, "board type 7");
    CHECK_HAS(f->message, "left as it is");
    const auto sent = dev.taken();
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].in && sent[0].request == rq::identify);
    CHECK(dev.firmware_running);
    CHECK(!rx.is_open());
}

TEST(running_firmware_that_does_not_know_identify_is_left_alone) {
    fake::Backend usb;
    fake::Spec spec;
    spec.firmware_running = true;
    spec.fail_request = rq::identify;
    spec.fail_code = fern::usb_error::pipe;
    fake::Device& dev = usb.add(spec);
    fern::Receiver rx(usb);
    const auto f = rx.open(request());
    REQUIRE(f.has_value());
    CHECK(f->code == fern::ErrorCode::no_device);
    CHECK_HAS(f->message, "does not answer as an RX-888");
    CHECK_EQ(dev.taken().size(), 1u);
    CHECK(dev.firmware_running);
}

TEST(opens_wait_for_udev_after_each_reenumeration) {
    // Refused twice after the reset and twice after the jump, as while udev
    // is still changing the new device node's owner.
    fake::Backend usb;
    fake::Spec spec;
    spec.firmware_running = true;
    spec.denied_for = 2;
    fake::Device& dev = usb.add(spec);
    fern::Receiver rx(usb);
    REQUIRE(!rx.open(request()));
    CHECK(dev.streaming);

    fake::Backend never;
    spec.denied_for = 1 << 30;
    never.add(spec);
    fern::Receiver refused(never);
    refused.set_reenumeration_timeout(std::chrono::milliseconds(200));
    const auto f = refused.open(request());
    REQUIRE(f.has_value());
    CHECK(f->code == fern::ErrorCode::usb);
    CHECK_HAS(f->message, "udev rule");
}

TEST(device_selection_explains_what_it_found) {
    {
        fake::Backend usb;
        fern::Receiver rx(usb);
        const auto f = rx.open(request());
        REQUIRE(f.has_value());
        CHECK(f->code == fern::ErrorCode::no_device);
        CHECK_HAS(f->message, "no RX-888 is plugged in");
    }
    {
        fake::Backend usb;
        fake::Spec a, b;
        a.port = "2-1";
        b.port = "2-2";
        usb.add(a);
        usb.add(b);
        fern::Receiver rx(usb);
        CHECK_HAS(rx.open(request())->message, "2 FX3 devices are plugged in");
        auto r = request();
        r.settings.device.kind = fern::DeviceSelector::Kind::index;
        r.settings.device.index = 2;
        CHECK_HAS(rx.open(r)->message, "index 2, but 2 FX3 devices are");
        r.settings.device.kind = fern::DeviceSelector::Kind::port;
        r.settings.device.port = "3-1";
        CHECK_HAS(rx.open(r)->message, "port 3-1");
        r.settings.device.kind = fern::DeviceSelector::Kind::serial;
        r.settings.device.serial = "0123456789ABCDEF";
        CHECK_HAS(rx.open(r)->message, "a serial is only known once it runs");
        r.settings.device.kind = fern::DeviceSelector::Kind::port;
        r.settings.device.port = "2-2";
        CHECK(!rx.open(r));
        CHECK_EQ(rx.identity().port, std::string("2-2"));
    }
}

TEST(a_serial_that_cannot_be_read_is_reported_as_such) {
    fake::Backend usb;
    fake::Spec spec;
    spec.firmware_running = true;
    spec.list_error = fern::usb_error::access;
    usb.add(spec);
    fern::Receiver rx(usb);
    auto r = request();
    r.settings.device.kind = fern::DeviceSelector::Kind::serial;
    r.settings.device.serial = "0123456789ABCDEF";
    const auto f = rx.open(r);
    REQUIRE(f.has_value());
    CHECK(f->code == fern::ErrorCode::usb);
    CHECK_HAS(f->message, "cannot be opened to read its serial");
    CHECK_HAS(f->message, "udev rule");
}

TEST(index_counts_ports_in_numeric_order) {
    fake::Backend usb;
    fake::Spec a, b;
    a.port = "2-1.10";
    b.port = "2-1.9";
    usb.add(a);
    usb.add(b);
    fern::Receiver rx(usb);
    auto r = request();
    r.settings.device.kind = fern::DeviceSelector::Kind::index;
    r.settings.device.index = 0;
    REQUIRE(!rx.open(r));
    CHECK_EQ(rx.identity().port, std::string("2-1.9"));
}

TEST(usb_failures_say_what_to_do) {
    {
        fake::Backend usb;
        fake::Spec spec;
        spec.open_error = fern::usb_error::access;
        usb.add(spec);
        fern::Receiver rx(usb);
        const auto f = rx.open(request());
        REQUIRE(f.has_value());
        CHECK(f->code == fern::ErrorCode::usb);
        CHECK_HAS(f->message, "udev rule");
    }
    {
        fake::Backend usb;
        fake::Spec spec;
        spec.open_error = fern::usb_error::busy;
        usb.add(spec);
        fern::Receiver rx(usb);
        const auto f = rx.open(request());
        REQUIRE(f.has_value());
        CHECK(f->code == fern::ErrorCode::busy);
    }
    {
        fake::Backend usb;
        fake::Spec spec;
        spec.gone_for = 1 << 30;  // never comes back after the jump
        usb.add(spec);
        fern::Receiver rx(usb);
        rx.set_reenumeration_timeout(std::chrono::milliseconds(200));
        const auto f = rx.open(request());
        REQUIRE(f.has_value());
        CHECK_HAS(f->message, "did not come back with its firmware running");
    }
    {
        fake::Backend usb;
        fake::Spec spec;
        spec.fail_request = rq::start_adc;
        spec.fail_code = fern::usb_error::timeout;
        usb.add(spec);
        fern::Receiver rx(usb);
        const auto f = rx.open(request(129600000));
        REQUIRE(f.has_value());
        CHECK_HAS(f->message, "clock at 129600000 Hz");
    }
    {
        fake::Backend usb;
        usb.enumerate_error = fern::usb_error::access;
        fern::Receiver rx(usb);
        CHECK_HAS(rx.open(request())->message, "USB is not usable here");
    }
}

TEST(a_firmware_file_that_is_not_one_is_refused_before_any_device_is_touched) {
    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    fern::Receiver rx(usb);
    auto r = request();
    r.settings.firmware = "/nonexistent/fw.img";
    const auto f = rx.open(r);
    REQUIRE(f.has_value());
    CHECK(f->code == fern::ErrorCode::invalid);
    CHECK_HAS(f->message, "cannot open /nonexistent/fw.img");
    r.settings.firmware = "/proc/self/cmdline";
    CHECK_HAS(rx.open(r)->message, "not an FX3 image");
    CHECK(dev.taken().empty());
}

TEST(apply_changes_only_what_it_carries) {
    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    fern::Receiver rx(usb);
    REQUIRE(!rx.open(request()));
    const size_t before = dev.taken().size();
    fern::LiveChange c;
    c.attenuation = 20;
    c.dither = true;
    REQUIRE(!rx.apply(c));
    const auto sent = dev.taken();
    REQUIRE(sent.size() == before + 2);
    CHECK_EQ(sent[before].index, fern::rx888::argument::attenuator);
    CHECK_EQ(sent[before].value, 40);
    CHECK_EQ(u32(sent[before + 1].data), fern::rx888::gpio::led | fern::rx888::gpio::dither);
    const auto json = fern::json::serialize(rx.settings_json(c));
    CHECK_EQ(json, std::string("{\"attenuation\":20,\"dither\":true}"));

    fern::LiveChange g;
    g.gain = fern::GainSetting::manual(-10);
    REQUIRE(!rx.apply(g));
    CHECK_EQ(dev.vga, static_cast<uint16_t>(fern::rx888::vga_byte(-10)));
    char expected[64];
    std::snprintf(expected, sizeof expected, "\"gain\":\"%.1f\"", fern::rx888::vga_db(fern::rx888::vga_byte(-10)));
    CHECK_HAS(fern::json::serialize(rx.settings_json(g)), expected);
}

TEST(a_refused_gain_change_leaves_the_gain_mode_as_it_was) {
    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    fern::Receiver rx(usb);
    REQUIRE(!rx.open(request()));
    REQUIRE(rx.effective().gain.automatic());
    const size_t step = rx.gain_step();
    const uint16_t vga = dev.vga;

    // Automatic to manual, refused by the board: AGC must stay in charge.
    dev.spec.fail_request = rq::set_argument;
    dev.spec.fail_code = fern::usb_error::timeout;
    fern::LiveChange manual;
    manual.gain = fern::GainSetting::manual(-10);
    CHECK(rx.apply(manual).has_value());
    CHECK(rx.effective().gain.automatic());
    CHECK_EQ(rx.gain_step(), step);
    CHECK_EQ(dev.vga, vga);

    // Manual to automatic, refused: the manual gain stays, and the step the
    // control would have started from is not taken either.
    REQUIRE(!rx.apply(manual));
    const double manual_db = rx.effective().gain.db;
    dev.spec.fail_request = rq::set_argument;
    dev.spec.fail_code = fern::usb_error::timeout;
    fern::LiveChange automatic;
    automatic.gain = fern::GainSetting{};
    CHECK(rx.apply(automatic).has_value());
    CHECK(!rx.effective().gain.automatic());
    CHECK_EQ(rx.effective().gain.db, manual_db);
    CHECK_EQ(rx.gain_step(), step);
}

TEST(close_stops_the_stream_the_clock_and_the_converter) {
    fake::Backend usb;
    fake::Device& dev = usb.add(fake::Spec{});
    {
        fern::Receiver rx(usb);
        REQUIRE(!rx.open(request()));
        const size_t before = dev.taken().size();
        CHECK(rx.close_by(std::chrono::steady_clock::now() + std::chrono::seconds(2), true));
        const auto sent = dev.taken();
        REQUIRE(sent.size() == before + 3);
        CHECK_EQ(sent[before].request, rq::stop_stream);
        CHECK_EQ(sent[before + 1].request, rq::start_adc);
        CHECK_EQ(u32(sent[before + 1].data), 0u);
        CHECK_EQ(u32(sent[before + 2].data), fern::rx888::gpio::adc_shutdown);
    }
    CHECK_EQ(dev.open_handles.load(), 0);
}
