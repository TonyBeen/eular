/*************************************************************************
    > File Name: utp.h
    > Author: hsz
    > Brief:
    > Created Time: Tue 23 Dec 2025 05:14:55 PM CST
 ************************************************************************/

#ifndef __UTP_UTP_H__
#define __UTP_UTP_H__

#include <string>
#include <memory>
#include <functional>

#include <event2/event.h>

#include <utp/platform.h>

namespace eular {
namespace utp {
class Context {
public:
    Context();
    ~Context();

public:
    static std::string version();
    static int32_t 

    bool initialize();
    void shutdown();
};

} // namespace utp
} // namespace eular

#endif // __UTP_UTP_H__
