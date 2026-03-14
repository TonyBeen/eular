/*************************************************************************
    > File Name: node.h
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 02:40:28 PM CST
 ************************************************************************/

#ifndef __CONFIG_YAML_NODE_H__
#define __CONFIG_YAML_NODE_H__

#include <string>
#include <memory>

#include <c4/charconv.hpp>

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

private:


private:
    std::shared_ptr<YamlNodePrivate>    m_private = nullptr;
};
} // namespace eular

#endif // __CONFIG_YAML_NODE_H__
