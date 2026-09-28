// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Runs the session loop in a thread with real pipes in place of fds 0 to 3
// and plays FernSDR's part.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fake_fx3.h"
#include "rx888.h"
#include "json.h"
#include "session.h"
#include "test.h"

using fern::json::Value;
using Clock = std::chrono::steady_clock;

namespace {

struct Pipe {
    int r = -1;
    int w = -1;
    Pipe() {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) == 0) {
            r = fds[0];
            w = fds[1];
        }
    }
    ~Pipe() {
        close_r();
        close_w();
    }
    void close_r() {
        if (r >= 0)
            ::close(r);
        r = -1;
    }
    void close_w() {
        if (w >= 0)
            ::close(w);
        w = -1;
    }
};

fern::SessionOptions quick_options() {
    fern::SessionOptions o;
    o.stats_interval = std::chrono::milliseconds(100);
    o.stall_timeout = std::chrono::milliseconds(800);
    o.shutdown_timeout = std::chrono::milliseconds(1500);
    o.ring_bytes = 1 << 20;
    return o;
}

const char* const open_line =
    "{\"type\":\"open\",\"sample_rate\":64800000,\"center\":0,\"signal\":\"real\",\"settings\":{}}";

class Harness {
public:
    // With nonblocking, the module's ends of fds 0, 1 and 3 are handed over
    // with O_NONBLOCK set, as a host that uses pipe2(O_NONBLOCK) would.
    explicit Harness(fake::Backend& backend, fern::SessionOptions options = quick_options(),
                     bool nonblocking = false) {
        REQUIRE(commands_.r >= 0 && samples_.r >= 0 && events_.r >= 0 && stop_.r >= 0);
        if (nonblocking)
            for (int fd : {commands_.r, samples_.w, events_.w})
                REQUIRE(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) == 0);
        fern::SessionIo io;
        io.commands = commands_.r;
        io.samples = samples_.w;
        io.events = events_.w;
        io.stop = stop_.r;
        thread_ = std::thread([this, &backend, io, options] {
            result_ = fern::run_session(backend, io, options);
            done_.store(true);
        });
    }

    ~Harness() {
        // Whatever happened, make the session end so that the thread can be
        // joined: this is what FernSDR's leaving looks like.
        commands_.close_w();
        samples_.close_r();
        events_.close_r();
        if (thread_.joinable())
            thread_.join();
    }

    void send(const std::string& line) {
        const std::string text = line + "\n";
        REQUIRE(::write(commands_.w, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }
    void close_commands() { commands_.close_w(); }
    void close_samples() { samples_.close_r(); }
    void signal_stop() { REQUIRE(::write(stop_.w, "x", 1) == 1); }
    // Makes the events pipe hold only one page, so that a few unread events
    // fill it.
    void shrink_events() { REQUIRE(::fcntl(events_.w, F_SETPIPE_SZ, 4096) >= 0); }

    // The next event on fd 3, or null after the timeout.
    Value event(int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const size_t nl = events_buf_.find('\n');
            if (nl != std::string::npos) {
                const std::string line = events_buf_.substr(0, nl);
                events_buf_.erase(0, nl + 1);
                Value v;
                std::string error;
                if (!fern::json::parse(line, v, error))
                    test::report(__FILE__, __LINE__, "event is not valid JSON: " + line + ": " + error);
                if (line.size() > 65536)
                    test::report(__FILE__, __LINE__, "event longer than 64 KiB");
                lines_.push_back(line);
                return v;
            }
            const int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left <= 0)
                return Value();
            struct pollfd pfd = {events_.r, POLLIN, 0};
            if (::poll(&pfd, 1, left) <= 0)
                continue;
            char buf[4096];
            const ssize_t n = ::read(events_.r, buf, sizeof buf);
            if (n <= 0)
                return Value();
            events_buf_.append(buf, static_cast<size_t>(n));
        }
    }

    // The next event of the given type, skipping stats. The timeout covers
    // all of them, so a stream of stats cannot keep a test waiting.
    Value expect(const std::string& type, int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            Value v = left > 0 ? event(static_cast<int>(left)) : Value();
            if (v.is_null()) {
                test::report(__FILE__, __LINE__, "no " + type + " event");
                throw test::Stop{};
            }
            const std::string t = v.find("type") ? v.find("type")->as_string() : "";
            if (t == type)
                return v;
            if (t != "stats") {
                test::report(__FILE__, __LINE__, "expected " + type + ", got " + lines_.back());
                throw test::Stop{};
            }
        }
    }

    // Reads exactly n sample bytes.
    std::vector<uint8_t> samples(size_t n, int timeout_ms = 3000) {
        std::vector<uint8_t> out;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (out.size() < n && Clock::now() < deadline) {
            struct pollfd pfd = {samples_.r, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
                continue;
            std::vector<uint8_t> buf(n - out.size());
            const ssize_t got = ::read(samples_.r, buf.data(), buf.size());
            if (got <= 0)
                break;
            out.insert(out.end(), buf.begin(), buf.begin() + got);
        }
        return out;
    }

    // Reads and discards sample bytes for a while; returns how many.
    size_t drain_samples(int ms) {
        size_t total = 0;
        const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
        std::vector<uint8_t> buf(1 << 16);
        while (Clock::now() < deadline) {
            struct pollfd pfd = {samples_.r, POLLIN, 0};
            if (::poll(&pfd, 1, 20) <= 0)
                continue;
            const ssize_t got = ::read(samples_.r, buf.data(), buf.size());
            if (got <= 0)
                break;
            total += static_cast<size_t>(got);
        }
        return total;
    }

    bool samples_pending() {
        struct pollfd pfd = {samples_.r, POLLIN, 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

    // Waits for the session to end and returns its exit status.
    int exit_status(int timeout_ms = 4000, bool clean = true) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done_.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(done_.load());
        thread_.join();
        CHECK_EQ(result_.clean, clean);
        return result_.status;
    }

    bool done() const { return done_.load(); }
    const std::vector<std::string>& lines() const { return lines_; }

private:
    Pipe commands_;
    Pipe samples_;
    Pipe events_;
    Pipe stop_;
    std::thread thread_;
    std::atomic<bool> done_{false};
    fern::SessionResult result_;
    std::string events_buf_;
    std::vector<std::string> lines_;
};

void check_hello(Harness& h) {
    const Value hello = h.event();
    REQUIRE(hello.is_object());
    CHECK_EQ(fern::json::serialize(hello),
             std::string("{\"type\":\"hello\",\"api\":1,\"id\":\"rx888\",\"version\":\"") + FERN_RX888_VERSION +
                 "\",\"kind\":\"input\"}");
}

std::string text_of(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f && f->is_string() ? f->as_string() : "";
}

// Sample i on fd 1, as the module must deliver it: the fake's ramp,
// unscrambled.
bool samples_are_the_ramp(const std::vector<uint8_t>& s) {
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const int16_t v = static_cast<int16_t>(static_cast<uint16_t>(s[i] | s[i + 1] << 8));
        if (v != fake::ramp_sample(i / 2))
            return false;
    }
    return true;
}

}  // namespace

