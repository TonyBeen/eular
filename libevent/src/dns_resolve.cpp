/*************************************************************************
    > File Name: dns_resolve.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月01日 星期五 18时02分14秒
 ************************************************************************/

#include "event/dns_resolve.h"

#include <string.h>
#include <assert.h>
#include <mutex>
#include <thread>
#include <chrono>
#include <unordered_map>

#include <ares.h>

#include "event/timer.h"

static std::once_flag g_initAres;

namespace ev {
// 必须在堆上
struct AresArg {
    DNSResolver*    self = nullptr;
    std::string     domain;

    void destroy() {
        delete this;
    }

private:
    ~AresArg() { }
};

struct ResoleEventArg {
    EventPoll   eventPoll;
};

class DNSResolverInternal
{
public:
    DNSResolverInternal()
    {
    }

    ~DNSResolverInternal()
    {
        reset();
    }

    void reset()
    {
        if (channel) {
            ares_destroy(channel);
            channel = nullptr;
        }

        manager = nullptr;
        timeout = 0;
        domainServer.clear();
    }

    ares_channel    channel = nullptr;
    std::unordered_map<socket_t, ResoleEventArg> socketPollMap;
    DNSManager*     manager = nullptr;
    uint32_t        timeout = 1000;
    std::string     domainServer;

    struct PendingDoamin {
        DNSResolver::DNSResolveCB   resolveCB;
        DNSResolver::HostType       type;
        EventTimer::SP              timer;
    };
    std::map<std::string, PendingDoamin>  domainMap; // 待解析的域名map
};

class DNSManagerInternal
{
public:
    DNSManagerInternal()
    {
        acquireTimeCB = [] () -> uint64_t {
            std::chrono::steady_clock::time_point tm = std::chrono::steady_clock::now();
            std::chrono::milliseconds mills = 
                std::chrono::duration_cast<std::chrono::milliseconds>(tm.time_since_epoch());
            return static_cast<uint64_t>(mills.count());
        };
    }

    ~DNSManagerInternal()
    {

    }

    EventLoop*                  eventLoop = nullptr;
    uint32_t                    cacheTimeout = 600;
    DNSManager::AcquireTimeCB   acquireTimeCB = nullptr;
    EventTimer                  timer; // 删除过期缓存定时器

