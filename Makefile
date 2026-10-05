# Fern-RX888, an RX-888 input module for FernSDR.
# SPDX-License-Identifier: GPL-2.0-or-later
#
#   make                        build/fern-rx888, linked against the system libusb
#   make static [ARCH=...]      build/static-ARCH/fern-rx888, fully static
#   make package [ARCH=...]     dist/rx888-VERSION-linux-ARCH.fernmod
#   make test                   unit, protocol, command line and package tests
#   make test-asan              the unit and protocol tests under ASan and UBSan
#
# ARCH is x86_64, aarch64 or armhf (ARMv7 with hardware floating point) and
# defaults to the machine's own.

VERSION := 0.1.1
.DEFAULT_GOAL := all

HOST_ARCH := $(shell uname -m | sed -e 's/^armv7.*/armhf/' -e 's/^arm64$$/aarch64/')
ARCH ?= $(HOST_ARCH)
ifeq ($(filter $(ARCH),x86_64 aarch64 armhf),)
$(error ARCH must be x86_64, aarch64 or armhf, not "$(ARCH)")
endif

CROSS_x86_64 := x86_64-linux-gnu-
CROSS_aarch64 := aarch64-linux-gnu-
CROSS_armhf := arm-linux-gnueabihf-
ifeq ($(ARCH),$(HOST_ARCH))
CROSS ?=
else
CROSS ?= $(CROSS_$(ARCH))
endif
ARCH_FLAGS_armhf := -march=armv7-a -mfpu=vfpv3-d16 -mfloat-abi=hard
ARCH_FLAGS := $(ARCH_FLAGS_$(ARCH))

OPT ?= -O2
CXXFLAGS_BASE := -std=c++17 $(OPT) -g -pthread -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 \
	-DFERN_RX888_VERSION='"$(VERSION)"' -Isrc
LIBUSB_CFLAGS := -std=gnu11 $(OPT) -g -pthread -fvisibility=hidden -Wall -Wextra \
	-Ithird_party/libusb-config -Ithird_party/libusb/libusb
# Third-party headers are included as system headers, so that the warning
# options above apply to the module's own code only.
VENDOR_LIBUSB_INCLUDE := -isystem third_party/libusb/libusb
# Only evaluated when the native build needs it.
SYSTEM_LIBUSB_CFLAGS = $(patsubst -I%,-isystem %,$(shell pkg-config --cflags libusb-1.0))
SYSTEM_LIBUSB_LIBS = $(shell pkg-config --libs libusb-1.0)

# The firmware image and the notices that travel with it, compiled in.
FIRMWARE := firmware/SDDC_FX3.img
FIRMWARE_NOTICES := firmware/NOTICE.md firmware/LICENSE-MIT.txt firmware/LICENSE-CYPRESS.txt
FIRMWARE_SRC := build/gen/firmware_blob.cpp
# The licences of the module and of the libusb that static builds carry.
NOTICES := LICENSE third_party/libusb/AUTHORS third_party/libusb/COPYING
NOTICES_SRC := build/gen/notices.cpp

MODULE_SRCS := src/json.cpp src/io.cpp src/log.cpp src/fx3.cpp src/rx888.cpp src/firmware.cpp src/settings.cpp \
	src/receiver.cpp src/stream.cpp src/session.cpp src/listing.cpp src/gain_control.cpp src/usb_socket.cpp
PROGRAM_SRCS := src/main.cpp src/libusb_backend.cpp
TEST_SRCS := tests/test_main.cpp tests/fake_fx3.cpp tests/test_json.cpp tests/test_rx888.cpp tests/test_firmware.cpp \
	tests/test_settings.cpp tests/test_receiver.cpp tests/test_stream.cpp tests/test_session.cpp \
	tests/test_listing.cpp tests/test_gain_control.cpp tests/test_usb_socket.cpp
LIBUSB_SRCS := core.c descriptor.c hotplug.c io.c sync.c strerror.c os/linux_usbfs.c os/linux_netlink.c \
	os/events_posix.c os/threads_posix.c

cxx_objs = $(patsubst %.cpp,$(1)/obj/%.o,$(2)) $(1)/obj/gen/firmware_blob.o $(1)/obj/gen/notices.o
libusb_objs = $(patsubst %.c,$(1)/obj/libusb/%.o,$(LIBUSB_SRCS))

$(FIRMWARE_SRC): $(FIRMWARE) $(FIRMWARE_NOTICES) tools/embed_firmware.py
	@mkdir -p $(@D)
	python3 tools/embed_firmware.py $(FIRMWARE) $(FIRMWARE_NOTICES) > $@.tmp
	mv $@.tmp $@

$(NOTICES_SRC): $(NOTICES) tools/embed_text.py
	@mkdir -p $(@D)
	python3 tools/embed_text.py module_licence LICENSE -- libusb_notices third_party/libusb/AUTHORS \
		third_party/libusb/COPYING > $@.tmp
	mv $@.tmp $@

