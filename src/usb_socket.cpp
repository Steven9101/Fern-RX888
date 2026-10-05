// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "usb_socket.h"

#include <climits>
#include <cstdlib>

namespace fern {

namespace {

bool resolve(const std::string& path, std::string& out) {
    char resolved[PATH_MAX];
    if (::realpath(path.c_str(), resolved) == nullptr)
        return false;
    out = resolved;
    return true;
}

}  // namespace

std::string usb_socket(const std::string& sysfs, const std::string& port) {
    const size_t dash = port.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= port.size())
        return port;
    const std::string bus = port.substr(0, dash);
    const std::string chain = port.substr(dash + 1);
    const size_t dot = chain.rfind('.');
    const std::string last = dot == std::string::npos ? chain : chain.substr(dot + 1);
    // A root port is usbB-portN under the root hub's interface B-0:1.0; a
    // port of a hub at B-C is B-C-portN under the hub's interface B-C:1.0.
    const std::string hub = dot == std::string::npos ? "usb" + bus : bus + "-" + chain.substr(0, dot);
    const std::string interface = dot == std::string::npos ? bus + "-0:1.0" : hub + ":1.0";
    const std::string dir = sysfs + "/" + interface + "/" + hub + "-port" + last;

    std::string self, peer;
    if (resolve(dir, self) && resolve(dir + "/peer", peer))
        return self < peer ? self : peer;
    std::string controller;
    if (resolve(sysfs + "/usb" + bus, controller)) {
        const size_t slash = controller.rfind('/');
        if (slash != std::string::npos && slash > 0)
            return controller.substr(0, slash) + ":" + chain;
    }
    return port;
}

}  // namespace fern
