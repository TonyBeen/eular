/*************************************************************************
    > File Name: dns_resolve.h
    > Author: hsz
    > Brief:
    > Created Time: 2024年11月01日 星期五 18时02分11秒
 ************************************************************************/

#ifndef __EVENT_DNS_RESOLVE_H__
#define __EVENT_DNS_RESOLVE_H__

#include <string>
#include <vector>
#include <map>

#include <event/event_base.h>
#include <event/event_loop.h>
#include <event/event_poll.h>

#define DNS_SUCCESS 0
#define DNS_TIMEOUT -1
#define DNS_FAILED  -2

struct ares_addrinfo;

namespace ev {
class DNSManager;
class DNSResolverInternal;

struct DNSResult {
    std::string address;
    uint64_t    resolvedTimeMS = UINT64_MAX; // 解析时间
    uint32_t    ttl;
    bool        isIPv6 = false;
};
using DNSResultVec = std::vector<DNSResult>;

class DNSResolver
{
    friend class DNSManager;
public:
    using DNSResolveCB = std::function<void(int32_t, const DNSResultVec &)>;
    enum HostType {
        IPv4 = 0x1,
        IPv6 = 0x2,
        Both = 0x3,
    };

    using Ptr = std::unique_ptr<DNSResolver>;
    using WP  = std::weak_ptr<DNSResolver>;
    using SP  = std::shared_ptr<DNSResolver>;

    DNSResolver(DNSManager *manager);
    ~DNSResolver();

    // 设置解析超时时间(ms), 默认1000
    void setResolveTimeout(uint32_t ms);

    // 设置域名服务器 多个域名以 ',' 分割. 如: "114.114.114.114,8.8.8.8"
    bool setDomainServer(const std::string &host);

    // 解析域名
    bool resolve(const std::string &domain, DNSResolveCB cb, HostType type = HostType::Both);

private:
    bool initChannel();
    void onResolveTimeout(const std::string &domain);
    static void OnSocketStateChanged(void *data, socket_t sock, int readable, int writable);
    static void OnDnsCallback(void *arg, int status, int timeouts, ares_addrinfo *res);

private:
    std::shared_ptr<DNSResolverInternal> m_internal;
};

class DNSManagerInternal;
class DNSManager
{
    friend class DNSResolver;
public:
    using AcquireTimeCB = std::function<uint64_t()>;

    using Ptr = std::unique_ptr<DNSManager>;
    using WP  = std::weak_ptr<DNSManager>;
    using SP  = std::shared_ptr<DNSManager>;

    DNSManager(EventLoop *loop);
    ~DNSManager();

    // 设置获取时间回调, 返回值ms
    void setAcquireTimeCB(AcquireTimeCB cb);

    // 添加指定域名的缓存, 超时时间受ttl控制
    void addDNSCache(const std::string &domian, const DNSResultVec &hostVec);

    // 移除指定域名的缓存
    void removeDNSCache(const std::string &domain);

    // 判断是否存在此域名
    bool hasDoaminCache(const std::string &domain);

    // 获取域名缓存
    const DNSResultVec *getDomainCache(const std::string &domain);

protected:
    // DNSResolver 添加的域名
    void addDNSCacheInternal(const std::string &domian, DNSResultVec hostVec);

private:
    std::shared_ptr<DNSManagerInternal> m_internal;
    
};

} // namespace ev

#endif // __EVENT_DNS_RESOLVE_H__
