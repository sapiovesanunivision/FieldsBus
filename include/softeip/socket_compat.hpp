// Thin portability layer: Winsock on Windows, BSD sockets elsewhere.
#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mmsystem.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <cstdint>

namespace softeip {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int closeSocket(socket_t s) { return closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
inline int closeSocket(socket_t s) { return ::close(s); }
#endif

// Initializes Winsock and requests 1 ms timer resolution (default is 15.6 ms,
// which would make every RPI below ~16 ms jittery). No-op on POSIX.
class SocketLibrary {
public:
    SocketLibrary()
    {
#ifdef _WIN32
        WSADATA wsa;
        ok_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        timeBeginPeriod(1);
#endif
    }
    ~SocketLibrary()
    {
#ifdef _WIN32
        timeEndPeriod(1);
        if (ok_)
            WSACleanup();
#endif
    }
    SocketLibrary(const SocketLibrary&) = delete;
    SocketLibrary& operator=(const SocketLibrary&) = delete;

private:
    bool ok_ = true;
};

inline void setReuseAddr(socket_t s)
{
    int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&on), sizeof on);
}

inline void setRecvTimeoutMs(socket_t s, unsigned ms)
{
#ifdef _WIN32
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif
}

inline void setNoDelay(socket_t s)
{
    int on = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&on), sizeof on);
}

// On Windows an ICMP "port unreachable" makes the *next* recvfrom() on a UDP
// socket fail with WSAECONNRESET. A PLC that is switched off would then break
// the I/O socket, so disable that behaviour.
inline void disableUdpConnReset(socket_t s)
{
#ifdef _WIN32
    BOOL off = FALSE;
    DWORD ret = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &ret, nullptr, nullptr);
#else
    (void)s;
#endif
}

inline int sendBytes(socket_t s, const uint8_t* p, size_t n)
{
    return ::send(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0);
}

inline bool sendAll(socket_t s, const uint8_t* p, size_t n)
{
    while (n > 0) {
        int r = sendBytes(s, p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

inline int recvBytes(socket_t s, uint8_t* p, size_t n)
{
    return ::recv(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0);
}

inline int sendTo(socket_t s, const uint8_t* p, size_t n, const sockaddr_in& to)
{
    return ::sendto(s, reinterpret_cast<const char*>(p), static_cast<int>(n), 0,
                    reinterpret_cast<const sockaddr*>(&to), sizeof to);
}

inline int recvFrom(socket_t s, uint8_t* p, size_t n, sockaddr_in& from)
{
    socklen_t len = sizeof from;
    return ::recvfrom(s, reinterpret_cast<char*>(p), static_cast<int>(n), 0,
                      reinterpret_cast<sockaddr*>(&from), &len);
}

} // namespace softeip
