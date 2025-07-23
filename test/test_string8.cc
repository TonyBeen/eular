#include <stdio.h>

#include <iostream>
#include <unordered_map>
#include <unordered_set>

#include <log/log.h>
#include <utils/string8.h>
#include <gtest/gtest.h>

#define LOG_TAG "String8-test"

using namespace eular;
using namespace std;

class String8Test : public testing::Test {
protected:
    virtual void SetUp() {
    }

    virtual void TearDown() {
    }
};

TEST_F(String8Test, Cstr) {
    String8 tmp("Hello, world!");
    EXPECT_STREQ(tmp.c_str(), "Hello, world!");
}

TEST_F(String8Test, OperatorPlus) {
    String8 src1("Hello, ");

    const char* ccsrc2 = "world!";
    String8 dst1 = src1 + ccsrc2;
    EXPECT_STREQ(dst1.c_str(), "Hello, world!");
    EXPECT_STREQ(src1.c_str(), "Hello, ");
    EXPECT_STREQ(ccsrc2, "world!");

    String8 ssrc2("world!");
    String8 dst2 = src1 + ssrc2;
    EXPECT_STREQ(dst2.c_str(), "Hello, world!");
    EXPECT_STREQ(src1.c_str(), "Hello, ");
    EXPECT_STREQ(ssrc2.c_str(), "world!");
}

TEST_F(String8Test, OperatorPlusEquals) {
    String8 src1("My voice");

    // Testing String8 += String8
    String8 src2(" is my passport.");
    src1 += src2;
    EXPECT_STREQ(src1.c_str(), "My voice is my passport.");
    EXPECT_STREQ(src2.c_str(), " is my passport.");

    // Adding const char* to the previous string.
    const char* src3 = " Verify me.";
    src1 += src3;
    EXPECT_STREQ(src1.c_str(), "My voice is my passport. Verify me.");
    EXPECT_STREQ(src2.c_str(), " is my passport.");
    EXPECT_STREQ(src3, " Verify me.");
}

TEST_F(String8Test, stringAppend) {
    String8 s;
    EXPECT_EQ(3, s.append("foo"));
    EXPECT_STREQ("foo", s.c_str());
    EXPECT_EQ(3, s.append("bar"));
    EXPECT_STREQ("foobar", s.c_str());
    EXPECT_EQ(0, s.append("baz", 0));
    EXPECT_STREQ("foobar", s.c_str());
}

TEST_F(String8Test, appendFormat) {
    const char *str1 = "Hello";
    const char *str2 = "World";
    String8 ret;
    ret.appendFormat("%s%s", str1, str2);
    EXPECT_STREQ(ret.c_str(), "HelloWorld");
}

TEST_F(String8Test, stringCompare) {
    String8 str1 = "hello";
    const char *str2 = "world";

    EXPECT_EQ(str1.compare("hello"), 0);
    EXPECT_LT(str1.compare(str2), 0);
}

TEST_F(String8Test, stringFind) {
    eular::String8 str2 = "sssabcssdeabcss";
    int index = str2.find_last_of("abc");
    EXPECT_EQ(index, 10);
    index = str2.find("abc");
    EXPECT_EQ(index, 3);
}

