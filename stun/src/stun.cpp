/*************************************************************************
    > File Name: stun.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2025年06月10日 星期二 17时33分37秒
 ************************************************************************/

#include "stun.h"

#include <map>
#include <exception>
#include <stdexcept>

#include "stun/msg.h"

#define STUN_IPV4_LENGTH        4
#define STUN_IPV6_LENGTH        16

#define STUN_MSG_HDR_SIZE   (20)    /* size of the STUN message header */
#define STUN_ATTR_HDR_SIZE  (4)     /* size of the STUN attribute header */

/* Retrieve the STUN method from the message-type field of the STUN message */
#define STUN_GET_METHOD(msg_type) ((msg_type) & 0xFEEF)

/* Determine if the message type is a request */
#define STUN_IS_REQUEST(msg_type) (((msg_type) & 0x0110) == 0x0000)

/* Determine if the message type is a successful response */
#define STUN_IS_SUCCESS_RESPONSE(msg_type) (((msg_type) & 0x0110) == 0x0100)

/* Determine if the message type is an error response */
#define STUN_IS_ERROR_RESPONSE(msg_type) (((msg_type) & 0x0110) == 0x0110)

/* Determine if the message type is a response */
#define STUN_IS_RESPONSE(msg_type) (((msg_type) & 0x0100) == 0x0100)

/* Determine if the message type is an indication message */
#define STUN_IS_INDICATION(msg_type) (((msg_type) & 0x0110) == 0x0010)

#define PADDING_SIZE(len) (((len) & 0x03) ? (4 - ((len) & 0x03)) : 0)

#define BUFFER_SIZE 512

namespace eular {
namespace stun {
struct StunMsgBuilderPrivate {
    stun_msg_hdr            msg_hdr; // STUN message header
    bool                    tsx_id_set = false; // Transaction ID set flag
    std::vector<uint8_t>    msg_buf; // message buf
};

StunMsgBuilder::StunMsgBuilder()
{
    m_impl = std::unique_ptr<StunMsgBuilderPrivate>(new StunMsgBuilderPrivate());
    m_impl->msg_hdr.type = STUN_BINDING_REQUEST;
    m_impl->msg_buf.reserve(BUFFER_SIZE);
}

StunMsgBuilder::StunMsgBuilder(StunMsgBuilder &&other)
{
    if (this == &other) {
        return;
    }

    m_impl = std::move(other.m_impl);
}

StunMsgBuilder &StunMsgBuilder::operator=(StunMsgBuilder &&other)
{
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }

