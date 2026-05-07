/*************************************************************************
    > File Name: status.h
    > Author: eular
    > Brief: 强类型错误状态封装类，用于内部错误传播。
    > Created Time: Thu 07 May 2026 04:30:00 PM CST
 ************************************************************************/

#ifndef __UTP_UTIL_STATUS_H__
#define __UTP_UTIL_STATUS_H__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <utp/errno.h>

namespace eular {
namespace utp {

/**
 * @class Status
 * @brief 强类型错误状态封装，借鉴 Google/Abseil 的 Status 设计。
 */
class Status {
public:
    static const size_t kMessageCapacity = 128;

    /**
     * @brief 默认构造成功状态
     */
    Status() : m_code(UTP_ERR_OK), m_msgSize(0)
    {
        m_msg[0] = '\0';
    }

    /**
     * @brief 构造成功状态
     */
    static Status OK() { return Status(); }

    /**
     * @brief 构造失败状态
     * @param code 错误码 (UTP_ERR_*)
     * @param msg 错误详细描述
     */
    static Status Error(utp_error_t code, const char *msg = nullptr)
    {
        Status status(code);
        if (msg != nullptr) {
            status.copyMessage(msg, std::strlen(msg));
        }
        return status;
    }

    static Status Error(utp_error_t code, const std::string &msg)
    {
        Status status(code);
        status.copyMessage(msg.data(), msg.size());
        return status;
    }

    template <size_t N>
    static Status ErrorLiteral(utp_error_t code, const char (&msg)[N])
    {
        Status status(code);
        const size_t len = (N > 0) ? (N - 1) : 0;
        status.copyMessage(msg, len);
        return status;
    }

    /**
     * @brief 是否为成功状态
     */
    bool ok() const { return m_code == UTP_ERR_OK; }

    /**
     * @brief 获取错误码
     */
    utp_error_t code() const { return m_code; }

    /**
     * @brief 获取错误详细描述
     */
    const char* message() const { return m_msg.data(); }
    size_t messageSize() const { return m_msgSize; }
    bool messageEmpty() const { return m_msgSize == 0; }

    /**
     * @brief 隐式转换为 bool，方便在 if 中判断
     */
    explicit operator bool() const { return ok(); }

private:
    explicit Status(utp_error_t code)
        : m_code(code), m_msgSize(0)
    {
        m_msg[0] = '\0';
    }

    void copyMessage(const char *msg, size_t len)
    {
        if (msg == nullptr || len == 0) {
            m_msgSize = 0;
            m_msg[0] = '\0';
            return;
        }

        const size_t n = (std::min)(len, kMessageCapacity - 1);
        std::memcpy(m_msg.data(), msg, n);
        m_msg[n] = '\0';
        m_msgSize = n;
    }

    utp_error_t m_code;
    size_t      m_msgSize;
    std::array<char, kMessageCapacity> m_msg;
};

/**
 * @brief 错误返回宏，类似于 Rust 的 ? 运算符
 */
#define UTP_RETURN_IF_ERROR(expr)            \
    do {                                     \
        const auto& _status = (expr);        \
        if (!_status.ok()) return _status;   \
    } while (0)

} // namespace utp
} // namespace eular

#endif // __UTP_UTIL_STATUS_H__