TEST_F(String8Test, otherFunction) {
    String8 str1 = "127.0.0.1:8000";
    int index = str1.find(":");
    ASSERT_EQ(index, 9);
    String8 left = str1.left(index);
    String8 right = str1.right(str1.length() - (index + 1));
    EXPECT_STREQ(left.c_str(), "127.0.0.1");
    EXPECT_STREQ(right.c_str(), "8000");

    EXPECT_EQ(str1[index], ':');
    EXPECT_EQ(str1[str1.length()], '\0');
    EXPECT_EQ(str1[1000], str1[str1.capacity()]);

    // 测试去除\t
    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trim('\t');
        EXPECT_STREQ(str2.c_str(), "12345");
    }

    // 测试中间存在\t情况下trim
    {
        String8 str2 = "He\tllo";
        str2.trim('\t');
        EXPECT_TRUE(6 == str2.length());
        EXPECT_STREQ(str2.c_str(), "He\tllo");

        str2 = "\t\tHe\tllo\t\t\t";
        str2.trim('\t');
        EXPECT_TRUE(6 == str2.length());
        EXPECT_STREQ(str2.c_str(), "He\tllo");
    }

    // 测试全部不为\t情况下trim
    {
        String8 str2 = "Hello";
        str2.trim('\t');
        EXPECT_TRUE(5 == str2.length());
        EXPECT_STREQ(str2.c_str(), "Hello");
    }

    // 测试全部为\t情况下trim
    {
        String8 str2 = "\t\t\t\t\t";
        str2.trim('\t');
        EXPECT_TRUE(0 == str2.length());
        EXPECT_STREQ(str2.c_str(), "");
    }

    // 测试只有一个不为\t情况下trim
    {
        String8 str2 = "\t\tc\t\t";
        str2.trim('\t');
        EXPECT_TRUE(1 == str2.length());
        EXPECT_STREQ(str2.c_str(), "c");
    }

    // 测试只有左侧存在\t情况下trim
    {
        String8 str2 = "\t\tHello";
        str2.trim('\t');
        EXPECT_TRUE(5 == str2.length());
        EXPECT_STREQ(str2.c_str(), "Hello");
    }

    // 测试只有右侧存在\t情况下trim
    {
        String8 str2 = "Hello\t\t";
        str2.trim('\t');
        EXPECT_TRUE(5 == str2.length());
        EXPECT_STREQ(str2.c_str(), "Hello");
    }

    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trimLeft('\t');
        EXPECT_STREQ(str2.c_str(), "12345\t\t\t");
    }

    {
        String8 str2 = "\t\t12345\t\t\t";
        str2.trimRight('\t');
        EXPECT_STREQ(str2.c_str(), "\t\t12345");
    }
    
    {
        String8 str2 = "123456789";
        String8 str3 = str2.reverse();
        EXPECT_STREQ(str3.c_str(), "987654321");
    }

    {
        String8 str2 = "123abc456abc789";
        str2.removeAll("abc");
        EXPECT_STREQ(str2.c_str(), "123456789");
    }

    {
        String8 str2 = "abcDEF";
        EXPECT_EQ(str2.strcasecmp("abcDef"), 0);
        str2.toUpper();
        EXPECT_STREQ(str2.c_str(), "ABCDEF");
        str2.toLower();
        EXPECT_STREQ(str2.c_str(), "abcdef");
    }

    {
        const char *val = "BBC ABCDAB ABCDABCDABDE";
        const char *key = "ABCDABD";
        EXPECT_EQ(eular::String8::KMP_strstr(val, key), strstr(val, key) - val);
    }
}

TEST_F(String8Test, copyAndAssign) {
    String8 str1 = "hello";
    String8 str2 = str1;
    EXPECT_EQ(str1.c_str(), str2.c_str());
    str2.append(" world");
    EXPECT_STREQ(str2.c_str(), "hello world");

    String8 str3;
    str3 = str1;
    EXPECT_STREQ(str3.c_str(), "hello");
    str3.append(" world");
    EXPECT_STREQ(str3.c_str(), "hello world");

    EXPECT_STREQ(str1.c_str(), "hello");
}

TEST_F(String8Test, Format) {
    {
        const char *str = "Hello World!";
        const String8 &Format = String8::Format("%s", str);
        EXPECT_TRUE(Format == str);
    }

    {
        int num = 996;
        const String8 &Format = String8::Format("%d", num);
        int num_2 = atoi(Format.c_str());
        EXPECT_EQ(num, num_2);
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

TEST_F(String8Test, support_unordered_map_set) {
    const char *hello = "Hello";
    const char *world = "World";

    String8 h = hello;
    String8 w = world;

    std::unordered_map<eular::String8, size_t> hashMap;
    hashMap.insert(std::make_pair(h, String8::Hash(h)));
    hashMap.insert(std::make_pair(w, String8::Hash(w)));

    EXPECT_EQ(hashMap.size(), 2);

    std::unordered_set<eular::String8> hashSet;
    hashSet.insert(h);
    hashSet.insert(w);

    EXPECT_EQ(hashSet.size(), 2);
}

int main(int argc, char* argv[])
{
    testing::InitGoogleTest(&argc, argv); 
    return RUN_ALL_TESTS();
}