    return *this;
}

void StunMsgBuilder::setMsgType(uint16_t msgType)
{
    m_impl->msg_hdr.type = msgType;
    m_impl->msg_hdr.length = 0;
    m_impl->msg_hdr.magic = STUN_MAGIC_COOKIE;
    m_impl->tsx_id_set = false; // Reset transaction ID set flag

    clearAttributes();
}

void StunMsgBuilder::setTransactionId(const uint8_t transactionId[STUN_TRX_ID_SIZE])
{
    m_impl->tsx_id_set = true;
    memcpy(m_impl->msg_hdr.tsx_id, transactionId, STUN_TRX_ID_SIZE);

    clearAttributes();
}

void StunMsgBuilder::setTransactionId(const std::array<uint8_t, STUN_TRX_ID_SIZE>& transactionId)
{
    m_impl->tsx_id_set = true;
    memcpy(m_impl->msg_hdr.tsx_id, transactionId.data(), STUN_TRX_ID_SIZE);

    clearAttributes();
}

void StunMsgBuilder::addAttribute(uint16_t type, const eular::any &value)
{
    if (m_impl->tsx_id_set == false) {
        // Transaction ID must be set before adding attributes
        return;
    }

    if (m_impl->msg_buf.empty()) {
        // Initialize message buffer with header if it's empty
        if (m_impl->msg_buf.capacity() < STUN_MSG_HDR_SIZE) {
            m_impl->msg_buf.reserve(STUN_MSG_HDR_SIZE);
        }

        m_impl->msg_hdr.type = htobe16(m_impl->msg_hdr.type);
        m_impl->msg_hdr.length = htobe16(m_impl->msg_hdr.length);
        m_impl->msg_hdr.magic = htobe32(m_impl->msg_hdr.magic);
        m_impl->msg_buf.resize(STUN_MSG_HDR_SIZE);
        memcpy(m_impl->msg_buf.data(), &m_impl->msg_hdr, STUN_MSG_HDR_SIZE);
    }

    int32_t attr_length = be16toh(m_impl->msg_hdr.length);
    switch (type) {
    case STUN_ATTR_MAPPED_ADDRESS:      /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_RESPONSE_ADDRESS:    /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_CHANGED_ADDRESS:     /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_SOURCE_ADDRESS:      /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_REFLECTED_FROM:      /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_ALTERNATE_SERVER:    /* stun_attr_sockaddr     | RFC 5389  */
    case STUN_ATTR_OTHER_ADDRESS:       /* stun_attr_sockaddr     | RFC 5780  */
    case STUN_ATTR_RESPONSE_ORIGIN:     /* stun_attr_sockaddr     | RFC 5780  */
    {
        const SocketAddress *addr = eular::any_cast<SocketAddress>(&value);
        if (addr != nullptr) {
            stun_attr_sockaddr stun_attr;
            stun_attr_sockaddr_init(&stun_attr, type, addr->getSockAddr());
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + sizeof(stun_attr)) {
                m_impl->msg_buf.reserve(m_impl->msg_buf.capacity() * 1.5);
            }
            auto size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(size + sizeof(stun_attr));
            memcpy(m_impl->msg_buf.data() + size, &stun_attr, sizeof(stun_attr));
            attr_length += sizeof(stun_attr);
        }
        break;
    }
    case STUN_ATTR_CHANGE_REQUEST:      /* stun_attr_uint32       | RFC 5780  */
    case STUN_ATTR_CHANNEL_NUMBER:      /* stun_attr_uint32       | RFC 5766  */
    case STUN_ATTR_LIFETIME:            /* stun_attr_uint32       | RFC 5766  */
    case STUN_ATTR_BANDWIDTH:           /* stun_attr_uint32       | RFC 5766  */
    case STUN_ATTR_REQUESTED_TRANSPORT: /* stun_attr_uint32       | RFC 5766  */
    case STUN_ATTR_TIMER_VAL:           /* stun_attr_uint32       | RFC 5766  */
    case STUN_ATTR_PRIORITY:            /* stun_attr_uint32       | RFC 5245  */
    case STUN_ATTR_CONNECTION_ID:       /* stun_attr_uint32       | RFC 6062  */
    case STUN_ATTR_FINGERPRINT:         /* stun_attr_uint32       | RFC 5389  */
    {
        const uint32_t *val = eular::any_cast<uint32_t>(&value);
        if (val != nullptr) {
            stun_attr_uint8 stun_attr;
            stun_attr_uint8_init(&stun_attr, type, *val);
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + sizeof(stun_attr)) {
                m_impl->msg_buf.reserve(m_impl->msg_buf.capacity() * 1.5);
            }
            size_t size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(size + sizeof(stun_attr));
            memcpy(m_impl->msg_buf.data() + size, &stun_attr, sizeof(stun_attr));
            attr_length += sizeof(stun_attr);
        }
        break;
    }
    case STUN_ATTR_USERNAME:            /* stun_attr_varsize      | RFC 5389  */
    case STUN_ATTR_PASSWORD:            /* stun_attr_varsize      | RFC 5389  */
    case STUN_ATTR_DATA:                /* stun_attr_varsize      | RFC 5766  */
    case STUN_ATTR_REALM:               /* stun_attr_varsize      | RFC 5389  */
    case STUN_ATTR_NONCE:               /* stun_attr_varsize      | RFC 5389  */
    case STUN_ATTR_PADDING:             /* stun_attr_varsize      | RFC 5780  */
    case STUN_ATTR_SOFTWARE:            /* stun_attr_varsize      | RFC 5389  */
    {
        const StunAttrVarSize *val = eular::any_cast<StunAttrVarSize>(&value);
        if (val != nullptr) {
            size_t attrLength = sizeof(stun_attr_hdr) + val->value.size() + PADDING_SIZE(val->value.size());
            stun_attr_varsize *stun_attr = (stun_attr_varsize *)malloc(attrLength);
            if (stun_attr == nullptr) {
                return;
            }

            stun_attr_varsize_init(stun_attr, type, val->value.data(), val->value.size(), 0);
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + attrLength) {
                m_impl->msg_buf.reserve((m_impl->msg_buf.size() + attrLength) * 1.5);
            }

            size_t size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(size + attrLength);
            memcpy(m_impl->msg_buf.data() + size, stun_attr, attrLength);
            free(stun_attr);
            attr_length += attrLength;
        }
        break;
    }
    case STUN_ATTR_MESSAGE_INTEGRITY:   /* stun_attr_msgint       | RFC 5389  */
    {
        const StunAttrVarSize *val = eular::any_cast<StunAttrVarSize>(&value);
        if (val != nullptr) {
            stun_msg_hdr *msg_hdr = (stun_msg_hdr *)m_impl->msg_buf.data();
            stun_attr_msgint stun_attr;
            stun_attr_msgint_init(&stun_attr, msg_hdr, val->value.data(), val->value.size());
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + sizeof(stun_attr)) {
                m_impl->msg_buf.reserve(m_impl->msg_buf.capacity() * 1.5);
            }
            size_t size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(size + sizeof(stun_attr));
            memcpy(m_impl->msg_buf.data() + size, &stun_attr, sizeof(stun_attr));
            attr_length += sizeof(stun_attr);
        }

        break;
    }
    case STUN_ATTR_ERROR_CODE:          /* stun_attr_errcode      | RFC 5389  */
    {
        const StunAttrErrorCode *val = eular::any_cast<StunAttrErrorCode>(&value);
        if (val != nullptr) {
            int32_t attrLength = sizeof(stun_attr_errcode) - 1 + val->error_reason.size() + PADDING_SIZE(val->error_reason.size());
            stun_attr_errcode *stun_attr = (stun_attr_errcode *)malloc(attrLength);
            if (stun_attr == nullptr) {
                return;
            }
            stun_attr_errcode_init(stun_attr, val->error_code, val->error_reason.c_str(), 0);
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + attrLength) {
                m_impl->msg_buf.reserve((m_impl->msg_buf.size() + attrLength) * 1.5);
            }
            size_t size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(m_impl->msg_buf.size() + attrLength);
            memcpy(m_impl->msg_buf.data() + size, stun_attr, attrLength);
            free(stun_attr);
            attr_length += attrLength;
        }
        break;
    }
    case STUN_ATTR_UNKNOWN_ATTRIBUTES:  /* stun_attr_unknown      | RFC 5389  */
        break;
    case STUN_ATTR_XOR_PEER_ADDRESS:    /* stun_attr_xor_sockaddr | RFC 5766  */
    case STUN_ATTR_XOR_RELAYED_ADDRESS: /* stun_attr_xor_sockaddr | RFC 5766  */
    case STUN_ATTR_XOR_MAPPED_ADDRESS:  /* stun_attr_xor_sockaddr | RFC 5389  */
    {
        const SocketAddress *addr = eular::any_cast<SocketAddress>(&value);
        if (addr != nullptr) {
            stun_attr_xor_sockaddr stun_attr;
            stun_attr_xor_sockaddr_init(&stun_attr, type, addr->getSockAddr(), &m_impl->msg_hdr);
            if (m_impl->msg_buf.capacity() < m_impl->msg_buf.size() + sizeof(stun_attr)) {
                m_impl->msg_buf.reserve(m_impl->msg_buf.capacity() * 1.5);
            }
            size_t size = m_impl->msg_buf.size();
            m_impl->msg_buf.resize(size + sizeof(stun_attr));
            memcpy(m_impl->msg_buf.data() + size, &stun_attr, sizeof(stun_attr));
            attr_length += sizeof(stun_attr);
        }
        break;
    }
    case STUN_ATTR_REQ_ADDRESS_FAMILY:  /* stun_attr_uint8        | RFC 6156  */
        break;
    case STUN_ATTR_EVEN_PORT:           /* stun_attr_uint8_pad    | RFC 5766  */
        break;
    case STUN_ATTR_DONT_FRAGMENT:       /* empty                  | RFC 5766  */
    case STUN_ATTR_USE_CANDIDATE:       /* empty                  | RFC 5245  */
        break;
    case STUN_ATTR_RESERVATION_TOKEN:   /* stun_attr_uint64       | RFC 5766  */
    case STUN_ATTR_ICE_CONTROLLED:      /* stun_attr_uint64       | RFC 5245  */
    case STUN_ATTR_ICE_CONTROLLING:     /* stun_attr_uint64       | RFC 5245  */
        break;
    case STUN_ATTR_RESPONSE_PORT:       /* stun_attr_uint16_pad   | RFC 5780  */
        break;
    default:
        break;
    }

    m_impl->msg_hdr.length = htobe16(attr_length);
    memcpy(m_impl->msg_buf.data(), &m_impl->msg_hdr, STUN_MSG_HDR_SIZE);
}

