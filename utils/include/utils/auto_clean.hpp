/*************************************************************************
    > File Name: auto_clean.hpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年07月01日 星期一 14时38分07秒
 ************************************************************************/

#ifndef __EULAR_UTILS_AUTO_CLEAN_H__
#define __EULAR_UTILS_AUTO_CLEAN_H__

#include <functional>

namespace eular {
class AutoClean
{
    using Callback = std::function<void(void)>;

public:
    AutoClean(Callback cleanCb) : m_cleanCb(cleanCb) { }
    ~AutoClean()
    {
        if (m_cleanCb)
        {
            m_cleanCb();
        }
    }

private:
    Callback m_cleanCb;
};

} // namespace eular

#endif // __EULAR_UTILS_AUTO_CLEAN_H__
