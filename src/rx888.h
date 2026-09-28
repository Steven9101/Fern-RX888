// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the RX-888 MkII and its FX3 firmware expect on the wire, and the
// arithmetic behind it. Nothing here touches USB; receiver.cpp sends what
// these functions compute, and the tests check them without hardware.
//
// The facts come from reading the firmware this module loads
// (ringof/rx888-firmware v0.1.0, firmware/README.md) and the datasheets of
// the parts on the board; docs/PROTOCOL.md says where each one is from.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fern::rx888 {

// Cypress's vendor id. The FX3 enumerates as the bootloader until firmware is
// loaded into its RAM, then as the running firmware.
constexpr uint16_t vendor_id = 0x04b4;
constexpr uint16_t bootloader_product = 0x00f3;
constexpr uint16_t firmware_product = 0x00f1;

// Samples arrive on this bulk IN endpoint, in bursts of 16 packets of
// 1024 bytes; a transfer must be a whole number of bursts.
constexpr uint8_t sample_endpoint = 0x81;
constexpr uint32_t transfer_multiple = 16384;

// Vendor requests. firmware_load is the FX3 bootloader's own; the rest are
// the firmware's. Every one is a vendor request to the device, OUT unless
// noted; unknown ones stall.
namespace request {
constexpr uint8_t firmware_load = 0xA0;  // wValue/wIndex: RAM address, low/high 16 bits
constexpr uint8_t start_stream = 0xAA;
constexpr uint8_t stop_stream = 0xAB;
constexpr uint8_t identify = 0xAC;  // IN, 4 bytes: board, firmware major, minor, requests served
constexpr uint8_t gpio = 0xAD;      // 4 bytes: the whole GPIO word, little-endian
constexpr uint8_t i2c_write = 0xAE;
constexpr uint8_t i2c_read = 0xAF;  // IN
constexpr uint8_t reset = 0xB1;     // back to the bootloader
constexpr uint8_t start_adc = 0xB2; // 4 bytes: ADC clock in Hz, little-endian; 0 stops it
constexpr uint8_t set_argument = 0xB6;  // wValue: value, wIndex: argument, 1 byte of padding
}  // namespace request

namespace argument {
constexpr uint16_t attenuator = 10;  // 0 to 63, half dB steps
constexpr uint16_t vga = 11;         // AD8370 gain byte
}  // namespace argument

// The first byte of the identify reply for an RX-888 MkII. Other SDDC boards
// (BBRF103, HF103, RX-888 mk1, RX999) wire their GPIOs differently, and the
// firmware this module loads supports the MkII only.
constexpr uint8_t board_rx888_mk2 = 4;

// The STARTADC acknowledgement can take a second while the clock settles;
// control transfers wait this long.
constexpr unsigned control_timeout_ms = 5000;

// Bits of the GPIO word.
namespace gpio {
constexpr uint32_t adc_shutdown = 1u << 5;
constexpr uint32_t dither = 1u << 6;
constexpr uint32_t randomizer = 1u << 7;
constexpr uint32_t bias_hf = 1u << 8;
constexpr uint32_t bias_vhf = 1u << 9;
constexpr uint32_t led = 1u << 11;
constexpr uint32_t vhf_enable = 1u << 15;
// Set, it drives the LTC2208's PGA pin low: the 2.25 Vpp input range.
// Clear, the 1.5 Vpp range, 3.5 dB more gain.
constexpr uint32_t wide_range = 1u << 16;
}  // namespace gpio

struct Board {
    bool adc_on = true;
    bool dither = false;
    bool randomizer = false;
    bool bias_tee = false;
    bool wide_range = false;
};

// The GPIO word for HF direct sampling: VHF input off, the LED on while the
// ADC runs.
uint32_t gpio_word(const Board& board);

// The step attenuator: 0 to 31.5 dB in 0.5 dB steps, sent as the number of
// half dB. attenuator_code() rounds to the nearest step and clamps.
constexpr double max_attenuation_db = 31.5;
uint16_t attenuator_code(double db);
double attenuator_db(uint16_t code);

// The AD8370 VGA. Its byte holds a 7-bit gain code and, in bit 7, the
// high-gain mode; the voltage gain is code * 0.055744, times 7.079458 in
// high-gain mode (the datasheet's 17 dB). Code 0 mutes the output.
constexpr double min_vga_db = -25.0;
constexpr double max_vga_db = 34.0;
double vga_db(uint8_t byte);
// The low-gain mode's highest gain, code 127.
constexpr double max_low_vga_db = 17.0;
// The byte for a gain: the high-gain mode, which has the lower noise, where
// one of its codes lands within half a dB, otherwise the low-gain mode's
// nearest code.
uint8_t vga_byte(double db);
// Gains the module's own control steps through, about 1 dB apart, ascending.
std::vector<uint8_t> vga_steps();

// Sample rates this module accepts. The LTC2208 converts up to 130 Msps;
// below 10 Msps the Si5351 path is untested.
constexpr uint32_t min_sample_rate = 10000000;
constexpr uint32_t max_sample_rate = 130000000;
// The rate the firmware's Si5351 arithmetic produces for a request: an even
// integer output divider under 900 MHz, and the PLL's fractional multiplier
// of the 27 MHz crystal truncated to twenty bits. Exact for 64.8 and
// 129.6 MHz; a few parts in 10^8 off for rates such as 100 MHz.
double achieved_sample_rate(uint32_t requested_hz);

// The LTC2208's randomizer XORs bits 1 to 15 of a sample with its bit 0, to
// keep the data lines from coupling into the input. Undoing it in place.
void derandomize(int16_t* samples, size_t count);

}  // namespace fern::rx888
