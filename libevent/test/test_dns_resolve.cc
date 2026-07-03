#include <memory>

#include <event/dns_resolve.h>
#include <event/timer.h>

int main()
{
    ev::EventLoop::Ptr eventLoop = std::make_unique<ev::EventLoop>();
    ev::EventTimer::Ptr eventaTimer = std::make_unique<ev::EventTimer>();
    ev::DNSManager::Ptr manager = std::make_unique<ev::DNSManager>(eventLoop.get());
    ev::DNSResolver::Ptr resolver = std::make_unique<ev::DNSResolver>(manager.get());

    resolver->setResolveTimeout(1000);
    resolver->resolve("www.example.com", [](int32_t code, const ev::DNSResultVec &resultVec) {
        if (code != DNS_SUCCESS) {
            printf("resolve www.example.com error: %d\n", code);
            return;
        }

        printf("www.example.com resolution successful\n");
        for (const auto &it : resultVec) {
            if (it.isIPv6) {
                printf("\tIPv6: %s, ttl: %u\n", it.address.c_str(), it.ttl);
            } else {
                printf("\tIPv4: %s, ttl: %u\n", it.address.c_str(), it.ttl);
            }
        }
    });

    resolver->setDomainServer("114.114.114.114,8.8.8.8");
    resolver->resolve("www.github.com", [](int32_t code, const ev::DNSResultVec &resultVec) {
        if (code != DNS_SUCCESS) {
            printf("resolve www.github.com error: %d\n", code);
            return;
        }

        printf("www.github.com resolution successful\n");
        for (const auto &it : resultVec) {
            if (it.isIPv6) {
                printf("\tIPv6: %s, ttl: %u\n", it.address.c_str(), it.ttl);
            } else {
                printf("\tIPv4: %s, ttl: %u\n", it.address.c_str(), it.ttl);
            }
        }
    });

    eventaTimer->reset(eventLoop->loop(), [&eventLoop] () {
        eventLoop->breakLoop();
        printf("event loop break\n");
    });

    eventaTimer->start(5000);
    eventLoop->dispatch();
    return 0;
}