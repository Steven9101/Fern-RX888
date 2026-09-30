# Third-party code

libusb is copied unmodified from its upstream repository, with only the
files the build compiles and its licence. The Makefile compiles them
directly; its own build system is not used.

## libusb

- Upstream: https://github.com/libusb/libusb
- Tag: `v1.0.30`, commit `87a55632db62c9bdc58cd31d3ccfa673f1bb017f`, the same as
  Fern-RTLSDR's
- License: LGPL-2.1-or-later, see `libusb/COPYING`; authors in `libusb/AUTHORS`
- Files: `libusb/core.c`, `descriptor.c`, `hotplug.c`, `io.c`, `sync.c`,
  `strerror.c`, `os/linux_usbfs.c`, `os/linux_netlink.c`,
  `os/events_posix.c`, `os/threads_posix.c` and the headers they include

Used only by `make static` and `make package`; the native `make` links the
system libusb. Built without udev: devices are found through sysfs and
usbfs, hotplug events arrive on a netlink socket. `libusb-config/config.h`
is written for this project in place of the header that libusb's configure
script generates; it defines what configure would define on glibc Linux with
`--disable-udev`.

## Updating

Clone the new tag with `git clone --depth 1 --branch <tag> <url>`, copy the
same files over the ones here, update the commit hash above and the
version `--notices` names in `src/main.cpp`, and run
`make test`, `make static` and `make static ARCH=aarch64`. The module uses
synchronous control transfers, asynchronous bulk transfers and
`libusb_dev_mem_alloc()` (`src/libusb_backend.cpp`).
