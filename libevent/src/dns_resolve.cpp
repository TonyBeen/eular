/*************************************************************************
    > File Name: dns_resolve.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月01日 星期五 18时02分14秒
 ************************************************************************/

#include "event/dns_resolve.h"

#include <string.h>
#include <assert.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>

#if EVENT_WRAPPER_USE_CARES
#include <ares.h>
#else
#include <event2/dns.h>
#include <event2/util.h>
#endif

#include "event/timer.h"

#if EVENT_WRAPPER_USE_CARES
static std::once_flag g_initAres;
#endif

namespace ev {

std::string DNSResolver::Trim(const std::string &value)
{
    auto begin = std::find_if_not(value.begin(), value.end(), [] (unsigned char ch) { return std::isspace(ch); });
    auto end = std::find_if_not(value.rbegin(), value.rend(), [] (unsigned char ch) { return std::isspace(ch); }).base();
    if (begin >= end) {
        return std::string();
    }

    return std::string(begin, end);
}

#if !EVENT_WRAPPER_USE_CARES
bool DNSResolver::ApplyNameServers(evdns_base *dnsBase, const std::string &domainServer)
{
    if (dnsBase == nullptr || domainServer.empty()) {
        return false;
    }

    if (evdns_base_clear_nameservers_and_suspend(dnsBase) != 0) {
        return false;
    }

    bool ok = false;
    size_t start = 0;
    while (start < domainServer.size()) {
        size_t commaPos = domainServer.find(',', start);
        std::string server = domainServer.substr(start, commaPos - start);
        server = Trim(server);
        if (!server.empty() && evdns_base_nameserver_ip_add(dnsBase, server.c_str()) == 0) {
            ok = true;
        }

        if (commaPos == std::string::npos) {
            break;
        }
        start = commaPos + 1;
    }

    evdns_base_resume(dnsBase);
    return ok;
}
#endif

// 必须在堆上
struct ResolveArg {
    DNSResolver*    self = nullptr;
    std::string     domain;

    void destroy() {
        delete this;
    }

private:
    ~ResolveArg() { }
};

#if EVENT_WRAPPER_USE_CARES
struct ResolveEventArg {
    EventPoll   eventPoll;
};

// c-ares 版本兼容: ares_getaddrinfo 在不同版本中返回类型可能是 int 或 void。
template <typename FnType>
struct AresGetaddrinfoDispatcher;

template <typename... Args>
struct AresGetaddrinfoDispatcher<void (*)(Args...)>
{
    static bool Call(Args... args)
    {
        ares_getaddrinfo(args...);
        return true;
    }
};

template <typename... Args>
struct AresGetaddrinfoDispatcher<int (*)(Args...)>
{
    static bool Call(Args... args)
    {
        return ares_getaddrinfo(args...) == ARES_SUCCESS;
    }
};
#endif

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
        for (auto &entry : domainMap) {
            if (entry.second.timer) {
                entry.second.timer->stop();
                entry.second.timer.reset();
            }
#if !EVENT_WRAPPER_USE_CARES
            if (entry.second.request != nullptr) {
                evdns_getaddrinfo_cancel(entry.second.request);
                entry.second.request = nullptr;
            }
#endif
        }
        domainMap.clear();

#if EVENT_WRAPPER_USE_CARES
        if (channel) {
            ares_destroy(channel);
            channel = nullptr;
        }
        socketPollMap.clear();
#else
        if (dnsBase) {
            // We already canceled outstanding requests above.
            evdns_base_free(dnsBase, 0);
            dnsBase = nullptr;
        }
#endif

        manager = nullptr;
        timeout = 0;
        domainServer.clear();
    }

#if EVENT_WRAPPER_USE_CARES
    ares_channel    channel = nullptr;
    std::unordered_map<socket_t, ResolveEventArg> socketPollMap;
#else
    evdns_base *dnsBase = nullptr;
#endif
    DNSManager*     manager = nullptr;
    uint32_t        timeout = 1000;
    std::string     domainServer;

    struct PendingDomain {
        DNSResolver::DNSResolveCB   resolveCB;
        DNSResolver::HostType       type;
        EventTimer::SP              timer;
#if !EVENT_WRAPPER_USE_CARES
        evdns_getaddrinfo_request  *request = nullptr;
#endif
    };
    std::map<std::string, PendingDomain>  domainMap; // 待解析的域名map
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
#if EVENT_WRAPPER_USE_CARES
    // NOTE linux下无实际意义
    std::call_once(g_initAres, [] () {
        ares_library_init(ARES_LIB_INIT_ALL);
        ::atexit(ares_library_cleanup);
    });
#endif

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
    if (host.empty()) {
        return false;
    }

    if (!initChannel()) {
        return false;
    }

    m_internal->domainServer = host;
