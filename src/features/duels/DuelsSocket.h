#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// UDP sockets. Kept apart from the game headers: <winsock2.h> and FTL's headers don't mix.
namespace Duels
{
    // An IPv4 or IPv6 address with port (a sockaddr_storage underneath).
    struct NetAddress
    {
        uint8_t storage[128] = {0};
        int length = 0;

        bool Valid() const { return length > 0; }
        bool operator==(const NetAddress &other) const;
        bool operator!=(const NetAddress &other) const { return !(*this == other); }
        std::string ToString() const;
    };

    // Resolves a host name, IPv4 or IPv6 literal (IPv6 with an optional "%scope") and a port.
    bool ResolveAddress(const std::string &host, uint16_t port, NetAddress &out, std::string &error);

    // 127.x.x.x or ::1
    bool IsLoopback(const NetAddress &address);

    // Non-blocking UDP socket. It is dual-stack where the system allows it (IPv6 socket that also carries IPv4),
    // otherwise IPv4 only.
    class UdpSocket
    {
    public:
        UdpSocket() {}
        ~UdpSocket() { Close(); }

        // Port 0 = any free port. Loopback-only sockets serve two games on one computer and trigger no firewall
        // prompt; they are IPv4 (127.0.0.1).
        bool Open(uint16_t port, bool loopbackOnly, std::string &error);
        void Close();
        bool IsOpen() const { return handle != INVALID; }
        uint16_t LocalPort() const { return localPort; }

        bool SendTo(const NetAddress &to, const uint8_t *data, size_t size);
        // Size of the datagram received, 0 if nothing is waiting, -1 on a real error.
        int ReceiveFrom(NetAddress &from, uint8_t *buffer, size_t capacity);

        std::string LastError() const { return lastError; }

    private:
        UdpSocket(const UdpSocket &);
        UdpSocket &operator=(const UdpSocket &);

        static const intptr_t INVALID = -1;
        intptr_t handle = INVALID;
        bool ipv6 = false;
        uint16_t localPort = 0;
        std::string lastError;
    };
}
