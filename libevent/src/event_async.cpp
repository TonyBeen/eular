/*************************************************************************
    > File Name: event_async.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 14时29分20秒
 ************************************************************************/

#include "event/async.h"
#include <assert.h>
#include <cstring>
#include <errno.h>
#include <stdio.h>
#include <unordered_set>
#include <vector>
#include <event2/event.h>

#define SOCK_PAIR_RECV 0
#define SOCK_PAIR_SEND 1

namespace ev {
namespace {
struct TokenHash {
    size_t operator()(const EventAsync::AsyncToken &token) const noexcept
    {
        return (static_cast<size_t>(token.id) << 32) ^ static_cast<size_t>(token.generation);
    }
};

struct TokenEqual {
    bool operator()(const EventAsync::AsyncToken &lhs, const EventAsync::AsyncToken &rhs) const noexcept
    {
        return lhs.id == rhs.id && lhs.generation == rhs.generation;
    }
};

static inline bool PopOneAsyncToken(
    const std::vector<uint8_t> &buffer, size_t *offset, EventAsync::AsyncToken *outToken)
{
    if (buffer.size() < *offset + sizeof(EventAsync::AsyncToken)) {
        return false;
    }

    std::memcpy(outToken, buffer.data() + *offset, sizeof(EventAsync::AsyncToken));
    *offset += sizeof(EventAsync::AsyncToken);
    return true;
}

static inline bool WouldBlock()
{
#if defined(_WIN32) || defined(_WIN64)
    int32_t code = WSAGetLastError();
    return code == WSAEWOULDBLOCK;
#else
    int32_t code = errno;
    return (code == EAGAIN || code == EWOULDBLOCK);
#endif
}
} // namespace

EventAsync::EventAsync(EventLoop::SP loop)
{
    m_asyncSnapshot = std::make_shared<AsyncMap>();
    reset(loop ? loop->loop() : nullptr);
}

EventAsync::EventAsync(const EventLoop *loop)
{
    m_asyncSnapshot = std::make_shared<AsyncMap>();
    reset(loop ? loop->loop() : nullptr);
}

EventAsync::EventAsync(event_base *base)
{
    m_asyncSnapshot = std::make_shared<AsyncMap>();
    reset(base);
}

EventAsync::~EventAsync()
{
    reset();
}

bool EventAsync::start() noexcept
{
    if (m_event == nullptr) {
        return false;
    }

    m_started = (0 == event_add(m_event, nullptr));
    return m_started;
}

void EventAsync::stop() noexcept
{
    if (m_event == nullptr) {
        m_started = false;
        return;
    }

    event_del(m_event);
    m_started = false;
}

bool EventAsync::addAsync(AsyncId id, AsyncCallback cb, AsyncMode mode)
{
    if (cb == nullptr) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mapMtx);
    std::shared_ptr<AsyncMap> nextMap = std::make_shared<AsyncMap>(*m_asyncSnapshot);
    if (nextMap->find(id) != nextMap->end()) {
        return false;
    }

    AsyncSlot slot;
    slot.mode = mode;
    // 严格隔离模式下给每次注册分配新代次, 避免新回调消费历史通知
    slot.generation = (mode == AsyncMode::StrictIsolation) ? m_nextGeneration++ : 0;
    slot.cb = std::move(cb);
    nextMap->emplace(id, std::move(slot));
    std::atomic_store(&m_asyncSnapshot, std::static_pointer_cast<const AsyncMap>(nextMap));
    return true;
}

void EventAsync::delAsync(AsyncId id)
{
    std::lock_guard<std::mutex> lock(m_mapMtx);
    std::shared_ptr<AsyncMap> nextMap = std::make_shared<AsyncMap>(*m_asyncSnapshot);
    nextMap->erase(id);
    std::atomic_store(&m_asyncSnapshot, std::static_pointer_cast<const AsyncMap>(nextMap));
}

