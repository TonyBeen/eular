#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#include <stdio.h>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <limits>

#include <utils/string8.h>
#include <catch/catch.hpp>

using namespace eular;
using namespace std;

// ==================== 构造函数测试 ====================

TEST_CASE("constructor_default", "[string8][constructor]") {
    String8 s;
    CHECK(s.empty());
    CHECK(s.length() == 0);
    CHECK(s.c_str() != nullptr);
    CHECK(s.c_str()[0] == '\0');
    CHECK(s.isLocal());  // 应该使用栈存储
    CHECK(s.capacity() >= 31);  // LOCAL_STRING_SIZE - 1
}

TEST_CASE("constructor_with_size", "[string8][constructor]") {
    // 小于栈容量
    {
        String8 s(10);
        CHECK(s.empty());
        CHECK(s.isLocal());
        CHECK(s.capacity() >= 10);
    }

    // 等于栈容量边界
    {
        String8 s(31);
        CHECK(s.empty());
        CHECK(s.isLocal());
    }

    // 大于栈容量，应使用堆
    {
        String8 s(100);
        CHECK(s.empty());
        CHECK(!s.isLocal());
        CHECK(s.capacity() >= 100);
    }

    // 零大小
    {
        String8 s(0u);
        CHECK(s.empty());
        CHECK(s.isLocal());
    }
}

TEST_CASE("constructor_from_cstring", "[string8][constructor]") {
    // 普通字符串
    {
        String8 s("Hello");
        CHECK(s.length() == 5);
        CHECK(std::string("Hello") == s.c_str());
        CHECK(s.isLocal());  // 小于32字节，应在栈上
    }

    // 空字符串
    {
        String8 s("");
        CHECK(s.empty());
        CHECK(s.length() == 0);
    }

    // nullptr
    {
        String8 s(static_cast<const char*>(nullptr));
        CHECK(s.empty());
    }

    // 恰好31字节（栈边界）
    {
        String8 s("1234567890123456789012345678901");  // 31字符
        CHECK(s.length() == 31);
        CHECK(s.isLocal());
    }

    // 恰好32字节（超过栈边界）
    {
        String8 s("12345678901234567890123456789012");  // 32字符
        CHECK(s.length() == 32);
        CHECK(!s.isLocal());  // 应该在堆上
    }

    // 长字符串
    {
        std::string longStr(1000, 'x');
        String8 s(longStr.c_str());
        CHECK(s.length() == 1000);
        CHECK(!s.isLocal());
    }
}

TEST_CASE("constructor_from_cstring_with_length", "[string8][constructor]") {
    // 正常情况
    {
        String8 s("Hello, World!", 5);
        CHECK(s.length() == 5);
        CHECK(std::string("Hello") == s.c_str());
    }

    // numChars = 0
    {
        String8 s("Hello", 0);
        CHECK(s.empty());
    }

    // numChars 大于实际长度
    {
        char something[128] = {'H', 'i', '\0'};
        String8 s(something, 100);
        CHECK(s.length() == 100);
        CHECK(s.toStdString() == std::string(something, 100));
    }

    // nullptr
    {
        String8 s(static_cast<const char*>(nullptr), 10);
        CHECK(s.empty());
    }

    // 边界：恰好填满栈
    {
        std::string str33(33, 'a');
        String8 s(str33.c_str(), 31);
        CHECK(s.length() == 31);
        CHECK(s.isLocal());
    }
}

TEST_CASE("constructor_from_std_string", "[string8][constructor]") {
    // 普通情况
    {
        std::string stdStr = "Hello, World!";
        String8 s(stdStr);
        CHECK(s.length() == stdStr.length());
        CHECK(std::string(s.c_str()) == stdStr);
    }

    // 空字符串
    {
        std::string stdStr = "";
        String8 s(stdStr);
        CHECK(s.empty());
    }

    // 包含空字符的字符串
    {
        std::string stdStr = std::string("Hi\0There", 8);
        String8 s(stdStr);
        CHECK(s.length() == 8);
        CHECK(s.toStdString() == stdStr);
    }
}

TEST_CASE("constructor_copy", "[string8][constructor]") {
    // 栈上字符串的拷贝
    {
        String8 s1("Hello");
        String8 s2(s1);
        CHECK(s2.length() == s1.length());
        CHECK(std::string(s2.c_str()) == std::string(s1.c_str()));
        CHECK(s2.isLocal());

        // 修改一个不影响另一个
        s2.append(" World");
        CHECK(std::string("Hello") == s1.c_str());
        CHECK(std::string("Hello World") == s2.c_str());
    }

    // 堆上字符串的拷贝
    {
        std::string longStr(100, 'x');
        String8 s1(longStr.c_str());
        String8 s2(s1);
        CHECK(s2.length() == s1.length());
        CHECK(!s2.isLocal());
    }

    // 空字符串拷贝
    {
        String8 s1;
        String8 s2(s1);
        CHECK(s2.empty());
    }
}

