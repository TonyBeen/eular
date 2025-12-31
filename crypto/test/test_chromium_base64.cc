/*************************************************************************
    > File Name: test_chromium_base64.cc
    > Author: hsz
    > Brief:
    > Created Time: Wed 07 Feb 2024 02:36:56 PM CST
 ************************************************************************/

#include "base64_chromium.h"
#include <utils/buffer.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <gtest/gtest.h>

using namespace eular;
using namespace std;

// ./test/testmain --gtest_filter=Chromium_Base64_Test.* 运行指定测试
TEST(Chromium_Base64_Test, test_encode_docode)
{
    const char *file = "test/for_base64_test.txt"; 

    ByteBuffer buf(4096);

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        perror("open error");
        return;
    }

    while (1) {
        uint8_t from[2048] = {0};
        int readSize = read(fd, from, sizeof(from));
        ASSERT_TRUE(readSize >= 0);
        if (readSize == 0) {
            break;
        }

        buf.append(from, readSize);
    }
    close(fd);

    void *pEncodeBuffer = malloc(chromium_base64_encode_len(buf.size()));
    ASSERT_TRUE(pEncodeBuffer != nullptr);

    uint64_t nEncodeLen = chromium_base64_encode(pEncodeBuffer, buf.const_data(), buf.size());
    ASSERT_TRUE(MODP_B64_ERROR != nEncodeLen);
    
    void *pDecodeBuffer = malloc(chromium_base64_decode_len(nEncodeLen));
    ASSERT_TRUE(pDecodeBuffer != nullptr);

    uint64_t nDecodeLen = chromium_base64_decode(pDecodeBuffer, pEncodeBuffer, nEncodeLen);
    ASSERT_TRUE(MODP_B64_ERROR != nDecodeLen);

    free(pEncodeBuffer);
    free(pDecodeBuffer);
}