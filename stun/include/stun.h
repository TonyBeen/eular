/*************************************************************************
    > File Name: stun.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月10日 星期二 17时33分19秒
 ************************************************************************/

#ifndef __STUN_H__
#define __STUN_H__

#include <stdint.h>

#include <vector>
#include <memory>
#include <optional>

#include <utils/any.hpp>

#include <stun_types.h>
#include <socket_address.h>

namespace eular {
namespace stun {
struct StunMsgBuilderPrivate;
class StunMsgBuilder {
public:
    using Ptr   = std::unique_ptr<StunMsgBuilder>;
    using SP    = std::shared_ptr<StunMsgBuilder>;
    using WP    = std::weak_ptr<StunMsgBuilder>;

    StunMsgBuilder();
    StunMsgBuilder(const StunMsgBuilder&) = delete;
    StunMsgBuilder& operator=(const StunMsgBuilder&) = delete;
    StunMsgBuilder(StunMsgBuilder &&other);
    StunMsgBuilder& operator=(StunMsgBuilder &&other);
    ~StunMsgBuilder() = default;

    void setMsgType(uint16_t msgType);
    void setTransactionId(const uint8_t transactionId[STUN_TRX_ID_SIZE]);

    void addAttribute(uint16_t type, const eular::any& value);
    void clearAttributes();

    const std::vector<uint8_t> &message() const;

private:
    std::unique_ptr<StunMsgBuilderPrivate> m_impl;
};

} // namespace stun
} // namespace eular

#endif // __STUN_H__