TEST_CASE("constructor_move", "[string8][constructor]") {
    // 栈上字符串的移动
    {
        String8 s1("Hello");
        const char* originalContent = s1.c_str();
        String8 s2(std::move(s1));

        CHECK(std::string("Hello") == s2.c_str());
        CHECK(s2.isLocal());
        // s1 应该处于有效但未定义状态
        CHECK(s1.c_str() != nullptr);  // 至少不是nullptr
    }

    // 堆上字符串的移动
    {
        std::string longStr(100, 'y');
        String8 s1(longStr.c_str());
        String8 s2(std::move(s1));

        CHECK(s2.length() == 100);
        CHECK(!s2.isLocal());
        CHECK(s1.empty());  // 移动后应该为空
        CHECK(s1.isLocal()); // 移动后应该回到栈状态
    }
}

// ==================== 访问方法测试 ====================

TEST_CASE("c_str_and_data", "[string8][access]") {
    String8 s("Hello");

    CHECK(s.c_str() != nullptr);
    CHECK(s.data() != nullptr);
    CHECK(std::string(s.c_str()) == "Hello");
    CHECK(std::string(s.data()) == "Hello");

    // 修改 data()
    s.data()[0] = 'J';
    CHECK(std::string("Jello") == s.c_str());
}

TEST_CASE("front_and_back", "[string8][access]") {
    String8 s("Hello");

    CHECK(s.front() == 'H');
    CHECK(s.back() == 'o');

    // 修改
    s.front() = 'J';
    s.back() = 'y';
    CHECK(std::string("Jelly") == s.c_str());

    // 单字符
    String8 s2("X");
    CHECK(s2.front() == 'X');
    CHECK(s2.back() == 'X');
    CHECK(&s2.front() == &s2.back());

    // 空字符串应该抛异常
    String8 empty;
    CHECK_THROWS(empty.front());
    CHECK_THROWS(empty.back());
}

TEST_CASE("operator_subscript", "[string8][access]") {
    String8 s("Hello");

    CHECK(s[0] == 'H');
    CHECK(s[1] == 'e');
    CHECK(s[4] == 'o');

    // 修改
    s[0] = 'J';
    CHECK(std::string("Jello") == s.c_str());

    // 边界访问
    CHECK(s[s.length()] == '\0');

    // const 版本
    const String8& cs = s;
    CHECK(cs[0] == 'J');
    CHECK(cs[s.length()] == '\0');
}

TEST_CASE("empty_length_capacity", "[string8][access]") {
    String8 s;
    CHECK(s.empty());
    CHECK(s.length() == 0);
    CHECK(s.capacity() >= 31);

    s.append("Hello");
    CHECK(!s.empty());
    CHECK(s.length() == 5);

    s.clear();
    CHECK(s.empty());
    CHECK(s.length() == 0);
    CHECK(s.capacity() >= 31);  // capacity 不应该减少
}

TEST_CASE("toStdString", "[string8][access]") {
    String8 s("Hello, World!");
    std::string stdStr = s.toStdString();

    CHECK(stdStr == "Hello, World!");
    CHECK(stdStr.length() == s.length());

    // 空字符串
    String8 empty;
    CHECK(empty.toStdString() == "");
}

TEST_CASE("isLocal", "[string8][access]") {
    // 栈存储
    String8 s1("Short");
    CHECK(s1.isLocal());

    // 堆存储
    std::string longStr(100, 'x');
    String8 s2(longStr.c_str());
    CHECK(!s2.isLocal());

    // 边界测试
    String8 s3("1234567890123456789012345678901");  // 31字符
    CHECK(s3.isLocal());

    String8 s4("12345678901234567890123456789012");  // 32字符
    CHECK(!s4.isLocal());
}

// ==================== 修改方法测试 ====================

TEST_CASE("clear", "[string8][modify]") {
    String8 s("Hello, World!");
    CHECK(!s.empty());

    s.clear();
    CHECK(s.empty());
    CHECK(s.length() == 0);
    CHECK(s.c_str()[0] == '\0');

    // 多次清除
    s.clear();
    CHECK(s.empty());

    // 清除空字符串
    String8 empty;
    empty.clear();
    CHECK(empty.empty());
}

TEST_CASE("reserve", "[string8][modify]") {
    String8 s;

    // 预留小于栈容量
    s.reserve(10);
    CHECK(s.capacity() >= 10);
    CHECK(s.isLocal());

    // 预留大于栈容量
    s.reserve(100);
    CHECK(s.capacity() >= 100);
    CHECK(!s.isLocal());

    // 预留更大的容量
    s.reserve(200);
    CHECK(s.capacity() >= 200);

    // 预留更小的容量不应减小实际容量
    size_t oldCap = s.capacity();
    s.reserve(50);
    CHECK(s.capacity() >= oldCap);

    // 带内容的字符串 reserve
    String8 s2("Hello");
    s2.reserve(100);
    CHECK(std::string("Hello") == s2.c_str());
    CHECK(s2.capacity() >= 100);
}

