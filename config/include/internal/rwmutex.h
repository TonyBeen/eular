/*************************************************************************
    > File Name: rwmutex.h
    > Author: hsz
    > Brief: config内部使用
    > Created Time: Thu 12 Jan 2023 10:22:59 AM CST
 ************************************************************************/

#ifndef __CONFIG_RWMUTEX_H__
#define __CONFIG_RWMUTEX_H__

#include <mutex>
#include <condition_variable>
#include <atomic>

class RWMutex
{
public:
    RWMutex();
    ~RWMutex();

    void rlock();
    void runlock();
    void wlock();
    void wunlock();

private:
    std::atomic<uint32_t>   m_readCount;
    std::atomic<bool>       m_writeCount;
    std::mutex              m_mutex;
    std::condition_variable m_cond;
};

#endif