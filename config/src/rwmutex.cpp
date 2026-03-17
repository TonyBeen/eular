/*************************************************************************
    > File Name: rwmutex.cpp
    > Author: hsz
    > Brief:
    > Created Time: Thu 12 Jan 2023 10:23:03 AM CST
 ************************************************************************/

#include "internal/rwmutex.h"
#include <assert.h>

RWMutex::RWMutex() :
    m_readCount(0),
    m_waitingWriters(0),
    m_writeCount(false)
{
}

RWMutex::~RWMutex()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    assert(m_readCount == 0 && m_writeCount == false && "unreleased locks exist");
}

void RWMutex::rlock()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cond.wait(lock, [this]() {
        return !m_writeCount && m_waitingWriters == 0;
    });

    ++m_readCount;
}

void RWMutex::runlock()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    assert(m_readCount > 0 && "runlock() called without rlock()");

    --m_readCount;
    if (m_readCount == 0) {
        lock.unlock();
        m_cond.notify_all();
    }
}

void RWMutex::wlock()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    ++m_waitingWriters;
    m_cond.wait(lock, [this]() {
        return m_readCount == 0 && !m_writeCount;
    });

    --m_waitingWriters;
    m_writeCount = true;
}

void RWMutex::wunlock()
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_writeCount) {
        m_writeCount = false;
        lock.unlock();
        m_cond.notify_all();
    }
}