TEST(session_full_exchange) {
    fake::Backend backend;
    fake::Device& dev = backend.add(fake::Spec{});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":64800000,\"center\":0,\"signal\":\"real\","
           "\"settings\":{\"gain\":\"auto\",\"attenuation\":3}}");
    const Value ready = h.expect("ready", 5000);
    CHECK_EQ(text_of(ready, "format"), std::string("s16"));
    CHECK_EQ(text_of(ready, "signal"), std::string("real"));
    CHECK_EQ(ready.find("sample_rate")->as_number(), 64800000.0);
    CHECK_EQ(ready.find("center")->as_number(), 0.0);
    CHECK_EQ(text_of(*ready.find("device"), "name"), std::string("RX-888 MkII"));
    CHECK_EQ(text_of(*ready.find("device"), "serial"), std::string("0123456789ABCDEF"));
    CHECK_EQ(text_of(*ready.find("settings"), "gain"), std::string("auto"));
    CHECK_EQ(ready.find("settings")->find("attenuation")->as_number(), 3.0);

    // Every sample on fd 1 is the fake's, in order, from the first one.
    const std::vector<uint8_t> s = h.samples(600000);
    REQUIRE(s.size() == 600000);
    CHECK(samples_are_the_ramp(s));

    const Value stats = h.expect("stats");
    CHECK(stats.find("samples")->as_number() > 0);
    // The ramp visits the converter's limits: 2 of every 65536 samples.
    CHECK(stats.find("clipping")->as_number() > 0);
    CHECK(stats.find("gain"));

    h.send("{\"type\":\"set\",\"id\":7,\"settings\":{\"attenuation\":12.5,\"bias_tee\":true}}");
    CHECK_EQ(fern::json::serialize(h.expect("applied")),
             std::string("{\"type\":\"applied\",\"id\":7,\"settings\":{\"attenuation\":12.5,\"bias_tee\":true}}"));
    CHECK_EQ(dev.attenuator, 25);
    CHECK((dev.gpio & fern::rx888::gpio::bias_hf) != 0);

    h.send("{\"type\":\"set\",\"id\":8,\"settings\":{\"randomizer\":true}}");
    const Value refused = h.expect("error");
    CHECK_EQ(refused.find("id")->as_number(), 8.0);
    CHECK_HAS(text_of(refused, "message"), "restart the band");
    CHECK(!refused.find("fatal")->as_bool());
    CHECK(h.drain_samples(100) > 0);

    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    // Stopped, clock off, converter shut down, and the device closed.
    CHECK(!dev.streaming);
    CHECK_EQ(dev.adc_hz, 0u);
    CHECK((dev.gpio & fern::rx888::gpio::adc_shutdown) != 0);
    CHECK_EQ(dev.open_handles.load(), 0);
}

