#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <stdio.h>

#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <log/log.h>
#include <utils/string8.h>
#include <catch/catch.hpp>

#define LOG_TAG "String8-test"

using namespace eular;
using namespace std;

TEST_CASE("test_cstr", "[string8]") {
    String8 tmp("Hello, world!");
    CHECK(std::string("Hello, world!") == tmp.c_str());
}

TEST_CASE("test_operator_plus", "[string8]") {
    String8 src1("Hello, ");

    const char* ccsrc2 = "world!";
    String8 dst1 = src1 + ccsrc2;
    CHECK(std::string("Hello, world!") == dst1.c_str());
    CHECK(std::string("Hello, ") == src1.c_str());
    CHECK(std::string("world!") == ccsrc2);

    String8 ssrc2("world!");
    String8 dst2 = src1 + ssrc2;
    CHECK(std::string("Hello, world!") == dst2.c_str());
    CHECK(std::string("Hello, ") == src1.c_str());
    CHECK(std::string("world!") == ssrc2.c_str());
}

TEST_CASE("test_operator_plus_equals", "[string8]") {
    String8 src1("My voice");

    // Testing String8 += String8
    String8 src2(" is my passport.");
    src1 += src2;
    CHECK(std::string("My voice is my passport.") == src1.c_str());
    CHECK(std::string(" is my passport.") == src2.c_str());

    // Adding const char* to the previous string.
    const char* src3 = " Verify me.";
    src1 += src3;
    CHECK(std::string("My voice is my passport. Verify me.") == src1.c_str());
    CHECK(std::string(" is my passport.") == src2.c_str());
    CHECK(std::string(" Verify me.") == src3);
}

TEST_CASE("test_string_append", "[string8]") {
    String8 s;
    CHECK(3 == s.append("foo"));
    CHECK(std::string("foo") == s.c_str());
    CHECK(3 == s.append("bar"));
    CHECK(std::string("foobar") == s.c_str());
    CHECK(0 == s.append("baz", 0));
    CHECK(std::string("foobar") == s.c_str());
}

TEST_CASE("test_append_format", "[string8]") {
    const char *str1 = "Hello";
    const char *str2 = "World";
    String8 ret;
    ret.appendFormat("%s%s", str1, str2);
    CHECK(std::string("HelloWorld") == ret.c_str());
}

TEST_CASE("test_string_compare", "[string8]") {
    String8 str1 = "hello";
    const char *str2 = "world";

    CHECK(0 == str1.compare("hello"));
    CHECK(str1.compare(str2) < 0);
}

TEST_CASE("test_string_find", "[string8]") {
    eular::String8 str2 = "sssabcssdeabcss";
    int index = str2.find_last_of("abc");
    CHECK(10 == index);
    index = str2.find("abc");
    CHECK(3 == index);
}