void StunMsgBuilder::clearAttributes()
{
    m_impl->msg_hdr.length = 0;
    m_impl->msg_buf.clear();
    m_impl->msg_buf.reserve(BUFFER_SIZE);
}

const std::vector<uint8_t> &StunMsgBuilder::message() const
{
    if (m_impl->msg_buf.empty()) {
        // Build message header
        m_impl->msg_buf.resize(STUN_MSG_HDR_SIZE);
        m_impl->msg_hdr.type = htobe16(m_impl->msg_hdr.type);
        m_impl->msg_hdr.length = htobe16(m_impl->msg_hdr.length);
        m_impl->msg_hdr.magic = htobe32(m_impl->msg_hdr.magic);

        memcpy(m_impl->msg_buf.data(), &m_impl->msg_hdr, STUN_MSG_HDR_SIZE);
    }
    return m_impl->msg_buf;
}

////////////////////////////////////StunMsgParser////////////////////////////////////
struct StunMsgParserPrivate {
    stun_msg_hdr                    msg_hdr; // STUN message header
    std::vector<uint8_t>            msg_buf; // message buf
    std::map<uint16_t, eular::any>  attributes; // Attributes map
};

StunMsgParser::StunMsgParser()
{
    m_impl = std::unique_ptr<StunMsgParserPrivate>(new StunMsgParserPrivate());
    m_impl->msg_buf.reserve(BUFFER_SIZE);
}