bool EventAsync::notify(AsyncId id) noexcept
{
    if (m_event == nullptr) {
        return false;
    }

    AsyncToken token;
    token.id = id;

    // notify() 只负责投递成功与否, 不校验ID是否存在, 从而避免热路径加锁
    std::shared_ptr<const AsyncMap> snapshot = std::atomic_load(&m_asyncSnapshot);
    if (snapshot) {
        AsyncMap::const_iterator it = snapshot->find(id);
        if (it != snapshot->end() && it->second.mode == AsyncMode::StrictIsolation) {
            token.generation = it->second.generation;
        }
    }

#if defined(MSG_NOSIGNAL)
    static const int kSendFlag = MSG_NOSIGNAL;
#else
    static const int kSendFlag = 0;
#endif

    uint8_t payload[sizeof(AsyncToken)] = {0};
    std::memcpy(payload, &token, sizeof(token));

    const char *p = reinterpret_cast<const char *>(payload);
    size_t left = sizeof(payload);
    while (left > 0) {
        auto sendSize = ::send(m_sockPair[SOCK_PAIR_SEND], p, static_cast<int32_t>(left), kSendFlag);
        if (sendSize > 0) {
            p += sendSize;
            left -= static_cast<size_t>(sendSize);
            continue;
        }

        return false;
    }

    return true;
}

void EventAsync::reset(event_base *loop)
{
    stop();
    if (m_event != nullptr) {
        event_free(m_event);
        m_event = nullptr;
    }

    if (m_sockPair[SOCK_PAIR_RECV] >= 0) {
        evutil_closesocket(m_sockPair[SOCK_PAIR_RECV]);
        evutil_closesocket(m_sockPair[SOCK_PAIR_SEND]);
        m_sockPair[SOCK_PAIR_RECV] = -1;
        m_sockPair[SOCK_PAIR_SEND] = -1;
    }

    if (loop != nullptr) {
        int32_t result = evutil_socketpair(AF_UNIX, SOCK_STREAM, 0, m_sockPair);
        assert(result == 0);
        if (result != 0) {
            return;
        }

        assert(0 == evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_RECV]));
        assert(0 == evutil_make_socket_nonblocking(m_sockPair[SOCK_PAIR_SEND]));

        m_event = event_new(loop, m_sockPair[SOCK_PAIR_RECV], EV_READ | EV_PERSIST, [](evutil_socket_t, short, void *data) {
            auto self = static_cast<EventAsync *>(data);

            // 流式socket没有消息边界，必须保留尾部半包到下一轮继续拼接。
            static thread_local std::vector<uint8_t> recvBuffer;
            if (recvBuffer.capacity() < 1024) {
                recvBuffer.reserve(1024);
            }

            do {
                char buffer[1024];
                auto nRecv = ::recv(self->m_sockPair[SOCK_PAIR_RECV], buffer, sizeof(buffer), 0);
                if (nRecv > 0) {
                    recvBuffer.insert(recvBuffer.end(), buffer, buffer + nRecv);
                } else {
                    if (!WouldBlock()) {
                        perror("recv error");
                    }
                    break;
                }
            } while (true);

            std::shared_ptr<const AsyncMap> snapshot = std::atomic_load(&self->m_asyncSnapshot);
            std::unordered_set<AsyncToken, TokenHash, TokenEqual> seenTokens;
            if (snapshot) {
                seenTokens.reserve(snapshot->size());
            }

            std::vector<AsyncCallback> callbacks;
            std::vector<AsyncId> callbackIds;
            size_t readOffset = 0;
            AsyncToken token;
            while (PopOneAsyncToken(recvBuffer, &readOffset, &token)) {
                // 消费阶段按token去重，同一轮里相同id/generation最多执行一次。
                if (!seenTokens.insert(token).second) {
                    continue;
                }

                if (!snapshot) {
                    continue;
                }

                AsyncMap::const_iterator it = snapshot->find(token.id);
                if (it == snapshot->end()) {
                    continue;
                }

                const AsyncSlot &slot = it->second;
                if (slot.mode == AsyncMode::StrictIsolation && slot.generation != token.generation) {
                    continue;
                }

                if (slot.cb) {
                    callbacks.push_back(slot.cb);
                    callbackIds.push_back(token.id);
                }
            }

            if (readOffset > 0) {
                recvBuffer.erase(
                    recvBuffer.begin(),
                    recvBuffer.begin() + static_cast<std::vector<uint8_t>::difference_type>(readOffset));
            }

            for (size_t i = 0; i < callbacks.size(); ++i) {
                callbacks[i](callbackIds[i]);
            }
        }, this);
    }
}
} // namespace ev