TEST_CASE("test_other_function", "[string8]") {
    String8 str1 = "127.0.0.1:8000";
    int index = str1.find(":");
    CHECK(9 == index);
    String8 left = str1.left(index);
    String8 right = str1.right(str1.length() - (index + 1));
    CHECK(std::string("127.0.0.1") == left.c_str());
    CHECK(std::string("8000") == right.c_str());

    CHECK(':' == str1[index]);
    CHECK('\0' == str1[str1.length()]);
    CHECK(str1[str1.length()] == str1[str1.capacity()]);

    // 测试去除\t
    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trim('\t');
        CHECK(std::string("12345") == str2.c_str());
    }

    // 测试中间存在\t情况下trim
    {
        String8 str2 = "He\tllo";
        str2.trim('\t');
        CHECK(6 == str2.length());
        CHECK(std::string("He\tllo") == str2.c_str());

        str2 = "\t\tHe\tllo\t\t\t";
        str2.trim('\t');
        CHECK(6 == str2.length());
        CHECK(std::string("He\tllo") == str2.c_str());
    }

    // 测试全部不为\t情况下trim
    {
        String8 str2 = "Hello";
        str2.trim('\t');
        CHECK(5 == str2.length());
        CHECK(std::string("Hello") == str2.c_str());
    }

    // 测试全部为\t情况下trim
    {
        String8 str2 = "\t\t\t\t\t";
        str2.trim('\t');
        CHECK(0 == str2.length());
        CHECK(std::string("") == str2.c_str());
    }

    // 测试只有一个不为\t情况下trim
    {
        String8 str2 = "\t\tc\t\t";
        str2.trim('\t');
        CHECK(1 == str2.length());
        CHECK(std::string("c") == str2.c_str());
    }

    // 测试只有左侧存在\t情况下trim
    {
        String8 str2 = "\t\tHello";
        str2.trim('\t');
        CHECK(5 == str2.length());
        CHECK(std::string("Hello") == str2.c_str());
    }

    // 测试只有右侧存在\t情况下trim
    {
        String8 str2 = "Hello\t\t";
        str2.trim('\t');
        CHECK(5 == str2.length());
        CHECK(std::string("Hello") == str2.c_str());
    }

    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trimLeft('\t');
        CHECK(5 == str2.length());
        CHECK(std::string("12345\t\t\t") == str2.c_str());
    }

    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trimRight('\t');
        CHECK(5 == str2.length());
        CHECK(std::string("\t\t12345") == str2.c_str());
    }
    
    {
        String8 str2 = "123456789";
        String8 str3 = str2.reverse();
        CHECK(std::string("987654321") == str3.c_str());
    }

    {
        String8 str2 = "123abc456abc789";
        str2.removeAll("abc");
        CHECK(std::string("123456789") == str2.c_str());
    }

    {
        String8 str2 = "abcDEF";
        CHECK(str2.strcasecmp("abcDef") == 0);
        str2.toUpper();
        CHECK(std::string("ABCDEF") == str2.c_str());
        str2.toLower();
        CHECK(std::string("abcdef") == str2.c_str());
    }

    {
        const char *val = "BBC ABCDAB ABCDABCDABDE";
        const char *key = "ABCDABD";
        CHECK(eular::String8::KMP_strstr(val, key) == strstr(val, key) - val);
    }
}

TEST_CASE("test_copyAndAssign", "[string8]") {
    String8 str1 = "hello";
    String8 str2 = str1;
    CHECK(str1.c_str() == str2.c_str());
    str2.append(" world");
    CHECK(std::string("hello world") == str2.c_str());

    String8 str3;
    str3 = str1;
    CHECK(std::string("hello") == str3.c_str());
    str3.append(" world");
    CHECK(std::string("hello world") == str3.c_str());

    CHECK(std::string("hello") == str1.c_str());
}

TEST_CASE("test_format", "[string8]") {
    {
        const char *str = "Hello World!";
        const String8 &Format = String8::Format("%s", str);
        CHECK(Format == str);
    }

    {
        int num = 996;
        const String8 &Format = String8::Format("%d", num);
        int num_2 = atoi(Format.c_str());
        CHECK(num == num_2);
    }

    {
        uint8_t buffer[] = {0x01, 0x02, 0x03, 0x04};
        String8 Format;
        for (int32_t i = 0; i < sizeof(buffer); ++i) {
            Format.appendFormat("0x%02x ", buffer[i]);
        }

        Format.clear();
        for (int32_t i = 0; i < sizeof(buffer); ++i) {
            Format.appendFormat("0x%02x ", buffer[i]);
        }
    }
}

TEST_CASE("test_support_unordered_map_set", "[string8]") {
    const char *hello = "Hello";
    const char *world = "World";

    String8 h = hello;
    String8 w = world;

    std::unordered_map<eular::String8, size_t> hashMap;
    hashMap.insert(std::make_pair(h, String8::Hash(h)));
    hashMap.insert(std::make_pair(w, String8::Hash(w)));

    CHECK(hashMap.size() == 2);

    std::unordered_set<eular::String8> hashSet;
    hashSet.insert(h);
    hashSet.insert(w);

    CHECK(hashSet.size() == 2);
}
