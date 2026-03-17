/*************************************************************************
    > File Name: node_private.hpp
    > Author: eular
    > Brief:
    > Created Time: Tue 17 Mar 2026 04:38:00 PM CST
 ************************************************************************/

#ifndef __CONFIG_JSON_NODE_PRIVATE_HPP__
#define __CONFIG_JSON_NODE_PRIVATE_HPP__

#include <memory>

#include <yyjson.h>

namespace eular {
struct JsonNodePrivate
{
    std::shared_ptr<void> snapshot;
    yyjson_doc*          document = nullptr;
    yyjson_val*          node = nullptr;

    void clear() {
        snapshot.reset();
        document = nullptr;
        node = nullptr;
    }
};

} // namespace eular


#endif // __CONFIG_JSON_NODE_PRIVATE_HPP__
