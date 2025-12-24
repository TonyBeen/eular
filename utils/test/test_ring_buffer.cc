/*************************************************************************
    > File Name: test_ring_buffer.cc
    > Author: hsz
    > Brief:
    > Created Time: Wed 24 Dec 2025 04:01:06 PM CST
 ************************************************************************/

#ifndef CATCH_CONFIG_MAIN
#define CATCH_CONFIG_MAIN
#endif

#ifndef CATCH_CONFIG_ENABLE_BENCHMARKING
#define CATCH_CONFIG_ENABLE_BENCHMARKING
#endif

#include <cstring>
#include <string>
#include <vector>

#include "catch/catch.hpp"
#include "utils/ring_buffer.h"

// ==================== 构造函数测试 ====================

TEST_CASE("eular::RingBuffer 构造函数", "[constructor]") {
    
    SECTION("正常容量构造") {
        eular::RingBuffer rb(1024);
        REQUIRE(rb.capacity() == 1024);
        REQUIRE(rb.readableSize() == 0);
        REQUIRE(rb.writableSize() == 1024);
        REQUIRE(rb. isEmpty() == true);
        REQUIRE(rb.isFull() == false);
    }
    
    SECTION("最小容量构造") {
        eular::RingBuffer rb(1);
        REQUIRE(rb.capacity() == 1);
        REQUIRE(rb.writableSize() == 1);
    }
    
    SECTION("大容量构造") {
        eular::RingBuffer rb(1024 * 1024);  // 1MB
        REQUIRE(rb.capacity() == 1024 * 1024);
    }
    
    SECTION("零容量构造应抛出异常") {
        REQUIRE_THROWS_AS(eular::RingBuffer(0), std::invalid_argument);
    }
}

// ==================== 移动语义测试 ====================

TEST_CASE("eular::RingBuffer 移动语义", "[move]") {
    
    SECTION("移动构造") {
        eular::RingBuffer rb1(256);
        const char* data = "test data";
        rb1.write(data, strlen(data));
        
        eular::RingBuffer rb2(std::move(rb1));
        
        REQUIRE(rb2.capacity() == 256);
        REQUIRE(rb2.readableSize() == strlen(data));
        
        char buf[32] = {0};
        rb2.read(buf, sizeof(buf));
        REQUIRE(strcmp(buf, data) == 0);
    }
    
    SECTION("移动赋值") {
        eular::RingBuffer rb1(256);
        const char* data = "move assign test";
        rb1.write(data, strlen(data));
        
        eular::RingBuffer rb2(64);
        rb2 = std::move(rb1);
        
        REQUIRE(rb2.capacity() == 256);
        REQUIRE(rb2.readableSize() == strlen(data));
    }
}

// ==================== write() 测试 ====================

TEST_CASE("eular::RingBuffer write()", "[write]") {
    
    SECTION("写入正常数据") {
        eular::RingBuffer rb(64);
        const char* data = "Hello, World!";
        size_t len = strlen(data);

        size_t written = rb.write(data, len);

        REQUIRE(written == len);
        REQUIRE(rb. readableSize() == len);
        REQUIRE(rb.writableSize() == 64 - len);
    }

    SECTION("写入空数据") {
        eular::RingBuffer rb(64);

        REQUIRE(rb.write(nullptr, 10) == 0);
        REQUIRE(rb.write("test", 0) == 0);
        REQUIRE(rb.readableSize() == 0);
    }

    SECTION("写入超过容量的数据") {
        eular::RingBuffer rb(16);
        char data[32];
        memset(data, 'A', sizeof(data));

        size_t written = rb.write(data, sizeof(data));

        REQUIRE(written == 16);
        REQUIRE(rb.isFull() == true);
        REQUIRE(rb.writableSize() == 0);
    }

    SECTION("缓冲区满时写入") {
        eular::RingBuffer rb(8);
        std::string data = "12345678";
        rb.write(data.c_str(), 8);

        REQUIRE(rb.write("more", 4) == 0);
    }

    SECTION("环形写入（跨越边界）") {
        eular::RingBuffer rb(16);

        // 写入10字节
        rb. write("0123456789", 10);
        // 读取8字节，移动读指针
        char buf[16];
        rb.read(buf, 8);
        // 现在写指针在位置10，读指针在位置8
        // 可写空间:  8 + 6 = 14

        // 写入12字节（会跨越边界）
        size_t written = rb.write("ABCDEFGHIJKL", 12);
        
        REQUIRE(written == 12);
        REQUIRE(rb.readableSize() == 14);  // 2 + 12
    }
}

