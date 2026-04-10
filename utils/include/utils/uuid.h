/*************************************************************************
    > File Name: uuid.h
    > Author: eular
    > Brief:
    > Created Time: Fri 10 Apr 2026 10:52:12 AM CST
 ************************************************************************/

#ifndef __UTILS_UUID_H__
#define __UTILS_UUID_H__

#include <array>
#include <string>

#include <utils/platform.h>

namespace eular {
using uuid_t = std::array<uint8_t, 16>;

class UTILS_API UUID
{
public:
    static const uuid_t UUID_NS_DNS;
    static const uuid_t UUID_NS_URL;

    static uuid_t       V5(const uuid_t& ns, const std::string& name);
    static std::string  ToString(const uuid_t& uuid);
};

} // namespace eular

#endif // __UTILS_UUID_H__
