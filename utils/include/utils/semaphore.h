/*************************************************************************
    > File Name: semaphore.h
    > Author: hsz
    > Desc: for semaphore
    > Created Time: 2026年04月24日 星期五
 ************************************************************************/

#ifndef __UTILS_SEMAPHORE_H__
#define __UTILS_SEMAPHORE_H__

#include <stdint.h>

#include <memory>

#include <utils/string8.h>

namespace eular {
class Semaphore final {
public:
    Semaphore(const char *semPath, uint8_t val);      // 此种走有名信号量，macOS 下不支持
    Semaphore(uint8_t valBase);                       // 此种走无名信号量
    Semaphore(const Semaphore &) = delete;
    Semaphore &operator=(const Semaphore &) = delete;
    ~Semaphore();

    bool post();
    bool wait();
    bool trywait();
    bool timedwait(uint32_t ms);

private:
    struct SemaphoreImpl;
    std::unique_ptr<SemaphoreImpl> mImpl;

    String8 mFilePath;  // 有名信号量使用
    bool    isNamedSemaphore;
};

} // namespace eular

#endif // __UTILS_SEMAPHORE_H__