// ==================== read() 测试 ====================

TEST_CASE("eular::RingBuffer read()", "[read]") {
    
    SECTION("读取正常数据") {
        eular::RingBuffer rb(64);
        const char* data = "Hello, World! ";
        rb.write(data, strlen(data));
        
        char buf[32] = {0};
        size_t read_bytes = rb.read(buf, sizeof(buf));
        
        REQUIRE(read_bytes == strlen(data));
        REQUIRE(strcmp(buf, data) == 0);
        REQUIRE(rb. isEmpty() == true);
    }
    
    SECTION("读取空缓冲区") {
        eular::RingBuffer rb(64);
        char buf[32];
        
        REQUIRE(rb.read(buf, sizeof(buf)) == 0);
    }
    
    SECTION("读取参数无效") {
        eular::RingBuffer rb(64);
        rb.write("test", 4);
        
        REQUIRE(rb.read(nullptr, 10) == 0);
        
        char buf[32];
        REQUIRE(rb.read(buf, 0) == 0);
        
        // 数据应该还在
        REQUIRE(rb.readableSize() == 4);
    }
    
    SECTION("读取部分数据") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[5] = {0};
        size_t read_bytes = rb.read(buf, 5);
        
        REQUIRE(read_bytes == 5);
        REQUIRE(std::string(buf) == "01234");
        REQUIRE(rb. readableSize() == 5);
    }
    
    SECTION("环形读取（跨越边界）") {
        eular::RingBuffer rb(16);
        
        // 填充并部分读取，使数据跨越边界
        rb. write("0123456789", 10);
        char temp[8];
        rb. read(temp, 8);
        rb.write("ABCDEFGHIJKL", 12);
        
        // 现在数据跨越边界：位置8-9存"89"，位置0-11存"ABCDEFGHIJKL"
        char buf[32] = {0};
        size_t read_bytes = rb. read(buf, sizeof(buf));
        
        REQUIRE(read_bytes == 14);
        REQUIRE(std::string(buf) == "89ABCDEFGHIJKL");
    }
}

// ==================== peek() 测试 ====================

TEST_CASE("eular::RingBuffer peek()", "[peek]") {
    
    SECTION("peek不移动读指针") {
        eular::RingBuffer rb(64);
        const char* data = "Hello, World! ";
        rb. write(data, strlen(data));
        
        char buf1[32] = {0};
        char buf2[32] = {0};
        
        size_t peek_bytes = rb. peek(buf1, sizeof(buf1));
        size_t peek_bytes2 = rb.peek(buf2, sizeof(buf2));
        
        REQUIRE(peek_bytes == strlen(data));
        REQUIRE(peek_bytes2 == strlen(data));
        REQUIRE(strcmp(buf1, buf2) == 0);
        REQUIRE(rb. readableSize() == strlen(data));  // 大小不变
    }
    
    SECTION("peek部分数据") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[5] = {0};
        rb.peek(buf, 5);
        
        REQUIRE(std::string(buf) == "01234");
        REQUIRE(rb.readableSize() == 10);
    }
    
    SECTION("peek空缓冲区") {
        eular::RingBuffer rb(64);
        char buf[32];
        
        REQUIRE(rb.peek(buf, sizeof(buf)) == 0);
    }
    
    SECTION("peek无效参数") {
        eular::RingBuffer rb(64);
        rb.write("test", 4);
        
        REQUIRE(rb.peek(nullptr, 10) == 0);
        
        char buf[32];
        REQUIRE(rb.peek(buf, 0) == 0);
    }
}

// ==================== peekAt() 测试 ====================

TEST_CASE("eular::RingBuffer peekAt()", "[peekAt]") {
    
    SECTION("peekAt指定偏移") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[5] = {0};
        size_t peeked = rb. peekAt(buf, 5, 3);  // 从偏移3开始
        
        REQUIRE(peeked == 5);
        REQUIRE(std::string(buf) == "34567");
    }
    
    SECTION("peekAt偏移超出范围") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[5];
        REQUIRE(rb.peekAt(buf, 5, 10) == 0);  // 偏移等于数据长度
        REQUIRE(rb.peekAt(buf, 5, 15) == 0);  // 偏移超过数据长度
    }
    
    SECTION("peekAt部分可用") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[10] = {0};
        size_t peeked = rb.peekAt(buf, 10, 7);  // 从偏移7开始，只有3字节可用
        
        REQUIRE(peeked == 3);
        REQUIRE(std::string(buf, 3) == "789");
    }
    
    SECTION("peekAt无效参数") {
        eular::RingBuffer rb(64);
        rb.write("test", 4);
        
        REQUIRE(rb.peekAt(nullptr, 10, 0) == 0);
        
        char buf[32];
        REQUIRE(rb.peekAt(buf, 0, 0) == 0);
    }
}

