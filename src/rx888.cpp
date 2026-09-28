// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include "rx888.h"

#include <algorithm>
#include <cmath>

namespace fern::rx888 {

uint32_t gpio_word(const Board& board) {
    uint32_t word = 0;
    if (!board.adc_on)
        word |= gpio::adc_shutdown;
    else
        word |= gpio::led;
    if (board.dither)
        word |= gpio::dither;
    if (board.randomizer)
        word |= gpio::randomizer;
    if (board.bias_tee)
        word |= gpio::bias_hf;
    if (board.wide_range)
        word |= gpio::wide_range;
    return word;
}

uint16_t attenuator_code(double db) {
    if (!(db > 0))
        return 0;
    const double steps = std::round(std::min(db, max_attenuation_db) * 2);
    return static_cast<uint16_t>(steps);
}

double attenuator_db(uint16_t code) { return std::min<uint16_t>(code, 63) / 2.0; }

namespace {

constexpr double vga_step = 0.055744;
constexpr double vga_high = 7.079458;

double vga_linear(uint8_t byte) {
    const double v = (byte & 0x7f) * vga_step;
    return byte & 0x80 ? v * vga_high : v;
}

}  // namespace

double vga_db(uint8_t byte) {
    const double v = vga_linear(byte);
    return v > 0 ? 20 * std::log10(v) : -INFINITY;
}

namespace {

// The code of one mode nearest to a gain, in dB rather than in volts: at the
// low codes one step is several dB, and rounding the voltage would pick the
// wrong neighbour.
uint8_t nearest_in_mode(double db, bool high) {
    const double target = std::pow(10.0, db / 20) / (vga_step * (high ? vga_high : 1));
    int best = 1;
    double best_error = INFINITY;
    for (int code = std::max(1, static_cast<int>(target) - 1); code <= std::min(127, static_cast<int>(target) + 2);
         ++code) {
        const double error = std::fabs(vga_db(static_cast<uint8_t>(code | (high ? 0x80 : 0))) - db);
        if (error < best_error) {
            best_error = error;
            best = code;
        }
    }
    return static_cast<uint8_t>(best | (high ? 0x80 : 0));
}

}  // namespace

uint8_t vga_byte(double db) {
    db = std::clamp(db, min_vga_db, max_vga_db);
    // The high-gain mode has the lower noise and is taken wherever one of its
    // codes lands within half a dB; its lowest codes are several dB apart,
    // and there, from 0 to about 11 dB, the low-gain mode is the closer one.
    if (db >= 0) {
        const uint8_t high = nearest_in_mode(db, true);
        if (std::fabs(vga_db(high) - db) <= 0.5 || db > max_low_vga_db)
            return high;
    }
    return nearest_in_mode(db, false);
}

std::vector<uint8_t> vga_steps() {
    std::vector<uint8_t> steps;
    for (int db = static_cast<int>(min_vga_db); db <= static_cast<int>(max_vga_db); ++db) {
        const uint8_t byte = vga_byte(db);
        if (steps.empty() || vga_db(byte) > vga_db(steps.back()) + 0.25)
            steps.push_back(byte);
    }
    return steps;
}

double achieved_sample_rate(uint32_t requested_hz) {
    if (requested_hz < min_sample_rate || requested_hz > max_sample_rate)
        return 0;
    constexpr uint64_t crystal = 27000000;
    constexpr uint64_t denominator = 1048575;
    uint64_t divider = 900000000ull / requested_hz;
    divider -= divider % 2;
    const uint64_t pll = divider * requested_hz;
    const uint64_t whole = pll / crystal;
    const uint64_t numerator = (pll % crystal) * denominator / crystal;
    const double multiplier = static_cast<double>(whole) + static_cast<double>(numerator) / denominator;
    return static_cast<double>(crystal) * multiplier / static_cast<double>(divider);
}

void derandomize(int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint16_t s = static_cast<uint16_t>(samples[i]);
        // 0xFFFE where bit 0 is set, 0 where it is not; written without a
        // branch so that the loop vectorises.
        samples[i] = static_cast<int16_t>(s ^ (static_cast<uint16_t>(0u - (s & 1u)) & 0xFFFEu));
    }
}

}  // namespace fern::rx888
