// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

namespace fern {

// The physical socket behind a USB port such as "2-1.4", as Linux describes
// it under `sysfs` (/sys/bus/usb/devices outside tests). Two ports with the
// same answer are one socket.
//
// An xHCI controller lists every USB 3 socket twice, once on its USB 2 bus
// and once on its USB 3 bus, and the two halves need not even have the same
// port number. The kernel links them: each port's directory has a `peer`
// link to its other half. The socket is the lesser of the two resolved
// paths, so both halves give the same text. Without a peer link (an older
// kernel, a USB 2 only socket) it is the controller's directory and the port
// chain; without sysfs at all, the port itself.
std::string usb_socket(const std::string& sysfs, const std::string& port);

}  // namespace fern
