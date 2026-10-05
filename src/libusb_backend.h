// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <string>

#include "fx3.h"

struct libusb_context;

namespace fern {

// The Backend on top of libusb.
class LibusbBackend : public Backend {
public:
    LibusbBackend();
    ~LibusbBackend() override;
    LibusbBackend(const LibusbBackend&) = delete;
    LibusbBackend& operator=(const LibusbBackend&) = delete;

    Enumeration enumerate() override;
    int open(const std::string& port, bool bootloader, std::unique_ptr<Fx3>& out) override;
    void sleep_ms(unsigned ms) override;
    std::string socket_of(const std::string& port) override;

private:
    libusb_context* context_ = nullptr;
    int init_error_ = 0;
};

// e.g. "libusb 1.0.30"
std::string libusb_version_text();

}  // namespace fern