TEST_CASE("resize", "[string8][modify]") {
    String8 s("Hello");

    // 缩小
    s.resize(3);
    CHECK(s.length() == 3);
    CHECK(std::string("Hel") == s.c_str());

    // 扩大
    s.resize(6);
    CHECK(s.length() == 6);
    CHECK(s[3] == '\0');

    // resize 到 0
    s.resize(0);
    CHECK(s.empty());

    // 从空字符串 resize
    String8 empty;
    empty.resize(5);
    CHECK(empty.length() == 5);
}

TEST_CASE("append_char", "[string8][modify]") {
    String8 s;

    s.append('H');
    CHECK(s.length() == 1);
    CHECK(std::string("H") == s.c_str());

    s.append('i');
    CHECK(s.length() == 2);
    CHECK(std::string("Hi") == s.c_str());

    // 追加空字符
    s.append('\0');
    CHECK(s.length() == 3);
}

TEST_CASE("append_string8", "[string8][modify]") {
    String8 s1("Hello");
    String8 s2(", World!");

    s1.append(s2);
    CHECK(std::string("Hello, World!") == s1.c_str());
    CHECK(std::string(", World!") == s2.c_str());  // s2 不变

    // 追加空字符串
    String8 empty;
    s1.append(empty);
    CHECK(std::string("Hello, World!") == s1.c_str());

    // 追加自身
    String8 s3("AB");
    s3.append(s3);
    CHECK(std::string("ABAB") == s3.c_str());
}

TEST_CASE("append_cstring", "[string8][modify]") {
    String8 s;

    CHECK(s.append("foo") == 3);
    CHECK(std::string("foo") == s.c_str());

    CHECK(s.append("bar") == 3);
    CHECK(std::string("foobar") == s.c_str());

    // nullptr
    CHECK(s.append(static_cast<const char*>(nullptr)) == 0);
    CHECK(std::string("foobar") == s.c_str());

    // 空字符串
    CHECK(s.append("") == 0);
    CHECK(std::string("foobar") == s.c_str());
}

TEST_CASE("append_cstring_with_length", "[string8][modify]") {
    String8 s;

    CHECK(s.append("Hello, World!", 5) == 5);
    CHECK(std::string("Hello") == s.c_str());

    // numChars = 0
    CHECK(s.append("baz", 0) == 0);
    CHECK(std::string("Hello") == s.c_str());

    // numChars 大于实际长度
    char something[128] = {'H', 'i', '\0'};
    s.clear();
    CHECK(s.append(something, 100) == 100);
    CHECK(s.length() == 100);
}

TEST_CASE("append_triggers_heap_allocation", "[string8][modify]") {
    String8 s;
    CHECK(s.isLocal());

    // 逐渐追加直到超过栈容量
    for (int i = 0; i < 10; ++i) {
        s.append("ABCD");
    }

    CHECK(s.length() == 40);
    CHECK(!s.isLocal());
}

// ==================== 字符串操作测试 ====================

TEST_CASE("left", "[string8][operation]") {
    String8 s("Hello, World!");

    CHECK(std::string("Hello") == s.left(5).c_str());
    CHECK(std::string("H") == s.left(1).c_str());
    CHECK(std::string("") == s.left(0).c_str());

    // n 大于长度
    String8 result = s.left(100);
    CHECK(std::string("Hello, World!") == result.c_str());

    // 空字符串
    String8 empty;
    CHECK(empty.left(5).empty());
}

TEST_CASE("right", "[string8][operation]") {
    String8 s("Hello, World!");

    CHECK(std::string("orld!") == s.right(5).c_str());
    CHECK(std::string("!") == s.right(1).c_str());
    CHECK(std::string("") == s.right(0).c_str());

    // n 大于长度
    String8 result = s.right(100);
    CHECK(std::string("Hello, World!") == result.c_str());

    // 空字符串
    String8 empty;
    CHECK(empty.right(5).empty());
}

TEST_CASE("trim", "[string8][operation]") {
    // 两端都有空格
    {
        String8 s("  Hello  ");
        s.trim();
        CHECK(std::string("Hello") == s.c_str());
    }

    // 只有左边
    {
        String8 s("   Hello");
        s.trim();
        CHECK(std::string("Hello") == s.c_str());
    }

    // 只有右边
    {
        String8 s("Hello   ");
        s.trim();
        CHECK(std::string("Hello") == s.c_str());
    }

    // 没有空格
    {
        String8 s("Hello");
        s.trim();
        CHECK(std::string("Hello") == s.c_str());
    }

    // 全是空格
    {
        String8 s("     ");
        s.trim();
        CHECK(s.empty());
    }

    // 中间有空格
    {
        String8 s("  Hello World  ");
        s.trim();
        CHECK(std::string("Hello World") == s.c_str());
    }

    // 自定义字符
    {
        String8 s("\t\tHello\t\t");
        s.trim('\t');
        CHECK(std::string("Hello") == s.c_str());
    }

    // 空字符串
    {
        String8 s;
        s.trim();
        CHECK(s.empty());
    }

    // 单字符
    {
        String8 s("X");
        s.trim();
        CHECK(std::string("X") == s.c_str());
    }

    // 单个空格包围的单字符
    {
        String8 s(" X ");
        s.trim();
        CHECK(std::string("X") == s.c_str());
    }
}

