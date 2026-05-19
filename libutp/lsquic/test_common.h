#ifndef UTP_LSQUIC_TEST_COMMON_H
#define UTP_LSQUIC_TEST_COMMON_H

#include <sys/queue.h>

struct sport;
TAILQ_HEAD(sport_head, sport);

struct sport {
    int dummy;
};

#endif
