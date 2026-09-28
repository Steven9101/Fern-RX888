// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "fx3.h"

#include <cctype>

namespace fern {

const char* usb_error_text(int code) {
    switch (code) {
    case usb_error::io: return "input/output error";
    case usb_error::invalid_param: return "invalid parameter";
    case usb_error::access: return "permission denied";
    case usb_error::no_device: return "no such device";
    case usb_error::not_found: return "not found";
    case usb_error::busy: return "busy";
    case usb_error::timeout: return "timeout";
    case usb_error::overflow: return "overflow";
    case usb_error::pipe: return "the device refused the request";
    case usb_error::interrupted: return "interrupted";
    case usb_error::no_mem: return "out of memory";
    case usb_error::not_supported: return "not supported";
    default: return "error";
    }
}

bool port_order(const UsbDevice& a, const UsbDevice& b) {
    size_t i = 0, j = 0;
    while (i < a.port.size() && j < b.port.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a.port[i])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b.port[j])) != 0;
        if (da && db) {
            unsigned long x = 0, y = 0;
            while (i < a.port.size() && std::isdigit(static_cast<unsigned char>(a.port[i])))
                x = x * 10 + static_cast<unsigned long>(a.port[i++] - '0');
            while (j < b.port.size() && std::isdigit(static_cast<unsigned char>(b.port[j])))
                y = y * 10 + static_cast<unsigned long>(b.port[j++] - '0');
            if (x != y)
                return x < y;
        } else {
            if (a.port[i] != b.port[j])
                return a.port[i] < b.port[j];
            ++i;
            ++j;
        }
    }
    return a.port.size() - i < b.port.size() - j;
}

}  // namespace fern
