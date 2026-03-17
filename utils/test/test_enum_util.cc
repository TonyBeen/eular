/*************************************************************************
    > File Name: test_enum_util.cc
    > Author: eular
    > Brief:
    > Created Time: Thu 26 Feb 2026 04:26:13 PM CST
 ************************************************************************/

#include <cstdint>
#include <string>

#include "catch/catch.hpp"
#include "utils/enum_util.hpp"

enum class SeqEnum : int
{
    A = 0,
    B,
    C,
    D = 10,
    E,
};

#define SEQ_ENUM_ITEMS(X)   \
    X(SeqEnum, A)           \
    X(SeqEnum, B)           \
    X(SeqEnum, C)           \
    X(SeqEnum, D)           \
    X(SeqEnum, E)           \

ENUM_UTIL_REGISTER_ENUM(SeqEnum, SEQ_ENUM_ITEMS, 5)

enum class FlagEnum : uint32_t
{
    None        = 0u,
    A           = (1u << 0),
    B           = (1u << 2),
    C           = (1u << 3),
    Combo_AB    = (A | B),           // 组合值：非单bit
    HighBit     = (1u << 31),
};

ENUM_UTIL_ENABLE_BITMASK(FlagEnum);

#define FLAG_ENUM_ITEMS(X)  \
    X(FlagEnum, None)       \
    X(FlagEnum, A)          \
    X(FlagEnum, B)          \
    X(FlagEnum, C)          \
    X(FlagEnum, Combo_AB)   \
    X(FlagEnum, HighBit)    \

ENUM_UTIL_REGISTER_ENUM(FlagEnum, FLAG_ENUM_ITEMS, 6)

TEST_CASE("to_underlying returns underlying integer", "[enum_util]")
{
    REQUIRE(enum_util::to_underlying(SeqEnum::A) == 0);
    REQUIRE(enum_util::to_underlying(SeqEnum::D) == 10);

    REQUIRE(enum_util::to_underlying(FlagEnum::None) == 0u);
    REQUIRE(enum_util::to_underlying(FlagEnum::A) == (1u << 0));
    REQUIRE(enum_util::to_underlying(FlagEnum::HighBit) == (1u << 31));
}

TEST_CASE("to_string for sequential enum hits each switch branch and default", "[enum_util]")
{
    REQUIRE(std::string(enum_util::to_string(SeqEnum::A)) == "A");
    REQUIRE(std::string(enum_util::to_string(SeqEnum::B)) == "B");
    REQUIRE(std::string(enum_util::to_string(SeqEnum::C)) == "C");
    REQUIRE(std::string(enum_util::to_string(SeqEnum::D)) == "D");
    REQUIRE(std::string(enum_util::to_string(SeqEnum::E)) == "E");

    // default branch: unknown value -> nullptr
    const SeqEnum unknown = static_cast<SeqEnum>(999);
    REQUIRE(enum_util::to_string(unknown) == nullptr);
}

TEST_CASE("from_string(const char*) for sequential enum covers null, match, and miss", "[enum_util]")
{
    SeqEnum out = SeqEnum::A;

    // null input branch
    REQUIRE(enum_util::from_string<SeqEnum>(static_cast<const char*>(nullptr), out) == false);

    // matches
    REQUIRE(enum_util::from_string<SeqEnum>("A", out) == true);
    REQUIRE(out == SeqEnum::A);
    REQUIRE(enum_util::from_string<SeqEnum>("D", out) == true);
    REQUIRE(out == SeqEnum::D);

    // miss branch
    REQUIRE(enum_util::from_string<SeqEnum>("NotAValue", out) == false);
}

TEST_CASE("from_string(std::string) forwards correctly", "[enum_util]")
{
    SeqEnum out;
    REQUIRE(enum_util::from_string<SeqEnum>(std::string("E"), out) == true);
    REQUIRE(out == SeqEnum::E);
}

