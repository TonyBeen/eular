/*************************************************************************
    > File Name: test_aes.cc
    > Author: hsz
    > Brief:
    > Created Time: Mon 27 Dec 2021 01:35:15 PM CST
 ************************************************************************/

#include "aes_openssl.h"
#include <gtest/gtest.h>

using namespace eular;

const char *something = "唧唧复唧唧，木兰当户织。不闻机杼声，唯闻女叹息。"
                        "问女何所思，问女何所忆。女亦无所思，女亦无所忆。"
                        "昨夜见军帖，可汗大点兵，军书十二卷，卷卷有爷名。"
                        "阿爷无大儿，木兰无长兄，愿为市鞍马，从此替爷征。"
                        "东市买骏马，西市买鞍鞯，南市买辔头，北市买长鞭。"
                        "旦辞爷娘去，暮宿黄河边，不闻爷娘唤女声，但闻黄河流水鸣溅溅。"
                        "旦辞黄河去，暮至黑山头，不闻爷娘唤女声，但闻燕山胡骑鸣啾啾。"
                        "万里赴戎机，关山度若飞。朔气传金柝，寒光照铁衣。"
                        "将军百战死，壮士十年归。归来见天子，天子坐明堂。"
                        "策勋十二转，赏赐百千强。可汗问所欲，木兰不用尚书郎，愿驰千里足，送儿还故乡。"
                        "爷娘闻女来，出郭相扶将；阿姊闻妹来，当户理红妆；小弟闻姊来，磨刀霍霍向猪羊。"
                        "开我东阁门，坐我西阁床，脱我战时袍，著我旧时裳。当窗理云鬓，对镜帖花黄。"
                        "出门看火伴，火伴皆惊忙：同行十二年，不知木兰是女郎。"
                        "雄兔脚扑朔，雌兔眼迷离；双兔傍地走，安能辨我是雄雌？";

TEST(AesTest, test_aes_encode_decode) {
    uint8_t userKey[16] = {
        '1', '2', '3', '4', '5', '6',
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    Aes aes(userKey, sizeof(userKey), Aes::KeyType::AES128, Aes::EncodeType::AESCBC);

    const uint32_t size = 4096;

    uint8_t out[size] = {0};
    uint8_t tmp[size] = {0};

    int encodeSize = aes.encode(out, (const uint8_t *)something, strlen(something));
    EXPECT_TRUE(encodeSize > 0);

    int decodeSize = aes.decode(tmp, out, encodeSize);
    EXPECT_TRUE(decodeSize == (int32_t)strlen(something));
    EXPECT_TRUE(memcmp(something, tmp, decodeSize) == 0);
}

TEST(AesTest, test_aes_wrong_key) {
    uint8_t userKey[16] = {
        '1', '2', '3', '4', '5', '6',
        '1', '2', '3', '4', '5', '6', '7', '8', '9', '0'};
    Aes aes(userKey, sizeof(userKey), Aes::KeyType::AES128, Aes::EncodeType::AESCBC);

    const uint32_t size = 4096;

    uint8_t out[size] = {0};
    uint8_t tmp[size] = {0};

    int encodeSize = aes.encode(out, (const uint8_t *)something, strlen(something));
    EXPECT_TRUE(encodeSize > 0);

    userKey[0] = '0';
    aes.setKey(userKey, sizeof(userKey));

    // 使用错误的key不会报错, 但是得到的结果与加密的数据不一致
    int decodeSize = aes.decode(tmp, out, encodeSize);
    EXPECT_FALSE(decodeSize == (int32_t)strlen(something));
    EXPECT_FALSE(memcmp(something, tmp, decodeSize) == 0);
}