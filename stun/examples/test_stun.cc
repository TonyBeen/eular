/*************************************************************************
    > File Name: test_stun.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月17日 星期二 10时42分17秒
 ************************************************************************/

#include <iostream>

#include <stun.h>
#include <stun_types.h>
#include <socket_address.h>

int main()
{
    eular::stun::StunMsgBuilder builder;
    builder.setMsgType(ENUM_CLASS(StunMsgType::STUN_BINDING_REQUEST));
    uint8_t trx_id[STUN_TRX_ID_SIZE] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef, 0x01, 0x23, 0x45, 0x67};
    builder.setTransactionId(trx_id);

    StunAttrVarSize username;
    const char *name = "testuser";
    username.value.resize(strlen(name));
    memcpy(username.value.data(), name, username.value.size());
    builder.addAttribute(ENUM_CLASS(StunAttributeType::STUN_ATTR_USERNAME), username);
    auto vec = builder.message();

    printf("STUN Message(%zu): ", vec.size());
    for (size_t i = 0; i < vec.size(); ++i) {
        printf("%02x ", vec[i]);
    }
    std::cout << std::dec << std::endl;

    eular::stun::StunMsgParser parser;
    if (!parser.parse(vec)) {
        std::cerr << "Failed to parse STUN message." << std::endl;
        return -1;
    }

    std::cout << "STUN Message Type: " << static_cast<int>(parser.msgType()) << std::endl;
    std::cout << "Transaction ID: ";
    for (size_t i = 0; i < STUN_TRX_ID_SIZE; ++i) {
        std::cout << std::hex << static_cast<int>(trx_id[i]);
        if (i < STUN_TRX_ID_SIZE - 1) {
            std::cout << " ";
        }
    }
    std::cout << std::dec << std::endl;

    const auto &attr_types = parser.getAttributeTypes();
    auto attr = parser.getAttribute(ENUM_CLASS(StunAttributeType::STUN_ATTR_USERNAME));
    if (attr) {
        const StunAttrVarSize *username_attr = eular::any_cast<StunAttrVarSize>(attr);
        if (username_attr) {
            std::cout << "Username: " << std::string(username_attr->value.begin(), username_attr->value.end()) << std::endl;
        } else {
            std::cerr << "Failed to cast attribute to StunAttrVarSize." << std::endl;
        }
    } else {
        std::cerr << "Username attribute not found." << std::endl;
    }
    return 0;
}
