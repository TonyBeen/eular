/*************************************************************************
    > File Name: sha_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时36分14秒
 ************************************************************************/

#include <stdio.h>
#include <string.h>

#include <sha.h>

using namespace eular;

int main(int argc, char **argv)
{
    std::string message = "Hello, world!";
    std::string hash_1 = crypto::SHA::Hash(crypto::SHA::SHA_1, message);
    printf("SHA-1 hash of '%s': %s\n", message.c_str(), hash_1.c_str());
    std::string hash_256 = crypto::SHA::Hash(crypto::SHA::SHA_256, message);
    printf("SHA-256 hash of '%s': %s\n", message.c_str(), hash_256.c_str());
    std::string hash_512 = crypto::SHA::Hash(crypto::SHA::SHA_512, message);
    printf("SHA-512 hash of '%s': %s\n", message.c_str(), hash_512.c_str());

    return 0;
}
