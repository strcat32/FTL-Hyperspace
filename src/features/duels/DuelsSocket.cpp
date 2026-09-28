#include "DuelsSocket.h"

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET NativeSocket;
typedef int SockLen;
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int NativeSocket;
typedef socklen_t SockLen;
#endif

namespace Duels
{
    static std::string ErrorText(const char *what)
    {
        char buffer[160];
#ifdef _WIN32
        snprintf(buffer, sizeof(buffer), "%s failed (WSA error %d)", what, WSAGetLastError());
#else
        snprintf(buffer, sizeof(buffer), "%s failed (%s)", what, strerror(errno));
#endif
        return buffer;
    }

    static bool StartNetworking(std::string &error)
    {
#ifdef _WIN32
        static bool started = false;
        if (started) return true;
        WSADATA data;
        int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0)
        {
            char buffer[80];
            snprintf(buffer, sizeof(buffer), "WSAStartup failed (%d)", result);
            error = buffer;
            return false;
        }
        started = true;
#else
        (void)error;
#endif
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Addresses
    // ------------------------------------------------------------------------------------------------------------

    bool NetAddress::operator==(const NetAddress &other) const
    {
        if (length != other.length || length == 0) return false;
        const sockaddr *a = (const sockaddr*)storage;
        const sockaddr *b = (const sockaddr*)other.storage;
        if (a->sa_family != b->sa_family) return false;
        if (a->sa_family == AF_INET)
        {
            const sockaddr_in *x = (const sockaddr_in*)a;
            const sockaddr_in *y = (const sockaddr_in*)b;
            return x->sin_port == y->sin_port && x->sin_addr.s_addr == y->sin_addr.s_addr;
        }
        if (a->sa_family == AF_INET6)
        {
            const sockaddr_in6 *x = (const sockaddr_in6*)a;
            const sockaddr_in6 *y = (const sockaddr_in6*)b;
            return x->sin6_port == y->sin6_port && std::memcmp(&x->sin6_addr, &y->sin6_addr, sizeof(x->sin6_addr)) == 0;
        }
        return std::memcmp(storage, other.storage, length) == 0;
    }

