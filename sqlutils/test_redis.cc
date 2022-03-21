/*************************************************************************
    > File Name: test_redis.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 10 Jan 2022 04:59:13 PM CST
 ************************************************************************/

#include "redis.h"
#include <assert.h>
#include <iostream>

using namespace std;
using namespace eular;

RedisInterface gRedis;

void test_key()
{
    cout << "测试键值对\n";
    assert(gRedis.setKeyValue("name", "eular") == 0);
    assert(gRedis.isKeyExist("name"));
    assert(gRedis.setKeyLifeCycle("name", 500000000));
    cout << "name.TTL = " << gRedis.getKeyTTLMS("name") << endl;
    std::vector<String8> val;
    std::vector<String8> key = {"name"};
    cout << "name.value = " << gRedis.getKeyValue("name") << endl;
    gRedis.delKey("name");
    assert(gRedis.isKeyExist("name") == false);
}

void test_hash()
{
    cout << "测试HashTable\n";
    std::vector<std::pair<String8, String8>> filedVal;
    filedVal.push_back(std::make_pair("name", "eular"));
    filedVal.push_back(std::make_pair("sex", "male"));
    filedVal.push_back(std::make_pair("ip", "127.0.0.1"));
    assert(gRedis.hashCreateOrReplace("HashTable", filedVal) == 0);
    String8 hash_name_value;
    assert(gRedis.hashGetKeyFiled("HashTable", "name", hash_name_value) == 0);
    cout << "HashTable.name = " << hash_name_value.c_str() << endl;
    filedVal.clear();
    assert(gRedis.hashGetKeyAll("HashTable", filedVal) == 3);
    for (const auto it : filedVal) {
        cout << "filed = " << it.first.c_str() << ", value = " << it.second.c_str() << endl;
    }

    const char *filed[] = {"name", "sex"};
    assert(gRedis.hashDelKeyOrFiled("HashTable", filed, 2) == 0);

    gRedis.delKey("HashTable");
}

int main(int argc, char **argv)
{
    assert(gRedis.connect("127.0.0.1", 6379, "eular123") == 0);

    test_hash();

    gRedis.disconnect();
    return 0;
}