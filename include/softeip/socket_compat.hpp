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
#include <mswsock.h>   // SIO_UDP_CONNRESET (Windows SDK 10.0.26100 defines it here, not in mstcpip.h)
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
//
// Windows 11 ignores timeBeginPeriod() for processes whose windows are minimized,
// occluded or absent (console apps started hidden, services, a minimized HMI) and
// falls back to the 15.6 ms tick: measured as ~65 instead of 100 packets/s at
// RPI 10 ms. Opting out of timer-resolution power throttling keeps the 1 ms
// request honoured regardless of window state.
#ifdef _WIN32
inline void honourTimerResolutionRequests()
{
    // SetProcessInformation(ProcessPowerThrottling, ...) is declared only for
    // _WIN32_WINNT >= 0x0602 and the library targets 0x0601, so resolve it at run
    // time (absent on Windows 7: then there is no throttling to opt out of).
    // Values from processthreadsapi.h (SDK 10.0.26100).
    struct PowerThrottlingState { ULONG version, controlMask, stateMask; };
    using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, LPVOID, DWORD);
    constexpr int kProcessPowerThrottling = 4;           // PROCESS_INFORMATION_CLASS::ProcessPowerThrottling
    constexpr ULONG kIgnoreTimerResolution = 0x4;        // PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!kernel32)
        return;
    auto fn = reinterpret_cast<SetProcessInformationFn>(
        reinterpret_cast<void*>(GetProcAddress(kernel32, "SetProcessInformation")));
    if (!fn)
        return;
    PowerThrottlingState state{1, kIgnoreTimerResolution, 0}; // stateMask 0 = always honour requests
    fn(GetCurrentProcess(), kProcessPowerThrottling, &state, sizeof state);
}
#endif

class SocketLibrary {
public:
    SocketLibrary()
    {
#ifdef _WIN32
        WSADATA wsa;
        ok_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        honourTimerResolutionRequests();
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

inline void setSendTimeoutMs(socket_t s, unsigned ms)
{
#ifdef _WIN32
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

inline bool setNonBlocking(socket_t s, bool on)
{
#ifdef _WIN32
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0)
        return false;
    return fcntl(s, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK)) == 0;
#endif
}

// Waits until the socket is readable. Returns 1 readable, 0 timeout, -1 error.
inline int waitReadable(socket_t s, unsigned ms)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(s, &rs);
    timeval tv{};
    tv.tv_sec = static_cast<long>(ms / 1000);
    tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
    int r = ::select(static_cast<int>(s + 1), &rs, nullptr, nullptr, &tv);
    return r > 0 ? 1 : (r == 0 ? 0 : -1);
}

// TCP connect that gives up after `ms` (a blocking connect to an unreachable host can hang
// for ~20 s on Windows). The socket is back in blocking mode on return.
inline bool connectWithTimeout(socket_t s, const sockaddr_in& to, unsigned ms)
{
    if (!setNonBlocking(s, true))
        return false;
    int r = ::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof to);
    bool ok = r == 0;
    if (!ok) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        if (pending) {
            fd_set ws, es;
            FD_ZERO(&ws);
            FD_ZERO(&es);
            FD_SET(s, &ws);
            FD_SET(s, &es);
            timeval tv{};
            tv.tv_sec = static_cast<long>(ms / 1000);
            tv.tv_usec = static_cast<long>((ms % 1000) * 1000);
            if (::select(static_cast<int>(s + 1), nullptr, &ws, &es, &tv) > 0 && FD_ISSET(s, &ws)) {
                int err = 0;
                socklen_t len = sizeof err;
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                ok = err == 0;
            }
        }
    }
    setNonBlocking(s, false);
    return ok;
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
