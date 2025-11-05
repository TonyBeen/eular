/*************************************************************************
    > File Name: file_info.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年10月30日 星期四 17时45分47秒
 ************************************************************************/

#ifndef __FILE_INFO_H__
#define __FILE_INFO_H__

#include <stdint.h>

enum FileTransferType {
    kFileTransferTypeInfo = 1,
    kFileTransferTypeContent = 2,
};

#pragma pack(1)

typedef struct FileInfo {
    uint16_t type;
    uint32_t file_size;
    uint32_t file_hash;
    uint32_t file_name_size;
    char     file_name[0];
} file_info_t;

typedef struct FileContent {
    uint16_t    type;
    int32_t     size;
    int32_t     offset;
    uint8_t     content[0];
} file_content_t;

#pragma pack()

uint32_t encode_file_info(file_info_t *file_info);
file_info_t *decode_file_info(const void *data, uint32_t size);

uint32_t encode_file_content(file_content_t *file_content);
file_content_t *decode_file_content(const void *data, uint32_t size);

#endif // __FILE_INFO_H__
