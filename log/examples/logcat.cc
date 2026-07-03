/*************************************************************************
    > File Name: logcat.cc
    > Author: hsz
    > Brief: server of log console
    > Created Time: Sun 23 Jan 2022 04:36:06 PM CST
 ************************************************************************/

#include <error.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <assert.h>
#include <map>
#include <string>
#include <sstream>

#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/epoll.h>

#include "nlohmann/json.hpp"
#include "callstack.h"

#define LOCAL_SOCK_PATH "/tmp/log_sock_server"
#define EPOLL_SIZE      (1024)

#define SEP_STR "\r\n\r\n"
static const uint32_t SEP_LEN = strlen(SEP_STR);

int gLocalServerSocket = -1;    // 本地套接字服务端
int gLocalClientSocket = -1;    // 本地套接字客户端
int tcpServerSocket = -1;       // 网络套接字服务端
std::map<int, sockaddr_in> gNetClientMap; // 网络套接字客户端map

typedef nlohmann::json Json;

std::string gJsonNotice; // 已携带分割符

#define RECV_BUFFER_SIZE    8192
static char gRecvBuf[RECV_BUFFER_SIZE];

void print(const char *perfix)
{
    printf("%s\n", perfix);
    printf("-h get help\n");
    printf("-p port listen on port.(default is 8000)\n");
    printf("-d Start in daemon mode\n");
    exit(0);
}

void catch_signal(int sig)
{
    std::string signalMsg;
    std::string strCallStack;
    eular::CallStack stack;
    char msgBuf[256] = {0};

    switch (sig) {
        case SIGINT:
            snprintf(msgBuf, sizeof(msgBuf), "SIGINT captured sig: %d\n", sig);
            break;
        case SIGABRT:
            snprintf(msgBuf, sizeof(msgBuf), "SIGABRT captured sig: %d\n", sig);
            stack.update();
            strCallStack = stack.toString();
            break;
        case SIGQUIT:
            snprintf(msgBuf, sizeof(msgBuf), "SIGQUIT captured sig: %d\n", sig);
            break;
        case SIGSEGV:
            snprintf(msgBuf, sizeof(msgBuf), "SIGSEGV captured sig: %d\n", sig);
            stack.update();
            strCallStack = stack.toString();
            break;
        default:
            printf("unhandle signal %d\n", sig);
            return;
    }

    printf("%s, unlink(%s)\n", msgBuf, LOCAL_SOCK_PATH);

    signalMsg = msgBuf;
    signalMsg.append(strCallStack);

    Json root;
    root["id"] = "error";
    root["keywords"] = "msg";
    root["msg"] = signalMsg;

    std::stringstream ss;
    ss << root;
    const std::string &jsonMsg = ss.str();

    for (auto it = gNetClientMap.begin(); it != gNetClientMap.end();) {
        ::send(it->first, jsonMsg.c_str(), jsonMsg.length(), 0);
        close(it->first);
        it = gNetClientMap.erase(it);
    }

    close(tcpServerSocket);
    close(gLocalClientSocket);
    close(gLocalServerSocket);
    unlink(LOCAL_SOCK_PATH);
    exit(0);
}

int InitSocket()
{
    gLocalServerSocket = ::socket(AF_LOCAL, SOCK_STREAM, 0);
    if (gLocalServerSocket < 0) {
        printf("%s() socket error. %d %s\n", __func__, errno, strerror(errno));
        return gLocalServerSocket;
    }

    unlink(LOCAL_SOCK_PATH);
    sockaddr_un saddr;
    saddr.sun_family = AF_LOCAL;
    snprintf(saddr.sun_path, sizeof(saddr.sun_path), LOCAL_SOCK_PATH);
    int nRetCode = ::bind(gLocalServerSocket, (sockaddr *)&saddr, sizeof(saddr));
    if (nRetCode < 0) {
        printf("%s() bind error. %d %s\n", __func__, errno, strerror(errno));
        goto error;
    }

    nRetCode = ::listen(gLocalServerSocket, 14);
    if (nRetCode < 0) {
        printf("%s() listen error. %d %s\n", __func__, errno, strerror(errno));
        goto error;
    }

    return gLocalServerSocket;

error:
    if (gLocalServerSocket > 0) {
        ::close(gLocalServerSocket);
        gLocalServerSocket = -1;
    }

    return nRetCode;
}