    std::map<uint64_t, DNSResolver::SP>             dnsResolverMap; // 待解析map
    std::map<std::string, std::vector<DNSResult>>   domainResolvedMap; // 缓存的域名
};

DNSResolver::DNSResolver(DNSManager *manager)
{
    // NOTE linux下无实际意义
    std::call_once(g_initAres, [] () {
        ares_library_init(ARES_LIB_INIT_ALL);
        ::atexit(ares_library_cleanup);
    });

    m_internal = std::make_shared<DNSResolverInternal>();
    m_internal->manager = manager;
}

DNSResolver::~DNSResolver()
{
    m_internal->reset();
    m_internal.reset();
}

void DNSResolver::setResolveTimeout(uint32_t ms)
{
    m_internal->timeout = ms;
}

bool DNSResolver::setDomainServer(const std::string &host)
{
    if (host.empty() || m_internal->channel == nullptr) {
        return false;
    }

    if (!initChannel()) {
        return false;
    }

    m_internal->domainServer = host;
    if (ARES_SUCCESS != ares_set_servers_csv(m_internal->channel, m_internal->domainServer.c_str())) {
        return false;
    }

    return true;
}

bool DNSResolver::initChannel()
{
    // 初始化 ares channel
    if (nullptr == m_internal->channel) {
        ares_options options;
        memset(&options, 0, sizeof(options));
        options.flags = 0;
        options.sock_state_cb = OnSocketStateChanged;
        options.sock_state_cb_data = this;

        int32_t status = ares_init_options(&m_internal->channel, &options, ARES_OPT_SOCK_STATE_CB);
        if (status != ARES_SUCCESS) {
            return false;
        }
    }

    return true;
}

void DNSResolver::onResolveTimeout(const std::string &domain)
{
    auto it = m_internal->domainMap.find(domain);
    if (it == m_internal->domainMap.end()) {
        return;
    }

    it->second.resolveCB(DNS_TIMEOUT, DNSResultVec{});
    m_internal->domainMap.erase(it);
}

void DNSResolver::OnSocketStateChanged(void *data, socket_t sock, int readable, int writable)
{
    assert(data != nullptr);
    DNSResolver *self = static_cast<DNSResolver *>(data);

    EventLoop *eventLoop = self->m_internal->manager->m_internal->eventLoop;
    EventPoll &eventPoll = self->m_internal->socketPollMap[sock].eventPoll;
    if (eventPoll.hasPending()) {
        eventPoll.stop();
    }

    EventPoll::event_t eventFlag = EventPoll::Event::None;
    if (readable) {
        eventFlag |= EventPoll::Event::Read;
    }

    if (writable) {
        eventFlag |= EventPoll::Event::Write;
    }

    if (eventFlag != EventPoll::Event::None) {
        eventPoll.reset(eventLoop, sock, (EventPoll::Event)eventFlag, [self] (socket_t sockFd, EventPoll::event_t events) {
                socket_t readSock = (events & EventPoll::Event::Read) ? sockFd : ARES_SOCKET_BAD;
                socket_t writeSock = (events & EventPoll::Event::Write) ? sockFd : ARES_SOCKET_BAD;
                ares_process_fd(self->m_internal->channel, readSock, writeSock);
            });
        eventPoll.start();
    }

    // if (readable && !self->m_internal->readEvent.hasPending()) {
    //     self->m_internal->readEvent.reset(self->m_internal->manager->m_internal->eventLoop,
    //         sock, EventPoll::Event::Read, [self] (socket_t sockFd, EventPoll::event_t events) {
    //             socket_t readSock = (events & EventPoll::Event::Read) ? sockFd : ARES_SOCKET_BAD;
    //             socket_t writeSock = (events & EventPoll::Event::Write) ? sockFd : ARES_SOCKET_BAD;
    //             ares_process_fd(self->m_internal->channel, readSock, writeSock);
    //         });
    //     self->m_internal->readEvent.start();
    // } else {
    //     self->m_internal->readEvent.stop();
    // }

    // if (writable && !self->m_internal->writeEvent.hasPending()) {
    //     self->m_internal->writeEvent.reset(self->m_internal->manager->m_internal->eventLoop,
    //         sock, EventPoll::Event::Write, [self] (socket_t sockFd, EventPoll::event_t events) {
    //             socket_t readSock = (events & EventPoll::Event::Read) ? sockFd : ARES_SOCKET_BAD;
    //             socket_t writeSock = (events & EventPoll::Event::Write) ? sockFd : ARES_SOCKET_BAD;
    //             ares_process_fd(self->m_internal->channel, readSock, writeSock);
    //         });
    //     self->m_internal->writeEvent.start();
    // } else {
    //     self->m_internal->writeEvent.stop();
    // }
}

void DNSResolver::OnDnsCallback(void *arg, int status, int timeouts, ares_addrinfo *res)
{
    assert(arg != nullptr);
    AresArg *aresArg = static_cast<AresArg *>(arg);
    std::shared_ptr<void> _clean(nullptr, [aresArg] (void *) {
        aresArg->destroy();
    });
    DNSResolver *self = aresArg->self;
    const std::string &domain = aresArg->domain;
    auto it = self->m_internal->domainMap.find(domain);
    if (it == self->m_internal->domainMap.end()) {
        return;
    }
    DNSResultVec resultVec;
    std::vector<std::string> cnameVec;
    bool found = false;

    uint64_t resolvedTimeMS = self->m_internal->manager->m_internal->acquireTimeCB();
    if (status == ARES_SUCCESS) {
        found = domain == res->name;
        for (ares_addrinfo_cname *iter = res->cnames; iter != nullptr; iter = iter->next) {
            if (domain == iter->alias) {
                found = true;
            }
            cnameVec.push_back(iter->alias);
        }
        cnameVec.push_back(res->name);

        for (ares_addrinfo_node *iter = res->nodes; iter != nullptr; iter = iter->ai_next) {
            DNSResult result;
            char addr[INET6_ADDRSTRLEN];

            if ((it->second.type & HostType::IPv4) && iter->ai_family == AF_INET) {
                sockaddr_in* sa = (sockaddr_in *)iter->ai_addr;
                ares_inet_ntop(AF_INET, &(sa->sin_addr), addr, INET_ADDRSTRLEN);
            } else if ( (it->second.type & HostType::IPv6) && iter->ai_family == AF_INET6) {
                result.isIPv6 = true;
                sockaddr_in6* sa = (sockaddr_in6 *)iter->ai_addr;
                ares_inet_ntop(AF_INET6, &(sa->sin6_addr), addr, INET6_ADDRSTRLEN);
            } else {
                continue;
            }

            result.address = addr;
            result.resolvedTimeMS = resolvedTimeMS;
            result.ttl = iter->ai_ttl;
            resultVec.emplace_back(result);
        }

        ares_freeaddrinfo(res);
    }

    try {
        if (found) {
            it->second.resolveCB(status == ARES_SUCCESS ? DNS_SUCCESS : DNS_FAILED, resultVec);
        } else {
            it->second.resolveCB(DNS_FAILED, DNSResultVec{});
        }
    } catch(...) {
    }

    self->m_internal->domainMap.erase(it);

    for (const auto &domainIt : cnameVec) {
        self->m_internal->manager->addDNSCacheInternal(domainIt, resultVec);
    }
}

bool DNSResolver::resolve(const std::string &domain, DNSResolveCB cb, HostType type)
{
    if (domain.empty() || cb == nullptr) {
        return false;
    }

    // 处于解析中
    if (m_internal->domainMap.find(domain) != m_internal->domainMap.end()) {
        return true;
    }

    // 从缓存查询
    const DNSResultVec *dnsResultVec = m_internal->manager->getDomainCache(domain);
    if (dnsResultVec && !dnsResultVec->empty()) {
        cb(DNS_SUCCESS, *dnsResultVec);
        return true;
    }

    if (!initChannel()) {
        return false;
    }

    ares_addrinfo_hints hint;
    memset(&hint, 0, sizeof(hint));
    switch (type) {
    case HostType::Both :
        hint.ai_family = AF_UNSPEC;
        break;
    case HostType::IPv4:
        hint.ai_family = AF_INET;
        break;
    case HostType::IPv6:
        hint.ai_family = AF_INET6;
        break;
    default:
        return false;
    }

    // m_internal->domainMap.emplace(domain, DNSResolverInternal::PendingDoamin{cb, type});
    auto &pendingDomain = m_internal->domainMap[domain];
    pendingDomain.resolveCB = std::move(cb);
    pendingDomain.type = type;
    auto timer = std::make_shared<EventTimer>();
    pendingDomain.timer = timer;

    AresArg *arg = new AresArg();
    arg->self = this;
    arg->domain = domain;
    ares_getaddrinfo(m_internal->channel, domain.c_str(), NULL, &hint, OnDnsCallback, arg);

    // 添加超时定时器
    timer->reset(m_internal->manager->m_internal->eventLoop, [domain, this] () {
        this->onResolveTimeout(domain);
    });
    return timer->start(m_internal->timeout);
}

DNSManager::DNSManager(EventLoop *loop)
{
    if (loop == nullptr) {
        throw std::runtime_error("invalid param EventLoop");
    }

    m_internal = std::make_shared<DNSManagerInternal>();
    m_internal->eventLoop = loop;
    m_internal->timer.reset(loop, [this] () {
        uint64_t currentTimeMS = m_internal->acquireTimeCB();
        for (auto it = m_internal->domainResolvedMap.begin(); it != m_internal->domainResolvedMap.end(); ) {
            if ((it->second[0].resolvedTimeMS + it->second[0].ttl * 1000) <= currentTimeMS) {
                it = m_internal->domainResolvedMap.erase(it);
            } else {
                ++it;
            }
        }
    });

    m_internal->timer.start(1000, 1000);
}

DNSManager::~DNSManager()
{
}

void DNSManager::setAcquireTimeCB(AcquireTimeCB cb)
{
    if (cb != nullptr) {
        m_internal->acquireTimeCB = cb;
    }
}

void DNSManager::addDNSCache(const std::string &domian, const DNSResultVec &hostVec)
{
    if (domian.empty() || hostVec.empty()) {
        return;
    }

    m_internal->domainResolvedMap[domian] = hostVec;
}

void DNSManager::removeDNSCache(const std::string &domain)
{
    if (!domain.empty()) {
        m_internal->domainResolvedMap.erase(domain);
    }
}

bool DNSManager::hasDoaminCache(const std::string &domain)
{
    auto it = m_internal->domainResolvedMap.find(domain);
    return it != m_internal->domainResolvedMap.end();
}

const DNSResultVec *DNSManager::getDomainCache(const std::string &domain)
{
    auto it = m_internal->domainResolvedMap.find(domain);
    if (it == m_internal->domainResolvedMap.end()) {
        return nullptr;
    }

    return &it->second;
}

void DNSManager::addDNSCacheInternal(const std::string &domian, DNSResultVec hostVec)
{
    if (domian.empty() || hostVec.empty()) {
        return;
    }

    m_internal->domainResolvedMap[domian] = std::move(hostVec);
}

} // namespace ev
