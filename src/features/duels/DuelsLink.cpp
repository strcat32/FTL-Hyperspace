#include "DuelsLink.h"

#include <algorithm>

namespace Duels
{
    enum MessageKind : uint8_t
    {
        KIND_RELIABLE = 1,
        KIND_UNRELIABLE = 2
    };

    // Header flags
    static const uint8_t FLAG_ACKS = 1;   // the ack fields are valid (we have received something)

    static const double KEEPALIVE_MS = 100.0;
    static const double ACK_DELAY_MS = 15.0;         // acks go out at most about once per frame
    static const double CLOCK_WINDOW_MS = 10000.0;   // clock samples older than this are forgotten
    static const size_t MAX_PACKETS_PER_UPDATE = 16;
    static const uint16_t MAX_OUT_OF_ORDER = 1024;

    // True if sequence number a is newer than b (16-bit, wrapping).
    static bool Newer(uint16_t a, uint16_t b)
    {
        uint16_t diff = (uint16_t)(a - b);
        return diff != 0 && diff < 32768;
    }

    void Link::Reset(uint32_t id, double now)
    {
        *this = Link();
        sessionId = id;
        lastSend = -1.0;
        (void)now;
    }

    bool Link::SendReliable(const Bytes &message)
    {
        if (message.size() > MAX_MESSAGE) return false;
        Pending entry;
        entry.id = nextReliableId++;
        entry.data = message;
        pending.push_back(entry);
        return true;
    }

    bool Link::SendUnreliable(const Bytes &message)
    {
        if (message.size() > MAX_MESSAGE) return false;
        unreliable.push_back(message);
        return true;
    }

    double Link::ResendInterval() const
    {
        return std::max(40.0, std::min(1000.0, rtt * 1.5 + 20.0));
    }

    uint32_t Link::PeekSession(const uint8_t *data, size_t size)
    {
        if (size < HEADER_SIZE) return 0;
        Reader reader(data, size);
        if (reader.U16() != MAGIC || reader.U8() != WIRE_VERSION) return 0;
        reader.U8();
        return reader.U32();
    }

    void Link::Update(double now, std::vector<Bytes> &packets)
    {
        double resend = ResendInterval();
        size_t nextPending = 0;
        bool keepalive = lastSend < 0.0 || now - lastSend >= KEEPALIVE_MS || (ackPending && now - lastSend >= ACK_DELAY_MS);

        for (size_t built = 0; built < MAX_PACKETS_PER_UPDATE; ++built)
        {
            // Pick what goes into this packet.
            std::vector<Pending*> reliableOut;
            std::vector<Bytes> unreliableOut;
            size_t size = HEADER_SIZE;

            for (; nextPending < pending.size(); ++nextPending)
            {
                Pending &entry = pending[nextPending];
                if (entry.acked || (entry.lastSent >= 0.0 && now - entry.lastSent < resend)) continue;
                size_t needed = 5 + entry.data.size();
                if (size + needed > MAX_PACKET) break;
                size += needed;
                reliableOut.push_back(&entry);
                if (reliableOut.size() == 255) break;
            }
            while (!unreliable.empty() && reliableOut.size() + unreliableOut.size() < 255)
            {
                size_t needed = 3 + unreliable.front().size();
                if (size + needed > MAX_PACKET) break;
                size += needed;
                unreliableOut.push_back(unreliable.front());
                unreliable.pop_front();
            }

            if (reliableOut.empty() && unreliableOut.empty() && !keepalive) break;
            keepalive = false;

            // Header
            uint16_t seq = nextSeq++;
            Writer writer;
            writer.U16(MAGIC);
            writer.U8(WIRE_VERSION);
            writer.U8(haveRemote ? FLAG_ACKS : 0);
            writer.U32(sessionId);
            writer.U16(seq);
            writer.U16(remoteSeq);
            writer.U32(haveRemote ? remoteBits : 0);
            writer.F64(now);
            writer.F64(peerSendTime);
            writer.F32(peerSendTime >= 0.0 ? (float)(now - peerSendReceivedAt) : 0.f);
            writer.U8((uint8_t)(reliableOut.size() + unreliableOut.size()));

            SentPacket &record = sent[seq % 256];
            record.valid = true;
            record.seq = seq;
            record.reliableIds.clear();

            for (Pending *entry : reliableOut)
            {
                writer.U8(KIND_RELIABLE);
                writer.U16(entry->id);
                writer.U16((uint16_t)entry->data.size());
                writer.Bytes(entry->data.data(), entry->data.size());
                record.reliableIds.push_back(entry->id);
                if (entry->lastSent >= 0.0) ++stats.reliableResent;
                else ++stats.reliableSent;
                entry->lastSent = now;
            }
            for (const Bytes &message : unreliableOut)
            {
                writer.U8(KIND_UNRELIABLE);
                writer.U16((uint16_t)message.size());
                writer.Bytes(message.data(), message.size());
                ++stats.unreliableSent;
            }

            stats.bytesSent += (uint32_t)writer.data.size();
            ++stats.packetsSent;
            packets.push_back(writer.data);
            lastSend = now;
            ackPending = false;
        }
    }

