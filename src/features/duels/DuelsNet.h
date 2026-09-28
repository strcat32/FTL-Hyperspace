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
        static const uint16_t PROTOCOL_VERSION = 1;   // bump whenever a message changes

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
            virtual void OnDisconnected(const std::string &reason) = 0;
            // Things the player should see: the relay's room code, the relay refusing, ...
            virtual void OnNotice(const std::string &text) { (void)text; }
        };

        void SetListener(Listener *listener);
        // Sent in the handshake. Versions must match exactly; a different build only gets a warning in the log.
        void SetIdentity(const std::string &playerName, const std::string &version, const std::string &build);

        // Hosting on loopback only (both games on this computer) avoids the Windows Firewall prompt.
        // Joining a loopback address uses a loopback socket for the same reason.
        bool Host(uint16_t port, bool loopbackOnly, std::string &message);
        bool Join(const std::string &host, uint16_t port, std::string &message);

        // Through a relay server (docs/design/relay-protocol.md), for players who can't reach each other directly:
        // the host gets a room code from the relay (announced through Listener::OnNotice), the guest joins with it.
        bool HostRelay(const std::string &server, uint16_t port, std::string &message);
        bool JoinRelay(const std::string &server, uint16_t port, const std::string &code, std::string &message);
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
    }
}
