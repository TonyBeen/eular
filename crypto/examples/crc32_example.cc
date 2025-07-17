/*************************************************************************
    > File Name: crc32_example.cc
    > Author: hsz
    > Brief:
    > Created Time: 2025年07月15日 星期二 11时37分19秒
 ************************************************************************/

#include <stdio.h>
#include <string.h>

#include <crc32.h>

int main(int argc, char **argv)
{
    const char *crc32_msg = "Hello, world!Hello, world!Hello, world!Hello, world!Hello, world!";
    uint64_t crc32_value = 0;
    crc32_value = crc32(crc32_value, (const unsigned char *)crc32_msg, strlen(crc32_msg));
    printf("CRC32 value: 0x%lx\n", crc32_value);
    if (crc32_value != 0x9E85985E) {
        printf("CRC32 value is incorrect!\n");
        return -1;
    }

    return 0;
}