TEST_CASE("bitmask operators | & ^ ~ |= &= ^=", "[enum_util][flags]")
{
    FlagEnum v = FlagEnum::None;

    v |= FlagEnum::A;
    REQUIRE(enum_util::to_underlying(v) == enum_util::to_underlying(FlagEnum::A));

    v |= FlagEnum::B;
    REQUIRE(enum_util::has(v, FlagEnum::A) == true);
    REQUIRE(enum_util::has(v, FlagEnum::B) == true);
    REQUIRE(enum_util::has(v, FlagEnum::C) == false);

    // operator&
    const FlagEnum ab = FlagEnum::A | FlagEnum::B;
    REQUIRE(enum_util::to_underlying(v & ab) == enum_util::to_underlying(ab));

    // operator^ and ^=
    FlagEnum x = FlagEnum::A;
    x ^= FlagEnum::A;
    REQUIRE(enum_util::to_underlying(x) == 0u);

    // operator~
    const auto not_none = ~FlagEnum::None;
    REQUIRE(enum_util::to_underlying(not_none) == ~0u);

    // &= clears
    v &= ~FlagEnum::A;
    REQUIRE(enum_util::has(v, FlagEnum::A) == false);
    REQUIRE(enum_util::has(v, FlagEnum::B) == true);
}

TEST_CASE("has/set/clear helpers cover branches", "[enum_util][flags]")
{
    FlagEnum v = FlagEnum::None;

    enum_util::set(v, FlagEnum::C);
    REQUIRE(enum_util::has(v, FlagEnum::C) == true);

    enum_util::clear(v, FlagEnum::C);
    REQUIRE(enum_util::has(v, FlagEnum::C) == false);

    // has false branch
    REQUIRE(enum_util::has(v, FlagEnum::A) == false);
}

TEST_CASE("to_string for flags enum covers known and default", "[enum_util][flags]")
{
    REQUIRE(std::string(enum_util::to_string(FlagEnum::A)) == "A");
    REQUIRE(std::string(enum_util::to_string(FlagEnum::Combo_AB)) == "Combo_AB");

    const FlagEnum unknown = static_cast<FlagEnum>(0x12345678u);
    REQUIRE(enum_util::to_string(unknown) == nullptr);
}

TEST_CASE("flags_to_string: v==0 returns registered zero name", "[enum_util][flags]")
{
    REQUIRE(enum_util::flags_to_string(FlagEnum::None) == "None");
    REQUIRE(enum_util::flags_to_string(static_cast<FlagEnum>(0)) == "None");
}

TEST_CASE("flags_to_string: nonzero builds list and skips non-single-bit Combo_AB", "[enum_util][flags]")
{
    // includes a single-bit and a high bit (still single-bit)
    FlagEnum v = FlagEnum::A | FlagEnum::HighBit;
    const std::string s = enum_util::flags_to_string(v);

    // Order follows registration order in FLAG_ENUM_ITEMS
    REQUIRE(s == "A|HighBit");

    // Combo_AB is non-single-bit -> should not appear when value contains A|B
    FlagEnum ab = FlagEnum::A | FlagEnum::B;
    REQUIRE(enum_util::flags_to_string(ab) == "A|B");
}

TEST_CASE("flags_from_string: parses tokens, trims whitespace, empty token skip, and failure", "[enum_util][flags]")
{
    FlagEnum out = FlagEnum::None;

    // success: single token
    REQUIRE(enum_util::flags_from_string<FlagEnum>("A", out) == true);
    REQUIRE(enum_util::to_underlying(out) == enum_util::to_underlying(FlagEnum::A));

    // success: multi token + whitespace trimming
    REQUIRE(enum_util::flags_from_string<FlagEnum>("  A |  C  ", out) == true);
    REQUIRE(enum_util::has(out, FlagEnum::A) == true);
    REQUIRE(enum_util::has(out, FlagEnum::C) == true);
    REQUIRE(enum_util::has(out, FlagEnum::B) == false);

    // success: empty tokens skipped
    REQUIRE(enum_util::flags_from_string<FlagEnum>("A||C", out) == true);
    REQUIRE(enum_util::flags_to_string(out) == "A|C");

    // failure: unknown token
    REQUIRE(enum_util::flags_from_string<FlagEnum>("A|NoSuchFlag", out) == false);
}

TEST_CASE("flags_from_string: custom separator", "[enum_util][flags]")
{
    FlagEnum out;
    REQUIRE(enum_util::flags_from_string<FlagEnum>("A,C,HighBit", out, ',') == true);
    REQUIRE(enum_util::flags_to_string(out) == "A|C|HighBit");
}