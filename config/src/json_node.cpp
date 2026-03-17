/*************************************************************************
    > File Name: json_node.cpp
    > Author: eular
    > Brief:
    > Created Time: Tue 17 Mar 2026 04:50:00 PM CST
 ************************************************************************/

#include "config/json/node.h"

#include <cctype>
#include <cstdio>

#include <yyjson.h>

#include "config/json/node_private.hpp"

namespace eular {
JsonNode::JsonNode()
{
}

JsonNode::~JsonNode()
{
}

bool JsonNode::valid() const noexcept
{
    return m_private != nullptr && m_private->node != nullptr;
}

JsonNode::NodeType JsonNode::type() const noexcept
{
    if (!valid()) {
        return NodeType::Null;
    }

    if (yyjson_is_obj(m_private->node)) {
        return NodeType::Map;
    }
    if (yyjson_is_arr(m_private->node)) {
        return NodeType::Sequence;
    }
    if (yyjson_is_null(m_private->node)) {
        return NodeType::Null;
    }

    return NodeType::Scalar;
}

bool JsonNode::isNull() const noexcept
{
    return type() == NodeType::Null;
}

bool JsonNode::isScalar() const noexcept
{
    return type() == NodeType::Scalar;
}

bool JsonNode::isSequence() const noexcept
{
    return type() == NodeType::Sequence;
}

bool JsonNode::isMap() const noexcept
{
    return type() == NodeType::Map;
}

uint32_t JsonNode::size() const noexcept
{
    if (!valid()) {
        return 0;
    }

    if (isSequence()) {
        return static_cast<uint32_t>(yyjson_arr_size(m_private->node));
    }
    if (isMap()) {
        return static_cast<uint32_t>(yyjson_obj_size(m_private->node));
    }

    return 0;
}

JsonNode JsonNode::at(const std::string &key) const noexcept
{
    JsonNode node;
    if (!valid() || !isMap()) {
        return node;
    }

    yyjson_val *child = yyjson_obj_getn(m_private->node, key.c_str(), key.size());
    if (child == nullptr) {
        return node;
    }

    std::shared_ptr<JsonNodePrivate> nodePrivate = std::make_shared<JsonNodePrivate>();
    nodePrivate->snapshot = m_private->snapshot;
    nodePrivate->document = m_private->document;
    nodePrivate->node = child;
    node.m_private = nodePrivate;
    return node;
}

JsonNode JsonNode::at(uint32_t index) const noexcept
{
    JsonNode node;
    if (!valid() || !isSequence()) {
        return node;
    }

    yyjson_val *child = yyjson_arr_get(m_private->node, index);
    if (child == nullptr) {
        return node;
    }

    std::shared_ptr<JsonNodePrivate> nodePrivate = std::make_shared<JsonNodePrivate>();
    nodePrivate->snapshot = m_private->snapshot;
    nodePrivate->document = m_private->document;
    nodePrivate->node = child;
    node.m_private = nodePrivate;
    return node;
}

std::string JsonNode::scalar() const noexcept
{
    if (!valid() || !isScalar()) {
        return std::string();
    }

    if (yyjson_is_str(m_private->node)) {
        const char *text = yyjson_get_str(m_private->node);
        size_t len = yyjson_get_len(m_private->node);
        if (text == nullptr || len == 0) {
            return std::string();
        }

        return std::string(text, len);
    }

    if (yyjson_is_bool(m_private->node)) {
        return yyjson_get_bool(m_private->node) ? "true" : "false";
    }

    if (yyjson_is_int(m_private->node)) {
        if (yyjson_is_uint(m_private->node)) {
            return std::to_string(static_cast<unsigned long long>(yyjson_get_uint(m_private->node)));
        }

        return std::to_string(static_cast<long long>(yyjson_get_sint(m_private->node)));
    }

    if (yyjson_is_num(m_private->node)) {
        char buf[64] = {0};
        snprintf(buf, sizeof(buf), "%.17g", yyjson_get_num(m_private->node));
        return std::string(buf);
    }

    if (yyjson_is_raw(m_private->node)) {
        const char *raw = yyjson_get_raw(m_private->node);
        size_t len = yyjson_get_len(m_private->node);
        if (raw == nullptr || len == 0) {
            return std::string();
        }

        return std::string(raw, len);
    }

    return std::string();
}

std::string JsonNode::trimScalar(const std::string &value) noexcept
{
    size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }

    size_t end = value.find_last_not_of(" \t\r\n");
    return std::string(value.c_str() + begin, end - begin + 1);
}

bool JsonNode::parseValue(const std::string &text, std::string &value) noexcept
{
    value = text;
    return true;
}

bool JsonNode::parseValue(const std::string &text, bool &value) noexcept
{
    std::string trimmed = trimScalar(text);
    if (trimmed.empty()) {
        return false;
    }

    std::string lower(trimmed);
    for (size_t i = 0; i < lower.size(); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lower[i])));
    }

    if (lower == "true" || lower == "yes") {
        value = true;
        return true;
    }
    if (lower == "false" || lower == "no") {
        value = false;
        return true;
    }

    return c4::from_chars(c4::to_csubstr(trimmed), &value);
}

}
