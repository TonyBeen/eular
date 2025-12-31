/*************************************************************************
    > File Name: test_consistent_hash.cc
    > Author: hsz
    > Brief:
    > Created Time: 2024年10月11日 星期五 20时24分41秒
 ************************************************************************/
#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <vector>
#include <map>

#include "catch/catch.hpp"
#include "utils/consistent_hash.h"

TEST_CASE("test_consistent_hash", "[EULAR]") {
    std::vector<std::string> vec;

    vec.emplace_back("192.168.1.10");
    vec.emplace_back("192.168.1.11");
    vec.emplace_back("192.168.1.12");
    vec.emplace_back("192.168.1.13");
    vec.emplace_back("192.168.1.14");

    // 虚拟节点的个数越多, 分布越均匀
    eular::ConsistentHash<std::string> chash(803);

    for (auto &it : vec) {
        chash.insertNode(it, &it);
    }

    std::map<std::string, uint32_t> statistic;
    for (uint32_t i = 0; i < 10000; ++i) {
        auto *node = chash.getNode(std::to_string(i));
        statistic[*node]++;
    }

    for (auto it = statistic.begin(); it != statistic.end(); ++it) {
        printf("IP: %s, Hit Counts: %u\n", it->first.c_str(), it->second);
    }
}