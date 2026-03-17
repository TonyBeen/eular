/*************************************************************************
    > File Name: yaml_node.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 08:09:04 PM CST
 ************************************************************************/

#include "config/yaml/node.h"

#include <cctype>
#include <cstring>

#include <yaml.h>

#include "config/yaml/node_private.hpp"

namespace eular {
YamlNode::YamlNode()
{
}

YamlNode::~YamlNode()
{
}

bool YamlNode::valid() const noexcept
{
    return m_private != nullptr && m_private->document != nullptr && m_private->node != nullptr && m_private->id > 0;
}

YamlNode::NodeType YamlNode::type() const noexcept
{
    if (!valid()) {
        return NodeType::Null;
    }

    switch (m_private->node->type) {
    case YAML_SCALAR_NODE:
        return NodeType::Scalar;
    case YAML_SEQUENCE_NODE:
        return NodeType::Sequence;
    case YAML_MAPPING_NODE:
        return NodeType::Map;
    default:
        break;
    }

    return NodeType::Null;
}

bool YamlNode::isNull() const noexcept
{
    return type() == NodeType::Null;
}

bool YamlNode::isScalar() const noexcept
{
    return type() == NodeType::Scalar;
}

bool YamlNode::isSequence() const noexcept
{
    return type() == NodeType::Sequence;
}

bool YamlNode::isMap() const noexcept
{
    return type() == NodeType::Map;
}

uint32_t YamlNode::size() const noexcept
{
    if (!valid()) {
        return 0;
    }

    if (isSequence()) {
        return static_cast<uint32_t>(m_private->node->data.sequence.items.top - m_private->node->data.sequence.items.start);
    }
    if (isMap()) {
        return static_cast<uint32_t>(m_private->node->data.mapping.pairs.top - m_private->node->data.mapping.pairs.start);
    }

    return 0;
}

YamlNode YamlNode::at(const std::string &key) const noexcept
{
    YamlNode node;
    if (!valid() || !isMap()) {
        return node;
    }

    for (yaml_node_pair_t *pair = m_private->node->data.mapping.pairs.start; pair < m_private->node->data.mapping.pairs.top; ++pair) {
        yaml_node_t *keyNode = yaml_document_get_node(m_private->document, pair->key);
        if (keyNode == nullptr || keyNode->type != YAML_SCALAR_NODE) {
            continue;
        }

        const char *rawKey = reinterpret_cast<const char *>(keyNode->data.scalar.value);
        size_t rawSize = keyNode->data.scalar.length;
        if (rawSize == key.size() && std::memcmp(rawKey, key.data(), rawSize) == 0) {
            yaml_node_t *valueNode = yaml_document_get_node(m_private->document, pair->value);
            if (valueNode == nullptr) {
                return YamlNode();
            }

            std::shared_ptr<YamlNodePrivate> nodePrivate = std::make_shared<YamlNodePrivate>();
            nodePrivate->snapshot = m_private->snapshot;
            nodePrivate->document = m_private->document;
            nodePrivate->node = valueNode;
            nodePrivate->id = pair->value;
            node.m_private = nodePrivate;
            return node;
        }
    }

    return node;
}

YamlNode YamlNode::at(uint32_t index) const noexcept
{
    YamlNode node;
    if (!valid() || !isSequence()) {
        return node;
    }

    uint32_t nitems = static_cast<uint32_t>(m_private->node->data.sequence.items.top - m_private->node->data.sequence.items.start);
    if (index >= nitems) {
        return node;
    }

    int32_t childId = m_private->node->data.sequence.items.start[index];
    yaml_node_t *childNode = yaml_document_get_node(m_private->document, childId);
    if (childNode == nullptr) {
        return node;
    }

    std::shared_ptr<YamlNodePrivate> nodePrivate = std::make_shared<YamlNodePrivate>();
    nodePrivate->snapshot = m_private->snapshot;
    nodePrivate->document = m_private->document;
    nodePrivate->node = childNode;
    nodePrivate->id = childId;
    node.m_private = nodePrivate;
    return node;
}

std::string YamlNode::scalar() const noexcept
{
    if (!valid() || !isScalar()) {
        return std::string();
    }

    const char *rawValue = reinterpret_cast<const char *>(m_private->node->data.scalar.value);
    size_t rawSize = m_private->node->data.scalar.length;
    if (rawValue == nullptr || rawSize == 0) {
        return std::string();
    }

    return std::string(rawValue, rawSize);
}

std::string YamlNode::trimScalar(const std::string &value) noexcept
{
    size_t begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return std::string();
    }

    size_t end = value.find_last_not_of(" \t\r\n");
    return std::string(value.c_str() + begin, end - begin + 1);
}

bool YamlNode::parseValue(const std::string &text, std::string &value) noexcept
{
    value = text;
    return true;
}

bool YamlNode::parseValue(const std::string &text, bool &value) noexcept
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

