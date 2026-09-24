/*************************************************************************
    > File Name: ack_info.cpp
    > Author: eular
    > Brief:
    > Created Time: Fri 30 Jan 2026 10:37:32 AM CST
 ************************************************************************/

#include "util/ack_info.h"
#include "ack_info.h"

void eular::utp::AckInfo::reset()
{
    largest_ack_packno = 0;
    ack_delay = 0;
    range_size = 0;
}