// Fern-RX888, an RX-888 input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <chrono>

#include "stream.h"
#include "test.h"

TEST(ring_rounds_capacity_to_a_power_of_two) {
    CHECK_EQ(fern::RingBuffer(1000).capacity(), size_t(1024));
    CHECK_EQ(fern::RingBuffer(4096).capacity(), size_t(4096));
    CHECK_EQ(fern::RingBuffer(0).capacity(), size_t(2));
}

TEST(ring_wraps_around) {
    fern::RingBuffer ring(16);
    std::vector<uint8_t> data(12);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i + 1);
    REQUIRE(ring.push(data.data(), 12));
    CHECK_EQ(ring.size(), size_t(12));
    const uint8_t* p = nullptr;
    REQUIRE(ring.peek(&p) == 12);
    CHECK_EQ(p[0], uint8_t(1));
    ring.consume(10);
    CHECK_EQ(ring.size(), size_t(2));

    // 12 more bytes: 4 fit before the end, 8 wrap to the start.
    REQUIRE(ring.push(data.data(), 12));
    CHECK_EQ(ring.size(), size_t(14));
    std::vector<uint8_t> out;
    while (ring.size() > 0) {
        const size_t n = ring.peek(&p);
        REQUIRE(n > 0);
        out.insert(out.end(), p, p + n);
        ring.consume(n);
    }
    REQUIRE(out.size() == 14);
    CHECK_EQ(out[0], uint8_t(11));
    CHECK_EQ(out[1], uint8_t(12));
    for (size_t i = 0; i < 12; ++i)
        CHECK_EQ(out[2 + i], uint8_t(i + 1));
    CHECK_EQ(ring.peek(&p), size_t(0));
}

TEST(ring_drops_whole_blocks_when_full) {
    fern::RingBuffer ring(16);
    const uint8_t block[6] = {1, 2, 3, 4, 5, 6};
    CHECK(ring.push(block, 6));
    CHECK(ring.push(block, 6));
    CHECK(!ring.push(block, 6));  // 12 used, 4 free: nothing is stored
    CHECK_EQ(ring.size(), size_t(12));
    CHECK(ring.push(block, 4));
    CHECK_EQ(ring.size(), size_t(16));
    CHECK(!ring.push(block, 1));
    ring.consume(6);
    CHECK(ring.push(block, 6));
    CHECK(!ring.push(block, 17));
}

// One producer pushes numbered blocks as fast as it can while one consumer
// reads slowly. Every byte read must belong to a block that was pushed
// whole, in order, and pushed plus dropped must account for every block.
// The consumer starts only once the producer has tried four times what the
// ring holds, so some blocks are always dropped: left to the scheduler, a
// fast machine's consumer sometimes kept up and nothing was.
TEST(ring_keeps_order_under_concurrency) {
    fern::RingBuffer ring(1 << 12);
    constexpr uint32_t blocks = 20000;
    constexpr size_t block_size = 256;
    std::atomic<bool> done{false};
    std::atomic<uint32_t> tried{0};
    uint32_t pushed = 0;
    uint32_t dropped = 0;

    std::thread producer([&] {
        std::vector<uint8_t> block(block_size);
        for (uint32_t n = 0; n < blocks; ++n, tried.store(n)) {
            std::memcpy(block.data(), &n, sizeof n);
            for (size_t i = sizeof n; i < block_size; ++i)
                block[i] = static_cast<uint8_t>(n + i);
            if (ring.push(block.data(), block.size()))
                ++pushed;
            else
                ++dropped;
        }
        done.store(true);
    });

    while (tried.load() < 4 * (1 << 12) / block_size && !done.load()) std::this_thread::yield();
    std::vector<uint8_t> partial;
    uint32_t received = 0;
    int64_t last = -1;
    bool in_order = true;
    bool intact = true;
    for (;;) {
        const uint8_t* p = nullptr;
        const size_t n = ring.peek(&p);
        if (n == 0) {
            if (done.load() && ring.size() == 0)
                break;
            std::this_thread::yield();
            continue;
        }
        const size_t take = n < 100 ? n : 100;
        partial.insert(partial.end(), p, p + take);
        ring.consume(take);
        while (partial.size() >= block_size) {
            uint32_t id;
            std::memcpy(&id, partial.data(), sizeof id);
            for (size_t i = sizeof id; i < block_size; ++i)
                if (partial[i] != static_cast<uint8_t>(id + i))
                    intact = false;
            if (static_cast<int64_t>(id) <= last)
                in_order = false;
            last = id;
            ++received;
            partial.erase(partial.begin(), partial.begin() + block_size);
        }
    }
    producer.join();
    CHECK(intact);
    CHECK(in_order);
    CHECK(partial.empty());
    CHECK_EQ(received, pushed);
    CHECK_EQ(pushed + dropped, blocks);
    CHECK(dropped > 0);  // the consumer is slower on purpose
}

namespace {

// Hands the stream one transfer of given samples, then waits to be
// cancelled, as a device would between transfers.
class OneTransfer : public fern::Fx3 {
public:
    explicit OneTransfer(std::vector<int16_t> samples) : samples_(std::move(samples)) {}
    int control_out(uint8_t, uint16_t, uint16_t, const uint8_t*, uint16_t, unsigned) override { return 0; }
    int control_in(uint8_t, uint16_t, uint16_t, uint8_t*, uint16_t, unsigned) override { return 0; }
    int stream(fern::SampleCallback cb, void* ctx, uint32_t, uint32_t) override {
        std::vector<uint8_t> bytes(samples_.size() * 2);
        std::memcpy(bytes.data(), samples_.data(), bytes.size());
        cb(bytes.data(), static_cast<uint32_t>(bytes.size()), ctx);
        while (!cancelled_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }
    void cancel_stream() override { cancelled_.store(true); }

private:
    std::vector<int16_t> samples_;
    std::atomic<bool> cancelled_{false};
};

std::vector<int16_t> read_samples(int fd, size_t count) {
    std::vector<int16_t> out(count);
    size_t got = 0;
    auto* p = reinterpret_cast<uint8_t*>(out.data());
    while (got < count * 2) {
        const ssize_t n = ::read(fd, p + got, count * 2 - got);
        if (n <= 0)
            break;
        got += static_cast<size_t>(n);
    }
    out.resize(got / 2);
    return out;
}

}  // namespace

TEST(stream_counts_clipping_and_peak_and_unscrambles_when_asked) {
    for (bool scrambled : {false, true}) {
        std::vector<int16_t> plain = {0, 100, -200, 32767, -32768, 32766, -32767, 7, -7};
        std::vector<int16_t> sent = plain;
        if (scrambled)
            for (int16_t& s : sent)
                if (s & 1)
                    s = static_cast<int16_t>(static_cast<uint16_t>(s) ^ 0xFFFE);
        OneTransfer dev(sent);
        int fds[2];
        REQUIRE(::pipe(fds) == 0);
        fern::Stream stream(dev, fds[1], -1, 1 << 16, 1, 16, scrambled);
        REQUIRE(stream.start());
        const auto got = read_samples(fds[0], plain.size());
        CHECK(got == plain);
        CHECK_EQ(stream.samples_clipped(), uint64_t{2});
        CHECK_EQ(stream.take_peak(), 32768u);
        CHECK_EQ(stream.take_peak(), 0u);
        stream.request_stop();
        CHECK(stream.wait(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
        CHECK_EQ(stream.samples_delivered(), uint64_t{plain.size()});
        ::close(fds[0]);
        ::close(fds[1]);
    }
}
