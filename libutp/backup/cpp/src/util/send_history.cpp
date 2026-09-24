/*************************************************************************
    > File Name: send_history.cpp
    > Author: eular
    > Brief:
    > Created Time: Thu 12 Feb 2026 11:26:25 AM CST
 ************************************************************************/

#include "util/send_history.h"
#include "logger/logger.h"

void eular::utp::SendHistory::update(utp_packno_t packno)
{
    if (_last_sent != packno - 1) {
        if (!(_flags & (SH_WARNED | SH_GAP_OK)) && packno > _warn_thresh) {
            UTP_LOGW("gap detected, last sent: %" PRIu64 ", current: %" PRIu64, _last_sent, packno);
            _flags |= SH_WARNED;
        }
    }

    if (packno > _last_sent) {
        _last_sent = packno;
    }
}
