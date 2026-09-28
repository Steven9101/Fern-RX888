// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <string>

#include "json.h"
#include "settings.h"
#include "test.h"

using fern::json::Value;

namespace {

Value parsed(const std::string& text) {
    Value v;
    std::string error;
    REQUIRE(fern::json::parse(text, v, error));
    return v;
}

Value open_with(const std::string& settings, const std::string& rate = "64800000", const std::string& center = "0",
                const std::string& signal = "\"real\"") {
    return parsed("{\"type\":\"open\",\"sample_rate\":" + rate + ",\"center\":" + center + ",\"signal\":" + signal +
                  ",\"settings\":" + settings + "}");
}

std::string refusal(const Value& open) {
    fern::OpenRequest r;
    const auto f = fern::parse_open(open, r);
    if (!f)
        return "";
    CHECK(f->code == fern::ErrorCode::invalid);
    return f->message;
}

}  // namespace

TEST(parse_open_takes_the_defaults) {
    fern::OpenRequest r;
    REQUIRE(!fern::parse_open(open_with("{}"), r));
    CHECK_EQ(r.sample_rate, 64800000u);
    CHECK(r.settings.gain.automatic());
    CHECK_EQ(r.settings.attenuation, 0.0);
    CHECK(!r.settings.bias_tee && !r.settings.dither && !r.settings.randomizer);
    CHECK(r.settings.adc_range == fern::AdcRange::narrow);
    CHECK(r.settings.firmware.empty());
    CHECK_EQ(r.settings.transfers, fern::default_transfers);
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::only);
}

TEST(parse_open_wants_a_real_band_from_zero_hertz_at_a_possible_rate) {
    CHECK_HAS(refusal(open_with("{}", "64800000", "0", "\"iq\"")), "signal = real");
    CHECK_HAS(refusal(open_with("{}", "64800000", "7100000")), "center = 0");
    CHECK_HAS(refusal(open_with("{}", "9999999")), "10000000 to 130000000");
    CHECK_HAS(refusal(open_with("{}", "130000001")), "130000000");
    CHECK_HAS(refusal(open_with("{}", "64800000.5")), "64800000");
    CHECK_EQ(refusal(open_with("{}", "129600000")), std::string());
    CHECK_EQ(refusal(open_with("{}", "10000000")), std::string());
}

TEST(parse_open_reads_every_setting) {
    fern::OpenRequest r;
    REQUIRE(!fern::parse_open(
        open_with("{\"device\":\"serial:0123abcdef\",\"gain\":\"12.5\",\"attenuation\":10.5,\"bias_tee\":true,"
                  "\"dither\":true,\"randomizer\":true,\"adc_range\":\"2.25\",\"firmware\":\"/opt/fw.img\","
                  "\"transfers\":24}"),
        r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::serial);
    CHECK_EQ(r.settings.device.serial, std::string("0123ABCDEF"));
    CHECK(!r.settings.gain.automatic());
    CHECK_EQ(r.settings.gain.db, 12.5);
    CHECK_EQ(r.settings.attenuation, 10.5);
    CHECK(r.settings.bias_tee && r.settings.dither && r.settings.randomizer);
    CHECK(r.settings.adc_range == fern::AdcRange::wide);
    CHECK_EQ(r.settings.firmware, std::string("/opt/fw.img"));
    CHECK_EQ(r.settings.transfers, 24u);
    REQUIRE(!fern::parse_open(open_with("{\"device\":\"port:2-1.4\",\"gain\":-20}"), r));
    CHECK(r.settings.device.kind == fern::DeviceSelector::Kind::port);
    CHECK_EQ(r.settings.device.port, std::string("2-1.4"));
    CHECK_EQ(r.settings.gain.db, -20.0);
    REQUIRE(!fern::parse_open(open_with("{\"device\":\"index:1\",\"gain\":\"AUTO\"}"), r));
    CHECK_EQ(r.settings.device.index, 1u);
    CHECK(r.settings.gain.automatic());
}

TEST(parse_open_refuses_values_it_cannot_honour) {
    CHECK_HAS(refusal(open_with("{\"device\":\"serial:xyz\"}")), "module.device");
    CHECK_HAS(refusal(open_with("{\"device\":\"port:2-\"}")), "module.device");
    CHECK_HAS(refusal(open_with("{\"device\":\"port:1.2-3\"}")), "module.device");
    CHECK_HAS(refusal(open_with("{\"gain\":\"40\"}")), "-25 to 34");
    CHECK_HAS(refusal(open_with("{\"gain\":\"loud\"}")), "module.gain");
    CHECK_HAS(refusal(open_with("{\"attenuation\":10.25}")), "steps of 0.5");
    CHECK_HAS(refusal(open_with("{\"attenuation\":32}")), "0 to 31.5");
    CHECK_HAS(refusal(open_with("{\"bias_tee\":\"yes\"}")), "yes or no");
    CHECK_HAS(refusal(open_with("{\"adc_range\":\"2\"}")), "1.5 or 2.25");
    CHECK_HAS(refusal(open_with("{\"firmware\":\"fw.img\"}")), "full path");
    CHECK_HAS(refusal(open_with("{\"transfers\":3}")), "4 to 24");
    CHECK_HAS(refusal(open_with("{\"transfers\":25}")), "4 to 24");
    CHECK_HAS(refusal(open_with("{\"gainn\":1,\"tuner\":2}")), "unknown settings module.gainn, module.tuner");
}

TEST(parse_set_takes_only_live_settings) {
    fern::LiveChange c;
    REQUIRE(!fern::parse_set(parsed("{\"gain\":\"20\",\"attenuation\":3,\"bias_tee\":true,\"dither\":false}"), c));
    CHECK(c.gain && !c.gain->automatic() && c.gain->db == 20);
    CHECK(c.attenuation && *c.attenuation == 3);
    CHECK(c.bias_tee && *c.bias_tee);
    CHECK(c.dither && !*c.dither);
    const auto f = fern::parse_set(parsed("{\"randomizer\":true}"), c);
    REQUIRE(f.has_value());
    CHECK_HAS(f->message, "restart the band");
    CHECK(fern::parse_set(parsed("[1]"), c).has_value());
}

TEST(the_schema_lists_every_setting_in_order_with_its_liveness) {
    const Value schema = fern::settings_schema();
    const char* keys[] = {"device", "gain", "attenuation", "bias_tee", "dither", "randomizer", "adc_range",
                          "firmware", "transfers"};
    const bool live[] = {false, true, true, true, true, false, false, false, false};
    REQUIRE(schema.items().size() == 9);
    for (size_t i = 0; i < 9; ++i) {
        const Value& s = schema.items()[i];
        CHECK_EQ(s.find("key")->as_string(), std::string(keys[i]));
        CHECK_EQ(s.find("live")->as_bool(), live[i]);
        CHECK(s.find("help") && !s.find("help")->as_string().empty());
    }
    const Value d = fern::describe_module();
    CHECK_EQ(d.find("id")->as_string(), std::string("rx888"));
    CHECK_EQ(d.find("kind")->as_string(), std::string("input"));
}
