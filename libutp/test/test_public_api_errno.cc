/*************************************************************************
    > File Name: test_public_api_errno.cc
    > Author: eular
    > Brief:
    > Created Time: Fri 28 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>

#include <event/loop.h>

#include "utp/errno.h"
#include "utp/utp.h"

using eular::utp::Config;
using eular::utp::Context;

TEST_CASE("Context public API maps WOULD_BLOCK to -1 with errno", "[Context][API][Errno]")
{
    Config cfg;
    ev::EventLoop loop;
    Context server(loop.loop(), &cfg);

    REQUIRE(server.bind("127.0.0.1", 0, "") == 0);
    REQUIRE(server.accept() == -1);
    REQUIRE(GetLastError() == UTP_ERR_WOULD_BLOCK);
}

TEST_CASE("Context public API maps invalid 0-RTT input to -1 with errno", "[Context][API][Errno]")
{
    Config cfg;
    ev::EventLoop loop;
    Context client(loop.loop(), &cfg);

    Context::Connect0RttInfo info;
    info.ip = "127.0.0.1";
    info.port = 0;

    REQUIRE(client.connect0Rtt(info) == -1);
    REQUIRE(GetLastError() == UTP_ERR_INVALID_PARAM);
}
