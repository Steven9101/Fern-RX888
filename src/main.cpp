// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/signalfd.h>
#include <unistd.h>

#include "failure.h"
#include "io.h"
#include "json.h"
#include "listing.h"
#include "log.h"
#include "firmware.h"
#include "libusb_backend.h"
#include "session.h"
#include "settings.h"

using namespace fern;

namespace {

const char usage_text[] =
    "usage: fern-rx888 --describe          print the module and its settings as JSON\n"
    "       fern-rx888 --list-devices      print the RX-888s this machine can see as JSON\n"
    "       fern-rx888 --fernsdr-module 1  run as a FernSDR input module (FernSDR starts it so)\n"
    "       fern-rx888 --notices           print the licences of the firmware this module carries\n"
    "       fern-rx888 --version\n";

int print(const std::string& text) {
    return write_all(STDOUT_FILENO, text.data(), text.size()) == 0 ? 0 : exit_status::internal;
}

int describe() {
    const std::string text = json::serialize(describe_module()) + "\n";
    if (text.size() > max_report_bytes) {
        std::fprintf(stderr, "fern-rx888: the description is larger than %zu bytes\n", max_report_bytes);
        return exit_status::internal;
    }
    return print(text);
}

int list() {
    LibusbBackend backend;
    return print(report_text(list_devices(backend)));
}

int notices() {
    return print(std::string("Firmware: firmware/SDDC_FX3.img, ringof/rx888-firmware 0.1.0, SHA-256 ") +
                 embedded_firmware_sha256 + "\n\n" + embedded_firmware_notices);
}

int module() {
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, 3}) {
        if (::fcntl(fd, F_GETFD) < 0) {
            std::fprintf(stderr,
                         "fern-rx888: fd %d is not open. --fernsdr-module is how FernSDR runs this program, "
                         "with commands on fd 0, samples on fd 1 and events on fd 3.\n",
                         fd);
            return exit_status::usage;
        }
    }
    std::signal(SIGPIPE, SIG_IGN);

    // SIGTERM and friends arrive through a signalfd, so that the session
    // loop sees them like any other input. Threads started later inherit
    // the mask.
    sigset_t stop_signals;
    sigemptyset(&stop_signals);
    sigaddset(&stop_signals, SIGTERM);
    sigaddset(&stop_signals, SIGINT);
    sigaddset(&stop_signals, SIGHUP);
    if (pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr) != 0) {
        log_line("could not block the stop signals");
        return exit_status::internal;
    }
    const int signal_fd = signalfd(-1, &stop_signals, SFD_CLOEXEC);
    if (signal_fd < 0) {
        log_line("could not create a signalfd: %s", std::strerror(errno));
        return exit_status::internal;
    }

    log_line("fern-rx888 %s with %s", module_version(), libusb_version_text().c_str());
    LibusbBackend backend;
    SessionIo io;
    io.stop = signal_fd;
    const SessionResult result = run_session(backend, io);
    if (!result.clean)
        _exit(result.status);
    ::close(signal_fd);
    return result.status;
}

}  // namespace

int main(int argc, char** argv) {
    const int n = argc - 1;
    const std::string_view first = n >= 1 ? argv[1] : "";
    if (n == 1 && first == "--describe")
        return describe();
    if (n == 1 && first == "--list-devices")
        return list();
    if (n == 1 && first == "--notices")
        return notices();
    if (n == 2 && first == "--fernsdr-module") {
        if (std::string_view(argv[2]) == "1")
            return module();
        std::fprintf(stderr, "fern-rx888: this module speaks module API 1, not %s\n", argv[2]);
        return exit_status::usage;
    }
    if (n == 1 && first == "--version")
        return print(std::string("fern-rx888 ") + module_version() + "\n");
    if (n == 1 && (first == "--help" || first == "-h"))
        return print(usage_text);
    std::fputs(usage_text, stderr);
    return exit_status::usage;
}
