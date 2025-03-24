/*************************************************************************
    > File Name: kcp_internal.h
    > Author: hsz
    > Brief:
    > Created Time: 2025年03月21日 星期五 10时50分35秒
 ************************************************************************/

#ifndef __KCP_INTERNAL_H__
#define __KCP_INTERNAL_H__

#include "kcp_def.h"
#include "kcp_protocol.h"

EXTERN_C_BEGIN

int32_t kcp_input(kcp_connection_t *conn, const kcp_proto_header_t *header);



EXTERN_C_END

#endif // __KCP_INTERNAL_H__
