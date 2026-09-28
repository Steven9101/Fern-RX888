// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>

#include "fake_fx3.h"
#include "listing.h"
#include "test.h"

TEST(listing_shows_running_and_waiting_devices_in_port_order_without_touching_them) {
    fake::Backend usb;
    fake::Spec a, b, c;
    a.port = "3-2";
    a.firmware_running = true;
    b.port = "3-10";
    c.port = "1-4";
    c.firmware_running = true;
    c.list_error = fern::usb_error::access;
    usb.add(a);
    fake::Device& waiting = usb.add(b);
    usb.add(c);
    const std::string text = fern::report_text(fern::list_devices(usb));
    CHECK_EQ(text,
             std::string("{\"devices\":["
                         "{\"index\":0,\"name\":\"RX-888\",\"serial\":\"\",\"port\":\"1-4\","
                         "\"state\":\"firmware running\",\"usable\":false,\"error\":\"no permission to open it; see "
                         "the udev rule in the README\"},"
                         "{\"index\":1,\"name\":\"RX888mk2\",\"serial\":\"0123456789ABCDEF\",\"port\":\"3-2\","
                         "\"state\":\"firmware running\",\"usable\":true},"
                         "{\"index\":2,\"name\":\"FX3 without firmware\",\"serial\":\"\",\"port\":\"3-10\","
                         "\"state\":\"waiting for firmware\",\"usable\":true}]}\n"));
    CHECK(waiting.taken().empty());
    CHECK(!waiting.firmware_running);
}

TEST(listing_with_nothing_plugged_in_is_empty_and_a_broken_usb_says_why) {
    fake::Backend usb;
    CHECK_EQ(fern::report_text(fern::list_devices(usb)), std::string("{\"devices\":[]}\n"));
    usb.enumerate_error = fern::usb_error::access;
    CHECK_HAS(fern::report_text(fern::list_devices(usb)), "\"error\":\"the fake USB is broken\"");
}
