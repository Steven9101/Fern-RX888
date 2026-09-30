// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "libusb_backend.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <libusb.h>
#include <linux/netlink.h>
#include <mutex>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "rx888.h"

namespace fern {

namespace {

// libusb fails to start in a sandbox that hides /dev/bus/usb or forbids the
// netlink socket it watches for hotplug events on. Say which, since both
// look like "no RX-888 plugged in" otherwise.
std::string usb_diagnostic(int code) {
    if (::access("/dev/bus/usb", F_OK) != 0)
        return "USB devices are not visible to this process: /dev/bus/usb does not exist. If FernSDR runs "
               "under systemd, its unit must not set PrivateDevices=yes";
    const int s = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (s < 0)
        return std::string("libusb needs a netlink socket to watch for USB devices, and this process may not "
                           "open one (") +
               std::strerror(errno) +
               "). If FernSDR runs under systemd, add AF_NETLINK to RestrictAddressFamilies in its unit";
    ::close(s);
    return std::string("libusb could not start (") + libusb_error_name(code) + ")";
}

std::string port_of(libusb_device* dev) {
    uint8_t ports[8];
    const int n = libusb_get_port_numbers(dev, ports, sizeof ports);
    std::string port = std::to_string(libusb_get_bus_number(dev)) + "-";
    for (int i = 0; i < n; ++i) {
        if (i > 0)
            port += ".";
        port += std::to_string(ports[i]);
    }
    return port;
}

bool ours(const libusb_device_descriptor& d, bool& bootloader) {
    if (d.idVendor != rx888::vendor_id)
        return false;
    if (d.idProduct == rx888::bootloader_product) {
        bootloader = true;
        return true;
    }
    if (d.idProduct == rx888::firmware_product) {
        bootloader = false;
        return true;
    }
    return false;
}

std::string string_descriptor(libusb_device_handle* h, uint8_t index) {
    if (index == 0)
        return "";
    unsigned char text[256] = {};
    const int n = libusb_get_string_descriptor_ascii(h, index, text, sizeof text - 1);
    return n > 0 ? std::string(reinterpret_cast<char*>(text), static_cast<size_t>(n)) : std::string();
}

class LibusbFx3 : public Fx3 {
public:
    LibusbFx3(libusb_context* context, libusb_device_handle* handle, bool claimed)
        : context_(context), handle_(handle), claimed_(claimed) {}
    ~LibusbFx3() override {
        if (claimed_)
            libusb_release_interface(handle_, 0);
        libusb_close(handle_);
    }

    int control_out(uint8_t request, uint16_t value, uint16_t index, const uint8_t* data, uint16_t len,
                    unsigned timeout_ms) override {
        return libusb_control_transfer(handle_, LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR, request, value,
                                       index, const_cast<uint8_t*>(data), len, timeout_ms);
    }

    int control_in(uint8_t request, uint16_t value, uint16_t index, uint8_t* data, uint16_t len,
                   unsigned timeout_ms) override {
        return libusb_control_transfer(handle_, LIBUSB_ENDPOINT_IN | LIBUSB_REQUEST_TYPE_VENDOR, request, value,
                                       index, data, len, timeout_ms);
    }

    int stream(SampleCallback cb, void* ctx, uint32_t transfers, uint32_t transfer_bytes) override;

    void cancel_stream() override {
        cancelling_.store(true);
        std::lock_guard<std::mutex> lock(mutex_);
        for (libusb_transfer* t : active_)
            if (t)
                libusb_cancel_transfer(t);
    }

private:
    struct Slot {
        LibusbFx3* owner;
        size_t index;
    };

    static void LIBUSB_CALL done(libusb_transfer* t);