#if EVENT_WRAPPER_USE_CARES
    return ARES_SUCCESS == ares_set_servers_csv(m_internal->channel, m_internal->domainServer.c_str());
#else
    return ApplyNameServers(m_internal->dnsBase, m_internal->domainServer);
#endif
}

bool DNSResolver::initChannel()
{
#if EVENT_WRAPPER_USE_CARES
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
#else
    if (nullptr == m_internal->dnsBase) {
        EventLoop *eventLoop = m_internal->manager->m_internal->eventLoop;
        m_internal->dnsBase = evdns_base_new(eventLoop->loop(), 1);
        if (m_internal->dnsBase == nullptr) {
            return false;
        }

        if (!m_internal->domainServer.empty() && !ApplyNameServers(m_internal->dnsBase, m_internal->domainServer)) {
            evdns_base_free(m_internal->dnsBase, 0);
            m_internal->dnsBase = nullptr;
            return false;
        }
    }
#endif

    return true;
}

void DNSResolver::onResolveTimeout(const std::string &domain)
{
    auto it = m_internal->domainMap.find(domain);
    if (it == m_internal->domainMap.end()) {
        return;
    }

#if !EVENT_WRAPPER_USE_CARES
    if (it->second.request != nullptr) {
        evdns_getaddrinfo_cancel(it->second.request);
        it->second.request = nullptr;
    }
#endif

    auto resolveCB = std::move(it->second.resolveCB);
    it->second.timer.reset();
    m_internal->domainMap.erase(it);
    resolveCB(DNS_TIMEOUT, DNSResultVec{});
}

#if EVENT_WRAPPER_USE_CARES
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
        eventPoll.reset(eventLoop->loop(), sock, (EventPoll::Event)eventFlag, [self](socket_t sockFd, EventPoll::event_t events) {
                socket_t readSock = (events & EventPoll::Event::Read) ? sockFd : ARES_SOCKET_BAD;
                socket_t writeSock = (events & EventPoll::Event::Write) ? sockFd : ARES_SOCKET_BAD;
                ares_process_fd(self->m_internal->channel, readSock, writeSock);
            });
        eventPoll.start();
    }
}

