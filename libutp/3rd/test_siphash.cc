/*************************************************************************
    > File Name: test_siphash.cc
    > Author: eular
    > Brief:
    > Created Time: Tue 10 Feb 2026 05:02:00 PM CST
 ************************************************************************/

#include <stdio.h>

#include "siphash.h"

int main(int argc, char **argv)
{
    uint8_t key[16] = {0};
    uint8_t data[15] = {0};
    uint64_t hash = siphash(data, 15, key);
    printf("siphash: %016" PRIx64 "\n", hash);
    return 0;
}