TEST_CASE("trimLeft", "[string8][operation]") {
    {
        String8 s("  Hello  ");
        s.trimLeft();
        CHECK(std::string("Hello  ") == s.c_str());
    }

    {
        String8 s("Hello");
        s.trimLeft();
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s("     ");
        s.trimLeft();
        CHECK(s.empty());
    }

    {
        String8 s("\t\tHello");
        s.trimLeft('\t');
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s;
        s.trimLeft();
        CHECK(s.empty());
    }
}

TEST_CASE("trimRight", "[string8][operation]") {
    {
        String8 s("  Hello  ");
        s.trimRight();
        CHECK(std::string("  Hello") == s.c_str());
    }

    {
        String8 s("Hello");
        s.trimRight();
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s("     ");
        s.trimRight();
        CHECK(s.empty());
    }

    {
        String8 s("Hello\t\t");
        s.trimRight('\t');
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s;
        s.trimRight();
        CHECK(s.empty());
    }
}

TEST_CASE("reverse", "[string8][operation]") {
    {
        String8 s("Hello");
        String8 r = s.reverse();
        CHECK(std::string("olleH") == r.c_str());
        CHECK(std::string("Hello") == s.c_str());  // 原字符串不变
    }

    {
        String8 s("A");
        CHECK(std::string("A") == s.reverse().c_str());
    }

    {
        String8 s("AB");
        CHECK(std::string("BA") == s.reverse().c_str());
    }

    {
        String8 s;
        CHECK(s.reverse().empty());
    }

    {
        String8 s("12345");
        CHECK(std::string("54321") == s.reverse().c_str());
    }
}

TEST_CASE("substr", "[string8][operation]") {
    String8 s("Hello, World!");

    CHECK(std::string("Hello") == s.substr(0, 4).c_str());
    CHECK(std::string("World!") == s.substr(7, 12).c_str());
    CHECK(std::string("H") == s.substr(0, 0).c_str());

    // start >= length
    CHECK(s.substr(100, 105).empty());

    // start > end
    CHECK(s.substr(5, 3).empty());

    // end >= length
    CHECK(std::string("World!") == s.substr(7, 100).c_str());

    // 空字符串
    String8 empty;
    CHECK(empty.substr(0, 5).empty());
}

TEST_CASE("contains", "[string8][operation]") {
    String8 s("Hello, World!");

    CHECK(s.contains("Hello"));
    CHECK(s.contains("World"));
    CHECK(s.contains(","));
    CHECK(s.contains("Hello, World!"));
    CHECK(!s.contains("Goodbye"));
    CHECK(!s.contains("hello"));  // 大小写敏感

    // 空字符串
    CHECK(s.contains(""));

    String8 empty;
    CHECK(!empty.contains("test"));
    CHECK(empty.contains(""));
}

TEST_CASE("removeAll", "[string8][operation]") {
    {
        String8 s("abcabcabc");
        s.removeAll("abc");
        CHECK(s.empty());
    }

    {
        String8 s("123abc456abc789");
        s.removeAll("abc");
        CHECK(std::string("123456789") == s.c_str());
    }

    {
        String8 s("Hello");
        s.removeAll("xyz");
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s("Hello");
        s.removeAll("");
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s("aaa");
        s.removeAll("a");
        CHECK(s.empty());
    }
}

TEST_CASE("replaceAll", "[string8][operation]") {
    {
        String8 s("Hello");
        CHECK(s.replaceAll('l', 'L') == 2);
        CHECK(std::string("HeLLo") == s.c_str());
    }

    {
        String8 s("Hello");
        CHECK(s.replaceAll('x', 'y') == 0);
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s("aaaa");
        CHECK(s.replaceAll('a', 'b') == 4);
        CHECK(std::string("bbbb") == s.c_str());
    }

    {
        String8 s("Hello");
        CHECK(s.replaceAll('H', 'H') == 0);  // 相同字符
        CHECK(std::string("Hello") == s.c_str());
    }

    {
        String8 s;
        CHECK(s.replaceAll('a', 'b') == 0);
    }
}

