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
            Handshake,   // getting a cookie, then the room (or the room list)
            InRoom,
            Failed
        };

        // A room waiting for a guest, as the relay's list shows it: a ranked room (protocol 3) with its host's rating.
        struct Listing
        {
            std::string code, roomName, hostName, version;
            bool password = false;
            bool ranked = false;
            int rating = 0;
        };

        // A ranked match's result, as this player's game saw it (protocol 3's RESULT; docs/design/ranked-play.md).
        struct MatchResult
        {
            enum : uint8_t { WON = 1, LOST = 2, DRAW = 3 };
            enum : uint8_t { FLAG_RANKED = 1, FLAG_PEER_LEFT = 2 };
            uint8_t outcome = DRAW;
            uint8_t halves = 0, peerHalves = 0;   // half points: ours, the other player's
            uint8_t rounds = 0;
            uint8_t flags = 0;
            uint32_t season = 0;
            uint8_t settings[32] = {0};             // the match's settings, hashed
            uint32_t seconds = 0;
            std::string note;                       // why not ranked, if not (at most 60 bytes)
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
                RoomList,      // rooms: this page of the open rooms; page, pages
                Error,         // text: what went wrong
                ResultRecorded // the relay has our RESULT
            };
            Event(Kind eventKind, const std::string &eventText = std::string(), uint64_t seed = 0)
                : kind(eventKind), text(eventText), matchSeed(seed), page(0), pages(0), errorCode(0), ranked(false), rating(0) {}

            Kind kind;
            std::string text;
            uint64_t matchSeed;
            std::vector<Listing> rooms;
            int page, pages;
            // Error: the relay's ERROR code (docs/design/relay-protocol.md: 2 no such room, 3 room full, 4 server full,
            // 5 another version, 6 rate limited, 8 wrong password, 9 the ticket refused, 10 a ranked room: a ticket
            // needed), or NO_ANSWER when it didn't answer.
            int errorCode;
            static const int NO_ANSWER = -1;
            static const int REFUSED = -2;    // not the relay's: the host's game refused the handshake (DuelsNet.cpp)
            static const int TICKET_REFUSED = 9;
            static const int RANKED_ROOM = 10;
            // RoomJoined, PeerJoined: the room is ranked (both came with a ticket), and the other player's rating.
            bool ranked;
            int rating;
        };

        class Client
        {
        public:
            // Starts a handshake: a new room (the host: its name, a password or "", shown in room lists or not), the
            // room with that code (the guest, with the room's password if it has one), or a page of the list of open
            // rooms (the client is idle again after it, with a RoomList event). A ticket from the master (its bytes and
            // its 32-byte key; protocol 3) makes the room ranked, or lets the guest into a ranked room.
            void Create(const std::string &name, const std::string &version, const std::string &roomName,
                        const std::string &password, bool listed, double now, const std::string &ticket = std::string(),
                        const std::string &ticketKey = std::string());
            void Join(const std::string &code, const std::string &password, const std::string &name,
                      const std::string &version, double now, const std::string &ticket = std::string(),
                      const std::string &ticketKey = std::string());
            void List(int page, const std::string &version, double now);
            void Reset();

            // What a room password travels as: the first 16 bytes of SHA-256("FTL:Duels room password" and the
            // password); all zeros for none.
            static void PasswordToken(const std::string &password, uint8_t token[16]);

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

            // A RESULT packet (protocol 3) for a ranked room's match; false while there is no room. The relay answers
            // each with a ResultRecorded event.
            bool Result(const MatchResult &result, std::vector<uint8_t> &packet);

            State GetState() const { return state; }
            const std::string &Code() const { return code; }
            uint64_t MatchSeed() const { return matchSeed; }
            // A ranked room (made with a ticket, or joined as one), and the other player's rating from their ticket.
            bool Ranked() const { return ranked; }
            int PeerRating() const { return peerRating; }
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

            enum class Mode { Create, Join, List };

            State state = State::Idle;
            Mode mode = Mode::Create;
            std::string name, version, code, roomName;
            std::string ticket, ticketKey;   // protocol 3: the master's ticket and its key
            bool ranked = false;
            int peerRating = 0;
            uint8_t passwordToken[16] = {0};
            bool listed = false;
            int listPage = 0;
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