// ==================== skip() 测试 ====================

TEST_CASE("eular::RingBuffer skip()", "[skip]") {
    
    SECTION("skip部分数据") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        size_t skipped = rb.skip(5);
        
        REQUIRE(skipped == 5);
        REQUIRE(rb.readableSize() == 5);
        
        char buf[10] = {0};
        rb.read(buf, sizeof(buf));
        REQUIRE(std:: string(buf) == "56789");
    }
    
    SECTION("skip所有数据") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        size_t skipped = rb. skip(10);
        
        REQUIRE(skipped == 10);
        REQUIRE(rb.isEmpty() == true);
    }
    
    SECTION("skip超过可读大小") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        size_t skipped = rb.skip(20);
        
        REQUIRE(skipped == 10);
        REQUIRE(rb.isEmpty() == true);
    }
    
    SECTION("skip更新累计读取计数") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        rb.skip(5);
        
        REQUIRE(rb.totalBytesRead() == 5);
    }
}

// ==================== clear() 和 reset() 测试 ====================

TEST_CASE("eular::RingBuffer clear()", "[clear]") {
    
    SECTION("clear清空数据但保留累计计数") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        rb.read(nullptr, 0);  // 不读取，只是确认
        
        char buf[5];
        rb.read(buf, 5);
        
        uint64_t written_before = rb.totalBytesWritten();
        uint64_t read_before = rb. totalBytesRead();
        
        rb.clear();
        
        REQUIRE(rb.isEmpty() == true);
        REQUIRE(rb.readableSize() == 0);
        REQUIRE(rb.writableSize() == 64);
        REQUIRE(rb.totalBytesWritten() == written_before);  // 保留
        REQUIRE(rb.totalBytesRead() == read_before);        // 保留
    }
}

TEST_CASE("eular::RingBuffer reset()", "[reset]") {
    
    SECTION("reset完全重置") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[5];
        rb.read(buf, 5);
        
        rb.reset();
        
        REQUIRE(rb.isEmpty() == true);
        REQUIRE(rb.totalBytesWritten() == 0);
        REQUIRE(rb.totalBytesRead() == 0);
    }
}

// ==================== 容量和状态查询测试 ====================

TEST_CASE("eular::RingBuffer 容量查询", "[capacity]") {
    
    SECTION("capacity()") {
        eular::RingBuffer rb(512);
        REQUIRE(rb.capacity() == 512);
        
        // 读写操作不影响容量
        rb.write("test", 4);
        REQUIRE(rb.capacity() == 512);
    }
    
    SECTION("readableSize() 和 writableSize()") {
        eular::RingBuffer rb(100);
        
        REQUIRE(rb.readableSize() == 0);
        REQUIRE(rb.writableSize() == 100);
        
        rb.write("0123456789", 10);
        REQUIRE(rb.readableSize() == 10);
        REQUIRE(rb.writableSize() == 90);
        
        char buf[5];
        rb.read(buf, 5);
        REQUIRE(rb.readableSize() == 5);
        REQUIRE(rb.writableSize() == 95);
    }
    
    SECTION("isEmpty()") {
        eular::RingBuffer rb(64);
        
        REQUIRE(rb.isEmpty() == true);
        
        rb.write("x", 1);
        REQUIRE(rb.isEmpty() == false);
        
        char c;
        rb.read(&c, 1);
        REQUIRE(rb. isEmpty() == true);
    }
    
    SECTION("isFull()") {
        eular::RingBuffer rb(8);
        
        REQUIRE(rb. isFull() == false);
        
        rb.write("12345678", 8);
        REQUIRE(rb.isFull() == true);
        
        char c;
        rb.read(&c, 1);
        REQUIRE(rb. isFull() == false);
    }
}

// ==================== 累计统计测试 ====================

