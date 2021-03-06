#include <utils/Buffer.h>
#include <utils/utils.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <bits/move.h>
#include <assert.h>

using namespace eular;

int main()
{
    ByteBuffer buf;
    printf("1 测试创建对象，调用函数-----------------\n");
    printf("capacity = %zu\n", buf.capacity());
    printf("data size = %zu\n", buf.size());
    printf("data = %p\n", buf.data());

    printf("2 测试移动赋值函数-----------------\n");
    ByteBuffer tmp("Hello World", 11);
    printf("data = %p; capacity = %zu; data size = %zu\n", tmp.data(), tmp.capacity(), tmp.size());

    buf = std::move(tmp);
    printf("data = %p; capacity = %zu; data size = %zu\n", buf.data(), buf.capacity(), buf.size());

    printf("3 测试append函数-----------------\n");
    buf.append((const uint8_t *)"-----", 5);
    printf("capacity = %zu\n", buf.capacity());
    printf("data size = %zu\n", buf.size());
    printf("data = %p\n", buf.data());
    printf("data should be \"Hello World-----\", real \"%s\"\n", buf.begin());

    printf("4 测试resize函数-----------------\n");
    buf.resize(1024);
    printf("capacity = %zu\n", buf.capacity());
    printf("data size = %zu\n", buf.size());
    printf("data = %p\n", buf.data());
    printf("data begin = %p(%s) end = %p index = 0->%c\n", buf.begin(), buf.begin(), buf.end(), buf[0]);
    buf[6] = '*';
    printf("data begin = %p(%s) end = %p index = 6->%c\n", buf.begin(), buf.begin(), buf.end(), buf[6]);

    printf("5 测试clear及赋值函数-----------------\n");
    buf.clear();
    printf("capacity = %zu\n", buf.capacity());
    printf("data size = %zu\n", buf.size());
    printf("data = %p\n", buf.data());
    printf("data begin = %p(%s) end = %p\n", buf.begin(), buf.begin(), buf.end());

    printf("6 测试移动构造函数--------------------\n");
    ByteBuffer tmp2("Hello World", 11);
    ByteBuffer con = std::move(tmp2);
    printf("data begin = %p(%s) end = %p\n", con.begin(), con.begin(), con.end());

    return 0;
}