TEST(session_unscrambles_the_randomizer) {
    fake::Backend backend;
    fake::Device& dev = backend.add(fake::Spec{});
    Harness h(backend);
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":129600000,\"center\":0,\"signal\":\"real\","
           "\"settings\":{\"randomizer\":true}}");
    const Value ready = h.expect("ready", 5000);
    CHECK_EQ(ready.find("sample_rate")->as_number(), 129600000.0);
    CHECK((dev.gpio & fern::rx888::gpio::randomizer) != 0);
    const std::vector<uint8_t> s = h.samples(400000);
    REQUIRE(s.size() == 400000);
    CHECK(samples_are_the_ramp(s));
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_reports_unplug) {
    fake::Backend backend;
    fake::Spec s;
    s.unplug_after = 4 << 20;
    backend.add(s);
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.drain_samples(100);
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK(e.find("fatal")->as_bool());
    CHECK_HAS(text_of(e, "message"), "unplugged");
    CHECK_EQ(h.exit_status(), 5);
}

TEST(session_reports_a_stream_that_does_not_start) {
    fake::Backend backend;
    fake::Spec s;
    s.stream_error = fern::usb_error::no_mem;
    backend.add(s);
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("invalid"));
    CHECK_HAS(text_of(e, "message"), "no USB memory for 16 transfers");
    CHECK_HAS(text_of(e, "message"), "usbfs_memory_mb");
    CHECK_EQ(h.exit_status(), 6);

    fake::Backend broken;
    s.stream_error = fern::usb_error::io;
    broken.add(s);
    Harness g(broken);
    check_hello(g);
    g.send(open_line);
    g.expect("ready", 5000);
    const Value f = g.expect("error");
    CHECK_EQ(text_of(f, "code"), std::string("lost"));
    CHECK_HAS(text_of(f, "message"), "did not start streaming");
    CHECK_EQ(g.exit_status(), 5);
}

TEST(session_reports_a_stall_and_leaves_the_device_to_the_kernel) {
    fake::Backend backend;
    fake::Spec s;
    s.stall_after = 1 << 20;
    fake::Device& dev = backend.add(s);
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    const Value e = h.expect("error", 4000);
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK_HAS(text_of(e, "message"), "sent no samples for 0.8 seconds");
    CHECK_EQ(h.exit_status(), 5);
    // No stop requests to a device that stopped answering.
    for (const auto& r : dev.taken())
        CHECK(r.request != fern::rx888::request::stop_stream);
}

TEST(session_reports_missing_and_foreign_devices) {
    {
        fake::Backend backend;
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error");
        CHECK_EQ(text_of(e, "code"), std::string("no-device"));
        CHECK_HAS(text_of(e, "message"), "no RX-888 is plugged in");
        CHECK_EQ(h.exit_status(), 3);
        CHECK(!h.samples_pending());
    }
    {
        fake::Backend backend;
        fake::Spec s;
        s.firmware_running = true;
        s.board = 2;  // an HF103
        backend.add(s);
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        CHECK_HAS(text_of(h.expect("error", 5000), "message"), "not an RX-888 MkII");
        CHECK_EQ(h.exit_status(), 3);
    }
}

TEST(session_refuses_iq_and_invalid_settings_before_touching_usb) {
    fake::Backend backend;
    fake::Device& dev = backend.add(fake::Spec{});
    {
        Harness h(backend);
        check_hello(h);
        h.send("{\"type\":\"open\",\"sample_rate\":64800000,\"center\":0,\"signal\":\"iq\"}");
        CHECK_HAS(text_of(h.expect("error"), "message"), "set signal = real");
        CHECK_EQ(h.exit_status(), 6);
    }
    {
        Harness h(backend);
        check_hello(h);
        h.send("{\"type\":\"open\",\"sample_rate\":64800000,\"center\":7000000,\"signal\":\"real\"}");
        CHECK_HAS(text_of(h.expect("error"), "message"), "center = 0");
        CHECK_EQ(h.exit_status(), 6);
    }
    CHECK(dev.taken().empty());
}

TEST(session_counts_dropped_samples_and_keeps_the_delivered_ones_exact) {
    fake::Backend backend;
    backend.add(fake::Spec{});
    fern::SessionOptions o = quick_options();
    o.ring_bytes = 1 << 20;
    Harness h(backend, o);
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double dropped = 0;
    for (int i = 0; i < 20 && dropped == 0; ++i)
        dropped = h.expect("stats").find("dropped")->as_number();
    CHECK(dropped > 0);
    const std::vector<uint8_t> s = h.samples(8192);
    REQUIRE(s.size() == 8192);
    CHECK(samples_are_the_ramp(s));
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_exits_on_eof_and_on_a_closed_sample_pipe) {
    {
        fake::Backend backend;
        Harness h(backend);
        check_hello(h);
        h.close_commands();
        CHECK_EQ(h.exit_status(), 0);
    }
    {
        fake::Backend backend;
        backend.add(fake::Spec{});
        Harness h(backend);
        check_hello(h);
        h.send(open_line);
        h.expect("ready", 5000);
        h.close_samples();
        CHECK_EQ(h.exit_status(), 0);
    }
}

TEST(session_stops_on_signal) {
    fake::Backend backend;
    backend.add(fake::Spec{});
    Harness h(backend);
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.signal_stop();
    CHECK_EQ(h.exit_status(), 0);
}