    libusb_context* context_;
    libusb_device_handle* handle_;
    bool claimed_;
    SampleCallback cb_ = nullptr;
    void* cb_ctx_ = nullptr;
    std::atomic<bool> cancelling_{false};
    std::mutex mutex_;
    std::vector<libusb_transfer*> active_;  // null once a transfer is back for good
    size_t in_flight_ = 0;
    int error_ = 0;
};

void LIBUSB_CALL LibusbFx3::done(libusb_transfer* t) {
    Slot* slot = static_cast<Slot*>(t->user_data);
    LibusbFx3* self = slot->owner;
    if (t->status == LIBUSB_TRANSFER_COMPLETED && t->actual_length > 0)
        self->cb_(t->buffer, static_cast<uint32_t>(t->actual_length), self->cb_ctx_);
    // The check for a cancel and the resubmit happen under the lock that
    // cancel_stream() takes, so a cancel cannot fall between them and miss
    // the transfer that is about to go out again.
    std::lock_guard<std::mutex> lock(self->mutex_);
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        if (!self->cancelling_.load()) {
            const int r = libusb_submit_transfer(t);
            if (r == 0)
                return;
            if (self->error_ == 0)
                self->error_ = r;
            self->cancelling_.store(true);
        }
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED && self->error_ == 0) {
        self->error_ = t->status == LIBUSB_TRANSFER_NO_DEVICE ? usb_error::no_device
                       : t->status == LIBUSB_TRANSFER_TIMED_OUT ? usb_error::timeout
                       : t->status == LIBUSB_TRANSFER_OVERFLOW  ? usb_error::overflow
                       : t->status == LIBUSB_TRANSFER_STALL     ? usb_error::pipe
                                                                : usb_error::io;
        // One transfer failing means the stream is gone: bring the others back.
        self->cancelling_.store(true);
    }
    self->active_[slot->index] = nullptr;
    --self->in_flight_;
    if (self->cancelling_.load())
        for (libusb_transfer* other : self->active_)
            if (other)
                libusb_cancel_transfer(other);
}

int LibusbFx3::stream(SampleCallback cb, void* ctx, uint32_t transfers, uint32_t transfer_bytes) {
    cb_ = cb;
    cb_ctx_ = ctx;
    error_ = 0;
    std::vector<libusb_transfer*> all(transfers, nullptr);
    std::vector<Slot> slots(transfers);
    std::vector<unsigned char*> buffers(transfers, nullptr);
    std::vector<bool> kernel_memory(transfers, false);
    int result = 0;
    for (uint32_t i = 0; i < transfers; ++i) {
        // Memory the kernel can hand to the host controller directly, where
        // it offers it; plain memory otherwise. Only on x86, whose DMA is
        // cache-coherent: on ARM the kernel maps that memory uncached, and
        // derandomizing, peak finding and copying 260 MB/s out of uncached
        // memory costs far more than the copy it saves.
#if defined(__x86_64__) || defined(__i386__)
        buffers[i] = libusb_dev_mem_alloc(handle_, transfer_bytes);
#endif
        kernel_memory[i] = buffers[i] != nullptr;
        if (!buffers[i])
            buffers[i] = static_cast<unsigned char*>(std::malloc(transfer_bytes));
        all[i] = libusb_alloc_transfer(0);
        if (!buffers[i] || !all[i]) {
            result = usb_error::no_mem;
            break;
        }
        slots[i] = Slot{this, i};
        libusb_fill_bulk_transfer(all[i], handle_, rx888::sample_endpoint, buffers[i], static_cast<int>(transfer_bytes),
                                  &LibusbFx3::done, &slots[i], 0);
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_.assign(transfers, nullptr);
        in_flight_ = 0;
        if (result == 0 && !cancelling_.load()) {
            for (uint32_t i = 0; i < transfers; ++i) {
                const int r = libusb_submit_transfer(all[i]);
                if (r != 0) {
                    result = r;
                    cancelling_.store(true);
                    break;
                }
                active_[i] = all[i];
                ++in_flight_;
            }
            if (cancelling_.load())
                for (libusb_transfer* t : active_)
                    if (t)
                        libusb_cancel_transfer(t);
        }
    }
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (in_flight_ == 0)
                break;
        }
        struct timeval tv = {0, 100000};
        const int r = libusb_handle_events_timeout_completed(context_, &tv, nullptr);
        if (r != 0 && r != LIBUSB_ERROR_INTERRUPTED && result == 0) {
            result = r;
            cancel_stream();
        }
    }
    for (uint32_t i = 0; i < transfers; ++i) {
        if (all[i])
            libusb_free_transfer(all[i]);
        if (buffers[i]) {
            if (kernel_memory[i])
                libusb_dev_mem_free(handle_, buffers[i], transfer_bytes);
            else
                std::free(buffers[i]);
        }
    }
    if (result == 0)
        result = error_;
    cancelling_.store(false);
    return result;
}

}  // namespace