    std::string NetAddress::ToString() const
    {
        if (!Valid()) return "(none)";
        char host[128] = {0};
        char service[16] = {0};
        if (getnameinfo((const sockaddr*)storage, (SockLen)length, host, sizeof(host), service, sizeof(service),
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0)
        {
            return "(unprintable address)";
        }
        std::string text = host;
        // IPv4 carried on a dual-stack socket shows as ::ffff:a.b.c.d; print it the familiar way.
        if (text.compare(0, 7, "::ffff:") == 0 && text.find('.') != std::string::npos) text = text.substr(7);
        if (text.find(':') != std::string::npos) return "[" + text + "]:" + service;
        return text + ":" + service;
    }

    bool ResolveAddress(const std::string &host, uint16_t port, NetAddress &out, std::string &error)
    {
        if (!StartNetworking(error)) return false;

        std::string name = host;
        if (name.size() > 2 && name[0] == '[' && name[name.size() - 1] == ']') name = name.substr(1, name.size() - 2);

        addrinfo hints;
        std::memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;
        char service[16];
        snprintf(service, sizeof(service), "%u", (unsigned)port);

        addrinfo *results = nullptr;
        int status = getaddrinfo(name.c_str(), service, &hints, &results);
        if (status != 0 || !results)
        {
            error = "cannot resolve '" + host + "'";
            return false;
        }
        // Prefer IPv4: it works on every LAN and through every router we care about.
        addrinfo *chosen = results;
        for (addrinfo *entry = results; entry; entry = entry->ai_next)
        {
            if (entry->ai_family == AF_INET)
            {
                chosen = entry;
                break;
            }
        }
        std::memset(out.storage, 0, sizeof(out.storage));
        std::memcpy(out.storage, chosen->ai_addr, chosen->ai_addrlen);
        out.length = (int)chosen->ai_addrlen;
        freeaddrinfo(results);
        return true;
    }

    bool IsLoopback(const NetAddress &address)
    {
        const sockaddr *family = (const sockaddr*)address.storage;
        if (!address.Valid()) return false;
        if (family->sa_family == AF_INET)
        {
            return (ntohl(((const sockaddr_in*)address.storage)->sin_addr.s_addr) >> 24) == 127;
        }
        if (family->sa_family == AF_INET6)
        {
            static const uint8_t loopback[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
            return std::memcmp(&((const sockaddr_in6*)address.storage)->sin6_addr, loopback, 16) == 0;
        }
        return false;
    }

    // An IPv4 address as IPv4-mapped IPv6 (::ffff:a.b.c.d), for sending from a dual-stack socket.
    static NetAddress MapToIpv6(const NetAddress &address)
    {
        const sockaddr_in *v4 = (const sockaddr_in*)address.storage;
        NetAddress mapped;
        sockaddr_in6 *v6 = (sockaddr_in6*)mapped.storage;
        v6->sin6_family = AF_INET6;
        v6->sin6_port = v4->sin_port;
        uint8_t *bytes = (uint8_t*)&v6->sin6_addr;
        bytes[10] = 0xff;
        bytes[11] = 0xff;
        std::memcpy(bytes + 12, &v4->sin_addr, 4);
        mapped.length = sizeof(sockaddr_in6);
        return mapped;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Socket
    // ------------------------------------------------------------------------------------------------------------

    static void CloseNative(NativeSocket socket)
    {
#ifdef _WIN32
        closesocket(socket);
#else
        close(socket);
#endif
    }

    static bool SetNonBlocking(NativeSocket socket)
    {
#ifdef _WIN32
        u_long enabled = 1;
        if (ioctlsocket(socket, FIONBIO, &enabled) != 0) return false;
        // Windows reports an earlier ICMP "port unreachable" as a receive error on UDP sockets; switch that off.
        BOOL reportReset = FALSE;
        DWORD returned = 0;
        WSAIoctl(socket, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &returned, nullptr, nullptr);
        return true;
#else
        int flags = fcntl(socket, F_GETFL, 0);
        return flags >= 0 && fcntl(socket, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
    }

    bool UdpSocket::Open(uint16_t port, bool loopbackOnly, std::string &error)
    {
        Close();
        if (!StartNetworking(error)) return false;

        // Dual-stack first, then plain IPv4. Loopback-only goes straight to IPv4 127.0.0.1.
        NativeSocket socket6 = loopbackOnly ? (NativeSocket)INVALID : ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (socket6 != (NativeSocket)INVALID)
        {
            int v6only = 0;
            setsockopt(socket6, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only));
            sockaddr_in6 address;
            std::memset(&address, 0, sizeof(address));
            address.sin6_family = AF_INET6;
            address.sin6_addr = in6addr_any;
            address.sin6_port = htons(port);
            if (bind(socket6, (const sockaddr*)&address, sizeof(address)) == 0 && SetNonBlocking(socket6))
            {
                handle = (intptr_t)socket6;
                ipv6 = true;
            }
            else
            {
                error = ErrorText("bind (IPv6)");
                CloseNative(socket6);
            }
        }
        if (handle == INVALID)
        {
            NativeSocket socket4 = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            if (socket4 == (NativeSocket)INVALID)
            {
                error = ErrorText("socket");
                return false;
            }
            sockaddr_in address;
            std::memset(&address, 0, sizeof(address));
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(loopbackOnly ? INADDR_LOOPBACK : INADDR_ANY);
            address.sin_port = htons(port);
            if (bind(socket4, (const sockaddr*)&address, sizeof(address)) != 0 || !SetNonBlocking(socket4))
            {
                error = ErrorText("bind");
                CloseNative(socket4);
                return false;
            }
            handle = (intptr_t)socket4;
            ipv6 = false;
        }

        sockaddr_storage bound;
        SockLen boundLength = sizeof(bound);
        localPort = port;
        if (getsockname((NativeSocket)handle, (sockaddr*)&bound, &boundLength) == 0)
        {
            if (bound.ss_family == AF_INET6) localPort = ntohs(((sockaddr_in6*)&bound)->sin6_port);
            else if (bound.ss_family == AF_INET) localPort = ntohs(((sockaddr_in*)&bound)->sin_port);
        }
        error.clear();
        return true;
    }

    void UdpSocket::Close()
    {
        if (handle != INVALID) CloseNative((NativeSocket)handle);
        handle = INVALID;
        localPort = 0;
    }

    bool UdpSocket::SendTo(const NetAddress &to, const uint8_t *data, size_t size)
    {
        if (handle == INVALID || !to.Valid()) return false;
        NetAddress target = to;
        const sockaddr *family = (const sockaddr*)to.storage;
        if (ipv6 && family->sa_family == AF_INET) target = MapToIpv6(to);
        if (!ipv6 && family->sa_family == AF_INET6)
        {
            lastError = "IPv6 address on an IPv4-only socket";
            return false;
        }
        int sent = (int)::sendto((NativeSocket)handle, (const char*)data, (int)size, 0, (const sockaddr*)target.storage,
                                 (SockLen)target.length);
        if (sent < 0)
        {
            lastError = ErrorText("sendto");
            return false;
        }
        return true;
    }

    int UdpSocket::ReceiveFrom(NetAddress &from, uint8_t *buffer, size_t capacity)
    {
        if (handle == INVALID) return -1;
        for (;;)
        {
            SockLen length = sizeof(from.storage);
            int received = (int)::recvfrom((NativeSocket)handle, (char*)buffer, (int)capacity, 0, (sockaddr*)from.storage,
                                           &length);
            if (received >= 0)
            {
                from.length = (int)length;
                // Keep one canonical form per peer: IPv4-mapped IPv6 becomes plain IPv4.
                const sockaddr_in6 *v6 = (const sockaddr_in6*)from.storage;
                const uint8_t *bytes = (const uint8_t*)&v6->sin6_addr;
                static const uint8_t prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
                if (v6->sin6_family == AF_INET6 && std::memcmp(bytes, prefix, 12) == 0)
                {
                    sockaddr_in v4;
                    std::memset(&v4, 0, sizeof(v4));
                    v4.sin_family = AF_INET;
                    v4.sin_port = v6->sin6_port;
                    std::memcpy(&v4.sin_addr, bytes + 12, 4);
                    std::memset(from.storage, 0, sizeof(from.storage));
                    std::memcpy(from.storage, &v4, sizeof(v4));
                    from.length = sizeof(v4);
                }
                return received;
            }
#ifdef _WIN32
            int code = WSAGetLastError();
            if (code == WSAEWOULDBLOCK) return 0;
            if (code == WSAECONNRESET || code == WSAEMSGSIZE) continue;   // stale ICMP report / oversized datagram
#else
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            if (errno == ECONNREFUSED || errno == EINTR) continue;
#endif
            lastError = ErrorText("recvfrom");
            return -1;
        }
    }
}
