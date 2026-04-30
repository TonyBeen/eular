/*************************************************************************
    > File Name: test_ring_buffer.cc
    > Author: eular
    > Brief:
 ************************************************************************/

#include <catch2/catch.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <random>
#include <vector>

#include "util/ring_buffer.h"

using eular::utp::RingBuffer;
using eular::utp::Stream;

TEST_CASE("RingBuffer: write/read keeps byte order", "[RingBuffer]")
{
    RingBuffer rb(8);

    const std::array<uint8_t, 5> first = {1, 2, 3, 4, 5};
    REQUIRE(rb.write(first.data(), first.size()) == first.size());
    REQUIRE(rb.size() == first.size());

    std::array<uint8_t, 3> out1{};
    REQUIRE(rb.read(out1.data(), out1.size()) == out1.size());
    REQUIRE(out1 == std::array<uint8_t, 3>{1, 2, 3});

    const std::array<uint8_t, 5> second = {6, 7, 8, 9, 10};
    REQUIRE(rb.write(second.data(), second.size()) == second.size());
    REQUIRE(rb.size() == 7);

    std::array<uint8_t, 7> out2{};
    REQUIRE(rb.read(out2.data(), out2.size()) == out2.size());
    REQUIRE(out2 == std::array<uint8_t, 7>{4, 5, 6, 7, 8, 9, 10});
    REQUIRE(rb.empty());
}

TEST_CASE("RingBuffer: readableViewsFrom handles wrapped data", "[RingBuffer]")
{
    RingBuffer rb(8);

    const std::array<uint8_t, 6> prefix = {1, 2, 3, 4, 5, 6};
    REQUIRE(rb.write(prefix.data(), prefix.size()) == prefix.size());

    std::array<uint8_t, 5> consumed{};
    REQUIRE(rb.read(consumed.data(), consumed.size()) == consumed.size());
    REQUIRE(consumed == std::array<uint8_t, 5>{1, 2, 3, 4, 5});

    const std::array<uint8_t, 4> wrap = {7, 8, 9, 10};
    REQUIRE(rb.write(wrap.data(), wrap.size()) == wrap.size());

    Stream::ConstBufferView views[2];
    const size_t count = rb.readableViewsFrom(views, 2, 4);
    REQUIRE(count == 2);
    REQUIRE(views[0].len == 1);
    REQUIRE(views[1].len == 2);
    REQUIRE(std::memcmp(views[0].data, std::array<uint8_t, 1>{8}.data(), 1) == 0);
    REQUIRE(std::memcmp(views[1].data, std::array<uint8_t, 2>{9, 10}.data(), 2) == 0);
}

TEST_CASE("RingBuffer: ensureFree grows and preserves payload", "[RingBuffer]")
{
    RingBuffer rb(4);

    const std::array<uint8_t, 3> src1 = {1, 2, 3};
    REQUIRE(rb.write(src1.data(), src1.size()) == src1.size());

    uint8_t one = 0;
    REQUIRE(rb.read(&one, 1) == 1);
    REQUIRE(one == 1);

    const std::array<uint8_t, 2> src2 = {4, 5};
    REQUIRE(rb.write(src2.data(), src2.size()) == src2.size());

    const size_t oldCap = rb.capacity();
    rb.ensureFree(6);
    REQUIRE(rb.capacity() >= rb.size() + 6);
    REQUIRE(rb.capacity() > oldCap);

    Stream::ConstBufferView views[2];
    const size_t count = rb.readableViews(views, rb.size());
    REQUIRE(count == 1);

    std::array<uint8_t, 4> out{};
    REQUIRE(rb.read(out.data(), out.size()) == out.size());
    REQUIRE(out == std::array<uint8_t, 4>{2, 3, 4, 5});
}

TEST_CASE("RingBuffer: writableViews can split and produce", "[RingBuffer]")
{
    RingBuffer rb(5);

    const std::array<uint8_t, 4> src = {1, 2, 3, 4};
    REQUIRE(rb.write(src.data(), src.size()) == src.size());
    rb.consume(3);

    Stream::MutableBufferView views[2];
    const size_t count = rb.writableViews(views, 4);
    REQUIRE(count == 2);
    REQUIRE(views[0].len == 1);
    REQUIRE(views[1].len == 3);

    const std::array<uint8_t, 4> append = {5, 6, 7, 8};
    std::memcpy(views[0].data, append.data(), views[0].len);
    std::memcpy(views[1].data, append.data() + views[0].len, views[1].len);
    rb.produce(views[0].len + views[1].len);

    std::array<uint8_t, 5> out{};
    REQUIRE(rb.read(out.data(), out.size()) == out.size());
    REQUIRE(out == std::array<uint8_t, 5>{4, 5, 6, 7, 8});
}