LibusbBackend::LibusbBackend() {
    init_error_ = libusb_init(&context_);
    if (init_error_ < 0)
        context_ = nullptr;
}

LibusbBackend::~LibusbBackend() {
    if (context_)
        libusb_exit(context_);
}

Enumeration LibusbBackend::enumerate() {
    Enumeration e;
    if (!context_) {
        e.error = init_error_;
        e.diagnostic = usb_diagnostic(init_error_);
        return e;
    }
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(context_, &list);
    if (n < 0) {
        e.error = static_cast<int>(n);
        e.diagnostic = usb_diagnostic(static_cast<int>(n));
        return e;
    }
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc) != 0)
            continue;
        bool bootloader = false;
        if (!ours(desc, bootloader))
            continue;
        UsbDevice d;
        d.port = port_of(list[i]);
        d.bootloader = bootloader;
        // Opened as startup would open it, so that usable means usable: a
        // bootloader this user may not open, or a running board whose
        // interface another program has claimed, is listed with the reason.
        // Neither the bootloader nor the board is reset or sent anything.
        libusb_device_handle* h = nullptr;
        const int r = libusb_open(list[i], &h);
        if (r == 0) {
            if (!bootloader) {
                d.serial = string_descriptor(h, desc.iSerialNumber);
                for (char& c : d.serial)
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                d.product = string_descriptor(h, desc.iProduct);
                const int claimed = libusb_claim_interface(h, 0);
                if (claimed == 0)
                    libusb_release_interface(h, 0);
                else
                    d.error = claimed;
            }
            libusb_close(h);
        } else {
            d.error = r;
        }
        e.devices.push_back(std::move(d));
    }
    libusb_free_device_list(list, 1);
    return e;
}

int LibusbBackend::open(const std::string& port, bool bootloader, std::unique_ptr<Fx3>& out) {
    if (!context_)
        return init_error_;
    libusb_device** list = nullptr;
    const ssize_t n = libusb_get_device_list(context_, &list);
    if (n < 0)
        return static_cast<int>(n);
    int result = usb_error::not_found;
    for (ssize_t i = 0; i < n; ++i) {
        libusb_device_descriptor desc;
        bool is_bootloader = false;
        if (libusb_get_device_descriptor(list[i], &desc) != 0 || !ours(desc, is_bootloader) ||
            is_bootloader != bootloader || port_of(list[i]) != port)
            continue;
        libusb_device_handle* h = nullptr;
        result = libusb_open(list[i], &h);
        if (result != 0)
            break;
        bool claimed = false;
        if (!bootloader) {
            // The sample endpoint belongs to interface 0.
            result = libusb_claim_interface(h, 0);
            if (result != 0) {
                libusb_close(h);
                break;
            }
            claimed = true;
        }
        out = std::make_unique<LibusbFx3>(context_, h, claimed);
        result = 0;
        break;
    }
    libusb_free_device_list(list, 1);
    return result;
}

void LibusbBackend::sleep_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

std::string libusb_version_text() {
    const struct libusb_version* v = libusb_get_version();
    return "libusb " + std::to_string(v->major) + "." + std::to_string(v->minor) + "." + std::to_string(v->micro);
}

}  // namespace fern
