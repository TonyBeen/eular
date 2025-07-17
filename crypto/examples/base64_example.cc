/*************************************************************************
    > File Name: base64_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时36分41秒
 ************************************************************************/

#include <stdio.h>
#include <string.h>
#include <base64.h>

using namespace eular;

void base64_example()
{
    const char *msg = "Hello, World! This is a test string for base64 encoding and decoding.";

    std::string encoded = crypto::Base64::Encrypt(msg, strlen(msg));
    printf("Encoded: %s\n", encoded.c_str());
    std::vector<uint8_t> decodedVec = crypto::Base64::Decrypt(encoded);
    std::string decoded((const char *)decodedVec.data(), decodedVec.size());
    if (decoded == msg) {
        printf("Base64 encoding and decoding successful!\n");
    } else {
        printf("Base64 encoding and decoding failed!\n");
    }
}

int main(int argc, char **argv)
{
    base64_example();
    return 0;
}