TEST_CASE("eular::RingBuffer 累计统计", "[statistics]") {
    
    SECTION("totalBytesWritten()") {
        eular::RingBuffer rb(16);
        
        REQUIRE(rb.totalBytesWritten() == 0);
        
        rb.write("12345", 5);
        REQUIRE(rb. totalBytesWritten() == 5);
        
        rb.write("67890", 5);
        REQUIRE(rb.totalBytesWritten() == 10);
    }
    
    SECTION("totalBytesRead()") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        REQUIRE(rb. totalBytesRead() == 0);
        
        char buf[5];
        rb.read(buf, 5);
        REQUIRE(rb.totalBytesRead() == 5);
        
        rb.read(buf, 5);
        REQUIRE(rb.totalBytesRead() == 10);
    }
    
    SECTION("writeOffset() 和 readOffset()") {
        eular::RingBuffer rb(64);
        
        REQUIRE(rb.writeOffset() == 0);
        REQUIRE(rb.readOffset() == 0);
        
        rb.write("0123456789", 10);
        REQUIRE(rb.writeOffset() == 10);
        REQUIRE(rb.readOffset() == 0);
        
        char buf[5];
        rb.read(buf, 5);
        REQUIRE(rb.writeOffset() == 10);
        REQUIRE(rb.readOffset() == 5);
    }
    
    SECTION("pendingDataOffset()") {
        eular::RingBuffer rb(64);
        rb.write("0123456789", 10);
        
        char buf[3];
        rb.read(buf, 3);
        
        REQUIRE(rb.pendingDataOffset() == 3);
    }
    
    SECTION("累计统计超过容量") {
        eular::RingBuffer rb(16);
        
        // 多次写入读取，总量超过容量
        for (int i = 0; i < 100; i++) {
            rb.write("ABCD", 4);
            char buf[4];
            rb.read(buf, 4);
        }
        
        REQUIRE(rb. totalBytesWritten() == 400);
        REQUIRE(rb. totalBytesRead() == 400);
        REQUIRE(rb. isEmpty() == true);
    }
}

// ==================== 零拷贝操作测试 ====================

TEST_CASE("eular::RingBuffer 零拷贝操作", "[zerocopy]") {
    
    SECTION("getWritableRegion() 和 commitWrite()") {
        eular::RingBuffer rb(64);
        
        uint8_t* ptr;
        size_t len;
        bool has_more = rb.getWritableRegion(ptr, len);
        
        REQUIRE(ptr != nullptr);
        REQUIRE(len == 64);
        REQUIRE(has_more == false);
        
        // 写入数据
        memcpy(ptr, "Hello", 5);
        rb.commitWrite(5);
        
        REQUIRE(rb. readableSize() == 5);
        REQUIRE(rb.totalBytesWritten() == 5);
    }
    
    SECTION("getReadableRegion() 和 commitRead()") {
        eular::RingBuffer rb(64);
        rb.write("Hello, World!", 13);
        
        const uint8_t* ptr;
        size_t len;
        bool has_more = rb.getReadableRegion(ptr, len);
        
        REQUIRE(ptr != nullptr);
        REQUIRE(len == 13);
        REQUIRE(has_more == false);
        REQUIRE(std::string((char*)ptr, len) == "Hello, World!");
        
        rb.commitRead(13);
        REQUIRE(rb.isEmpty() == true);
        REQUIRE(rb. totalBytesRead() == 13);
    }
    
    SECTION("零拷贝空缓冲区") {
        eular::RingBuffer rb(64);
        
        const uint8_t* read_ptr;
        size_t read_len;
        rb.getReadableRegion(read_ptr, read_len);
        
        REQUIRE(read_ptr == nullptr);
        REQUIRE(read_len == 0);
    }
    
    SECTION("零拷贝满缓冲区") {
        eular::RingBuffer rb(8);
        rb.write("12345678", 8);

        uint8_t* write_ptr;
        size_t write_len;
        rb.getWritableRegion(write_ptr, write_len);

        REQUIRE(write_ptr == nullptr);
        REQUIRE(write_len == 0);
    }
    
    SECTION("零拷贝跨边界") {
        eular::RingBuffer rb(16);

        // 制造跨边界场景
        rb.write("0123456789AB", 12);
        char temp[10];
        rb.read(temp, 10);
        rb.write("CDEFGHIJKLMN", 12);

        // 数据跨越边界
        const uint8_t* ptr;
        size_t len;
        bool has_more = rb.getReadableRegion(ptr, len);

        REQUIRE(has_more == true); // 有更多数据在开头
        REQUIRE(len == 6); // 第一段：位置 10 - 15
    }
}