TEST_CASE("toLower", "[string8][operation]") {
    {
        String8 s("HELLO");
        s.toLower();
        CHECK(std::string("hello") == s.c_str());
    }

    {
        String8 s("Hello World");
        s.toLower();
        CHECK(std::string("hello world") == s.c_str());
    }

    {
        String8 s("hello");
        s.toLower();
        CHECK(std::string("hello") == s.c_str());
    }

    {
        String8 s("123ABC");
        s.toLower();
        CHECK(std::string("123abc") == s.c_str());
    }

    {
        String8 s;
        s.toLower();
        CHECK(s.empty());
    }

    // 指定范围
    {
        String8 s("HELLO WORLD");
        s.toLower(0, 5);
        CHECK(std::string("hello WORLD") == s.c_str());
    }

    {
        String8 s("HELLO WORLD");
        s.toLower(6, 5);
        CHECK(std::string("HELLO world") == s.c_str());
    }

    // 范围超出
    {
        String8 s("HELLO");
        s.toLower(0, 100);
        CHECK(std::string("hello") == s.c_str());
    }

    // start 超出
    {
        String8 s("HELLO");
        s.toLower(100, 5);
        CHECK(std::string("HELLO") == s.c_str());
    }
}

TEST_CASE("toUpper", "[string8][operation]") {
    {
        String8 s("hello");
        s.toUpper();
        CHECK(std::string("HELLO") == s.c_str());
    }

    {
        String8 s("Hello World");
        s.toUpper();
        CHECK(std::string("HELLO WORLD") == s.c_str());
    }

    {
        String8 s("HELLO");
        s.toUpper();
        CHECK(std::string("HELLO") == s.c_str());
    }

    {
        String8 s("123abc");
        s.toUpper();
        CHECK(std::string("123ABC") == s.c_str());
    }

    {
        String8 s;
        s.toUpper();
        CHECK(s.empty());
    }

    // 指定范围
    {
        String8 s("hello world");
        s.toUpper(0, 5);
        CHECK(std::string("HELLO world") == s.c_str());
    }
}

// ==================== 查找方法测试 ====================

TEST_CASE("find_String8", "[string8][find]") {
    String8 s("Hello, World! Hello!");

    CHECK(s.find(String8("Hello")) == 0);
    CHECK(s.find(String8("World")) == 7);
    CHECK(s.find(String8("Hello"), 1) == 14);
    CHECK(s.find(String8("NotFound")) == -1);

    // 空字符串
    CHECK(s.find(String8("")) == 0);

    String8 empty;
    CHECK(empty.find(String8("test")) == -1);
}

TEST_CASE("find_cstring", "[string8][find]") {
    String8 s("Hello, World! Hello!");

    CHECK(s.find("Hello") == 0);
    CHECK(s.find("World") == 7);
    CHECK(s.find("Hello", 1) == 14);
    CHECK(s.find("NotFound") == -1);
    CHECK(s.find("!") == 12);

    // 边界情况
    CHECK(s.find("Hello", 14) == 14);
    CHECK(s.find("Hello", 15) == -1);
}

TEST_CASE("find_char", "[string8][find]") {
    String8 s("Hello, World!");

    CHECK(s.find('H') == 0);
    CHECK(s.find('o') == 4);
    CHECK(s.find('o', 5) == 8);
    CHECK(s.find('!') == 12);
    CHECK(s.find('x') == -1);

    // start 超出范围
    CHECK(s.find('H', 100) == -1);

    String8 empty;
    CHECK(empty.find('a') == -1);
}

TEST_CASE("findChar", "[string8][find]") {
    String8 s("  Hello  ");
    int begin = -1, end = -1;

    s.findChar(begin, end, ' ');
    CHECK(begin == 0);
    CHECK(end == 8);

    begin = end = -1;
    s.findChar(begin, end, 'x');
    CHECK(begin == -1);
    CHECK(end == -1);

    String8 s2("Hello");
    begin = end = -1;
    s2.findChar(begin, end, 'l');
    CHECK(begin == 2);
    CHECK(end == 3);
}

TEST_CASE("findNotChar", "[string8][find]") {
    String8 s("  Hello  ");
    int begin = -1, end = -1;

    s.findNotChar(begin, end, ' ');
    CHECK(begin == 2);
    CHECK(end == 6);

    String8 s2("     ");
    begin = end = -1;
    s2.findNotChar(begin, end, ' ');
    CHECK(begin == -1);
    CHECK(end == -1);

    String8 s3("Hello");
    begin = end = -1;
    s3.findNotChar(begin, end, ' ');
    CHECK(begin == 0);
    CHECK(end == 4);
}

TEST_CASE("find_last_of", "[string8][find]") {
    String8 s("sssabcssdeabcss");

    CHECK(s.find_last_of("abc") == 10);
    CHECK(s.find_last_of("sss") == 0);
    CHECK(s.find_last_of("ss") == 13);
    CHECK(s.find_last_of("xyz") == -1);

    // 单字符
    CHECK(s.find_last_of("s") == 14);

    // 整个字符串
    CHECK(s.find_last_of("sssabcssdeabcss") == 0);

    // String8 版本
    CHECK(s.find_last_of(String8("abc")) == 10);

    // 空字符串
    String8 empty;
    CHECK(empty.find_last_of("test") == -1);

    // 查找空字符串
    CHECK(s.find_last_of("") == -1);
}

