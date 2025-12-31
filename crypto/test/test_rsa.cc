/*************************************************************************
    > File Name: test_rsa.cc
    > Author: hsz
    > Brief:
    > Created Time: Fri 24 Dec 2021 11:25:20 AM CST
 ************************************************************************/

#include "rsa_openssl.h"
#include <gtest/gtest.h>
#include <stdio.h>
#include "utils/errors.h"

using namespace eular;

static const char *publicKeyFile = "public.pem";
static const char *privateKeyFile = "private.pem";

static const String8 encryptedContent = "This is an encrypted section of content";

class GtestRsa : public testing::Test
{
public:
    void SetUp() override
    {
        int32_t nRet = Status::OK;

        nRet = Rsa::GenerateKey(publicKeyFile, privateKeyFile, 512);
        ASSERT_TRUE(Status::OK == nRet);
        // nRet = Rsa::GenerateKey(publicKeyFile, privateKeyFile, 1024);
        // ASSERT_TRUE(Status::OK == nRet);
        // nRet = Rsa::GenerateKey(publicKeyFile, privateKeyFile, 4096);
        // ASSERT_TRUE(Status::OK == nRet);
        // nRet = Rsa::GenerateKey(publicKeyFile, privateKeyFile, 8012);
        // ASSERT_TRUE(Status::OK == nRet);

        pRsa = new (std::nothrow) Rsa(publicKeyFile, privateKeyFile);
        ASSERT_TRUE(pRsa != nullptr);
    }

    void TearDown() override
    {
        delete pRsa;

        remove(publicKeyFile);
        remove(privateKeyFile);
    }

    Rsa *pRsa;
};

// 测试公钥加密，私钥解密
TEST_F(GtestRsa, test_publicEncode_privateDecode) {
    const int32_t bufSize = 1024;
    const uint8_t *from = (const uint8_t *)(encryptedContent.c_str());
    size_t fromLen = encryptedContent.length();

    unsigned char out[bufSize] = {0};
    unsigned char tmp[bufSize] = {0};

    // 公钥加密
    int32_t encodeLen = pRsa->publicEncode(tmp, from, fromLen);
    ASSERT_TRUE(encodeLen > 0);
    
    // 获取解密数据后的长度, 保证解密后的数据不会溢出
    EXPECT_TRUE(pRsa->getDecodeSpaceByDataLen(encodeLen, true) < sizeof(out));

    // 私钥解密
    int32_t decodeLen = pRsa->privateDecode(out, tmp, encodeLen);
    ASSERT_TRUE(decodeLen > 0);
    EXPECT_TRUE(encryptedContent == (char *)out);
}

// 测试私钥加密，公钥解密
TEST_F(GtestRsa, test_privateEncode_publicDecode) {
    const int32_t bufSize = 1024;
    const uint8_t *from = (const uint8_t *)(encryptedContent.c_str());
    size_t fromLen = encryptedContent.length();

    unsigned char out[bufSize] = {0};
    unsigned char tmp[bufSize] = {0};

    // 私钥加密
    int32_t encodeSize = pRsa->privateEncode(tmp, from, fromLen);
    ASSERT_TRUE(encodeSize > 0);

    // 获取解密数据后的长度, 保证解密后的数据不会溢出
    EXPECT_TRUE(pRsa->getDecodeSpaceByDataLen(encodeSize, false) < sizeof(out));

    // 公钥解密
    int32_t decodeSize = pRsa->publicDecode(out, tmp, encodeSize);
    ASSERT_TRUE(decodeSize > 0);
    EXPECT_TRUE(encryptedContent == (char *)out);
}