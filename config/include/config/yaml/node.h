/*************************************************************************
    > File Name: node.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 02:40:28 PM CST
 ************************************************************************/

#ifndef __CONFIG_YAML_NODE_H__
#define __CONFIG_YAML_NODE_H__

#include <string>
#include <stdint.h>
#include <memory>

#include <c4/charconv.hpp>
#include <c4/std/string.hpp>

#include <config/exports.h>

namespace eular {
struct YamlNodePrivate;
class CONFIG_API YamlNode {
    friend class YamlParser;
public:
    enum class NodeType {
        Null,
        Scalar,
        Sequence,
        Map
    };

    YamlNode();
    ~YamlNode();

    bool            valid() const noexcept;
    NodeType        type() const noexcept;

    bool            isNull() const noexcept;
    bool            isScalar() const noexcept;
    bool            isSequence() const noexcept;
    bool            isMap() const noexcept;

    uint32_t        size() const noexcept;

    YamlNode        at(const std::string &key) const noexcept;
    YamlNode        at(uint32_t index) const noexcept;

    std::string     scalar() const noexcept;

    template<typename T>
    bool as(T *value) const noexcept
    {
        if (value == nullptr || !isScalar()) {
            return false;
        }

        T temp;
        if (!parseValue(scalar(), temp)) {
            return false;
        }

        *value = temp;
        return true;
    }

    template<typename T>
    T as(const T &defaultValue = T()) const noexcept
    {
        T value;
        if (!as(&value)) {
            return defaultValue;
        }

        return value;
    }

private:
    static std::string trimScalar(const std::string &value) noexcept;
    static bool parseValue(const std::string &text, std::string &value) noexcept;
    static bool parseValue(const std::string &text, bool &value) noexcept;

    template<typename T>
    static bool parseValue(const std::string &text, T &value) noexcept
    {
        std::string trimmed = trimScalar(text);
        if (trimmed.empty()) {
            return false;
        }

        return c4::from_chars(c4::to_csubstr(trimmed), &value);
    }

private:
    std::shared_ptr<YamlNodePrivate>    m_private = nullptr;
};
} // namespace eular

#endif // __CONFIG_YAML_NODE_H__
