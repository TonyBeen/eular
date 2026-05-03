/*************************************************************************
    > File Name: fiu_local.h
    > Brief: Linux-only fault injection compatibility header.
 ************************************************************************/

#ifndef __UTP_UTIL_FIU_LOCAL_H__
#define __UTP_UTIL_FIU_LOCAL_H__

#include <stddef.h>

#include "utp/platform.h"

#if defined(OS_LINUX) && defined(UTP_ENABLE_FAULT_INJECTION)

// FIU_ENABLE activates the real fiu_* symbols in <fiu.h> (otherwise they
// become empty stubs that never initialise the wtable, causing fiu_enable to
// crash with a SIGSEGV).
#ifndef FIU_ENABLE
#define FIU_ENABLE
#endif

#include <fiu.h>
#include <fiu-control.h>

#else

static inline int fiu_init(unsigned int flags)
{
    UNUSED(flags);
    return 0;
}

static inline int fiu_fail(const char *name)
{
    UNUSED(name);
    return 0;
}

static inline void *fiu_failinfo(void)
{
    return NULL;
}

static inline int fiu_enable(const char *name, int failnum, void *failinfo, unsigned int flags)
{
    UNUSED(name);
    UNUSED(failnum);
    UNUSED(failinfo);
    UNUSED(flags);
    return 0;
}

static inline int fiu_disable(const char *name)
{
    UNUSED(name);
    return 0;
}

#define fiu_do_on(name, action) do { UNUSED(name); } while (0)
#define fiu_exit_on(name) do { UNUSED(name); } while (0)
#define fiu_return_on(name, retval) do { UNUSED(name); } while (0)

#endif

#endif // __UTP_UTIL_FIU_LOCAL_H__
