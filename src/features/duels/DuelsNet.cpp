#include "DuelsNet.h"
#include "Duels.h"
#include "DuelsLink.h"
#include "DuelsSocket.h"
#include "DuelsTrace.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <random>

namespace Duels
{
    namespace Net
    {
        enum SessionMessage : uint8_t
        {
            MSG_HELLO = 1,     // joiner -> host: protocol, version, build, name
            MSG_WELCOME = 2,   // host -> joiner: the same, accepted
            MSG_REJECT = 3,    // host -> joiner: reason
            MSG_BYE = 4        // either side: reason
        };

        static const double JOIN_TIMEOUT_MS = 10000.0;
        static const double SILENCE_TIMEOUT_MS = 10000.0;

        struct DelayedPacket
        {
            double releaseAt;
            Link::Bytes data;
        };

        struct Session
        {
            Listener *listener = nullptr;
            std::string name = "player";
            std::string version;
            std::string build;

            Phase phase = Phase::Idle;
            bool host = false;
            UdpSocket socket;
            Link link;
            NetAddress peer;
            std::string peerName;
            double phaseStart = 0.0;
            double now = 0.0;

            // Test conditions
            double delayMs = 0.0;
            double jitterMs = 0.0;
            double lossPercent = 0.0;
            std::deque<DelayedPacket> outbox;
            std::mt19937 random;
            uint32_t simulatedLosses = 0;
        };

        static Session g_session;