// ==================== 比较方法测试 ====================

TEST_CASE("compare", "[string8][compare]") {
    String8 s1("abc");
    String8 s2("abd");
    String8 s3("abc");
    String8 s4("ab");
    String8 s5("abcd");

    CHECK(s1.compare(s3) == 0);
    CHECK(s1.compare(s2) < 0);
    CHECK(s2.compare(s1) > 0);
    CHECK(s1.compare(s4) > 0);
    CHECK(s1.compare(s5) < 0);

    // C字符串版本
    CHECK(s1.compare("abc") == 0);
    CHECK(s1.compare("abd") < 0);
    CHECK(s1.compare("abb") > 0);
}

TEST_CASE("ncompare", "[string8][compare]") {
    String8 s1("Hello, World!");
    String8 s2("Hello, Everyone!");

    CHECK(s1.ncompare(s2, 7) == 0);
    CHECK(s1.ncompare(s2, 8) != 0);

    // C字符串版本
    CHECK(s1.ncompare("Hello", 5) == 0);
    CHECK(s1.ncompare("Hellx", 4) == 0);
    CHECK(s1.ncompare("Hellx", 5) != 0);

    // nullptr
    CHECK(s1.ncompare(static_cast<const char*>(nullptr), 5) > 0);
}

TEST_CASE("casecmp", "[string8][compare]") {
    String8 s1("Hello");
    String8 s2("HELLO");
    String8 s3("hello");
    String8 s4("World");

    CHECK(s1.casecmp(s2) == 0);
    CHECK(s1.casecmp(s3) == 0);
    CHECK(s1.casecmp(s4) != 0);

    // C字符串版本
    CHECK(s1.casecmp("HELLO") == 0);
    CHECK(s1.casecmp("hello") == 0);
    CHECK(s1.casecmp("world") != 0);
}

// ==================== 运算符测试 ====================

TEST_CASE("operator_assignment", "[string8][operator]") {
    // String8 赋值
    {
        String8 s1("Hello");
        String8 s2;
        s2 = s1;
        CHECK(std::string("Hello") == s2.c_str());

        // 自赋值
        s1 = s1;
        CHECK(std::string("Hello") == s1.c_str());
    }

    // C字符串赋值
    {
        String8 s;
        s = "World";
        CHECK(std::string("World") == s.c_str());

        s = "";
        CHECK(s.empty());
    }

    // 移动赋值
    {
        String8 s1("Hello");
        String8 s2;
        s2 = std::move(s1);
        CHECK(std::string("Hello") == s2.c_str());

        // 自移动赋值
        s2 = std::move(s2);
        CHECK(std::string("Hello") == s2.c_str());
    }
}

TEST_CASE("operator_plus", "[string8][operator]") {
    String8 s1("Hello");
    String8 s2(", World!");

    String8 s3 = s1 + s2;
    CHECK(std::string("Hello, World!") == s3.c_str());
    CHECK(std::string("Hello") == s1.c_str());
    CHECK(std::string(", World!") == s2.c_str());

    // 与 C 字符串
    String8 s4 = s1 + "!!!";
    CHECK(std::string("Hello!!!") == s4.c_str());
}

TEST_CASE("operator_plus_equals", "[string8][operator]") {
    String8 s("Hello");

    s += ", ";
    CHECK(std::string("Hello, ") == s.c_str());

    s += String8("World!");
    CHECK(std::string("Hello, World!") == s.c_str());
}

TEST_CASE("operator_comparison", "[string8][operator]") {
    String8 s1("abc");
    String8 s2("abd");
    String8 s3("abc");

    // String8 比较
    CHECK(s1 == s3);
    CHECK(!(s1 == s2));
    CHECK(s1 != s2);
    CHECK(!(s1 != s3));
    CHECK(s1 < s2);
    CHECK(!(s2 < s1));
    CHECK(s1 <= s2);
    CHECK(s1 <= s3);
    CHECK(s2 > s1);
    CHECK(!(s1 > s2));
    CHECK(s2 >= s1);
    CHECK(s1 >= s3);

    // C字符串比较
    CHECK(s1 == "abc");
    CHECK(s1 != "abd");
    CHECK(s1 < "abd");
    CHECK(s1 <= "abc");
    CHECK(s1 > "abb");
    CHECK(s1 >= "abc");
}

// ==================== 格式化测试 ====================