void OnLocalSocketReadEvent(std::string jsonContent)
{
    if (jsonContent.length() == 0) {
        return;
    }

    printf("json: %s\n", jsonContent.c_str());

    try
    {
        auto jsonConfig = Json::parse(jsonContent);
        if (jsonConfig["id"].get<std::string>() == "notice") {
            gJsonNotice = jsonContent;
            gJsonNotice.append(SEP_STR);
        }
    }
    catch(const std::exception& e)
    {
        printf("json parse error: %s\n", e.what());
        return;
    }

    jsonContent.append(SEP_STR);
    for (auto it  = gNetClientMap.begin(); it != gNetClientMap.end(); ++it) {
        ::send(it->first, jsonContent.c_str(), jsonContent.length(), 0);
    }
}

void OnTcpSocketReadEvent(const std::string &jsonContent)
{
    (void)jsonContent;
    // TODO 解析json
}

int32_t _main(int32_t port)
{
    signal(SIGINT, catch_signal);
    signal(SIGQUIT, catch_signal);
    signal(SIGABRT, catch_signal);
    signal(SIGSEGV, catch_signal);
    signal(SIGPIPE, SIG_IGN);

    assert(InitSocket() > 0);

    int tcpServerSocket = ::socket(AF_INET, SOCK_STREAM, 0);
    if (tcpServerSocket < 0) {
        printf("%s() socket error. %d %s\n", __func__, errno, strerror(errno));
        return -1;
    }

    sockaddr_in srvAddr;
    srvAddr.sin_family = AF_INET;
    srvAddr.sin_port = htons(port > 0 ? port : 8000);
    srvAddr.sin_addr.s_addr = INADDR_ANY;
    int retCode = ::bind(tcpServerSocket, (sockaddr *)&srvAddr, sizeof(srvAddr));
    if (retCode < 0) {
        printf("%s() bind error. %d %s\n", __func__, errno, strerror(errno));
        return -1;
    }

    ::listen(tcpServerSocket, 128);

    int reuse = 1;
    setsockopt(tcpServerSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    epoll_event events[EPOLL_SIZE];
    int epollFd = epoll_create(EPOLL_SIZE);
    if (epollFd < 0) {
        printf("%s() epoll_create error. %d %s\n", __func__, errno, strerror(errno));
        return -1;
    }

    int flag = fcntl(tcpServerSocket, F_GETFL);
    flag |= O_NONBLOCK;
    fcntl(tcpServerSocket, F_SETFL, flag);

    epoll_event event;
    event.data.fd = gLocalServerSocket;
    event.events = EPOLLIN;
    assert(epoll_ctl(epollFd, EPOLL_CTL_ADD, gLocalServerSocket, &event) == 0);

    event.data.fd = tcpServerSocket;
    event.events = EPOLLIN;
    assert(epoll_ctl(epollFd, EPOLL_CTL_ADD, tcpServerSocket, &event) == 0);

    printf("LocalServerSocket = %d, NetServerSocket = %d, epollFd = %d, waiting...\n", gLocalServerSocket, tcpServerSocket, epollFd);
    while (true) {
        int nev = epoll_wait(epollFd, events, EPOLL_SIZE, -1);
        if (nev < 0) {
            printf("epoll_wait error. [%d,%s]\n", errno, strerror(errno));
            exit(0);
        }
        for (int i = 0; i < nev; ++i) {
            epoll_event &ev = events[i];

            if (ev.data.fd == gLocalServerSocket && gLocalClientSocket <= 0) {
                sockaddr_un client;
                socklen_t len = sizeof(client);
                gLocalClientSocket = ::accept(gLocalServerSocket, (sockaddr *)&client, &len);
                if (gLocalClientSocket <= 0) {
                    perror("accept error");
                } else {
                    printf("accept local socket client. %d %s\n", gLocalClientSocket, client.sun_path);
                    flag = fcntl(gLocalClientSocket, F_GETFL);
                    flag |= O_NONBLOCK;
                    fcntl(gLocalClientSocket, F_SETFL, flag);

                    event.data.fd = gLocalClientSocket;
                    event.events = EPOLLIN;
                    epoll_ctl(epollFd, EPOLL_CTL_ADD, gLocalClientSocket, &event);
                }

                continue;
            }

            if (ev.data.fd == tcpServerSocket) {
                sockaddr_in clientAddr;
                socklen_t addrLen = sizeof(sockaddr_in);

                int clientFd = ::accept(tcpServerSocket, (sockaddr *)&clientAddr, &addrLen);
                if (clientFd > 0) {
                    flag = fcntl(clientFd, F_GETFL);
                    flag |= O_NONBLOCK;
                    fcntl(clientFd, F_SETFL, flag);

                    epoll_event epEvent;
                    epEvent.data.fd = clientFd;
                    epEvent.events = EPOLLIN;
                    int32_t nRet = epoll_ctl(epollFd, EPOLL_CTL_ADD, clientFd, &epEvent);
                    if (0 != nRet) {
                        static const char *errorMsgJson =
                            "{\"id\": \"error\", \"keywords\": [\"msg\"], \"msg\": \"epoll_ctl error: %s\"}";
                        char msg[256] = { '\0' };
                        snprintf(msg, sizeof(msg), errorMsgJson, strerror(errno));
                        ::send(clientFd, msg, strlen(msg), 0);
                        perror("epoll_ctl error");
                        close(clientFd);
                    } else {
                        gNetClientMap[clientFd] = clientAddr;
                        if (!gJsonNotice.empty()) {
                            ::send(clientFd, gJsonNotice.c_str(), gJsonNotice.length(), 0);
                        }
                    }
                }

                continue;
            }

            if (ev.events & EPOLLIN) {  // 本地套接字读事件
                if (ev.data.fd == gLocalClientSocket) {
                    static std::string jsonContent;
                    jsonContent.reserve(RECV_BUFFER_SIZE);

                    while (true) {
                        memset(gRecvBuf, 0, sizeof(gRecvBuf));
                        int32_t nRecv = ::recv(ev.data.fd, gRecvBuf, sizeof(gRecvBuf), 0);
                        if (nRecv == 0) { // 没有携带结束符
                            break;
                        }

                        if (nRecv < 0) {
                            if (errno != EAGAIN) {
                                printf("recv(%d) error: %d, %s\n", ev.data.fd, errno, strerror(errno));
                                close(ev.data.fd);
                                epoll_ctl(epollFd, EPOLL_CTL_DEL, ev.data.fd, nullptr);
                            }
                            break;
                        }

                        jsonContent.append(gRecvBuf);
                        size_t sepIndex = jsonContent.find(SEP_STR);
                        if (sepIndex != std::string::npos)
                        {
                            std::string jsonConfig(jsonContent.c_str(), sepIndex);
                            jsonContent.erase(0, sepIndex + SEP_LEN);

                            // 将本地套接字发送的数据转发
                            OnLocalSocketReadEvent(std::move(jsonConfig));
                        }
                    }
                } else {
                    // FIXME 暂时不知客户端数据怎么处理 TCP 客户端数据
                    while (true) {
                        int32_t nRecv = ::recv(ev.data.fd, gRecvBuf, sizeof(gRecvBuf), MSG_PEEK);
                        if (nRecv <= 0) {
                            break;
                        }
                    }
                }
            }

            if (ev.events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) { // 退出事件
                printf("EPOLLERR 0x%X client %d exit.\n", ev.events, ev.data.fd);
                close(ev.data.fd);
                epoll_ctl(epollFd, EPOLL_CTL_DEL, ev.data.fd, nullptr);
                if (ev.data.fd == gLocalClientSocket) {
                    gLocalClientSocket = -1;
                } else {
                    gNetClientMap.erase(ev.data.fd);
                }
            }
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    int32_t cmd = '\0';
    int32_t port = 8000;
    bool isDaemon = false;
    std::string path;
    while ((cmd = ::getopt(argc, argv, "hdp:")) != -1) {
        switch (cmd) {
        case 'h':
            print(argv[0]);
            break;
        case 'd':
            isDaemon = true;
            break;
        case 'p':
            port = atoi(optarg);
        default:
            break;
        }
    }

    if (isDaemon)
    {
        int32_t ret = daemon(1, 0);
        (void)ret;

        pid_t pid = fork();
        if (pid == 0) { // 子进程
            printf("子进程开始: %d", getpid());
            return _main(port);
        } else if (pid < 0) {   // 出错
            printf("%s() fork error. [%d, %s]", __func__, errno, strerror(errno));
            _exit(0);
        } else {    // 父进程
            int status = 0;
            waitpid(pid, &status, 0);

            if (status == SIGKILL) {
                printf("The child process(%d) was killed", pid);
            }
        }

        return 0;
    }

    return _main(port);
}
