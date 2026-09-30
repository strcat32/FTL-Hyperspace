#pragma once

#include "DuelsWire.h"

#include <cstdint>
#include <string>

// Session between two players: one hosts (listens on a UDP port), the other joins. The handshake checks that both
// run compatible versions; then messages flow both ways over a Link (reliable and unreliable). No game headers here,
// so the netcode can be tested without FTL; the game layer (DuelsMatch.cpp) plugs in through Listener.
namespace Duels
{
    namespace Net
    {
        static const uint16_t DEFAULT_PORT = 47620;
        static const uint16_t PROTOCOL_VERSION = 5;   // bump whenever a message changes

        // Message types below this are the session's own; the game layer uses the rest.
        static const uint8_t FIRST_GAME_MESSAGE = 16;

        enum class Phase
        {
            Idle,        // no socket
            Hosting,     // waiting for someone to join
            Joining,     // handshake sent, waiting for the host's answer
            Connected
        };

        class Listener
        {
        public:
            virtual ~Listener() {}
            virtual void OnConnected() = 0;
            virtual void OnMessage(uint8_t type, Reader &reader) = 0;
            // opponentGone: the other player left, or lost the connection while we still reach the relay. False
            // when we are the one cut off, or when nobody can tell (a direct connection that went quiet).
            virtual void OnDisconnected(const std::string &reason, bool opponentGone) = 0;
            // Things the player should see: the relay's room code, the relay refusing, ...
            virtual void OnNotice(const std::string &text) { (void)text; }
            // The connection is lost while a match can go on (SetMatchToken): the session waits for the other player
            // to come back (cutOff false) or tries to come back itself (true). OnConnected follows if it works, with
            // Resumed() true, and OnDisconnected when the time is up.
            virtual void OnConnectionLost(const std::string &reason, bool cutOff) { (void)reason; (void)cutOff; }
        };

        // Coming back after a lost connection (roadmap 3.1, docs/design/match-flow.md): how long the match waits.
        static const double REJOIN_GRACE_MS = 60000.0;
        // The game's match (0: none, nothing to come back to); it goes with the handshake, so a player coming back
        // continues the same match, and a stranger can't take the missing player's place.
        void SetMatchToken(uint64_t token);
        // The last OnConnected continued the match of before.
        bool Resumed();
        // While the connection is lost and the match waits: the time left, and whether we are the one cut off.
        bool Reconnecting(double &msLeft, bool &cutOff);

        void SetListener(Listener *listener);
        // Sent in the handshake. Versions must match exactly; a different build only gets a warning in the log.
        void SetIdentity(const std::string &playerName, const std::string &version, const std::string &build);
        // Debug mode (Duels.h) goes with the handshake, so the other player knows.
        void SetDebugFlag(bool debug);
        bool PeerDebug();

        // Hosting on loopback only (both games on this computer) avoids the Windows Firewall prompt.
        // Joining a loopback address uses a loopback socket for the same reason.
        bool Host(uint16_t port, bool loopbackOnly, std::string &message);
        bool Join(const std::string &host, uint16_t port, std::string &message);

        // Through a relay server (docs/design/relay-protocol.md), for players who can't reach each other directly:
        // the host gets a room code from the relay (announced through Listener::OnNotice), the guest joins with it.
        // The host's room has a name, a password or none (""), and shows in the relay's room list or not.
        bool HostRelay(const std::string &server, uint16_t port, const std::string &roomName, const std::string &password,
                       bool listed, std::string &message);
        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, const std::string &password,
                       std::string &message);
        // The relay's list of rooms waiting for a guest, a page at a time; it comes as notices (Listener::OnNotice).
        // Has a socket of its own: a session isn't touched.
        bool ListRelayRooms(const std::string &server, uint16_t port, int page, std::string &message);
        bool UsesRelay();
        std::string RelayCode();
        uint64_t MatchSeed();   // from the relay, 0 without one

        void Leave(const std::string &reason);

        // Once per frame (also in menus): receive, deliver, time out, send.
        void Update(double now);

        Phase GetPhase();
        bool IsConnected();
        bool IsHost();
        std::string PeerName();
        std::string Status();

        // The connection's numbers, for the on-screen network display (DuelsHud.cpp). Counters run since connecting.
        struct Numbers
        {
            bool connected = false;
            bool relay = false;
            std::string relayCode;
            double rttMs = -1.0, bestRttMs = -1.0;
            uint32_t packetsSent = 0, packetsReceived = 0, packetsMissed = 0, bytesSent = 0, bytesReceived = 0;
            uint32_t reliableResent = 0;
            size_t pendingReliable = 0;
        };
        Numbers GetNumbers();

        // Game messages (type >= FIRST_GAME_MESSAGE). Returns false if not connected or the message is too big.
        bool Send(uint8_t type, const Writer &body, bool reliable);

        // The two machines' clocks, related through the link's estimate.
        bool HasClock();
        double PeerToLocalTime(double peerTime);
        double LocalToPeerTime(double localTime);
        double RttMs();

        // Test conditions for our outgoing packets: fixed delay, random jitter (+/-) and loss in percent.
        // Setting the same on both sides simulates a symmetric connection.
        void Simulate(double delayMs, double jitterMs, double lossPercent);
        // A pulled cable: nothing goes out or comes in, the relay's packets too (tests of coming back to a match).
        void SimulateCut(bool cut);
    }
}
