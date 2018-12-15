/*
 *  IXSocketConnect.cpp
 *  Author: Benjamin Sergeant
 *  Copyright (c) 2018 Machine Zone, Inc. All rights reserved.
 */

#include "IXSocketConnect.h"
#include "IXDNSLookup.h"

#ifdef _WIN32
# include <basetsd.h>
# include <WinSock2.h>
# include <ws2def.h>
# include <WS2tcpip.h>
# include <io.h>
#else
# include <unistd.h>
# include <errno.h>
# include <netdb.h>
# include <netinet/tcp.h>
# include <sys/socket.h>
# include <sys/time.h>
# include <sys/select.h>
# include <sys/stat.h>
#endif

#include <string.h>
#include <fcntl.h>
#include <sys/types.h>

// Android needs extra headers for TCP_NODELAY and IPPROTO_TCP
#ifdef ANDROID
# include <linux/in.h>
# include <linux/tcp.h>
#endif

namespace
{
    void closeSocket(int fd)
    {
#ifdef _WIN32
        closesocket(fd);
#else
        ::close(fd);
#endif
    }
}

namespace ix 
{
    //
    // This function can be cancelled every 50 ms
    // This is important so that we don't block the main UI thread when shutting down a connection which is
    // already trying to reconnect, and can be blocked waiting for ::connect to respond.
    //
    bool SocketConnect::connectToAddress(const struct addrinfo *address, 
                                         int& sockfd,
                                         std::string& errMsg,
                                         const CancellationRequest& isCancellationRequested)
    {
        sockfd = -1;

        int fd = socket(address->ai_family,
                        address->ai_socktype,
                        address->ai_protocol);
        if (fd < 0)
        {
            errMsg = "Cannot create a socket";
            return false;
        }

        // Set the socket to non blocking mode, so that slow responses cannot
        // block us for too long
        SocketConnect::configure(fd);

        if (::connect(fd, address->ai_addr, address->ai_addrlen) == -1
            && errno != EINPROGRESS)
        {
            closeSocket(fd);
            errMsg = strerror(errno);
            return false;
        }

        //
        // If during a connection attempt the request remains idle for longer
        // than the timeout interval, the request is considered to have timed
        // out. The default timeout interval is 60 seconds.
        //
        // See https://developer.apple.com/documentation/foundation/nsmutableurlrequest/1414063-timeoutinterval?language=objc
        //
        // 60 seconds timeout, each time we wait for 50ms with select -> 1200 attempts
        //
        int selectTimeOut = 50 * 1000; // In micro-seconds => 50ms
        int maxRetries = 60 * 1000 * 1000 / selectTimeOut;

        for (int i = 0; i < maxRetries; ++i)
        {
            if (isCancellationRequested())
            {
                closeSocket(fd);
                errMsg = "Cancelled";
                return false;
            }

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);

            // 50ms timeout
            struct timeval timeout;
            timeout.tv_sec = 0;
            timeout.tv_usec = selectTimeOut;

            select(fd + 1, nullptr, &wfds, nullptr, &timeout);

            // Nothing was written to the socket, wait again.
            if (!FD_ISSET(fd, &wfds)) continue;

            // Something was written to the socket
            int optval = -1;
            socklen_t optlen = sizeof(optval);

            // getsockopt() puts the errno value for connect into optval so 0
            // means no-error.
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &optval, &optlen) == -1 ||
                optval != 0)
            {
                closeSocket(fd);
                errMsg = strerror(optval);
                return false;
            }
            else
            {
                // Success !
                sockfd = fd;
                return true;
            }
        }

        closeSocket(fd);
        errMsg = "connect timed out after 60 seconds";
        return false;
    }

    int SocketConnect::connect(const std::string& hostname,
                               int port,
                               std::string& errMsg,
                               const CancellationRequest& isCancellationRequested)
    {
        //
        // First do DNS resolution
        //
        DNSLookup dnsLookup(hostname, port);
        struct addrinfo *res = dnsLookup.resolve(errMsg, isCancellationRequested);
        if (res == nullptr)
        {
            return -1;
        }

        int sockfd = -1;

        // iterate through the records to find a working peer
        struct addrinfo *address;
        bool success = false;
        for (address = res; address != nullptr; address = address->ai_next)
        {
            //
            // Second try to connect to the remote host
            //
            success = connectToAddress(address, sockfd, errMsg, isCancellationRequested);
            if (success)
            {
                break;
            }
        }

        freeaddrinfo(res);
        return sockfd;
    }

    void SocketConnect::configure(int sockfd)
    {
        // 1. disable Nagle's algorithm
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char*) &flag, sizeof(flag));

        // 2. make socket non blocking
#ifdef _WIN32
        unsigned long nonblocking = 1;
        ioctlsocket(_sockfd, FIONBIO, &nonblocking);
#else
        fcntl(sockfd, F_SETFL, O_NONBLOCK); // make socket non blocking
#endif

        // 3. (apple) prevent SIGPIPE from being emitted when the remote end disconnect
#ifdef SO_NOSIGPIPE
        int value = 1;
        setsockopt(sockfd, SOL_SOCKET, SO_NOSIGPIPE, 
                   (void *)&value, sizeof(value));
#endif
    }
}