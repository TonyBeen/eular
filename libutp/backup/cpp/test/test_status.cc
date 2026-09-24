/*************************************************************************
    > File Name: test_status.cc
    > Author: eular
    > Brief: Status 类单元测试。
    > Created Time: Thu 07 May 2026 04:40:00 PM CST
 ************************************************************************/

#include <catch2/catch.hpp>
#include <cstring>

#include "util/status.h"

using namespace eular::utp;

TEST_CASE("Status: OK status behavior", "[Status]")
{
    Status s;
    REQUIRE(s.ok());
    REQUIRE(s.code()  == 0);
    REQUIRE(s.messageEmpty());
    REQUIRE(s.messageSize() == 0);
    REQUIRE(static_cast<bool>(s) == true);

    Status s_ok = Status::OK();
    REQUIRE(s_ok.ok());
    REQUIRE(s_ok.code()  == 0);
    REQUIRE(s_ok.messageEmpty());
    REQUIRE(s_ok.messageSize() == 0);
}

TEST_CASE("Status: Error status behavior", "[Status]")
{
    Status s = Status::Error(UTP_ERR_INTERNAL_ERROR, "internal error message");
    REQUIRE_FALSE(s.ok());
    REQUIRE(s.code() == UTP_ERR_INTERNAL_ERROR);
    REQUIRE(std::strcmp(s.message(), "internal error message") == 0);
    REQUIRE(s.messageSize() == std::strlen("internal error message"));
    REQUIRE(static_cast<bool>(s) == false);
}

TEST_CASE("Status: UTP_RETURN_IF_ERROR macro", "[Status]")
{
    auto return_ok = []() -> Status {
        UTP_RETURN_IF_ERROR(Status::OK());
        return Status::OK();
    };

    auto return_error = []() -> Status {
        UTP_RETURN_IF_ERROR(Status::Error(UTP_ERR_INVALID_PARAM, "invalid param"));
        return Status::OK();
    };

    REQUIRE(return_ok().ok());
    
    Status err = return_error();
    REQUIRE_FALSE(err.ok());
    REQUIRE(err.code() == UTP_ERR_INVALID_PARAM);
    REQUIRE(std::strcmp(err.message(), "invalid param") == 0);
    REQUIRE(err.messageSize() == std::strlen("invalid param"));
}
