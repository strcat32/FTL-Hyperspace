#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The client side of the relay protocol (docs/design/relay-protocol.md): the handshake that gets a room, and the
// packets of a session. No sockets here: DuelsNet.cpp sends and receives, this turns link packets into relay packets and
// back. No game headers, so the netcode tests build it outside the game.
namespace Duels
{
    namespace Relay
    {
        static const uint16_t DEFAULT_PORT = 47700;

        enum class State
        {
            Idle,
            Handshake,   // getting a cookie, then the room
            InRoom,
            Failed
        };

        struct Event
        {
            enum Kind
            {
                RoomCreated,   // text: the room code
                RoomJoined,    // text: the host's name
                PeerJoined,    // text: the guest's name
                PeerLeft,
                RoomClosing,
                Error          // text: what went wrong
            };
            Kind kind;
            std::string text;
            uint64_t matchSeed;
        };

        class Client
        {
        public:
            // Starts a handshake: a new room (the host) or the room with that code (the guest).
            void Create(const std::string &name, const std::string &version, double now);
            void Join(const std::string &code, const std::string &name, const std::string &version, double now);
            void Reset();

            // A datagram from the relay. A link packet relayed from the other player lands in `payload` (returns
            // true); anything else may add events.
            bool Receive(const uint8_t *data, size_t size, double now, std::vector<uint8_t> &payload, std::vector<Event> &events);

            // What to send to the relay now: handshake retries, and a heartbeat PING when nothing has come from the
            // relay for a while (it keeps the router's mapping and the room alive, and shows whether we still reach
            // the relay). While the other player's packets flow, no pings are needed. May add an Error event.
            void Update(double now, std::vector<std::vector<uint8_t>> &packets, std::vector<Event> &events);

            // A link packet for the other player as a DATA packet. False while there is no room.
            bool Wrap(const uint8_t *payload, size_t size, std::vector<uint8_t> &packet);

            // A LEAVE packet (false if not in a room); the client is reset afterwards.
            bool Leave(std::vector<uint8_t> &packet);

            State GetState() const { return state; }
            const std::string &Code() const { return code; }
            uint64_t MatchSeed() const { return matchSeed; }
            double RttMs() const { return rttMs; }
            // Whether a valid packet came from the relay within the last `withinMs` (in the room).
            bool HasContact(double now, double withinMs) const { return state == State::InRoom && now - lastReceivedMs <= withinMs; }

            static bool IsRoomCode(const std::string &text);

        private:
            void StartHandshake(double now);
            void SendHandshake(std::vector<std::vector<uint8_t>> &packets);
            void Tag(std::vector<uint8_t> &packet) const;
            bool Verify(const uint8_t *data, size_t size) const;
            bool AcceptSeq(uint32_t seq);

            State state = State::Idle;
            bool creating = true;
            std::string name, version, code;
            uint64_t nonce = 0;
            uint8_t cookie[16] = {0};
            bool haveCookie = false;
            double startMs = 0.0, lastSendMs = -1.0e9;
            int restarts = 0;

            uint32_t clientId = 0;
            uint8_t key[16] = {0};
            uint32_t sendSeq = 0;
            uint32_t highestSeq = 0;
            uint64_t seenMask = 0;
            uint64_t matchSeed = 0;
            double lastPingMs = -1.0e9;
            double lastReceivedMs = -1.0e9;   // the last valid packet from the relay
            double rttMs = -1.0;
        };
    }
}