TEST_CASE("Format", "[string8][format]") {
    // 字符串格式化
    {
        String8 s = String8::Format("%s, %s!", "Hello", "World");
        CHECK(std::string("Hello, World!") == s.c_str());
    }

    // 整数格式化
    {
        String8 s = String8::Format("%d + %d = %d", 1, 2, 3);
        CHECK(std::string("1 + 2 = 3") == s.c_str());
    }

    // 浮点数格式化
    {
        String8 s = String8::Format("%.2f", 3.14159);
        CHECK(std::string("3.14") == s.c_str());
    }

    // 十六进制格式化
    {
        String8 s = String8::Format("0x%08X", 255);
        CHECK(std::string("0x000000FF") == s.c_str());
    }

    // 空格式化
    {
        String8 s = String8::Format("%s", "");
        CHECK(s.empty());
    }

    // 长格式化（超过栈容量）
    {
        char buffer[100];
        memset(buffer, 'A', 99);
        buffer[99] = '\0';
        String8 s = String8::Format("%s", buffer);
        CHECK(s.length() == 99);
        CHECK(!s.isLocal());
    }
}

TEST_CASE("appendFormat", "[string8][format]") {
    String8 s("Values: ");

    s.appendFormat("%d, ", 1);
    CHECK(std::string("Values: 1, ") == s.c_str());

    s.appendFormat("%d, ", 2);
    CHECK(std::string("Values: 1, 2, ") == s.c_str());

    s.appendFormat("%s", "done");
    CHECK(std::string("Values: 1, 2, done") == s.c_str());

    // 追加到空字符串
    String8 empty;
    empty.appendFormat("Test %d", 42);
    CHECK(std::string("Test 42") == empty.c_str());

    // 十六进制数组
    {
        uint8_t buffer[] = {0x01, 0x02, 0x03, 0x04};
        String8 hex;
        for (size_t i = 0; i < sizeof(buffer); ++i) {
            hex.appendFormat("0x%02x ", buffer[i]);
        }
        CHECK(std::string("0x01 0x02 0x03 0x04 ") == hex.c_str());
    }
}

// ==================== 静态方法测试 ====================

TEST_CASE("KMP_strstr", "[string8][static]") {
    // 正常匹配
    {
        const char* val = "BBC ABCDAB ABCDABCDABDE";
        const char* key = "ABCDABD";
        int result = String8::KMP_strstr(val, key);
        CHECK(result == static_cast<int>(strstr(val, key) - val));
    }

    // 开头匹配
    {
        const char* val = "Hello, World!";
        const char* key = "Hello";
        CHECK(String8::KMP_strstr(val, key) == 0);
    }

    // 结尾匹配
    {
        const char* val = "Hello, World!";
        const char* key = "World!";
        CHECK(String8::KMP_strstr(val, key) == 7);
    }

    // 不匹配
    {
        const char* val = "Hello, World!";
        const char* key = "Goodbye";
        CHECK(String8::KMP_strstr(val, key) == -1);
    }

    // 空字符串
    {
        CHECK(String8::KMP_strstr("Hello", "") == 0);
    }

    // nullptr
    {
        CHECK(String8::KMP_strstr(nullptr, "test") == -1);
        CHECK(String8::KMP_strstr("test", nullptr) == -1);
        CHECK(String8::KMP_strstr(nullptr, nullptr) == -1);
    }

    // 子串比母串长
    {
        CHECK(String8::KMP_strstr("Hi", "Hello") == -1);
    }

    // 单字符匹配
    {
        CHECK(String8::KMP_strstr("Hello", "e") == 1);
    }
}

TEST_CASE("Hash", "[string8][static]") {
    String8 s1("Hello");
    String8 s2("Hello");
    String8 s3("World");
    String8 empty;

    // 相同字符串应该有相同的哈希值
    CHECK(String8::Hash(s1) == String8::Hash(s2));

    // 不同字符串通常有不同的哈希值
    CHECK(String8::Hash(s1) != String8::Hash(s3));

    // 空字符串的哈希
    size_t emptyHash = String8::Hash(empty);
    CHECK(emptyHash != String8::Hash(s1));
}

// ==================== 容器支持测试 ====================

TEST_CASE("unordered_map_support", "[string8][container]") {
    std::unordered_map<String8, int> map;

    map["one"] = 1;
    map["two"] = 2;
    map["three"] = 3;

    CHECK(map.size() == 3);
    CHECK(map["one"] == 1);
    CHECK(map["two"] == 2);
    CHECK(map["three"] == 3);

    // 查找
    auto it = map.find(String8("two"));
    CHECK(it != map.end());
    CHECK(it->second == 2);

    // 不存在的键
    it = map.find(String8("four"));
    CHECK(it == map.end());
}

TEST_CASE("unordered_set_support", "[string8][container]") {
    std::unordered_set<String8> set;

    set.insert("apple");
    set.insert("banana");
    set.insert("cherry");
    set.insert("apple");  // 重复

    CHECK(set.size() == 3);
    CHECK(set.count("apple") == 1);
    CHECK(set.count("banana") == 1);
    CHECK(set.count("grape") == 0);
}

// ==================== 流输出测试 ====================

TEST_CASE("stream_output", "[string8][stream]") {
    String8 s("Hello, World!");
    std::ostringstream oss;
    oss << s;
    CHECK(oss.str() == "Hello, World!");

    // 空字符串
    String8 empty;
    std::ostringstream oss2;
    oss2 << empty;
    CHECK(oss2.str() == "");
}

