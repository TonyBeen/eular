#define CATCH_CONFIG_MAIN
#include <ntrs_auth.h>
#include <ntrs_codec.h>

#include <cstring>
#include <string>

#include "catch/catch.hpp"

namespace {

static void PutU16Be(uint8_t* p, uint16_t value)
{
    p[0] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[1] = static_cast<uint8_t>(value & 0xFFu);
}

static void PutU32Be(uint8_t* p, uint32_t value)
{
    p[0] = static_cast<uint8_t>((value >> 24) & 0xFFu);
    p[1] = static_cast<uint8_t>((value >> 16) & 0xFFu);
    p[2] = static_cast<uint8_t>((value >> 8) & 0xFFu);
    p[3] = static_cast<uint8_t>(value & 0xFFu);
}

}  // namespace

TEST_CASE("MessageAddBytesByTag failure leaves message unchanged")
{
    eular::ntrs::Message msg;
    uint8_t              data[eular::ntrs::Message::STORAGE_SIZE];

    memset(data, 0xA5, sizeof(data));
    eular::ntrs::MessageInit(&msg, eular::ntrs::MessageType::AUTH_REQ, 1);

    REQUIRE_FALSE(eular::ntrs::MessageAddBytesByTag(&msg, eular::ntrs::FieldTag::TOKEN, data, sizeof(data)));
    REQUIRE(msg.field_count == 0);
    REQUIRE(msg.storage_len == 0);
    REQUIRE(eular::ntrs::MessageGetStringByTag(&msg, eular::ntrs::FieldTag::TOKEN)[0] == '\0');
}

TEST_CASE("DecodeMessage failure leaves no partial field")
{
    uint8_t              frame[eular::ntrs::FRAME_HDR_SIZE + eular::ntrs::TLV_HDR_SIZE + 1];
    eular::ntrs::Message msg;

    memset(frame, 0, sizeof(frame));
    PutU32Be(frame, eular::ntrs::FRAME_MAGIC);
    frame[4] = eular::ntrs::FRAME_VERSION;
    frame[5] = static_cast<uint8_t>(eular::ntrs::MessageType::AUTH_RSP);
    PutU32Be(frame + 12, eular::ntrs::TLV_HDR_SIZE + 1);
    PutU16Be(frame + eular::ntrs::FRAME_HDR_SIZE, static_cast<uint16_t>(eular::ntrs::FieldTag::LEASE_DEFAULT_SEC));
    PutU16Be(frame + eular::ntrs::FRAME_HDR_SIZE + 2, 1);
    frame[eular::ntrs::FRAME_HDR_SIZE + eular::ntrs::TLV_HDR_SIZE] = 30;

    REQUIRE_FALSE(eular::ntrs::DecodeMessage(frame, sizeof(frame), &msg));
    REQUIRE(msg.field_count == 0);
}

TEST_CASE("probe authorization uses keyed HMAC and requires a secret")
{
    std::string auth = eular::ntrs::MintProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345);
    std::string tampered = auth;
    tampered[tampered.size() - 1] = tampered[tampered.size() - 1] == '0' ? '1' : '0';

    REQUIRE(auth.size() == 64);
    REQUIRE(auth == "be59f5dc6243bdb3c3c6612ca82c137b3e9c40fa39c5605fa95cdedc91b1cd80");
    REQUIRE(auth != eular::ntrs::MintProbeAuthorization("other-secret", "peer-a", "192.0.2.10", 33478, "token", 12345));
    REQUIRE(eular::ntrs::ValidateProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345, auth));
    REQUIRE_FALSE(
        eular::ntrs::ValidateProbeAuthorization("secret", "peer-a", "192.0.2.10", 33478, "token", 12345, tampered));
    REQUIRE(eular::ntrs::MintProbeAuthorization("", "peer-a", "192.0.2.10", 33478, "token", 12345).empty());
    REQUIRE_FALSE(eular::ntrs::ValidateProbeAuthorization("", "peer-a", "192.0.2.10", 33478, "token", 12345, auth));
}
