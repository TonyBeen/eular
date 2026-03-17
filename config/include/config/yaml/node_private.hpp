/*************************************************************************
    > File Name: node_private.hpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Mar 2026 08:22:27 PM CST
 ************************************************************************/

#ifndef __CONFIG_YAML_NODE_PRIVATE_HPP__
#define __CONFIG_YAML_NODE_PRIVATE_HPP__

#include <memory>

#include <yaml.h>

namespace eular {
struct YamlNodePrivate
{
    std::shared_ptr<void> snapshot;
    yaml_document_t*    document = nullptr;
    yaml_node_t*        node = nullptr;
    int32_t             id = 0;

    void clear() {
        snapshot.reset();
        document = nullptr;
        node = nullptr;
        id = 0;
    }
};

} // namespace eular


#endif // __CONFIG_YAML_NODE_PRIVATE_HPP__
