/*************************************************************************
    > File Name: md5_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时36分28秒
 ************************************************************************/

#include <stdio.h>
#include <string.h>

#include <md5.h>

using namespace eular;

int main(int argc, char **argv)
{
    char data[1024];
    std::string md5_hash = crypto::MD5::Hash(data, sizeof(data));
    printf("MD5 hash of data: %s\n", md5_hash.c_str());
    return 0;
}
