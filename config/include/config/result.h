/*************************************************************************
    > File Name: result.h
    > Author: eular
    > Brief:
    > Created Time: Tue 10 Mar 2026 03:52:47 PM CST
 ************************************************************************/

#ifndef __CONFIG_RESULT_H__
#define __CONFIG_RESULT_H__

#include <stdint.h>
#include <string>

#include <config/exports.h>

namespace eular {
enum ConfigCode : int32_t {
    CONFIG_OK                   = 0,
    CONFIG_INVALID_ARGUMENT     = -1,
    CONFIG_NOT_FOUND            = -2,
    CONFIG_FILE_OP_ERROR        = -3,
    CONFIG_FILE_EMPTY           = -4,
    CONFIG_INIT_ERROR           = -5,
    CONFIG_NO_MEMORY            = -6,
    CONFIG_PARSE_ERROR          = -7,
    CONFIG_UNSUPPORTED          = -8,
};

class CONFIG_API ConfigResult
{
public:
    ConfigResult() : m_code(CONFIG_OK), m_message("OK") {}
    ConfigResult(int32_t code, const char *message) : m_code(code), m_message(message != nullptr ? message : "") {}
    ConfigResult(int32_t code, const std::string &message) : m_code(code), m_message(message) {}
    ConfigResult(const ConfigResult&) = default;
    ConfigResult& operator=(const ConfigResult&) = default;
    ~ConfigResult() = default;

    int32_t code() const { return m_code; }
    const char* message() const { return m_message.c_str(); }
    const std::string& messageText() const { return m_message; }

private:
    int32_t     m_code;
    std::string m_message;
};

} // namespace eular

#endif // __CONFIG_RESULT_H__
