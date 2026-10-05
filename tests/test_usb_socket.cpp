// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "test.h"
#include "usb_socket.h"

namespace {

// A sysfs laid out as Linux lays it out: devices under the controller's
// directory, /sys/bus/usb/devices holding links to them.
struct FakeSysfs {
    std::string root;
    FakeSysfs() {
        char pattern[] = "/tmp/fern-rx888-sysfs-XXXXXX";
        root = ::mkdtemp(pattern);
        ::mkdir((root + "/bus").c_str(), 0755);
    }
    ~FakeSysfs() {
        if (std::system(("rm -rf '" + root + "'").c_str()) != 0)
            std::fprintf(stderr, "could not remove %s\n", root.c_str());
    }
    std::string devices() const { return root + "/bus"; }
    void dir(const std::string& path) {
        std::string at;
        size_t from = 0;
        while (from != std::string::npos) {
            const size_t slash = path.find('/', from + 1);
            at = path.substr(0, slash);
            ::mkdir((root + at).c_str(), 0755);
            from = slash;
        }
    }
    void link(const std::string& target, const std::string& name) {
        if (::symlink((root + target).c_str(), (root + name).c_str()) != 0)
            std::fprintf(stderr, "symlink %s failed\n", name.c_str());
    }
    // A root hub on a controller, with its ports.
    void bus(const std::string& controller, int bus, int ports) {
        const std::string b = std::to_string(bus);
        const std::string hub = controller + "/usb" + b;
        for (int p = 1; p <= ports; ++p)
            dir(hub + "/" + b + "-0:1.0/usb" + b + "-port" + std::to_string(p));
        link(hub, "/bus/usb" + b);
        link(hub + "/" + b + "-0:1.0", "/bus/" + b + "-0:1.0");
    }
    void peers(const std::string& a, const std::string& b) {
        link(b, a + "/peer");
        link(a, b + "/peer");
    }
};

const std::string xhci = "/devices/pci0000:00/0000:00:14.0";

}  // namespace

TEST(both_halves_of_a_usb3_socket_are_one_socket) {
    FakeSysfs fs;
    fs.bus(xhci, 1, 12);
    fs.bus(xhci, 2, 6);
    // Numbered differently on the two buses, as many Intel controllers are.
    fs.peers(xhci + "/usb1/1-0:1.0/usb1-port4", xhci + "/usb2/2-0:1.0/usb2-port2");
    fs.peers(xhci + "/usb1/1-0:1.0/usb1-port5", xhci + "/usb2/2-0:1.0/usb2-port3");
    const std::string a = fern::usb_socket(fs.devices(), "1-4");
    CHECK_EQ(a, fern::usb_socket(fs.devices(), "2-2"));
    CHECK(a != fern::usb_socket(fs.devices(), "2-4"));
    CHECK(a != fern::usb_socket(fs.devices(), "1-5"));
    CHECK_EQ(fern::usb_socket(fs.devices(), "1-5"), fern::usb_socket(fs.devices(), "2-3"));
}

TEST(a_socket_behind_a_usb3_hub_is_found_through_the_hubs_ports) {
    FakeSysfs fs;
    fs.bus(xhci, 1, 4);
    fs.bus(xhci, 2, 4);
    fs.peers(xhci + "/usb1/1-0:1.0/usb1-port1", xhci + "/usb2/2-0:1.0/usb2-port1");
    // A USB 3 hub is two hubs, one on each bus, with peered ports.
    fs.dir(xhci + "/usb1/1-1/1-1:1.0/1-1-port3");
    fs.dir(xhci + "/usb2/2-1/2-1:1.0/2-1-port3");
    fs.link(xhci + "/usb1/1-1/1-1:1.0", "/bus/1-1:1.0");
    fs.link(xhci + "/usb2/2-1/2-1:1.0", "/bus/2-1:1.0");
    fs.peers(xhci + "/usb1/1-1/1-1:1.0/1-1-port3", xhci + "/usb2/2-1/2-1:1.0/2-1-port3");
    CHECK_EQ(fern::usb_socket(fs.devices(), "1-1.3"), fern::usb_socket(fs.devices(), "2-1.3"));
    CHECK(fern::usb_socket(fs.devices(), "1-1.3") != fern::usb_socket(fs.devices(), "1-1"));
}

TEST(without_peer_links_the_controller_and_chain_decide) {
    FakeSysfs fs;
    fs.bus(xhci, 1, 4);
    fs.bus(xhci, 2, 4);
    fs.bus("/devices/pci0000:00/0000:03:00.0", 3, 4);
    CHECK_EQ(fern::usb_socket(fs.devices(), "1-4"), fern::usb_socket(fs.devices(), "2-4"));
    CHECK(fern::usb_socket(fs.devices(), "1-4") != fern::usb_socket(fs.devices(), "3-4"));
}

TEST(without_sysfs_a_port_is_only_itself) {
    CHECK_EQ(fern::usb_socket("/nonexistent-sysfs", "1-4"), std::string("1-4"));
    CHECK_EQ(fern::usb_socket("/nonexistent-sysfs", "garbage"), std::string("garbage"));
    CHECK_EQ(fern::usb_socket("/nonexistent-sysfs", "1-"), std::string("1-"));
}
