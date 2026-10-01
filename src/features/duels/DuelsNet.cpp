#include "DuelsNet.h"
#include "Duels.h"
#include "DuelsDemo.h"
#include "DuelsLink.h"
#include "DuelsRelay.h"
#include "DuelsSocket.h"
#include "DuelsTrace.h"

#include <chrono>
#include <cstdio>
#include <deque>
#include <memory>
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
        // Through a relay, after the other player went quiet: whether we still reach the relay ourselves (its
        // heartbeat answers come every 3 s while nothing else does), and how long to wait for the relay to say the
        // other player is gone.
        static const double RELAY_CONTACT_MS = 8000.0;
        static const double RELAY_VERDICT_WAIT_MS = 5000.0;

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
            std::string dataHash;      // the game's data (roadmap 4.1)

            Phase phase = Phase::Idle;
            bool host = false;
            UdpSocket socket;
            Link link;
            NetAddress peer;
            std::string peerName;
            double phaseStart = 0.0;
            double now = 0.0;

            // Through a relay: `peer` is the relay then, and link packets travel inside its DATA packets.
            bool relay = false;
            Relay::Client relayClient;
            std::string relayCode;
            std::string relayServer;   // the relay's address as typed
            bool debug = false;        // ours, and the other player's from the handshake
            bool peerDebug = false;

            // Test conditions
            double delayMs = 0.0;
            double jitterMs = 0.0;
            double lossPercent = 0.0;
            std::deque<DelayedPacket> outbox;
            std::mt19937 random;
            uint32_t simulatedLosses = 0;
            bool cut = false;          // test: nothing goes out or comes in (a pulled cable), the relay's packets too
            uint32_t cutPackets = 0;

            // Coming back to a match after a lost connection. `host` stays the match's role; `acceptsJoin` is who
            // answers a handshake (the host, or whoever waits for the other to come back).
            bool acceptsJoin = false;
            uint64_t matchToken = 0;
            uint64_t peerToken = 0;
            bool resumed = false;
            enum class Lost { None, Waiting, Rejoining };
            Lost lost = Lost::None;
            double lostAt = 0.0;
            // A replay (DuelsDemo.cpp): connected without a socket.
            bool replay = false;
            double replayClock = 0.0;   // the recorded opponent's clock less ours
            double replayHostClock = 0.0;   // the recorded host's clock less ours (the recorder's own, if it hosted)
            double nextAttempt = 0.0;
            uint32_t attempts = 0;
            // How to come back: the relay's room (with its password as typed), or the host's address.
            bool backViaRelay = false;
            std::string backServer;
            uint16_t backPort = 0;
            std::string backCode;
            std::string relayPassword;
            bool roomPrivate = false;   // the room we host isn't in the relay's room list
        };

        static Session g_session;

        // A look at the relay's list of open rooms, with a socket of its own.
        struct Browser
        {
            UdpSocket socket;
            NetAddress relay;
            Relay::Client client;
            bool active = false;
        };

        static Browser g_browser;

        // The room search over every relay of the list (SearchRooms): one searcher per relay.
        struct Searcher
        {
            RelayAddress address;
            UdpSocket socket;
            NetAddress relay;
            Relay::Client client;
            bool active = false;
        };

        static std::vector<std::unique_ptr<Searcher>> g_searchers;
        static RoomSearch g_search;
        static const int SEARCH_PAGES = 10;

        // The last error a relay gave the session's room.
        static int g_lastRelayError = 0;
        static std::string g_lastRelayErrorText;

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
            Session &s = g_session;
            // Leaving a relay room at once, rather than letting it time out.
            std::vector<uint8_t> leave;
            if (s.relay && s.relayClient.Leave(leave) && s.socket.IsOpen() && !s.cut) s.socket.SendTo(s.peer, leave.data(), leave.size());
            s.relay = false;
            s.relayClient.Reset();
            s.relayCode.clear();
            s.socket.Close();
            s.outbox.clear();
            s.peer = NetAddress();
            s.peerName.clear();
            SetPhase(Phase::Idle);
        }

        static void Notice(const std::string &text)
        {
            Log("Net: %s", text.c_str());
            if (g_session.listener) g_session.listener->OnNotice(text);
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
            writer.U8(g_session.debug ? 1 : 0);   // flags: 1 = debug mode
            writer.U32((uint32_t)(g_session.matchToken & 0xffffffffu));   // the match to continue (0: a new one)
            writer.U32((uint32_t)(g_session.matchToken >> 32));
            writer.Str(g_session.dataHash);
        }

        // Pushes the link's packets through the simulated conditions to the socket (through the relay: inside its
        // DATA packets, and with its handshake and pings).
        static void Flush()
        {
            Session &s = g_session;
            std::vector<Link::Bytes> packets;
            if (s.relay)
            {
                std::vector<std::vector<uint8_t>> control;
                std::vector<Relay::Event> events;
                s.relayClient.Update(s.now, control, events);
                for (const std::vector<uint8_t> &packet : control)
                {
                    if (s.cut) ++s.cutPackets;
                    else s.socket.SendTo(s.peer, packet.data(), packet.size());
                }
                if (!events.empty())
                {
                    g_lastRelayError = events.front().errorCode;
                    g_lastRelayErrorText = events.front().text;
                    Notice("relay: " + events.front().text);
                    Close();
                    return;
                }
                // Nothing for the other player before the room exists (reliable messages wait in the link).
                if (s.relayClient.GetState() != Relay::State::InRoom) return;
            }
            s.link.Update(s.now, packets);
            if (s.relay)
            {
                for (Link::Bytes &packet : packets)
                {
                    Link::Bytes wrapped;
                    if (s.relayClient.Wrap(packet.data(), packet.size(), wrapped)) packet.swap(wrapped);
                    else packet.clear();
                }
            }
            std::uniform_real_distribution<double> unit(0.0, 1.0);
            for (Link::Bytes &packet : packets)
            {
                if (packet.empty()) continue;
                if (s.lossPercent > 0.0 && unit(s.random) * 100.0 < s.lossPercent)
                {
                    ++s.simulatedLosses;
                    continue;
                }
                double delay = s.delayMs;
                if (s.jitterMs > 0.0) delay += (unit(s.random) * 2.0 - 1.0) * s.jitterMs;
                if (s.cut)
                {
                    ++s.cutPackets;
                    continue;
                }
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

        static void Disconnect(const std::string &reason, bool notifyPeer, bool opponentGone = false)
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
            bool waited = s.lost != Session::Lost::None;
            s.lost = Session::Lost::None;
            Close();
            if ((before == Phase::Connected || waited) && s.listener) s.listener->OnDisconnected(reason, opponentGone);
        }

        static const double REJOIN_RETRY_MS = 3000.0;

        // The connection is gone. With a match to continue (SetMatchToken), the session doesn't end: it waits for the
        // other player to come back, or tries to come back itself (cutOff), until REJOIN_GRACE_MS is over.
        static void LoseConnection(const std::string &reason, bool cutOff, bool opponentGone)
        {
            Session &s = g_session;
            if (s.matchToken == 0 || s.phase != Phase::Connected)
            {
                Disconnect(reason, false, opponentGone);
                return;
            }
            s.lostAt = s.now;
            s.attempts = 0;
            s.resumed = false;
            Log("Net: %s; the match waits %.0f s for %s", reason.c_str(), REJOIN_GRACE_MS / 1000.0,
                cutOff ? "us to come back" : "the other player to come back");
            if (cutOff)
            {
                // Back the same way: the same room at the relay, or the host's address.
                s.backViaRelay = s.relay;
                if (s.relay) s.backCode = s.relayCode;
                s.lost = Session::Lost::Rejoining;
                s.nextAttempt = s.now + REJOIN_RETRY_MS;
                Close();
            }
            else
            {
                // Through a relay the room stays open for them; directly, the first packet from whoever comes back
                // opens the session again (only the same match's player is let in, HandleMessage).
                s.lost = Session::Lost::Waiting;
                s.acceptsJoin = true;
                s.link.Reset(0, s.now);
                if (!s.relay) s.peer = NetAddress();
                s.outbox.clear();
                SetPhase(Phase::Hosting);
            }
            if (s.listener) s.listener->OnConnectionLost(reason, cutOff);
        }

        static bool CheckIdentity(Reader &reader, std::string &peerName, std::string &problem)
        {
            uint16_t protocol = reader.U16();
            std::string version = reader.Str();
            std::string build = reader.Str();
            peerName = reader.Str();
            uint8_t flags = reader.U8();
            uint64_t tokenLow = reader.U32();
            uint64_t tokenHigh = reader.U32();
            std::string dataHash = reader.Str();
            if (!reader.Ok())
            {
                problem = "malformed handshake";
                return false;
            }
            g_session.peerDebug = (flags & 1) != 0;
            g_session.peerToken = tokenLow | (tokenHigh << 32);
            if (protocol != PROTOCOL_VERSION || version != g_session.version)
            {
                char buffer[200];
                snprintf(buffer, sizeof(buffer), "version mismatch: FTL: Duels %s (protocol %u) vs %s (protocol %u)",
                         g_session.version.c_str(), (unsigned)PROTOCOL_VERSION, version.c_str(), (unsigned)protocol);
                problem = buffer;
                return false;
            }
            if (dataHash != g_session.dataHash)
            {
                // Changed weapons, drones, augments or ships would change the fight (roadmap 4.1, layer 5). (The text
                // goes to the other side too: it names the host's and the guest's.)
                std::string ours = g_session.dataHash.empty() ? std::string("-") : g_session.dataHash;
                std::string theirs = dataHash.empty() ? std::string("-") : dataHash;
                bool host = IsHost();
                problem = "the game's data differs (weapons, drones, augments or player ships): the host's " + (host ? ours : theirs) +
                          ", the guest's " + (host ? theirs : ours);
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

            if (type == MSG_HELLO && s.acceptsJoin && s.phase == Phase::Hosting)
            {
                std::string peerName, problem;
                bool accepted = CheckIdentity(reader, peerName, problem);
                // A match waiting for its other player lets only them back in.
                if (accepted && s.lost == Session::Lost::Waiting && (s.peerToken == 0 || s.peerToken != s.matchToken))
                {
                    accepted = false;
                    problem = "a match is waiting for its other player to come back";
                }
                if (!accepted)
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
                s.resumed = s.matchToken != 0 && s.peerToken == s.matchToken;
                s.lost = Session::Lost::None;
                Writer body;
                WriteIdentity(body);
                SendControl(MSG_WELCOME, body);
                SetPhase(Phase::Connected);
                Log("Net: %s %s from %s", peerName.c_str(), s.resumed ? "came back" : "joined", s.peer.ToString().c_str());
                if (s.listener) s.listener->OnConnected();
                return;
            }
            if (type == MSG_WELCOME && !s.acceptsJoin && s.phase == Phase::Joining)
            {
                std::string peerName, problem;
                bool accepted = CheckIdentity(reader, peerName, problem);
                // Coming back, it must be our match that is waiting there.
                if (accepted && s.lost == Session::Lost::Rejoining && s.peerToken != s.matchToken)
                {
                    accepted = false;
                    problem = "the other game is in another match now";
                }
                if (!accepted)
                {
                    Disconnect(problem, true);
                    return;
                }
                s.peerName = peerName;
                s.resumed = s.matchToken != 0 && s.peerToken == s.matchToken;
                s.lost = Session::Lost::None;
                SetPhase(Phase::Connected);
                Log("Net: %s %s at %s", s.resumed ? "back with" : "joined", peerName.c_str(), s.peer.ToString().c_str());
                if (s.listener) s.listener->OnConnected();
                return;
            }
            if (type == MSG_REJECT && !s.acceptsJoin)
            {
                std::string reason = reader.Str();
                Log("Net: the host refused: %s", reason.c_str());
                // Coming back, the next try may work (the other side may not be waiting yet).
                Close();
                return;
            }
            if (type == MSG_BYE)
            {
                std::string reason = reader.Str();
                Disconnect("the other player left: " + reason, false, true);
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

        void SetGameData(const std::string &hash) { g_session.dataHash = hash; }

        void SetDebugFlag(bool debug) { g_session.debug = debug; }
        bool PeerDebug() { return g_session.peerDebug; }
        void SetPeerDebug() { g_session.peerDebug = true; }

        // A try to come back to a match (UpdateLost) goes through Join and JoinRelay: the match's role and the way
        // back stay as they were.
        static bool g_attempt = false;

        bool Host(uint16_t port, bool loopbackOnly, std::string &message)
        {
            Session &s = g_session;
            if (s.phase != Phase::Idle || s.lost != Session::Lost::None) Disconnect("starting a new session", true);
            std::string error;
            if (!s.socket.Open(port, loopbackOnly, error))
            {
                message = "cannot listen on UDP port " + std::to_string(port) + ": " + error;
                return false;
            }
            s.host = true;
            s.acceptsJoin = true;
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
            if (s.phase != Phase::Idle || (!g_attempt && s.lost != Session::Lost::None)) Disconnect("starting a new session", true);
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
            if (!g_attempt)
            {
                s.host = false;
                s.backServer = hostName;
                s.backPort = port;
            }
            s.acceptsJoin = false;
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

        static bool OpenRelay(const std::string &server, uint16_t port, std::string &message)
        {
            Session &s = g_session;
            if (s.phase != Phase::Idle || (!g_attempt && s.lost != Session::Lost::None)) Disconnect("starting a new session", true);
            NetAddress address;
            std::string error;
            if (!ResolveAddress(server, port, address, error))
            {
                message = error;
                return false;
            }
            if (!s.socket.Open(0, IsLoopback(address), error))
            {
                message = "cannot open a UDP socket: " + error;
                return false;
            }
            s.relay = true;
            s.peer = address;
            if (!g_attempt)
            {
                s.backServer = server;
                s.backPort = port;
            }
            // As typed, for the other player (with the port if it isn't the usual one; an IPv6 address in brackets).
            s.relayServer = server;
            if (port != Relay::DEFAULT_PORT)
            {
                s.relayServer = (server.find(':') != std::string::npos ? "[" + server + "]" : server) + ":" + std::to_string(port);
            }
            s.random.seed(NewSessionId());
            return true;
        }

        bool HostRelay(const std::string &server, uint16_t port, const std::string &roomName, const std::string &password,
                       bool listed, std::string &message)
        {
            Session &s = g_session;
            g_lastRelayError = 0;
            g_lastRelayErrorText.clear();
            if (!OpenRelay(server, port, message)) return false;
            s.host = true;
            s.acceptsJoin = true;
            s.relayPassword = password;
            s.roomPrivate = !listed;
            s.link.Reset(0, s.now);
            s.relayClient.Create(s.name, s.version, roomName, password, listed, s.now);
            SetPhase(Phase::Hosting);
            message = "asking the relay " + s.peer.ToString() + " for a room";
            Log("Net: %s", message.c_str());
            return true;
        }

        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, const std::string &password,
                       std::string &message)
        {
            Session &s = g_session;
            g_lastRelayError = 0;
            g_lastRelayErrorText.clear();
            if (!Relay::Client::IsRoomCode(code))
            {
                message = "a room code has 6 letters and digits, e.g. K7M4QX";
                return false;
            }
            if (!OpenRelay(server, port, message)) return false;
            if (!g_attempt)
            {
                s.host = false;
                s.relayPassword = password;
            }
            s.acceptsJoin = false;
            s.link.Reset(NewSessionId(), s.now);
            s.relayClient.Join(code, password, s.name, s.version, s.now);
            SetPhase(Phase::Joining);
            message = "joining room " + code + " at the relay " + s.peer.ToString();
            Log("Net: %s", message.c_str());
            return true;
        }

        bool ListRelayRooms(const std::string &server, uint16_t port, int page, std::string &message)
        {
            Browser &b = g_browser;
            b.socket.Close();
            b.active = false;
            NetAddress address;
            std::string error;
            if (!ResolveAddress(server, port, address, error))
            {
                message = error;
                return false;
            }
            if (!b.socket.Open(0, IsLoopback(address), error))
            {
                message = "cannot open a UDP socket: " + error;
                return false;
            }
            b.relay = address;
            b.client.List(page, g_session.version, g_session.now);
            b.active = true;
            message = "asking the relay " + address.ToString() + " for its open rooms";
            Log("Net: %s", message.c_str());
            return true;
        }

        // The room list's answer: one notice per room.
        static void UpdateBrowser(double now)
        {
            Browser &b = g_browser;
            if (!b.active) return;
            std::vector<Relay::Event> events;
            uint8_t buffer[2048];
            for (int guard = 0; guard < 16; ++guard)
            {
                NetAddress from;
                int size = b.socket.ReceiveFrom(from, buffer, sizeof(buffer));
                if (size <= 0) break;
                if (from != b.relay) continue;
                Link::Bytes payload;
                b.client.Receive(buffer, (size_t)size, now, payload, events);
            }
            std::vector<std::vector<uint8_t>> packets;
            b.client.Update(now, packets, events);
            for (const std::vector<uint8_t> &packet : packets) b.socket.SendTo(b.relay, packet.data(), packet.size());
            for (const Relay::Event &event : events)
            {
                if (event.kind == Relay::Event::Error)
                {
                    Notice("relay: " + event.text);
                    continue;
                }
                if (event.kind != Relay::Event::RoomList) continue;
                if (event.rooms.empty())
                {
                    Notice(event.page > 0 ? "no more open rooms at the relay" : "no open rooms at the relay");
                    continue;
                }
                Notice("open rooms at the relay (page " + std::to_string(event.page + 1) + " of " + std::to_string(event.pages) +
                       "); join <code> [password]:");
                for (const Relay::Listing &room : event.rooms)
                {
                    std::string line = "  " + room.code + "  " + (room.roomName.empty() ? "(no name)" : "\"" + room.roomName + "\"") +
                                       ", " + room.hostName;
                    if (room.version != g_session.version) line += ", version " + room.version;
                    if (room.password) line += ", password";
                    Notice(line);
                }
            }
            if (b.client.GetState() != Relay::State::Handshake)
            {
                b.active = false;
                b.socket.Close();
            }
        }

        int LastRelayError(std::string *text)
        {
            if (text) *text = g_lastRelayErrorText;
            return g_lastRelayError;
        }

        const std::string &Version()
        {
            return g_session.version;
        }

        void SearchRooms(const std::vector<RelayAddress> &relays)
        {
            g_searchers.clear();
            g_search = RoomSearch();
            for (const RelayAddress &address : relays)
            {
                ++g_search.relays;
                std::unique_ptr<Searcher> searcher(new Searcher());
                searcher->address = address;
                std::string error;
                if (!ResolveAddress(address.server, address.port, searcher->relay, error) ||
                    !searcher->socket.Open(0, IsLoopback(searcher->relay), error))
                {
                    ++g_search.failed;
                    g_search.errors.push_back(address.name + ": " + error);
                    continue;
                }
                searcher->client.List(0, g_session.version, g_session.now);
                searcher->active = true;
                g_searchers.push_back(std::move(searcher));
            }
            Log("Net: asking %d relay%s for their open rooms", g_search.relays, g_search.relays == 1 ? "" : "s");
        }

        const RoomSearch &Search()
        {
            return g_search;
        }

        // The searchers' answers: the rooms of a page, the next page asked for (the relay's client is idle again after
        // each), or the relay's failure.
        static void UpdateSearch(double now)
        {
            for (std::unique_ptr<Searcher> &searcher : g_searchers)
            {
                Searcher &s = *searcher;
                if (!s.active) continue;
                std::vector<Relay::Event> events;
                uint8_t buffer[2048];
                for (int guard = 0; guard < 16; ++guard)
                {
                    NetAddress from;
                    int size = s.socket.ReceiveFrom(from, buffer, sizeof(buffer));
                    if (size <= 0) break;
                    if (from != s.relay) continue;
                    Link::Bytes payload;
                    s.client.Receive(buffer, (size_t)size, now, payload, events);
                }
                std::vector<std::vector<uint8_t>> packets;
                s.client.Update(now, packets, events);
                for (const std::vector<uint8_t> &packet : packets) s.socket.SendTo(s.relay, packet.data(), packet.size());
                for (const Relay::Event &event : events)
                {
                    if (event.kind == Relay::Event::Error)
                    {
                        s.active = false;
                        ++g_search.failed;
                        g_search.errors.push_back(s.address.name + ": " + event.text);
                        Log("Net: the room search at %s: %s", s.address.name.c_str(), event.text.c_str());
                        break;
                    }
                    if (event.kind != Relay::Event::RoomList) continue;
                    for (const Relay::Listing &listing : event.rooms)
                    {
                        FoundRoom room;
                        room.relay = s.address;
                        room.code = listing.code;
                        room.roomName = listing.roomName;
                        room.hostName = listing.hostName;
                        room.version = listing.version;
                        room.password = listing.password;
                        g_search.rooms.push_back(room);
                    }
                    if (!event.rooms.empty() && event.page + 1 < event.pages && event.page + 1 < SEARCH_PAGES)
                    {
                        s.client.List(event.page + 1, g_session.version, now);
                    }
                    else
                    {
                        s.active = false;
                        ++g_search.answered;
                        Log("Net: the room search at %s: %u rooms in all so far", s.address.name.c_str(), (unsigned)g_search.rooms.size());
                    }
                }
                if (!s.active) s.socket.Close();
            }
        }

        bool UsesRelay() { return g_session.relay; }
        std::string RelayCode() { return g_session.relayCode; }
        bool RoomPrivate() { return g_session.host && g_session.roomPrivate; }
        uint64_t MatchSeed() { return g_session.relay ? g_session.relayClient.MatchSeed() : 0; }

        // A packet from the relay: link packets inside go on as if they came from the other player; its own events
        // steer the session.
        static bool FromRelay(const uint8_t *data, size_t size, Link::Bytes &payload)
        {
            Session &s = g_session;
            std::vector<Relay::Event> events;
            bool isPayload = s.relayClient.Receive(data, size, s.now, payload, events);
            for (const Relay::Event &event : events)
            {
                switch (event.kind)
                {
                case Relay::Event::RoomCreated:
                    s.relayCode = event.text;
                    // The other player needs the relay's address as they reach it; a relay on this computer has
                    // another one for them.
                    if (IsLoopback(s.peer))
                    {
                        Notice("room " + event.text + " is open at the relay on this computer. The other player types:  join relay " +
                               event.text + " <this computer's address>");
                    }
                    else
                    {
                        Notice("room " + event.text + " is open at the relay. The other player types:  join relay " + event.text +
                               " " + s.relayServer);
                    }
                    break;
                case Relay::Event::RoomJoined:
                {
                    s.relayCode = s.relayClient.Code();
                    Log("Net: in room %s at the relay (host %s)", s.relayCode.c_str(), event.text.c_str());
                    Writer body;
                    WriteIdentity(body);
                    SendControl(MSG_HELLO, body);
                    break;
                }
                case Relay::Event::PeerJoined:
                    Log("Net: %s entered the room", event.text.c_str());
                    break;
                case Relay::Event::PeerLeft:
                    if (s.phase == Phase::Connected)
                    {
                        LoseConnection("the other player left or lost the connection", false, true);
                        return false;
                    }
                    // Still waiting for the handshake: wait for the next guest.
                    s.link.Reset(0, s.now);
                    break;
                case Relay::Event::RoomClosing:
                    Disconnect("the relay closed the room", false);
                    return false;
                case Relay::Event::RoomList:
                    break;   // only the room list's own client asks for it
                case Relay::Event::Error:
                    g_lastRelayError = event.errorCode;
                    g_lastRelayErrorText = event.text;
                    Notice("relay: " + event.text);
                    Close();
                    return false;
                }
            }
            return isPayload;
        }

        void Leave(const std::string &reason)
        {
            if (g_session.replay)
            {
                EndReplay(reason);
                return;
            }
            if (g_session.phase == Phase::Idle && g_session.lost == Session::Lost::None) return;
            Disconnect(reason, true);
        }

        // Coming back to a match: a try every few seconds; the match ends when the time is up.
        static void UpdateLost(double now)
        {
            Session &s = g_session;
            if (s.lost == Session::Lost::Waiting && now - s.lostAt > REJOIN_GRACE_MS)
            {
                Disconnect("the other player didn't come back", false, true);
                return;
            }
            if (s.lost != Session::Lost::Rejoining) return;
            if (now - s.lostAt > REJOIN_GRACE_MS)
            {
                Disconnect("couldn't get back into the match in time", false);
                return;
            }
            if (s.phase != Phase::Idle || now < s.nextAttempt) return;
            s.nextAttempt = now + REJOIN_RETRY_MS;
            ++s.attempts;
            std::string message;
            g_attempt = true;
            bool started = s.backViaRelay ? JoinRelay(s.backServer, s.backPort, s.backCode, s.relayPassword, message)
                                          : Join(s.backServer, s.backPort, message);
            g_attempt = false;
            Log("Net: trying to come back (%u): %s%s", s.attempts, message.c_str(), started ? "" : " (failed)");
        }

        void Update(double now)
        {
            Session &s = g_session;
            s.now = now;
            if (s.replay)
            {
                Demo::ReplayFrame(now);
                return;
            }
            UpdateBrowser(now);
            UpdateSearch(now);
            UpdateLost(now);
            if (s.phase == Phase::Idle) return;

            uint8_t buffer[2048];
            std::vector<Link::Bytes> delivered;
            for (int guard = 0; guard < 256 && s.socket.IsOpen(); ++guard)
            {
                NetAddress from;
                int size = s.socket.ReceiveFrom(from, buffer, sizeof(buffer));
                if (size <= 0) break;
                if (s.cut)
                {
                    ++s.cutPackets;
                    continue;
                }

                if (s.relay)
                {
                    if (from != s.peer) continue;
                    Link::Bytes payload;
                    if (!FromRelay(buffer, (size_t)size, payload))
                    {
                        if (s.phase == Phase::Idle) return;
                        continue;
                    }
                    // The first packet of a guest opens the host's link session, as below.
                    if (s.phase == Phase::Hosting && s.link.SessionId() == 0)
                    {
                        uint32_t sessionId = Link::PeekSession(payload.data(), payload.size());
                        if (sessionId == 0) continue;
                        s.link.Reset(sessionId, now);
                        Log("Net: the other player's first packet came through the relay, starting the handshake");
                    }
                    delivered.clear();
                    if (!s.link.Receive(payload.data(), payload.size(), now, delivered)) continue;
                    for (const Link::Bytes &message : delivered)
                    {
                        HandleMessage(message);
                        if (s.phase == Phase::Idle) return;
                    }
                    continue;
                }

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

            if (s.phase == Phase::Hosting && s.peer.Valid() && s.link.SessionId() != 0 && s.link.HasReceived() &&
                now - s.link.LastReceiveTime() > JOIN_TIMEOUT_MS)
            {
                // A handshake that never completed: wait for the next player instead.
                Log("Net: %s went quiet during the handshake", s.peer.ToString().c_str());
                if (!s.relay) s.peer = NetAddress();
                s.link.Reset(0, now);
            }
            // (Through a relay the join may first wait for the relay; its own handshake has a timeout.)
            if (s.phase == Phase::Joining && now - s.phaseStart > JOIN_TIMEOUT_MS * (s.relay ? 2.0 : 1.0))
            {
                Log("Net: no answer from %s", s.peer.ToString().c_str());
                Close();
                return;
            }
            if (s.phase == Phase::Connected && s.link.HasReceived() && now - s.link.LastReceiveTime() > SILENCE_TIMEOUT_MS)
            {
                // A direct connection can't tell which side lost it. Through a relay, the relay knows who is still
                // there and says so (EVENT 2 for the other player, above); without contact to the relay ourselves,
                // we are the one cut off.
                if (!s.relay)
                {
                    // Nobody can tell which side lost it: the host waits, the guest tries to come back.
                    LoseConnection("connection lost", !s.host, false);
                    return;
                }
                if (!s.relayClient.HasContact(now, RELAY_CONTACT_MS))
                {
                    LoseConnection("connection lost: no contact to the relay", true, false);
                    return;
                }
                if (now - s.link.LastReceiveTime() > SILENCE_TIMEOUT_MS + RELAY_VERDICT_WAIT_MS)
                {
                    LoseConnection("the other player lost the connection", false, true);
                    return;
                }
            }
            if (s.peer.Valid()) Flush();
        }

        Phase GetPhase() { return g_session.phase; }
        bool IsConnected() { return g_session.phase == Phase::Connected; }
        bool IsHost() { return g_session.host; }
        std::string PeerName() { return g_session.peerName; }
        std::string OwnName() { return g_session.name; }
        std::string OwnVersion() { return g_session.version; }
        std::string OwnBuild() { return g_session.build; }

        Numbers GetNumbers()
        {
            const Session &s = g_session;
            Numbers n;
            n.connected = s.phase == Phase::Connected;
            n.relay = s.relay;
            n.relayCode = s.relayCode;
            n.rttMs = s.link.RttMs();
            n.bestRttMs = s.link.BestRttMs();
            const Link::Stats &st = s.link.GetStats();
            n.packetsSent = st.packetsSent;
            n.packetsReceived = st.packetsReceived;
            n.packetsMissed = st.packetsMissed;
            n.bytesSent = st.bytesSent;
            n.bytesReceived = st.bytesReceived;
            n.reliableResent = st.reliableResent;
            n.pendingReliable = s.link.PendingReliable();
            return n;
        }

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
            std::string text = buffer;
            if (s.relay)
            {
                snprintf(buffer, sizeof(buffer), " | through the relay %s, room %s, ", s.peer.ToString().c_str(),
                         s.relayCode.empty() ? "(none yet)" : s.relayCode.c_str());
                text += buffer;
                if (s.relayClient.RttMs() < 0.0)
                {
                    text += "round trip to the relay not measured yet";
                }
                else
                {
                    snprintf(buffer, sizeof(buffer), "%.1f ms round trip to the relay", s.relayClient.RttMs());
                    text += buffer;
                }
            }
            return text;
        }

        bool Send(uint8_t type, const Writer &body, bool reliable)
        {
            if (g_session.phase != Phase::Connected || type < FIRST_GAME_MESSAGE || g_session.replay) return false;
            Demo::Sent(type, body.data.data(), body.data.size());
            Writer message;
            message.U8(type);
            message.Bytes(body.data.data(), body.data.size());
            return reliable ? g_session.link.SendReliable(message.data) : g_session.link.SendUnreliable(message.data);
        }

        bool HasClock() { return g_session.replay || g_session.link.HasClock(); }
        double PeerToLocalTime(double peerTime) { return peerTime - (g_session.replay ? g_session.replayClock : g_session.link.ClockOffsetMs()); }
        double LocalToPeerTime(double localTime) { return localTime + (g_session.replay ? g_session.replayClock : g_session.link.ClockOffsetMs()); }

        void BeginReplay(const std::string &peerName)
        {
            Session &s = g_session;
            if (s.phase != Phase::Idle || s.lost != Session::Lost::None) Disconnect("a replay begins", true);
            s.replay = true;
            s.replayClock = 0.0;
            s.replayHostClock = 0.0;
            s.host = false;   // the match's flow follows the recorded host's messages, as a guest's does
            s.acceptsJoin = false;
            s.resumed = false;
            s.lost = Session::Lost::None;
            s.peerName = peerName;
            SetPhase(Phase::Connected);
            Log("Net: a replay, %s's side shown as the opponent's", peerName.c_str());
            if (s.listener) s.listener->OnConnected();
        }

        void EndReplay(const std::string &reason)
        {
            Session &s = g_session;
            if (!s.replay) return;
            s.replay = false;
            ReplayClockOff();   // real time again (DuelsTrace.h)
            Log("Net: the replay ended (%s)", reason.c_str());
            Close();
            if (s.listener) s.listener->OnDisconnected(reason, false);
        }

        bool Replaying() { return g_session.replay; }

        void Deliver(uint8_t type, const uint8_t *data, size_t size)
        {
            if (!g_session.replay || !g_session.listener) return;
            Reader reader(data, size);
            g_session.listener->OnMessage(type, reader);
        }

        void SetReplayClock(double peerMinusLocalMs) { g_session.replayClock = peerMinusLocalMs; }
        double ReplayClock() { return g_session.replayClock; }
        void SetReplayHostClock(double hostMinusLocalMs) { g_session.replayHostClock = hostMinusLocalMs; }
        double ReplayHostClock() { return g_session.replayHostClock; }
        double HostToLocalTime(double hostTime) { return g_session.replay ? hostTime - g_session.replayHostClock : PeerToLocalTime(hostTime); }
        double RttMs() { return g_session.link.RttMs(); }

        void SetMatchToken(uint64_t token) { g_session.matchToken = token; }
        bool Resumed() { return g_session.resumed; }

        bool Reconnecting(double &msLeft, bool &cutOff)
        {
            const Session &s = g_session;
            if (s.lost == Session::Lost::None) return false;
            msLeft = REJOIN_GRACE_MS - (s.now - s.lostAt);
            if (msLeft < 0.0) msLeft = 0.0;
            cutOff = s.lost == Session::Lost::Rejoining;
            return true;
        }

        void SimulateCut(bool cut)
        {
            g_session.cut = cut;
            Log("Net: test: the network is %s", cut ? "cut (nothing goes out or comes in)" : "back");
        }

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