// ==================== 边界条件测试 ====================

TEST_CASE("eular::RingBuffer 边界条件", "[boundary]") {
    
    SECTION("单字节操作") {
        eular::RingBuffer rb(1);
        
        char c = 'X';
        REQUIRE(rb.write(&c, 1) == 1);
        REQUIRE(rb.isFull() == true);
        
        char r;
        REQUIRE(rb.read(&r, 1) == 1);
        REQUIRE(r == 'X');
        REQUIRE(rb. isEmpty() == true);
    }
    
    SECTION("完全填满再完全清空循环") {
        eular::RingBuffer rb(8);
        
        for (int cycle = 0; cycle < 10; cycle++) {
            char data[8];
            memset(data, 'A' + cycle, 8);
            
            REQUIRE(rb.write(data, 8) == 8);
            REQUIRE(rb.isFull() == true);
            
            char buf[8];
            REQUIRE(rb. read(buf, 8) == 8);
            REQUIRE(rb.isEmpty() == true);
            
            for (int i = 0; i < 8; i++) {
                REQUIRE(buf[i] == 'A' + cycle);
            }
        }
        
        REQUIRE(rb.totalBytesWritten() == 80);
        REQUIRE(rb.totalBytesRead() == 80);
    }
    
    SECTION("读写指针在各个位置") {
        eular::RingBuffer rb(8);

        // 测试读写指针在不同位置时的行为
        for (size_t offset = 0; offset < 8; offset++) {
            rb.clear();

            // 移动指针到特定位置
            if (offset > 0) {
                char temp[8];
                memset(temp, 'x', offset);
                rb.write(temp, offset);
                rb.read(temp, offset);
            }

            // 现在读写指针都在offset位置
            char data[] = "TEST";
            REQUIRE(rb.write(data, 4) == 4);

            char buf[4] = {0};
            REQUIRE(rb.read(buf, 4) == 4);
            REQUIRE(std::string(buf, 4) == "TEST");
        }
    }
}

// ==================== 压力测试 ====================

TEST_CASE("eular::RingBuffer 压力测试", "[stress]") {
    
    SECTION("大量小数据读写") {
        eular::RingBuffer rb(1024);
        
        const int iterations = 10000;
        uint64_t expected_total = 0;
        
        for (int i = 0; i < iterations; i++) {
            char data[10];
            snprintf(data, sizeof(data), "%d", i);
            size_t len = strlen(data);
            
            rb.write(data, len);
            expected_total += len;
            
            // 保持缓冲区不会满
            if (rb.readableSize() > 512) {
                char buf[256];
                rb. read(buf, 256);
            }
        }
        
        // 清空剩余数据
        while (!rb.isEmpty()) {
            char buf[64];
            rb. read(buf, sizeof(buf));
        }
        
        REQUIRE(rb.totalBytesWritten() == expected_total);
        REQUIRE(rb. totalBytesRead() == expected_total);
    }
    
    SECTION("大数据块读写") {
        eular::RingBuffer rb(64 * 1024);  // 64KB
        
        std::vector<uint8_t> write_data(32 * 1024);  // 32KB
        for (size_t i = 0; i < write_data.size(); i++) {
            write_data[i] = static_cast<uint8_t>(i & 0xFF);
        }
        
        // 写入
        size_t written = rb.write(write_data.data(), write_data.size());
        REQUIRE(written == write_data.size());
        
        // 读取
        std::vector<uint8_t> read_data(32 * 1024);
        size_t read_bytes = rb.read(read_data.data(), read_data.size());
        REQUIRE(read_bytes == write_data.size());
        
        // 验证数据完整性
        REQUIRE(write_data == read_data);
    }
}

// ==================== 数据完整性测试 ====================

TEST_CASE("eular::RingBuffer 数据完整性", "[integrity]") {
    
    SECTION("跨边界数据完整性") {
        eular::RingBuffer rb(32);
        
        // 创建一个跨边界的场景
        char padding[20];
        memset(padding, 'P', 20);
        rb.write(padding, 20);
        
        char temp[15];
        rb. read(temp, 15);  // 读指针现在在15
        
        // 写入跨边界数据
        const char* test_data = "0123456789ABCDEF";  // 16字节
        rb.write(test_data, 16);
        
        // 读取并验证
        rb.skip(5);  // 跳过剩余的padding
        
        char result[17] = {0};
        rb.read(result, 16);
        
        REQUIRE(std::string(result) == std::string(test_data));
    }
}