void DNSResolver::OnDnsCallback(void *arg, int status, int /*timeouts*/, ares_addrinfo *res)
{
    assert(arg != nullptr);
    ResolveArg *resolveArg = static_cast<ResolveArg *>(arg);
    std::shared_ptr<void> _clean(nullptr, [resolveArg] (void *) {
        resolveArg->destroy();
    });
    DNSResolver *self = resolveArg->self;
    const std::string &domain = resolveArg->domain;
    auto it = self->m_internal->domainMap.find(domain);
    if (it == self->m_internal->domainMap.end()) {
        return;
    }
    DNSResultVec resultVec;
    std::vector<std::string> cnameVec;
    bool found = false;

    uint64_t resolvedTimeMS = self->m_internal->manager->m_internal->acquireTimeCB();
    if (status == ARES_SUCCESS && res != nullptr) {
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
            } else if ((it->second.type & HostType::IPv6) && iter->ai_family == AF_INET6) {
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

    {
        auto resolveCB = std::move(it->second.resolveCB);
        it->second.timer.reset();
        self->m_internal->domainMap.erase(it);

        if (!resultVec.empty()) {
            self->m_internal->manager->addDNSCacheInternal(domain, resultVec);
            for (const auto &domainIt : cnameVec) {
                self->m_internal->manager->addDNSCacheInternal(domainIt, resultVec);
            }
        }

        try {
            resolveCB(found && !resultVec.empty() ? DNS_SUCCESS : DNS_FAILED,
                      found && !resultVec.empty() ? resultVec : DNSResultVec{});
        } catch(...) {
        }
    }
}
#else
void DNSResolver::OnDnsCallback(int status, addrinfo *res, void *arg)
{
    assert(arg != nullptr);
    ResolveArg *resolveArg = static_cast<ResolveArg *>(arg);
    std::shared_ptr<void> _clean(nullptr, [resolveArg] (void *) {
        resolveArg->destroy();
    });

    DNSResolver *self = resolveArg->self;
    const std::string &domain = resolveArg->domain;
    auto it = self->m_internal->domainMap.find(domain);
    if (it == self->m_internal->domainMap.end()) {
        if (res != nullptr) {
            evutil_freeaddrinfo(res);
        }
        return;
    }

    it->second.request = nullptr;

    DNSResultVec resultVec;
    std::vector<std::string> cnameVec;
    uint64_t resolvedTimeMS = self->m_internal->manager->m_internal->acquireTimeCB();
    uint32_t defaultTTL = self->m_internal->manager->m_internal->cacheTimeout;
    if (defaultTTL == 0) {
        defaultTTL = 60;
    }

    if (status == 0 && res != nullptr) {
        if (res->ai_canonname != nullptr && res->ai_canonname[0] != '\0') {
            cnameVec.emplace_back(res->ai_canonname);
        }

        for (evutil_addrinfo *iter = res; iter != nullptr; iter = iter->ai_next) {
            DNSResult result;
            char addr[INET6_ADDRSTRLEN] = {0};

            if ((it->second.type & HostType::IPv4) && iter->ai_family == AF_INET) {
                sockaddr_in *sa = reinterpret_cast<sockaddr_in *>(iter->ai_addr);
                if (evutil_inet_ntop(AF_INET, &(sa->sin_addr), addr, sizeof(addr)) == nullptr) {
                    continue;
                }
            } else if ((it->second.type & HostType::IPv6) && iter->ai_family == AF_INET6) {
                result.isIPv6 = true;
                sockaddr_in6 *sa = reinterpret_cast<sockaddr_in6 *>(iter->ai_addr);
                if (evutil_inet_ntop(AF_INET6, &(sa->sin6_addr), addr, sizeof(addr)) == nullptr) {
                    continue;
                }
            } else {
                continue;
            }

            result.address = addr;
            result.resolvedTimeMS = resolvedTimeMS;
            result.ttl = defaultTTL;
            resultVec.emplace_back(result);
        }
    }

    if (res != nullptr) {
        evutil_freeaddrinfo(res);
    }

    {
        auto resolveCB = std::move(it->second.resolveCB);
        it->second.timer.reset();
        self->m_internal->domainMap.erase(it);

        if (!resultVec.empty()) {
            self->m_internal->manager->addDNSCacheInternal(domain, resultVec);
            for (const auto &domainIt : cnameVec) {
                self->m_internal->manager->addDNSCacheInternal(domainIt, resultVec);
            }
        }

        try {
            resolveCB(!resultVec.empty() ? DNS_SUCCESS : DNS_FAILED,
                      !resultVec.empty() ? resultVec : DNSResultVec{});
        } catch(...) {
        }
    }
}
#endif

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

    // m_internal->domainMap.emplace(domain, DNSResolverInternal::PendingDomain{cb, type});
    auto &pendingDomain = m_internal->domainMap[domain];
    pendingDomain.resolveCB = std::move(cb);
    pendingDomain.type = type;
    auto timer = std::make_shared<EventTimer>();
    pendingDomain.timer = timer;

    ResolveArg *arg = new ResolveArg();
    arg->self = this;
    arg->domain = domain;

#if EVENT_WRAPPER_USE_CARES
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
        arg->destroy();
        m_internal->domainMap.erase(domain);
        return false;
    }

    bool queued = AresGetaddrinfoDispatcher<decltype(&ares_getaddrinfo)>::Call(
        m_internal->channel, domain.c_str(), NULL, &hint, OnDnsCallback, arg);
    if (!queued) {
        arg->destroy();
        m_internal->domainMap.erase(domain);
        return false;
    }
#else
    evutil_addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    switch (type) {
    case HostType::Both:
        hints.ai_family = AF_UNSPEC;
        break;
    case HostType::IPv4:
        hints.ai_family = AF_INET;
        break;
    case HostType::IPv6:
        hints.ai_family = AF_INET6;
        break;
    default:
        arg->destroy();
        m_internal->domainMap.erase(domain);
        return false;
    }

    pendingDomain.request = evdns_getaddrinfo(m_internal->dnsBase, domain.c_str(), NULL, &hints, OnDnsCallback, arg);
    if (pendingDomain.request == nullptr) {
        arg->destroy();
        m_internal->domainMap.erase(domain);
        return false;
    }
#endif

    // 添加超时定时器
    timer->reset(m_internal->manager->m_internal->eventLoop->loop(), [domain, this]() {
        this->onResolveTimeout(domain);
    });
    if (!timer->start(m_internal->timeout)) {
#if EVENT_WRAPPER_USE_CARES
        // ares 没有提供按域名取消的接口，标记超时让回调自行忽略
        m_internal->domainMap.erase(domain);
#else
        evdns_getaddrinfo_cancel(pendingDomain.request);
        m_internal->domainMap.erase(domain);
#endif
        return false;
    }
    return true;
}

DNSManager::DNSManager(EventLoop *loop)
{
    if (loop == nullptr) {
        throw std::runtime_error("invalid param EventLoop");
    }

    m_internal = std::make_shared<DNSManagerInternal>();
    m_internal->eventLoop = loop;
    m_internal->timer.reset(loop->loop(), [this]() {
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