TEST_CASE("RingBuffer: defensive behavior for zero-length and null pointers", "[RingBuffer]")
{
    RingBuffer rb(4);

    Stream::ConstBufferView rviews[2];
    Stream::MutableBufferView wviews[2];

    REQUIRE(rb.readableViews(nullptr, 4) == 0);
    REQUIRE(rb.readableViewsFrom(nullptr, 0, 4) == 0);
    REQUIRE(rb.writableViews(nullptr, 4) == 0);

    REQUIRE(rb.readableViews(rviews, 0) == 0);
    REQUIRE(rb.readableViewsFrom(rviews, 0, 0) == 0);
    REQUIRE(rb.writableViews(wviews, 0) == 0);

    REQUIRE(rb.readableViewsFrom(rviews, 1, 4) == 0);
    REQUIRE(rb.write(nullptr, 0) == 0);
    REQUIRE(rb.read(nullptr, 4) == 0);
    std::array<uint8_t, 1> one{};
    REQUIRE(rb.read(one.data(), 0) == 0);

    std::array<uint8_t, 3> src = {1, 2, 3};
    REQUIRE(rb.write(src.data(), src.size()) == src.size());
    const size_t before = rb.size();

    rb.produce(100);
    REQUIRE(rb.size() == rb.capacity());

    rb.consume(100);
    REQUIRE(rb.size() == 0);

    std::array<uint8_t, 2> dst{};
    REQUIRE(rb.read(dst.data(), dst.size()) == 0);
    REQUIRE(before > 0);
}

TEST_CASE("RingBuffer: randomized read/write remains consistent across growth", "[RingBuffer]")
{
    RingBuffer rb(2);
    std::deque<uint8_t> ref;
    std::mt19937 rng(123456u);

    for (int step = 0; step < 600; ++step) {
        const bool doWrite = ref.empty() || ((rng() % 100) < 65);
        if (doWrite) {
            const size_t n = static_cast<size_t>((rng() % 9) + 1);
            std::vector<uint8_t> in(n);
            for (size_t i = 0; i < n; ++i) {
                in[i] = static_cast<uint8_t>(rng() & 0xffu);
                ref.push_back(in[i]);
            }
            REQUIRE(rb.write(in.data(), in.size()) == in.size());
        } else {
            const size_t want = std::min<size_t>(static_cast<size_t>((rng() % 9) + 1), ref.size());
            std::vector<uint8_t> out(want, 0);
            REQUIRE(rb.read(out.data(), out.size()) == want);
            for (size_t i = 0; i < want; ++i) {
                REQUIRE(out[i] == ref.front());
                ref.pop_front();
            }
        }

        REQUIRE(rb.size() == ref.size());
        REQUIRE(rb.freeSize() == rb.capacity() - rb.size());

        Stream::ConstBufferView views[2];
        const size_t count = rb.readableViews(views, rb.size());
        size_t flattened = 0;
        std::vector<uint8_t> snapshot;
        for (size_t i = 0; i < count; ++i) {
            if (views[i].data != nullptr && views[i].len > 0) {
                flattened += views[i].len;
                const uint8_t *p = static_cast<const uint8_t *>(views[i].data);
                snapshot.insert(snapshot.end(), p, p + views[i].len);
            }
        }
        REQUIRE(flattened == ref.size());
        REQUIRE(snapshot.size() == ref.size());
        for (size_t i = 0; i < snapshot.size(); ++i) {
            REQUIRE(snapshot[i] == ref[i]);
        }
    }

    if (!ref.empty()) {
        std::vector<uint8_t> out(ref.size(), 0);
        REQUIRE(rb.read(out.data(), out.size()) == out.size());
        for (size_t i = 0; i < out.size(); ++i) {
            REQUIRE(out[i] == ref.front());
            ref.pop_front();
        }
    }
    REQUIRE(ref.empty());
    REQUIRE(rb.empty());
}
