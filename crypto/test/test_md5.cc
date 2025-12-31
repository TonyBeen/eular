/*************************************************************************
    > File Name: test_md5.cc
    > Author: eular
    > Brief: 测试md5摘要
    > Created Time: Tue 28 Dec 2021 09:50:21 AM CST
 ************************************************************************/

#include <utils/buffer.h>
#include <utils/string8.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <iostream>
#include <log/log.h>
#include <gtest/gtest.h>

#include "md5_openssl.h"

using namespace eular;
using namespace std;

#define LOG_TAG "Test Md5"

#define MD5_RESULT  "D4F79B1AE00410C123EFD3D4B075BBED"

const char *gFileName = "test/for_md5_test.jpg";

TEST(Md5_Test, test_openssl_encode) {
    Md5 md5;
    uint8_t out[Md5::MD5_BUF_SIZE] = {0};
    const uint8_t *from = nullptr;
    uint32_t fromLen = 0;

    FILE *fp = fopen(gFileName, "r");
    if (fp == nullptr) {
        perror("open for_md5_test.jpg error");
    }
    ASSERT_NE(fp, nullptr);

    struct stat st;
    stat(gFileName, &st);
    char timeBuf[128] = {0};
    strftime(timeBuf, 128, "%Y-%m-%d %H:%M:%S", localtime(&st.st_ctim.tv_sec));

    ByteBuffer buf(st.st_size);
    int readSize = 0;
    uint8_t tmp[1024] = {0};
    while (!feof(fp)) {
        readSize = fread(tmp, 1, sizeof(tmp), fp);
        ASSERT_TRUE(readSize > 0);
        buf.append(tmp, readSize);
    }

    from = buf.const_data();
    fromLen = buf.size();
    
    ASSERT_EQ(st.st_size, fromLen);
    
    md5.encode(out, from, fromLen);

    String8 format(Md5::MD5_BUF_SIZE * 2);
    for (int i = 0; i < Md5::MD5_BUF_SIZE; ++i) {
        format.appendFormat("%02x", out[i]);
    }

    format.toUpper();
    EXPECT_TRUE(format == MD5_RESULT);
}