StunMsgParser::StunMsgParser(const void *data, size_t size)
{
    m_impl = std::unique_ptr<StunMsgParserPrivate>(new StunMsgParserPrivate());
    m_impl->msg_buf.reserve(BUFFER_SIZE);

    if (parse(data, size) == false) {
        throw std::runtime_error("Failed to parse STUN message");
    }
}

StunMsgParser::StunMsgParser(const std::vector<uint8_t>& data)
{
    m_impl = std::unique_ptr<StunMsgParserPrivate>(new StunMsgParserPrivate());
    m_impl->msg_buf.reserve(BUFFER_SIZE);

    if (parse(data.data(), data.size()) == false) {
        throw std::runtime_error("Failed to parse STUN message");
    }
}

StunMsgParser::StunMsgParser(StunMsgParser &&other)
{
    if (this == &other) {
        return;
    }

    m_impl = std::move(other.m_impl);
}

StunMsgParser &StunMsgParser::operator=(StunMsgParser &&other)
{
    if (this != &other) {
        m_impl = std::move(other.m_impl);
    }

    return *this;
}

bool StunMsgParser::parse(const void *data, size_t size)
{
    if (data == nullptr || size < STUN_MSG_HDR_SIZE) {
        return false; // Invalid data or size
    }

    if (size < STUN_MSG_HDR_SIZE) {
        return false; // Not enough data for header
    }

    const stun_msg_hdr *msg_hdr = (const stun_msg_hdr *)data;
    const stun_attr_hdr *attr_hdr = nullptr;
    if (!stun_msg_verify(msg_hdr, size)) {
        return false; // Invalid STUN message header
    }

    m_impl->msg_hdr.type = stun_msg_type(msg_hdr);
    m_impl->msg_hdr.length = be16toh(msg_hdr->length);
    memcpy(m_impl->msg_hdr.tsx_id, msg_hdr->tsx_id, STUN_TRX_ID_SIZE);

    while ((attr_hdr = stun_msg_next_attr(msg_hdr, attr_hdr)) != nullptr) {
        uint16_t attr_type = be16toh(attr_hdr->type);
        uint16_t attr_length = be16toh(attr_hdr->length);
        printf("Parsing attribute type: %u, length: %u\n", attr_type, attr_length);

        switch (attr_type) {
        case STUN_ATTR_MAPPED_ADDRESS:      /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_RESPONSE_ADDRESS:    /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_CHANGED_ADDRESS:     /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_SOURCE_ADDRESS:      /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_REFLECTED_FROM:      /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_ALTERNATE_SERVER:    /* stun_attr_sockaddr     | RFC 5389  */
        case STUN_ATTR_OTHER_ADDRESS:       /* stun_attr_sockaddr     | RFC 5780  */
        case STUN_ATTR_RESPONSE_ORIGIN:     /* stun_attr_sockaddr     | RFC 5780  */
        {
            const stun_attr_sockaddr *attr_sock_addr = (const stun_attr_sockaddr *)attr_hdr;
            struct sockaddr_storage sock_addr;
            stun_attr_sockaddr_read(attr_sock_addr, (struct sockaddr *)&sock_addr);
            SocketAddress address((struct sockaddr *)&sock_addr);
            m_impl->attributes[attr_type] = address;
            break;
        }
        case STUN_ATTR_CHANGE_REQUEST:      /* stun_attr_uint32       | RFC 5780  */
        case STUN_ATTR_CHANNEL_NUMBER:      /* stun_attr_uint32       | RFC 5766  */
        case STUN_ATTR_LIFETIME:            /* stun_attr_uint32       | RFC 5766  */
        case STUN_ATTR_BANDWIDTH:           /* stun_attr_uint32       | RFC 5766  */
        case STUN_ATTR_REQUESTED_TRANSPORT: /* stun_attr_uint32       | RFC 5766  */
        case STUN_ATTR_TIMER_VAL:           /* stun_attr_uint32       | RFC 5766  */
        case STUN_ATTR_PRIORITY:            /* stun_attr_uint32       | RFC 5245  */
        case STUN_ATTR_CONNECTION_ID:       /* stun_attr_uint32       | RFC 6062  */
        case STUN_ATTR_FINGERPRINT:         /* stun_attr_uint32       | RFC 5389  */
        {
            const stun_attr_uint32 *attr_uint32 = (const stun_attr_uint32 *)attr_hdr;
            m_impl->attributes[attr_type] = stun_attr_uint32_read(attr_uint32);
            break;
        }
        case STUN_ATTR_USERNAME:            /* stun_attr_varsize      | RFC 5389  */
        case STUN_ATTR_PASSWORD:            /* stun_attr_varsize      | RFC 5389  */
        case STUN_ATTR_DATA:                /* stun_attr_varsize      | RFC 5766  */
        case STUN_ATTR_REALM:               /* stun_attr_varsize      | RFC 5389  */
        case STUN_ATTR_NONCE:               /* stun_attr_varsize      | RFC 5389  */
        case STUN_ATTR_PADDING:             /* stun_attr_varsize      | RFC 5780  */
        case STUN_ATTR_SOFTWARE:            /* stun_attr_varsize      | RFC 5389  */
        {
            const stun_attr_varsize *attr_varsize = (const stun_attr_varsize *)attr_hdr;
            if (attr_length > 0) {
                std::vector<uint8_t> value(attr_varsize->value, attr_varsize->value + attr_length);
                StunAttrVarSize stun_attr_varsize;
                stun_attr_varsize.value = std::move(value);
                m_impl->attributes[attr_type] = std::move(stun_attr_varsize);
                printf("Parsed attribute type: %u, value size: %zu\n", attr_type, value.size());
            } else {
                m_impl->attributes[attr_type] = StunAttrVarSize();
            }
            break;
        }
        case STUN_ATTR_MESSAGE_INTEGRITY:   /* stun_attr_msgint       | RFC 5389  */
        {
            const stun_attr_msgint *attr_msgint = (const stun_attr_msgint *)attr_hdr;
            std::vector<uint8_t> value(attr_msgint->hmac, attr_msgint->hmac + attr_length);
            StunAttrVarSize stun_attr_varsize;
            stun_attr_varsize.value = std::move(value);
            m_impl->attributes[attr_type] = std::move(stun_attr_varsize);

            break;
        }
        case STUN_ATTR_ERROR_CODE:          /* stun_attr_errcode      | RFC 5389  */
        {
            const stun_attr_errcode *attr_errcode = (const stun_attr_errcode *)attr_hdr;
            StunAttrErrorCode stun_attr_error_code;
            stun_attr_error_code.error_code = stun_attr_errcode_status(attr_errcode);
            stun_attr_error_code.error_reason.assign(attr_errcode->err_reason, attr_errcode->err_reason + attr_length - 1);
            m_impl->attributes[attr_type] = std::move(stun_attr_error_code);
            break;
        }
        case STUN_ATTR_UNKNOWN_ATTRIBUTES:  /* stun_attr_unknown      | RFC 5389  */
        {
            const stun_attr_unknown *attr_unknown = (const stun_attr_unknown *)attr_hdr;
            std::vector<uint16_t> unknown_attrs(attr_unknown->attrs, attr_unknown->attrs + attr_length / sizeof(uint16_t));
            m_impl->attributes[attr_type] = std::move(unknown_attrs);
            break;
        }
        case STUN_ATTR_XOR_PEER_ADDRESS:    /* stun_attr_xor_sockaddr | RFC 5766  */
        case STUN_ATTR_XOR_RELAYED_ADDRESS: /* stun_attr_xor_sockaddr | RFC 5766  */
        case STUN_ATTR_XOR_MAPPED_ADDRESS:  /* stun_attr_xor_sockaddr | RFC 5389  */
        {
            const stun_attr_xor_sockaddr *attr_xor_sockaddr = (const stun_attr_xor_sockaddr *)attr_hdr;
            struct sockaddr_storage sock_addr;
            stun_attr_xor_sockaddr_read(attr_xor_sockaddr, msg_hdr, (struct sockaddr *)&sock_addr);
            SocketAddress address((struct sockaddr *)&sock_addr);
            m_impl->attributes[attr_type] = std::move(address);
            break;
        }
        case STUN_ATTR_REQ_ADDRESS_FAMILY:  /* stun_attr_uint8        | RFC 6156  */
        case STUN_ATTR_EVEN_PORT:           /* stun_attr_uint8_pad    | RFC 5766  */
        case STUN_ATTR_DONT_FRAGMENT:       /* empty                  | RFC 5766  */
        case STUN_ATTR_USE_CANDIDATE:       /* empty                  | RFC 5245  */
        case STUN_ATTR_RESERVATION_TOKEN:   /* stun_attr_uint64       | RFC 5766  */
        case STUN_ATTR_ICE_CONTROLLED:      /* stun_attr_uint64       | RFC 5245  */
        case STUN_ATTR_ICE_CONTROLLING:     /* stun_attr_uint64       | RFC 5245  */
        case STUN_ATTR_RESPONSE_PORT:       /* stun_attr_uint16_pad   | RFC 5780  */
        default:
            break;
        }
    }

    return true;
}

bool StunMsgParser::parse(const std::vector<uint8_t>& data)
{
    return parse(data.data(), data.size());
}

uint16_t StunMsgParser::msgType() const
{
    return m_impl->msg_hdr.type;
}

const uint8_t* StunMsgParser::transactionId() const
{
    return m_impl->msg_hdr.tsx_id;
}

const eular::any *StunMsgParser::getAttribute(uint16_t type) const
{
    auto it = m_impl->attributes.find(type);
    if (it != m_impl->attributes.end()) {
        return &it->second;
    }

    return nullptr; // Attribute not found
}

std::vector<uint16_t> StunMsgParser::getAttributeTypes() const
{
    std::vector<uint16_t> types(m_impl->attributes.size());
    for (const auto &pair : m_impl->attributes) {
        types.push_back(pair.first);
    }

    return types;
}

} // namespace stun
} // namespace eular
