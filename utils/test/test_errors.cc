#include <errno.h>

#include <string>

#include "catch/catch.hpp"
#include "utils/errors.h"

TEST_CASE("status_to_string_maps_known_values", "[errors]") {
    CHECK(std::string(StatusToString(STATUS(OK))) == "OK");
    CHECK(std::string(StatusToString(STATUS(NOT_FOUND))) == "No such file or directory");
    CHECK(std::string(StatusToString(STATUS(TIMED_OUT))) == "Something timed out");
}

TEST_CASE("status_to_string_handles_unknown_values", "[errors]") {
    CHECK(std::string(StatusToString(123456)) == "Unknown error");
}

TEST_CASE("get_last_errno_and_format_errno_work", "[errors]") {
    errno = ENOENT;
    CHECK(GetLastErrno() == ENOENT);

    const std::string formatted = FormatErrno(ENOENT);
    CHECK(!formatted.empty());
}