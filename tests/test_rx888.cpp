// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <cmath>

#include "rx888.h"
#include "test.h"

using namespace fern::rx888;

TEST(gpio_word_turns_the_converter_on_with_the_led_and_nothing_else) {
    Board b;
    CHECK_EQ(gpio_word(b), gpio::led);
    b.adc_on = false;
    CHECK_EQ(gpio_word(b), gpio::adc_shutdown);
}

TEST(gpio_word_sets_each_switch_on_its_own_bit_and_never_the_vhf_input) {
    Board b;
    b.dither = true;
    CHECK_EQ(gpio_word(b), gpio::led | gpio::dither);
    b = Board{};
    b.randomizer = true;
    CHECK_EQ(gpio_word(b), gpio::led | gpio::randomizer);
    b = Board{};
    b.bias_tee = true;
    CHECK_EQ(gpio_word(b), gpio::led | gpio::bias_hf);
    b = Board{};
    b.wide_range = true;
    CHECK_EQ(gpio_word(b), gpio::led | gpio::wide_range);
    b.dither = b.randomizer = b.bias_tee = true;
    CHECK((gpio_word(b) & (gpio::vhf_enable | gpio::bias_vhf | gpio::adc_shutdown)) == 0);
    // The bits the firmware documents for the MkII.
    CHECK_EQ(gpio::adc_shutdown, 1u << 5);
    CHECK_EQ(gpio::dither, 1u << 6);
    CHECK_EQ(gpio::randomizer, 1u << 7);
    CHECK_EQ(gpio::bias_hf, 1u << 8);
    CHECK_EQ(gpio::wide_range, 1u << 16);
}

TEST(attenuator_code_counts_half_decibels_rounds_and_clamps) {
    CHECK_EQ(attenuator_code(0), 0);
    CHECK_EQ(attenuator_code(0.5), 1);
    CHECK_EQ(attenuator_code(10), 20);
    CHECK_EQ(attenuator_code(10.2), 20);
    CHECK_EQ(attenuator_code(10.3), 21);
    CHECK_EQ(attenuator_code(31.5), 63);
    CHECK_EQ(attenuator_code(40), 63);
    CHECK_EQ(attenuator_code(-3), 0);
    CHECK_EQ(attenuator_code(NAN), 0);
    CHECK_EQ(attenuator_db(21), 10.5);
    CHECK_EQ(attenuator_db(99), 31.5);
}

TEST(vga_gain_follows_the_ad8370_formula_in_both_modes) {
    // Code 127 in high-gain mode: 127 * 0.055744 * 7.079458, 34.0 dB.
    CHECK(std::fabs(vga_db(0x80 | 127) - 34.0) < 0.05);
    // Code 1 in low-gain mode: -25.1 dB.
    CHECK(std::fabs(vga_db(1) - (-25.08)) < 0.05);
    // The high-gain mode is 17 dB above the low one for the same code.
    CHECK(std::fabs(vga_db(0x80 | 40) - vga_db(40) - 17.0) < 0.01);
    CHECK(std::isinf(vga_db(0)));
}

TEST(vga_byte_prefers_the_quieter_high_gain_mode_where_it_is_accurate) {
    for (double db = -25; db <= 34; db += 0.25) {
        const uint8_t b = vga_byte(db);
        const bool high = (b & 0x80) != 0;
        CHECK((b & 0x7f) >= 1);
        if (db < 0)
            CHECK(!high);
        if (db >= 12)
            CHECK(high);
        // No other code of that mode comes closer.
        for (int code = 1; code <= 127; ++code) {
            const uint8_t other = static_cast<uint8_t>(code | (high ? 0x80 : 0));
            if (std::fabs(vga_db(other) - db) < std::fabs(vga_db(b) - db) - 1e-9) {
                CHECK_EQ(static_cast<int>(b), static_cast<int>(other));
                break;
            }
        }
    }
    // From -6 dB up some code lies within half a dB, and it is found.
    for (double db = -6; db <= 34; db += 0.25)
        CHECK(std::fabs(vga_db(vga_byte(db)) - db) <= 0.5);
    CHECK_EQ(vga_byte(-100), vga_byte(-25));
    CHECK_EQ(vga_byte(100), static_cast<uint8_t>(0x80 | 127));
    CHECK(std::fabs(vga_db(127) - max_low_vga_db) < 0.01);
}

TEST(vga_steps_rise_by_about_a_decibel_across_the_whole_range) {
    const auto steps = vga_steps();
    REQUIRE(steps.size() > 40);
    for (size_t i = 1; i < steps.size(); ++i) {
        CHECK(vga_db(steps[i]) > vga_db(steps[i - 1]));
        if (vga_db(steps[i - 1]) > -8)
            CHECK(vga_db(steps[i]) - vga_db(steps[i - 1]) < 2.0);
    }
    CHECK(vga_db(steps.front()) < -20);
    CHECK(vga_db(steps.back()) > 33);
}

TEST(achieved_sample_rate_is_exact_where_the_si5351_can_be) {
    CHECK(std::fabs(achieved_sample_rate(64800000) - 64800000.0) < 1e-6);
    CHECK(std::fabs(achieved_sample_rate(129600000) - 129600000.0) < 1e-6);
    // Elsewhere a few parts in 10^8 low, well inside FernSDR's 100 ppm.
    for (uint32_t hz : {100000000u, 64000000u, 130000000u, 10000000u, 76800000u}) {
        const double got = achieved_sample_rate(hz);
        CHECK(got <= hz + 1e-6);
        CHECK((hz - got) / hz < 1e-6);
    }
    CHECK_EQ(achieved_sample_rate(9999999), 0.0);
    CHECK_EQ(achieved_sample_rate(130000001), 0.0);
}

TEST(derandomize_undoes_the_ltc2208_randomizer_and_leaves_even_samples_alone) {
    for (int v = -32768; v <= 32767; v += 1) {
        const uint16_t plain = static_cast<uint16_t>(v);
        const uint16_t scrambled = (plain & 1) ? plain ^ 0xFFFE : plain;
        int16_t s = static_cast<int16_t>(scrambled);
        derandomize(&s, 1);
        if (s != static_cast<int16_t>(plain)) {
            CHECK_EQ(s, static_cast<int16_t>(plain));
            break;
        }
    }
    int16_t block[4] = {1, 2, 3, -1};
    derandomize(block, 4);
    CHECK_EQ(block[0], static_cast<int16_t>(0xFFFF));
    CHECK_EQ(block[1], 2);
    CHECK_EQ(block[2], static_cast<int16_t>(0xFFFD));
    CHECK_EQ(block[3], 1);
}
