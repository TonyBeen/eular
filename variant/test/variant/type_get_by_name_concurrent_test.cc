#include <catch/catch.hpp>

#include <variant/type.h>
#include <variant/detail/type/type_register.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace rttr;

namespace
{

struct concurrent_name_type
{
    int value = 0;
};

} // namespace

TEST_CASE("type::get_by_name() concurrent access", "[type][concurrency]")
{
    auto target_type = type::get<concurrent_name_type>();
    std::string name_a = "concurrent_name_type_a";
    std::string name_b = "concurrent_name_type_b";
    detail::type_register::custom_name(target_type, name_a);

    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<int> valid_reads{0};
    std::atomic<int> invalid_reads{0};

    std::thread writer([&]() {
        while (!start.load(std::memory_order_acquire))
        {
        }

        for (int i = 0; i < 50000; ++i)
        {
            detail::type_register::custom_name(target_type, ((i % 2) == 0 ? name_b : name_a));
        }

        stop.store(true, std::memory_order_release);
    });

    std::vector<std::thread> readers;
    for (int i = 0; i < 4; ++i)
    {
        readers.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire))
            {
            }

            while (!stop.load(std::memory_order_acquire))
            {
                auto t1 = type::get_by_name(name_a);
                auto t2 = type::get_by_name(name_b);

                if (t1.is_valid())
                    ++valid_reads;
                else
                    ++invalid_reads;

                if (t2.is_valid())
                    ++valid_reads;
                else
                    ++invalid_reads;
            }
        });
    }

    start.store(true, std::memory_order_release);

    writer.join();
    for (auto& reader : readers)
        reader.join();

    CHECK(valid_reads.load() > 0);
    CHECK(invalid_reads.load() >= 0);

    detail::type_register::custom_name(target_type, name_a);
}