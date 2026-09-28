// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "listing.h"

#include <algorithm>
#include <cctype>
#include <vector>

#include "log.h"

namespace fern {

namespace {

std::string open_error(int r) {
    switch (r) {
    case usb_error::busy:
        return "in use by another program";
    case usb_error::access:
        return "no permission to open it; see the udev rule in the README";
    case usb_error::no_device:
        return "it disappeared while it was being listed";
    default:
        return std::string("could not be opened (") + usb_error_text(r) + ")";
    }
}

}  // namespace

json::Value list_devices(Backend& backend) {
    Enumeration e = backend.enumerate();
    std::sort(e.devices.begin(), e.devices.end(), port_order);
    json::Value devices = json::Value::array();
    uint32_t index = 0;
    for (const UsbDevice& u : e.devices) {
        json::Value d = json::Value::object();
        d.set("index", index++);
        d.set("name", u.bootloader ? std::string("FX3 without firmware") : (u.product.empty() ? std::string("RX-888") : u.product));
        d.set("serial", u.serial);
        d.set("port", u.port);
        d.set("state", u.bootloader ? "waiting for firmware" : "firmware running");
        if (u.error != 0) {
            d.set("usable", false);
            d.set("error", open_error(u.error));
        } else {
            d.set("usable", true);
        }
        devices.push(std::move(d));
    }
    json::Value report = json::Value::object();
    report.set("devices", std::move(devices));
    if (e.error != 0)
        report.set("error", e.diagnostic);
    return report;
}

std::string report_text(const json::Value& report) {
    std::string text = json::serialize(report) + "\n";
    if (text.size() <= max_report_bytes)
        return text;
    std::vector<json::Value> kept;
    if (const json::Value* list = report.find("devices"))
        for (const json::Value& d : list->items())
            kept.push_back(d);
    for (;;) {
        json::Value out = json::Value::object();
        json::Value list = json::Value::array();
        for (const json::Value& d : kept)
            list.push(d);
        out.set("devices", std::move(list));
        for (const json::Member& m : report.members())
            if (m.key != "devices")
                out.set(m.key, m.value);
        text = json::serialize(out) + "\n";
        if (text.size() <= max_report_bytes || kept.empty())
            break;
        kept.pop_back();
    }
    log_line("the device list was shortened to fit into %zu bytes", max_report_bytes);
    return text;
}

}  // namespace fern
