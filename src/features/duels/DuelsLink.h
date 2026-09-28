#pragma once

#include "DuelsWire.h"

#include <cstdint>
#include <deque>
#include <map>
#include <vector>

namespace Duels
{
    // A connection between two peers over an unreliable datagram transport (UDP):
    //  - reliable messages arrive exactly once and in order; they are resent until acknowledged,
    //  - unreliable messages ride along in the next packet and may be lost (state snapshots),
    //  - every packet carries acknowledgements and timestamps. From those the link estimates the round-trip time and
    //    the offset between the two peers' clocks (NTP-style, trusting the samples with the lowest round trip).
    // It knows nothing about sockets or the game: packets go in and out as byte vectors. Times are milliseconds.
    class Link
    {
    public:
        typedef std::vector<uint8_t> Bytes;

        static const uint16_t MAGIC = 0x4446;      // "FD"
        static const uint8_t WIRE_VERSION = 1;
        static const size_t MAX_PACKET = 1200;     // below common path MTUs, so packets are never fragmented
        static const size_t HEADER_SIZE = 37;
        static const size_t MAX_MESSAGE = MAX_PACKET - HEADER_SIZE - 5;

        void Reset(uint32_t sessionId, double now);
        uint32_t SessionId() const { return sessionId; }

        // Queue a message; false if it can't fit into one packet.
        bool SendReliable(const Bytes &message);
        bool SendUnreliable(const Bytes &message);

        // The packets to send now: new reliable messages, reliable messages due for a resend, unreliable messages,
        // and a keepalive when nothing went out for a while (acks and clock samples ride on every packet).
        void Update(double now, std::vector<Bytes> &packets);

        // Session id of a Duels packet, or 0 if the data is not one.
        static uint32_t PeekSession(const uint8_t *data, size_t size);

        // Processes one received packet and appends the messages it delivers, in order.
        // Returns false for packets that are malformed or belong to another session.
        bool Receive(const uint8_t *data, size_t size, double now, std::vector<Bytes> &delivered);

        double LastReceiveTime() const { return lastReceive; }
        bool HasReceived() const { return lastReceive >= 0.0; }
        double RttMs() const { return rtt; }                 // smoothed round-trip time
        double BestRttMs() const { return bestRtt; }         // lowest recent sample; the clock offset comes from it
        bool HasClock() const { return !clockSamples.empty(); }
        double ClockOffsetMs() const { return clockOffset; } // peer clock minus local clock
        size_t PendingReliable() const { return pending.size(); }

        struct Stats
        {
            uint32_t packetsSent = 0;
            uint32_t packetsReceived = 0;
            uint32_t packetsRejected = 0;
            uint32_t duplicatePackets = 0;
            uint32_t reliableSent = 0;
            uint32_t reliableResent = 0;
            uint32_t reliableDelivered = 0;
            uint32_t unreliableSent = 0;
            uint32_t unreliableDelivered = 0;
            uint32_t bytesSent = 0;
            uint32_t bytesReceived = 0;
        };
        const Stats &GetStats() const { return stats; }

    private:
        struct Pending
        {
            uint16_t id = 0;
            Bytes data;
            double lastSent = -1.0;
            bool acked = false;
        };

        struct SentPacket
        {
            bool valid = false;
            uint16_t seq = 0;
            std::vector<uint16_t> reliableIds;
        };

        struct ClockSample
        {
            double time;
            double rtt;
            double offset;
        };

        void AckPacket(uint16_t seq);
        void AddClockSample(double now, double rttSample, double offsetSample);
        double ResendInterval() const;

        uint32_t sessionId = 0;

        // Sending
        uint16_t nextSeq = 0;
        uint16_t nextReliableId = 0;
        std::deque<Pending> pending;
        std::deque<Bytes> unreliable;
        SentPacket sent[256];
        double lastSend = -1.0;

        // Receiving
        bool haveRemote = false;
        uint16_t remoteSeq = 0;
        uint32_t remoteBits = 0;
        bool ackPending = false;
        uint16_t nextDeliverId = 0;
        std::map<uint16_t, Bytes> outOfOrder;
        double lastReceive = -1.0;

        // Clock
        double peerSendTime = -1.0;      // send time of the newest packet received, in the peer's clock
        double peerSendReceivedAt = -1.0;
        double rtt = 200.0;
        double bestRtt = -1.0;
        double clockOffset = 0.0;
        std::deque<ClockSample> clockSamples;

        Stats stats;
    };
}