// ==================== COW (Copy-On-Write) 测试 ====================

TEST_CASE("copy_on_write_behavior", "[string8][cow]") {
    // 注意：根据您的优化后的实现，可能不再使用 COW
    // 这些测试验证独立性
    String8 s1("Hello");
    String8 s2 = s1;

    // 修改 s2 不影响 s1
    s2.append(" World");
    CHECK(std::string("Hello") == s1.c_str());
    CHECK(std::string("Hello World") == s2.c_str());

    String8 s3 = s1;
    s3[0] = 'J';
    CHECK(std::string("Hello") == s1.c_str());
    CHECK(std::string("Jello") == s3.c_str());

    // 超过32字节字符串
    String8 longStr("This is a long string that exceeds thirty-two bytes.");
    String8 longStrCopy = longStr;
    longStrCopy.append(" Additional text.");
    CHECK(std::string("This is a long string that exceeds thirty-two bytes.") == longStr.c_str());
    CHECK(std::string("This is a long string that exceeds thirty-two bytes. Additional text.") == longStrCopy.c_str());
}

// ==================== 边界条件和压力测试 ====================

TEST_CASE("boundary_stack_heap_transition", "[string8][boundary]") {
    // 从栈到堆的转换
    String8 s;
    CHECK(s.isLocal());

    // 添加到恰好填满栈
    for (int i = 0; i < 31; ++i) {
        s.append("a");
    }
    CHECK(s.length() == 31);
    CHECK(s.isLocal());

    // 再添加一个字符，应该转到堆
    s.append("b");
    CHECK(s.length() == 32);
    CHECK(!s.isLocal());
}

TEST_CASE("large_string_operations", "[string8][stress]") {
    // 大字符串操作
    std::string largeStr(10000, 'x');
    String8 s(largeStr.c_str());

    CHECK(s.length() == 10000);
    CHECK(!s.isLocal());

    // 查找
    CHECK(s.find("xxx") == 0);
    CHECK(s.contains("xxxx"));

    // 修改
    s.toLower();
    CHECK(s.length() == 10000);

    // 追加
    s.append("yyy");
    CHECK(s.length() == 10003);

    // 清除
    s.clear();
    CHECK(s.empty());
}

TEST_CASE("repeated_operations", "[string8][stress]") {
    String8 s;

    // 重复追加和清除
    for (int i = 0; i < 100; ++i) {
        s.append("Hello");
        CHECK(s.length() == 5);
        s.clear();
        CHECK(s.empty());
    }

    // 重复格式化
    for (int i = 0; i < 100; ++i) {
        s.appendFormat("%d", i);
    }
    CHECK(!s.empty());
}

TEST_CASE("special_characters", "[string8][special]") {
    // 包含特殊字符
    String8 s1("Hello\tWorld\n");
    CHECK(s1.length() == 12);
    CHECK(s1.contains("\t"));
    CHECK(s1.contains("\n"));

    // 中文字符（UTF-8）
    String8 s2("你好世界");
    CHECK(!s2.empty());
    CHECK(s2.length() > 4);  // UTF-8 编码的中文每个字符占多个字节

    // 特殊 ASCII 字符
    String8 s3("!@#$%^&*()");
    CHECK(s3.length() == 10);

    // 数字和字母混合
    String8 s4("abc123ABC");
    s4.toLower();
    CHECK(std::string("abc123abc") == s4.c_str());
}

TEST_CASE("empty_string_edge_cases", "[string8][edge]") {
    String8 empty;

    // 各种操作在空字符串上
    CHECK(empty.length() == 0);
    CHECK(empty.empty());
    CHECK(empty.c_str()[0] == '\0');

    CHECK(empty.find("test") == -1);
    CHECK(empty.find('a') == -1);
    CHECK(empty.find_last_of("test") == -1);

    CHECK(empty.left(5).empty());
    CHECK(empty.right(5).empty());
    CHECK(empty.substr(0, 5).empty());
    CHECK(empty.reverse().empty());

    CHECK(!empty.contains("test"));
    CHECK(!empty.removeAll("test"));
    CHECK(empty.replaceAll('a', 'b') == 0);

    empty.trim();
    CHECK(empty.empty());

    empty.toLower();
    CHECK(empty.empty());

    empty.toUpper();
    CHECK(empty.empty());

    // 比较
    String8 empty2;
    CHECK(empty == empty2);
    CHECK(empty == "");
}

TEST_CASE("self_operations", "[string8][edge]") {
    // 自追加
    {
        String8 s("AB");
        s.append(s);
        CHECK(std::string("ABAB") == s.c_str());
    }

    // 自赋值
    {
        String8 s("Hello");
        s = s;
        CHECK(std::string("Hello") == s.c_str());
    }

    // 自移动
    {
        String8 s("Hello");
        s = std::move(s);
        CHECK(std::string("Hello") == s.c_str());
    }
}