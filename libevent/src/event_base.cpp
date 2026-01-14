/*************************************************************************
    > File Name: event_base.cpp
    > Author: hsz
    > Brief:
    > Created Time: 2024年09月30日 星期一 11时11分04秒
 ************************************************************************/

#include "event/event_base.h"
#include <memory>

#if defined(_WIN32) || defined(_WIN64)
#pragma comment(lib, "ws2_32.lib")

struct socket_initialization
{
    socket_initialization()
    {
        WSADATA wsaData;
        WORD wVersionRequested = MAKEWORD(2, 2);
        WSAStartup(wVersionRequested, &wsaData);
    }

    ~socket_initialization()
    {
        WSACleanup();
    }
};

static std::unique_ptr<socket_initialization> g_initSocket(new socket_initialization);

#endif