        static uint32_t NewSessionId()
        {
            // std::random_device is deterministic on some MinGW versions; mix in the clock instead.
            uint64_t ticks = (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count();
            std::mt19937 generator((uint32_t)(ticks ^ (ticks >> 32)) ^ (uint32_t)(uintptr_t)&ticks);
            uint32_t id = generator();
            return id == 0 ? 1 : id;
        }

        static void SetPhase(Phase phase)
        {
            g_session.phase = phase;
            g_session.phaseStart = g_session.now;
        }

        static void Close()
        {
            g_session.socket.Close();
            g_session.outbox.clear();
            g_session.peer = NetAddress();
            g_session.peerName.clear();
            SetPhase(Phase::Idle);
        }

        static void SendControl(uint8_t type, const Writer &body)
        {
            Writer message;
            message.U8(type);
            message.Bytes(body.data.data(), body.data.size());
            g_session.link.SendReliable(message.data);
        }

        static void WriteIdentity(Writer &writer)
        {
            writer.U16(PROTOCOL_VERSION);
            writer.Str(g_session.version);
            writer.Str(g_session.build);
            writer.Str(g_session.name);
        }

        // Pushes the link's packets through the simulated conditions to the socket.
        static void Flush()
        {
            Session &s = g_session;
            std::vector<Link::Bytes> packets;
            s.link.Update(s.now, packets);
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            for (Link::Bytes &packet : packets)
            {
                if (s.lossPercent > 0.0 && unit(s.random) * 100.0 < s.lossPercent)
                {
                    ++s.simulatedLosses;
                    continue;
                }
                double delay = s.delayMs;
                if (s.jitterMs > 0.0) delay += (unit(s.random) * 2.0 - 1.0) * s.jitterMs;
                if (delay <= 0.0 && s.outbox.empty())
                {
                    s.socket.SendTo(s.peer, packet.data(), packet.size());
                    continue;
                }
                DelayedPacket delayed;
                delayed.releaseAt = s.now + (delay > 0.0 ? delay : 0.0);
                delayed.data.swap(packet);
                // Keep the outbox sorted by release time (jitter can reorder packets, as real networks do).
                auto it = s.outbox.end();
                while (it != s.outbox.begin() && (it - 1)->releaseAt > delayed.releaseAt) --it;
                s.outbox.insert(it, delayed);
            }
            while (!s.outbox.empty() && s.outbox.front().releaseAt <= s.now)
            {
                s.socket.SendTo(s.peer, s.outbox.front().data.data(), s.outbox.front().data.size());
                s.outbox.pop_front();
            }
        }

        static void Disconnect(const std::string &reason, bool notifyPeer)
        {
            Session &s = g_session;
            Phase before = s.phase;
            if (notifyPeer && s.peer.Valid() && (before == Phase::Connected || before == Phase::Joining))
            {
                Writer body;
                body.Str(reason);
                SendControl(MSG_BYE, body);
                s.outbox.clear();
                double saved = s.delayMs;
                s.delayMs = 0.0;
                s.jitterMs = 0.0;
                Flush();
                s.delayMs = saved;
            }
            Log("Net: disconnected (%s)", reason.c_str());
            Close();
            if (before == Phase::Connected && s.listener) s.listener->OnDisconnected(reason);
        }

        static bool CheckIdentity(Reader &reader, std::string &peerName, std::string &problem)
        {
            uint16_t protocol = reader.U16();
            std::string version = reader.Str();
            std::string build = reader.Str();
            peerName = reader.Str();
            if (!reader.Ok())
            {
                problem = "malformed handshake";
                return false;
            }
            if (protocol != PROTOCOL_VERSION || version != g_session.version)
            {
                char buffer[200];
                snprintf(buffer, sizeof(buffer), "version mismatch: FTL: Duels %s (protocol %u) vs %s (protocol %u)",
                         g_session.version.c_str(), (unsigned)PROTOCOL_VERSION, version.c_str(), (unsigned)protocol);
                problem = buffer;
                return false;
            }
            if (build != g_session.build)
            {
                Log("Net: warning: peer runs build %s, we run %s", build.c_str(), g_session.build.c_str());
            }
            return true;
        }

        static void HandleMessage(const Link::Bytes &bytes)
        {
            Session &s = g_session;
            Reader reader(bytes);
            uint8_t type = reader.U8();
            if (!reader.Ok()) return;

            if (type == MSG_HELLO && s.host && s.phase == Phase::Hosting)
            {
                std::string peerName, problem;
                if (!CheckIdentity(reader, peerName, problem))
                {
                    Log("Net: rejecting %s: %s", s.peer.ToString().c_str(), problem.c_str());
                    Writer body;
                    body.Str(problem);
                    SendControl(MSG_REJECT, body);
                    Flush();
                    // Back to waiting for someone else.
                    s.peer = NetAddress();
                    s.link.Reset(0, s.now);
                    return;
                }
                s.peerName = peerName;
                Writer body;
                WriteIdentity(body);
                SendControl(MSG_WELCOME, body);
                SetPhase(Phase::Connected);
                Log("Net: %s joined from %s", peerName.c_str(), s.peer.ToString().c_str());
                if (s.listener) s.listener->OnConnected();
                return;
            }
            if (type == MSG_WELCOME && !s.host && s.phase == Phase::Joining)
            {
                std::string peerName, problem;
                if (!CheckIdentity(reader, peerName, problem))
                {
                    Disconnect(problem, true);
                    return;
                }
                s.peerName = peerName;
                SetPhase(Phase::Connected);
                Log("Net: joined %s at %s", peerName.c_str(), s.peer.ToString().c_str());
                if (s.listener) s.listener->OnConnected();
                return;
            }
            if (type == MSG_REJECT && !s.host)
            {
                std::string reason = reader.Str();
                Log("Net: the host refused: %s", reason.c_str());
                Close();
                return;
            }
            if (type == MSG_BYE)
            {
                std::string reason = reader.Str();
                Disconnect("the other player left: " + reason, false);
                return;
            }
            if (type >= FIRST_GAME_MESSAGE && s.phase == Phase::Connected && s.listener)
            {
                s.listener->OnMessage(type, reader);
            }
        }

        // ------------------------------------------------------------------------------------------------------------

        void SetListener(Listener *listener)
        {
            g_session.listener = listener;
        }

        void SetIdentity(const std::string &playerName, const std::string &version, const std::string &build)
        {
            g_session.name = playerName;
            g_session.version = version;
            g_session.build = build;
        }

        bool Host(uint16_t port, bool loopbackOnly, std::string &message)
        {
            Session &s = g_session;
            if (s.phase != Phase::Idle) Disconnect("starting a new session", true);
            std::string error;
            if (!s.socket.Open(port, loopbackOnly, error))
            {
                message = "cannot listen on UDP port " + std::to_string(port) + ": " + error;
                return false;
            }
            s.host = true;
            s.peer = NetAddress();
            s.link.Reset(0, s.now);
            s.random.seed(NewSessionId());
            SetPhase(Phase::Hosting);
            message = "hosting on UDP port " + std::to_string(s.socket.LocalPort()) + (loopbackOnly ? " (this computer only)" : "");
            Log("Net: %s", message.c_str());
            return true;
        }

        bool Join(const std::string &hostName, uint16_t port, std::string &message)
        {
            Session &s = g_session;
            if (s.phase != Phase::Idle) Disconnect("starting a new session", true);
            NetAddress address;
            std::string error;
            if (!ResolveAddress(hostName, port, address, error))
            {
                message = error;
                return false;
            }
            if (!s.socket.Open(0, IsLoopback(address), error))
            {
                message = "cannot open a UDP socket: " + error;
                return false;
            }
            s.host = false;
            s.peer = address;
            s.link.Reset(NewSessionId(), s.now);
            s.random.seed(NewSessionId());
            Writer body;
            WriteIdentity(body);
            SendControl(MSG_HELLO, body);
            SetPhase(Phase::Joining);
            message = "joining " + address.ToString();
            Log("Net: %s (local port %u)", message.c_str(), (unsigned)s.socket.LocalPort());
            return true;
        }

        void Leave(const std::string &reason)
        {
            if (g_session.phase == Phase::Idle) return;
            Disconnect(reason, true);
        }

        void Update(double now)
        {
            Session &s = g_session;
            s.now = now;
            if (s.phase == Phase::Idle) return;

            uint8_t buffer[2048];
            std::vector<Link::Bytes> delivered;
            for (int guard = 0; guard < 256 && s.socket.IsOpen(); ++guard)
            {
                NetAddress from;
                int size = s.socket.ReceiveFrom(from, buffer, sizeof(buffer));
                if (size <= 0) break;

                if (s.phase == Phase::Hosting && !s.peer.Valid())
                {
                    // The first well-formed packet opens the session with its sender.
                    uint32_t sessionId = Link::PeekSession(buffer, (size_t)size);
                    if (sessionId == 0) continue;
                    s.peer = from;
                    s.link.Reset(sessionId, now);
                    Log("Net: packet from %s, starting the handshake", from.ToString().c_str());
                }
                if (from != s.peer) continue;   // strangers are ignored while a session runs

                delivered.clear();
                if (!s.link.Receive(buffer, (size_t)size, now, delivered)) continue;
                for (const Link::Bytes &message : delivered)
                {
                    HandleMessage(message);
                    if (s.phase == Phase::Idle) return;
                }
            }

            if (s.phase == Phase::Hosting && s.peer.Valid() && now - s.link.LastReceiveTime() > JOIN_TIMEOUT_MS)
            {
                // A handshake that never completed: wait for the next player instead.
                Log("Net: %s went quiet during the handshake", s.peer.ToString().c_str());
                s.peer = NetAddress();
                s.link.Reset(0, now);
            }
            if (s.phase == Phase::Joining && now - s.phaseStart > JOIN_TIMEOUT_MS)
            {
                Log("Net: no answer from %s", s.peer.ToString().c_str());
                Close();
                return;
            }
            if (s.phase == Phase::Connected && s.link.HasReceived() && now - s.link.LastReceiveTime() > SILENCE_TIMEOUT_MS)
            {
                Disconnect("connection lost", false);
                return;
            }
            if (s.peer.Valid()) Flush();
        }

        Phase GetPhase() { return g_session.phase; }
        bool IsConnected() { return g_session.phase == Phase::Connected; }
        bool IsHost() { return g_session.host; }
        std::string PeerName() { return g_session.peerName; }

        std::string Status()
        {
            const Session &s = g_session;
            const Link::Stats &st = s.link.GetStats();
            char buffer[512];
            const char *phase = s.phase == Phase::Idle ? "idle" : s.phase == Phase::Hosting ? "hosting"
                              : s.phase == Phase::Joining ? "joining" : "connected";
            snprintf(buffer, sizeof(buffer),
                     "%s%s port %u peer %s (%s) rtt %.1f ms (best %.1f) clock %+.1f ms | sent %u pkts, recv %u, "
                     "resent %u msgs, pending %u | sim %.0f+-%.0f ms %.1f%% loss (%u dropped)",
                     phase, s.host ? " (host)" : "", (unsigned)s.socket.LocalPort(), s.peer.ToString().c_str(),
                     s.peerName.empty() ? "-" : s.peerName.c_str(), s.link.RttMs(), s.link.BestRttMs(),
                     s.link.ClockOffsetMs(), st.packetsSent, st.packetsReceived, st.reliableResent,
                     (unsigned)s.link.PendingReliable(), s.delayMs, s.jitterMs, s.lossPercent, s.simulatedLosses);
            return buffer;
        }

        bool Send(uint8_t type, const Writer &body, bool reliable)
        {
            if (g_session.phase != Phase::Connected || type < FIRST_GAME_MESSAGE) return false;
            Writer message;
            message.U8(type);
            message.Bytes(body.data.data(), body.data.size());
            return reliable ? g_session.link.SendReliable(message.data) : g_session.link.SendUnreliable(message.data);
        }

        bool HasClock() { return g_session.link.HasClock(); }
        double PeerToLocalTime(double peerTime) { return peerTime - g_session.link.ClockOffsetMs(); }
        double LocalToPeerTime(double localTime) { return localTime + g_session.link.ClockOffsetMs(); }
        double RttMs() { return g_session.link.RttMs(); }

        void Simulate(double delayMs, double jitterMs, double lossPercent)
        {
            g_session.delayMs = delayMs < 0.0 ? 0.0 : delayMs;
            g_session.jitterMs = jitterMs < 0.0 ? 0.0 : jitterMs;
            g_session.lossPercent = lossPercent < 0.0 ? 0.0 : (lossPercent > 100.0 ? 100.0 : lossPercent);
            Log("Net: simulating %.0f ms +- %.0f ms delay and %.1f%% loss on outgoing packets", g_session.delayMs,
                g_session.jitterMs, g_session.lossPercent);
        }
    }
}
