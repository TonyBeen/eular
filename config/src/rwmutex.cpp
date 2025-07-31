/*************************************************************************
    > File Name: rwmutex.cpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 12 Jan 2023 10:23:03 AM CST
 ************************************************************************/

#include "rwmutex.h"
#include <assert.h>
#include <exception>

RWMutex::RWMutex() :
    m_readCount(0),
    m_writeCount(false)
{
}

RWMutex::~RWMutex()
{
    assert(m_readCount == 0 && m_writeCount == false && "unreleased locks exist");
}

// TODO 防止虚假唤醒
void RWMutex::rlock()
{
    std::unique_lock<std::mutex> lock;
    if (m_writeCount) {
        std::unique_lock<std::mutex> temp(m_mutex);
        lock.swap(temp);
        m_cond.wait(lock);
    }
    ++m_readCount;
}

void RWMutex::runlock()
{
    --m_readCount;
    if (m_readCount == 0) {
        m_cond.notify_one();
    }
}

void RWMutex::wlock()
{
    std::unique_lock<std::mutex> lock;
    if (m_readCount > 0 || m_writeCount) {
        std::unique_lock<std::mutex> temp(m_mutex);
        lock.swap(temp);
        m_cond.wait(lock);
    }
    m_writeCount = true;
}

void RWMutex::wunlock()
{
    if (m_writeCount) {
        m_writeCount = false;
        m_cond.notify_all();
    }
}