    void Link::AckPacket(uint16_t seq)
    {
        SentPacket &record = sent[seq % 256];
        if (!record.valid || record.seq != seq) return;
        record.valid = false;
        for (uint16_t id : record.reliableIds)
        {
            for (Pending &entry : pending)
            {
                if (entry.id == id)
                {
                    entry.acked = true;
                    break;
                }
            }
        }
        while (!pending.empty() && pending.front().acked) pending.pop_front();
    }

    void Link::AddClockSample(double now, double rttSample, double offsetSample)
    {
        rtt = clockSamples.empty() ? rttSample : rtt * 0.875 + rttSample * 0.125;

        clockSamples.push_back(ClockSample{now, rttSample, offsetSample});
        while (clockSamples.size() > 1 && (now - clockSamples.front().time > CLOCK_WINDOW_MS || clockSamples.size() > 64))
        {
            clockSamples.pop_front();
        }

        // The sample with the lowest round trip had the least queueing, so its offset is the most trustworthy.
        const ClockSample *best = &clockSamples.front();
        for (const ClockSample &sample : clockSamples)
        {
            if (sample.rtt < best->rtt) best = &sample;
        }
        bestRtt = best->rtt;
        clockOffset = best->offset;
    }

    bool Link::Receive(const uint8_t *data, size_t size, double now, std::vector<Bytes> &delivered)
    {
        Reader reader(data, size);
        if (size < HEADER_SIZE || reader.U16() != MAGIC || reader.U8() != WIRE_VERSION)
        {
            ++stats.packetsRejected;
            return false;
        }
        uint8_t flags = reader.U8();
        if (reader.U32() != sessionId)
        {
            ++stats.packetsRejected;
            return false;
        }
        uint16_t seq = reader.U16();
        uint16_t ack = reader.U16();
        uint32_t ackBits = reader.U32();
        double sendTime = reader.F64();
        double echoTime = reader.F64();
        float echoDelay = reader.F32();
        uint8_t count = reader.U8();
        if (!reader.Ok())
        {
            ++stats.packetsRejected;
            return false;
        }

        ++stats.packetsReceived;
        stats.bytesReceived += (uint32_t)size;
        lastReceive = now;
        ackPending = true;

        // Which packets we have seen, for the acks we send back.
        bool duplicate = false;
        bool stale = false;
        if (!haveRemote)
        {
            haveRemote = true;
            remoteSeq = seq;
            remoteBits = 0;
        }
        else if (Newer(seq, remoteSeq))
        {
            uint16_t shift = (uint16_t)(seq - remoteSeq);
            remoteBits = shift >= 32 ? (shift == 32 ? 0x80000000u : 0u) : ((remoteBits << shift) | (1u << (shift - 1)));
            remoteSeq = seq;
        }
        else
        {
            uint16_t age = (uint16_t)(remoteSeq - seq);
            if (age == 0)
            {
                duplicate = true;
            }
            else if (age <= 32)
            {
                uint32_t bit = 1u << (age - 1);
                if (remoteBits & bit) duplicate = true;
                remoteBits |= bit;
            }
            else
            {
                stale = true;
            }
        }
        if (duplicate)
        {
            ++stats.duplicatePackets;
            return true;
        }

        // Their acks for our packets.
        if (flags & FLAG_ACKS)
        {
            AckPacket(ack);
            for (int bit = 0; bit < 32; ++bit)
            {
                if (ackBits & (1u << bit)) AckPacket((uint16_t)(ack - 1 - bit));
            }
        }

        // Clock: our timestamp came back after echoDelay ms at the peer.
        if (echoTime >= 0.0)
        {
            double rttSample = now - echoTime - echoDelay;
            if (rttSample >= 0.0 && rttSample < 10000.0)
            {
                AddClockSample(now, rttSample, sendTime - (now - rttSample * 0.5));
            }
        }
        if (peerSendTime < 0.0 || sendTime > peerSendTime)
        {
            peerSendTime = sendTime;
            peerSendReceivedAt = now;
        }

        // Messages
        for (uint8_t i = 0; i < count; ++i)
        {
            uint8_t kind = reader.U8();
            if (kind == KIND_RELIABLE)
            {
                uint16_t id = reader.U16();
                uint16_t length = reader.U16();
                const uint8_t *body = reader.Position();
                if (!reader.Skip(length)) break;

                uint16_t ahead = (uint16_t)(id - nextDeliverId);
                if (ahead >= 32768) continue;   // already delivered
                if (ahead == 0)
                {
                    delivered.push_back(Bytes(body, body + length));
                    ++stats.reliableDelivered;
                    ++nextDeliverId;
                    for (auto it = outOfOrder.find(nextDeliverId); it != outOfOrder.end(); it = outOfOrder.find(nextDeliverId))
                    {
                        delivered.push_back(it->second);
                        ++stats.reliableDelivered;
                        outOfOrder.erase(it);
                        ++nextDeliverId;
                    }
                }
                else if (ahead < MAX_OUT_OF_ORDER && outOfOrder.find(id) == outOfOrder.end())
                {
                    outOfOrder[id] = Bytes(body, body + length);
                }
            }
            else if (kind == KIND_UNRELIABLE)
            {
                uint16_t length = reader.U16();
                const uint8_t *body = reader.Position();
                if (!reader.Skip(length)) break;
                if (stale) continue;            // an old snapshot is worse than none
                delivered.push_back(Bytes(body, body + length));
                ++stats.unreliableDelivered;
            }
            else
            {
                break;
            }
        }
        return true;
    }
}
