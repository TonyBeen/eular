#include <cctype>
#include <string>

#include "catch/catch.hpp"
#include "utils/uuid.h"

TEST_CASE("uuid_v5_rfc4122_dns_vector", "[uuid]") {
    // RFC 4122 Appendix B: DNS namespace + "www.widgets.com"
    const eular::uuid_t uuid = eular::UUID::V5(eular::UUID::UUID_NS_DNS, "www.widgets.com");
    CHECK(eular::UUID::ToString(uuid) == "21f7f8de-8051-5b89-8680-0195ef798b6a");
}

TEST_CASE("uuid_v5_is_deterministic", "[uuid]") {
    const eular::uuid_t a = eular::UUID::V5(eular::UUID::UUID_NS_DNS, "example.com");
    const eular::uuid_t b = eular::UUID::V5(eular::UUID::UUID_NS_DNS, "example.com");
    const eular::uuid_t c = eular::UUID::V5(eular::UUID::UUID_NS_DNS, "example.org");
    const eular::uuid_t d = eular::UUID::V5(eular::UUID::UUID_NS_URL, "example.com");

    CHECK(a == b);
    CHECK(a != c);
    CHECK(a != d);
}

TEST_CASE("uuid_v5_version_and_variant_bits", "[uuid]") {
    const eular::uuid_t uuid = eular::UUID::V5(eular::UUID::UUID_NS_URL, "https://example.com/path");

    // Version must be 5.
    CHECK((uuid[6] >> 4) == 0x5);
    // Variant must be RFC 4122 (10xxxxxx).
    CHECK((uuid[8] & 0xC0) == 0x80);
}

TEST_CASE("uuid_to_string_format", "[uuid]") {
    const eular::uuid_t uuid = eular::UUID::V5(eular::UUID::UUID_NS_DNS, "format-check");
    const std::string s = eular::UUID::ToString(uuid);

    REQUIRE(s.size() == 36);
    CHECK(s[8] == '-');
    CHECK(s[13] == '-');
    CHECK(s[18] == '-');
    CHECK(s[23] == '-');

    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            continue;
        }
        CHECK(std::isxdigit(static_cast<unsigned char>(s[i])) != 0);
        CHECK(!(s[i] >= 'A' && s[i] <= 'F'));
    }
}