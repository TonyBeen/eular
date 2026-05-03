#include <catch2/catch.hpp>

#include "utp/errno.h"
#include "utp/platform.h"
#include "socket/mmsg.h"
#include "util/fiu_local.h"

#if defined(OS_LINUX) && defined(USE_SENDMMSG) && defined(UTP_ENABLE_FAULT_INJECTION)

TEST_CASE("Fault injection: mmsg malloc failure maps to no-memory", "[FaultInjection][Mmsg]")
{
    REQUIRE(fiu_init(0) == 0);
    REQUIRE(fiu_enable("mem/mmsg/malloc", 1, NULL, 0) == 0);

    eular::utp::MultipleMsg msg(2, 1200);
    REQUIRE_FALSE(msg.valid());
    REQUIRE(utp_get_last_error() == UTP_ERR_NO_MEMORY);

    REQUIRE(fiu_disable("mem/mmsg/malloc") == 0);
}

#else

TEST_CASE("Fault injection: disabled build is a no-op", "[FaultInjection]")
{
    SUCCEED();
}

#endif
