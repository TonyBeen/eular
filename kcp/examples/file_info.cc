/*************************************************************************
    > File Name: file_info.c
    > Author: hsz
    > Brief:
    > Created Time: 2025年10月31日 星期五 15时37分04秒
 ************************************************************************/

#include "file_info.h"

#include <stdlib.h>
#include <utils/endian.hpp>

uint32_t encode_file_info(file_info_t *file_info)
{
    if (file_info == NULL) {
        return 0;
    }
    uint32_t total_size = sizeof(file_info_t) + file_info->file_name_size;
#ifdef __linux__
    file_info->file_size = htobe32(file_info->file_size);
    file_info->file_hash = htobe32(file_info->file_hash);
    file_info->file_name_size = htobe32(file_info->file_name_size);
#endif

    return total_size;
}

file_info_t *decode_file_info(const void *data, uint32_t size)
{
    if (data == NULL || size < sizeof(file_info_t)) {
        return NULL;
    }
    file_info_t *file_info = (file_info_t *)data;
#ifdef __linux__
    file_info->file_size = be32toh(file_info->file_size);
    file_info->file_hash = be32toh(file_info->file_hash);
    file_info->file_name_size = be32toh(file_info->file_name_size);
#endif
    if (size != (sizeof(file_info_t) + file_info->file_name_size)) {
        return NULL;
    }

    return file_info;
}

uint32_t encode_file_content(file_content_t *file_content)
{
    if (file_content == NULL) {
        return 0;
    }

    uint32_t total_size = sizeof(file_content_t) + file_content->size;
#ifdef __linux__
    file_content->size = htobe32(file_content->size);
    file_content->offset = htobe32(file_content->offset);
#endif

    return total_size;
}

file_content_t *decode_file_content(const void *data, uint32_t size)
{
    if (data == NULL || size < sizeof(file_content_t)) {
        return NULL;
    }
    file_content_t *file_content = (file_content_t *)data;
#ifdef __linux__
    file_content->size = be32toh(file_content->size);
    file_content->offset = be32toh(file_content->offset);
#endif
    if (size != (sizeof(file_content_t) + file_content->size)) {
        return NULL;
    }

    return file_content;
}
