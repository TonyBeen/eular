/*************************************************************************
    > File Name: test_network_path.cc
    > Author: eular
    > Brief:
    > Created Time: Wed 18 Mar 2026
 ************************************************************************/

#include <catch2/catch.hpp>
#include "util/status.h"

#include "utp/errno.h"
#include "util/network_path.h"

using eular::utp::Address;
using eular::utp::Status;
using eular::utp::FramePathChallenge;
using eular::utp::Status;
using eular::utp::FramePathResponse;
using eular::utp::Status;
using eular::utp::NetworkPath;
using eular::utp::Status;

TEST_CASE("NetworkPath: address change triggers validating", "[NetworkPath]")
{
    NetworkPath path;
    path.bindPeerAddress(Address("10.0.0.1", 10000));

    REQUIRE(path.state() == NetworkPath::kPathValidated);
    REQUIRE_FALSE(path.detectPeerAddressChange(Address("10.0.0.1", 10000)));

    REQUIRE(path.detectPeerAddressChange(Address("10.0.0.1", 10001)));
    REQUIRE(path.state() == NetworkPath::kPathValidating);
    REQUIRE(path.needPathValidation());
}

TEST_CASE("NetworkPath: challenge-response validates path", "[NetworkPath]")
{
    NetworkPath path;
    path.bindPeerAddress(Address("10.0.0.1", 10000));
    REQUIRE(path.detectPeerAddressChange(Address("10.0.0.1", 10001)));

    FramePathChallenge challenge;
    REQUIRE(path.makePathChallenge(challenge, 1000)  == 0);
    REQUIRE(path.hasInFlightChallenge());

    FramePathResponse response;
    path.makePathResponse(challenge, response);
    REQUIRE(path.onPathResponse(response));

    REQUIRE(path.state() == NetworkPath::kPathValidated);
    REQUIRE_FALSE(path.needPathValidation());
}

TEST_CASE("NetworkPath: timeout can move path to failed", "[NetworkPath]")
{
    NetworkPath path(/*challengeTimeoutMs=*/100, /*maxChallengeRetries=*/2);
    path.bindPeerAddress(Address("192.168.1.2", 9000));
    REQUIRE(path.detectPeerAddressChange(Address("192.168.1.2", 9001)));

    FramePathChallenge challenge;
    REQUIRE(path.makePathChallenge(challenge, 0)  == 0);

    // first timeout: still can retry
    REQUIRE_FALSE(path.onTimeout(100));
    REQUIRE(path.state() == NetworkPath::kPathValidating);
    REQUIRE(path.canRetryChallenge());

    REQUIRE(path.makePathChallenge(challenge, 101)  == 0);

    // second timeout: retries exhausted -> failed
    REQUIRE(path.onTimeout(201));
    REQUIRE(path.state() == NetworkPath::kPathFailed);
    REQUIRE_FALSE(path.canRetryChallenge());
}

TEST_CASE("NetworkPath: mismatched response should be ignored", "[NetworkPath]")
{
    NetworkPath path;
    path.bindPeerAddress(Address("172.16.0.2", 7000));
    REQUIRE(path.detectPeerAddressChange(Address("172.16.0.2", 7001)));

    FramePathChallenge challenge;
    REQUIRE(path.makePathChallenge(challenge, 10)  == 0);

    FramePathResponse badResponse;
    badResponse.data = {0, 1, 2, 3, 4, 5, 6, 7};
    if (badResponse.data == challenge.data) {
        badResponse.data[0] ^= 0xFF;
    }

    REQUIRE_FALSE(path.onPathResponse(badResponse));
    REQUIRE(path.state() == NetworkPath::kPathValidating);
}