# Compile rules for one build directory.
#   $(1) directory, $(2) C compiler, $(3) C++ compiler, $(4) extra flags,
#   $(5) libusb include flags for code that includes libusb.h
define compile_rules
$(1)/obj/src/%.o: src/%.cpp
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) $(5) -MMD -MP -c $$< -o $$@
$(1)/obj/tests/%.o: tests/%.cpp
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) -MMD -MP -c $$< -o $$@
$(1)/obj/gen/firmware_blob.o: $(FIRMWARE_SRC)
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) -c $$< -o $$@
$(1)/obj/gen/notices.o: $(NOTICES_SRC)
	@mkdir -p $$(@D)
	$(3) $$(CXXFLAGS_BASE) $(4) -c $$< -o $$@
$(1)/obj/libusb/%.o: third_party/libusb/libusb/%.c
	@mkdir -p $$(@D)
	$(2) $$(LIBUSB_CFLAGS) $(4) -MMD -MP -c $$< -o $$@
endef

NATIVE_DIR := build/native
STATIC_DIR := build/static-$(ARCH)
TEST_DIR := build/test
ASAN_DIR := build/asan
SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined

$(eval $(call compile_rules,$(NATIVE_DIR),$(CC),$(CXX),,$$(SYSTEM_LIBUSB_CFLAGS)))
$(eval $(call compile_rules,$(STATIC_DIR),$(CROSS)gcc,$(CROSS)g++,$(ARCH_FLAGS) -ffunction-sections -fdata-sections,$(VENDOR_LIBUSB_INCLUDE)))
$(eval $(call compile_rules,$(TEST_DIR),$(CC),$(CXX),,))
$(eval $(call compile_rules,$(ASAN_DIR),$(CC),$(CXX),$(SANITIZE),))

NATIVE_OBJS := $(call cxx_objs,$(NATIVE_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS))
STATIC_OBJS := $(call cxx_objs,$(STATIC_DIR),$(MODULE_SRCS) $(PROGRAM_SRCS)) $(call libusb_objs,$(STATIC_DIR))
TEST_OBJS := $(call cxx_objs,$(TEST_DIR),$(MODULE_SRCS) $(TEST_SRCS))
ASAN_OBJS := $(call cxx_objs,$(ASAN_DIR),$(MODULE_SRCS) $(TEST_SRCS))

PACKAGE := dist/rx888-$(VERSION)-linux-$(ARCH).fernmod
# The settings in every package come from --describe of a binary that runs
# here, built from the same sources, so all platforms carry the same list.
DESCRIBE_BIN := build/static-$(HOST_ARCH)/fern-rx888

.PHONY: all static package test test-asan clean print-version check-libusb check-toolchain FORCE

all: build/fern-rx888

check-libusb:
	@pkg-config --exists libusb-1.0 || { echo "The native build needs the libusb-1.0 development files" \
		"(Debian and Ubuntu: apt install libusb-1.0-0-dev pkg-config). make static needs neither." >&2; exit 1; }

$(NATIVE_DIR)/obj/src/libusb_backend.o: | check-libusb

build/fern-rx888: $(NATIVE_OBJS) | check-libusb
	$(CXX) -pthread -o $@ $^ $(SYSTEM_LIBUSB_LIBS)

TOOLCHAIN_PACKAGES_x86_64 := gcc-x86-64-linux-gnu g++-x86-64-linux-gnu
TOOLCHAIN_PACKAGES_aarch64 := gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
TOOLCHAIN_PACKAGES_armhf := gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf

check-toolchain:
	@command -v $(CROSS)g++ >/dev/null 2>&1 || { echo "make static ARCH=$(ARCH) needs $(CROSS)gcc and $(CROSS)g++" \
		"(Debian and Ubuntu: apt install $(TOOLCHAIN_PACKAGES_$(ARCH)))." >&2; exit 1; }

$(STATIC_OBJS): | check-toolchain

static: $(STATIC_DIR)/fern-rx888

$(STATIC_DIR)/fern-rx888: $(STATIC_OBJS)
	$(CROSS)g++ $(ARCH_FLAGS) -static -pthread -s -Wl,--gc-sections -o $@ $^

ifneq ($(ARCH),$(HOST_ARCH))
$(DESCRIBE_BIN): FORCE
	$(MAKE) static ARCH=$(HOST_ARCH)
endif

package: $(PACKAGE)

$(PACKAGE): $(STATIC_DIR)/fern-rx888 $(DESCRIBE_BIN) tools/mkfernmod.py tools/check_fernmod.py
	@mkdir -p dist
	python3 tools/mkfernmod.py --executable $(STATIC_DIR)/fern-rx888 --describe-with $(DESCRIBE_BIN) \
		--platform linux-$(ARCH) --version $(VERSION) --output $@
	python3 tools/check_fernmod.py $@

$(TEST_DIR)/fern-rx888-tests: $(TEST_OBJS)
	$(CXX) -pthread -o $@ $^

$(ASAN_DIR)/fern-rx888-tests: $(ASAN_OBJS)
	$(CXX) $(SANITIZE) -pthread -o $@ $^

test: $(TEST_DIR)/fern-rx888-tests build/fern-rx888
	$(TEST_DIR)/fern-rx888-tests
	sh tests/cli_test.sh build/fern-rx888
	sh tests/package_test.sh build/fern-rx888

test-asan: $(ASAN_DIR)/fern-rx888-tests
	ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=print_stacktrace=1 $(ASAN_DIR)/fern-rx888-tests

print-version:
	@echo $(VERSION)

clean:
	rm -rf build dist

FORCE:

-include $(shell find build -name '*.d' 2>/dev/null)
