#include "DuelsRelay.h"
#include "DuelsCrypto.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

namespace Duels
{
    namespace Relay
    {
        enum PacketType : uint8_t
        {
            HELLO = 0x01,
            COOKIE = 0x02,
            CREATE = 0x03,
            CREATED = 0x04,
            JOIN = 0x05,
            JOINED = 0x06,
            DATA = 0x07,
            RELAYED = 0x08,
            PING = 0x09,
            PONG = 0x0A,
            LEAVE = 0x0B,
            EVENT = 0x0C,
            ERROR_ = 0x0E,
            LIST = 0x10,
            ROOMS = 0x11,
            RESULT = 0x12
        };

        static const uint8_t PROTOCOL_VERSION = 3;
        enum : uint8_t { FLAG_LISTED = 1, FLAG_PASSWORD = 2 };   // CREATE; a listing's flag 1: password, 2: ranked
        static const uint8_t ROOM_RANKED = 1;                     // JOINED's and EVENT's flags
        static const size_t PROOF_SIZE = 16;

        static const size_t HEADER_SIZE = 4;
        static const size_t TAG_SIZE = 16;
        static const size_t HELLO_SIZE = 128;
        static const size_t REQUEST_MIN_SIZE = 96;
        static const size_t LIST_SIZE = 1200;
        static const double RETRY_MS = 500.0;
        static const double HANDSHAKE_TIMEOUT_MS = 10000.0;
        static const double QUIET_MS = 3000.0;   // no packet from the relay for this long: a heartbeat PING
        static const char *const CODE_LETTERS = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

        static void Header(Writer &w, uint8_t type)
        {
            w.U8(0x46);
            w.U8(0x44);
            w.U8(PROTOCOL_VERSION);
            w.U8(type);
        }

        static void U64(Writer &w, uint64_t value)
        {
            w.U32((uint32_t)(value & 0xffffffffu));
            w.U32((uint32_t)(value >> 32));
        }

        static uint64_t U64(Reader &r)
        {
            uint64_t low = r.U32();
            uint64_t high = r.U32();
            return low | (high << 32);
        }

        static void PadTo(Writer &w, size_t size)
        {
            while (w.data.size() < size) w.U8(0);
        }

        static uint64_t NewNonce()
        {
            // Only ties answers to requests (the cookie protects the relay), so the clock mixed with an address will do.
            static std::mt19937_64 generator(
                (uint64_t)std::chrono::high_resolution_clock::now().time_since_epoch().count() ^ (uint64_t)(uintptr_t)&generator);
            uint64_t value = generator();
            return value == 0 ? 1 : value;
        }

        // At most `bytes` bytes of UTF-8 text, without splitting a character.
        static std::string CutUtf8(const std::string &text, size_t bytes)
        {
            if (text.size() <= bytes) return text;
            while (bytes > 0 && ((unsigned char)text[bytes] & 0xC0) == 0x80) --bytes;
            return text.substr(0, bytes);
        }

        static std::string Upper(std::string text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return (char)std::toupper(c); });
            return text;
        }

        bool Client::IsRoomCode(const std::string &text)
        {
            if (text.size() != 6) return false;
            for (char c : Upper(text))
            {
                if (!std::strchr(CODE_LETTERS, c) || c == '\0') return false;
            }
            return true;
        }

        void Client::Reset()
        {
            *this = Client();
        }

        void Client::StartHandshake(double now)
        {
            state = State::Handshake;
            nonce = NewNonce();
            haveCookie = false;
            startMs = now;
            lastSendMs = -1.0e9;
        }

        void Client::PasswordToken(const std::string &password, uint8_t token[16])
        {
            std::memset(token, 0, 16);
            if (password.empty()) return;
            static const char *const PREFIX = "FTL:Duels room password";
            Crypto::Sha256 hash;
            hash.Update((const uint8_t*)PREFIX, std::strlen(PREFIX));
            hash.Update((const uint8_t*)password.data(), password.size());
            uint8_t digest[Crypto::SHA256_SIZE];
            hash.Final(digest);
            std::memcpy(token, digest, 16);
        }

        void Client::Create(const std::string &playerName, const std::string &gameVersion, const std::string &newRoomName,
                            const std::string &password, bool showInList, double now, const std::string &newTicket,
                            const std::string &newTicketKey)
        {
            Reset();
            mode = Mode::Create;
            name = CutUtf8(playerName, 32);
            version = CutUtf8(gameVersion, 32);
            roomName = CutUtf8(newRoomName, 32);
            PasswordToken(password, passwordToken);
            listed = showInList;
            if (newTicketKey.size() == 32)
            {
                ticket = newTicket;
                ticketKey = newTicketKey;
            }
            StartHandshake(now);
        }

        void Client::Join(const std::string &roomCode, const std::string &password, const std::string &playerName,
                          const std::string &gameVersion, double now, const std::string &newTicket, const std::string &newTicketKey)
        {
            Reset();
            mode = Mode::Join;
            code = Upper(roomCode);
            name = CutUtf8(playerName, 32);
            version = CutUtf8(gameVersion, 32);
            PasswordToken(password, passwordToken);
            if (newTicketKey.size() == 32)
            {
                ticket = newTicket;
                ticketKey = newTicketKey;
            }
            StartHandshake(now);
        }

        void Client::List(int page, const std::string &gameVersion, double now)
        {
            Reset();
            mode = Mode::List;
            version = CutUtf8(gameVersion, 32);
            listPage = std::max(0, std::min(page, 65535));
            StartHandshake(now);
        }

        void Client::SendHandshake(std::vector<std::vector<uint8_t>> &packets)
        {
            Writer w;
            if (!haveCookie)
            {
                Header(w, HELLO);
                U64(w, nonce);
                PadTo(w, HELLO_SIZE);
            }
            else if (mode == Mode::List)
            {
                Header(w, LIST);
                U64(w, nonce);
                w.Bytes(cookie, sizeof(cookie));
                w.U16((uint16_t)listPage);
                PadTo(w, LIST_SIZE);
            }
            else
            {
                bool creating = mode == Mode::Create;
                Header(w, creating ? CREATE : JOIN);
                U64(w, nonce);
                w.Bytes(cookie, sizeof(cookie));
                if (!creating) w.Str(code);
                w.Str(name);
                w.Str(version);
                if (creating)
                {
                    w.Str(roomName);
                    bool password = std::any_of(passwordToken, passwordToken + 16, [](uint8_t b) { return b != 0; });
                    w.U8((uint8_t)((listed ? FLAG_LISTED : 0) | (password ? FLAG_PASSWORD : 0)));
                }
                w.Bytes(passwordToken, sizeof(passwordToken));
                // Protocol 3: the ticket (empty: none) and, after it, the proof that we hold its key: HMAC-SHA256 of
                // every byte so far with the key, its first 16 bytes.
                if (!ticket.empty() && ticket.size() <= 255)
                {
                    w.Str(ticket);
                    uint8_t mac[Crypto::SHA256_SIZE];
                    Crypto::HmacSha256((const uint8_t*)ticketKey.data(), ticketKey.size(), w.data.data(), w.data.size(), mac);
                    w.Bytes(mac, PROOF_SIZE);
                }
                else w.U8(0);
                PadTo(w, REQUEST_MIN_SIZE);
            }
            packets.push_back(w.data);
        }

        void Client::Tag(std::vector<uint8_t> &packet) const
        {
            uint8_t mac[Crypto::SHA256_SIZE];
            Crypto::HmacSha256(key, sizeof(key), packet.data(), packet.size(), mac);
            packet.insert(packet.end(), mac, mac + TAG_SIZE);
        }

        bool Client::Verify(const uint8_t *data, size_t size) const
        {
            if (size < HEADER_SIZE + TAG_SIZE) return false;
            uint8_t mac[Crypto::SHA256_SIZE];
            Crypto::HmacSha256(key, sizeof(key), data, size - TAG_SIZE, mac);
            return Crypto::Equal(mac, data + size - TAG_SIZE, TAG_SIZE);
        }

        // Each sequence number once, within the last 64.
        bool Client::AcceptSeq(uint32_t seq)
        {
            if (seq == 0) return false;
            if (seq > highestSeq)
            {
                uint32_t shift = seq - highestSeq;
                seenMask = shift >= 64 ? 0 : seenMask << shift;
                seenMask |= 1;
                highestSeq = seq;
                return true;
            }
            uint32_t age = highestSeq - seq;
            if (age >= 64 || (seenMask >> age) & 1) return false;
            seenMask |= (uint64_t)1 << age;
            return true;
        }

        bool Client::Receive(const uint8_t *data, size_t size, double now, std::vector<uint8_t> &payload, std::vector<Event> &events)
        {
            if (size < HEADER_SIZE || data[0] != 0x46 || data[1] != 0x44 || data[2] != PROTOCOL_VERSION) return false;
            uint8_t type = data[3];
            Reader r(data + HEADER_SIZE, size - HEADER_SIZE);

            if (state == State::Handshake)
            {
                if (type != COOKIE && type != CREATED && type != JOINED && type != ROOMS && type != ERROR_) return false;
                if (U64(r) != nonce || !r.Ok()) return false;
                if (type == COOKIE && !haveCookie)
                {
                    const uint8_t *bytes = r.Position();
                    if (!r.Skip(sizeof(cookie))) return false;
                    std::memcpy(cookie, bytes, sizeof(cookie));
                    haveCookie = true;
                    lastSendMs = -1.0e9;   // ask for the room at once
                }
                else if (type == ROOMS && mode == Mode::List && haveCookie)
                {
                    Event list{Event::RoomList, "", 0};
                    list.page = r.U16();
                    list.pages = r.U16();
                    int count = r.U8();
                    for (int i = 0; i < count && r.Ok(); ++i)
                    {
                        Listing room;
                        room.code = r.Str();
                        room.roomName = r.Str();
                        room.hostName = r.Str();
                        room.version = r.Str();
                        uint8_t flags = r.U8();
                        room.password = (flags & 1) != 0;
                        room.ranked = (flags & 2) != 0;
                        room.rating = r.U16();
                        list.rooms.push_back(room);
                    }
                    if (!r.Ok()) return false;
                    state = State::Idle;
                    events.push_back(list);
                }
                else if (type == CREATED && mode == Mode::Create && haveCookie)
                {
                    std::string roomCode = r.Str();
                    uint32_t id = r.U32();
                    const uint8_t *bytes = r.Position();
                    if (!r.Skip(sizeof(key)) || !r.Ok()) return false;
                    code = roomCode;
                    clientId = id;
                    std::memcpy(key, bytes, sizeof(key));
                    state = State::InRoom;
                    lastReceivedMs = now;
                    // The relay took the ticket (else ERROR 9): the room is ranked.
                    ranked = !ticket.empty();
                    ticket.clear();
                    ticketKey.clear();
                    Event created{Event::RoomCreated, code, 0};
                    created.ranked = ranked;
                    events.push_back(created);
                }
                else if (type == JOINED && mode == Mode::Join && haveCookie)
                {
                    uint32_t id = r.U32();
                    const uint8_t *bytes = r.Position();
                    if (!r.Skip(sizeof(key))) return false;
                    uint8_t newKey[16];
                    std::memcpy(newKey, bytes, sizeof(newKey));
                    uint64_t seed = U64(r);
                    std::string hostName = r.Str();
                    uint8_t flags = r.U8();
                    int hostRating = r.U16();
                    if (!r.Ok()) return false;
                    clientId = id;
                    std::memcpy(key, newKey, sizeof(key));
                    matchSeed = seed;
                    state = State::InRoom;
                    lastReceivedMs = now;
                    ranked = (flags & ROOM_RANKED) != 0;
                    peerRating = hostRating;
                    ticket.clear();
                    ticketKey.clear();
                    Event joined{Event::RoomJoined, hostName, seed};
                    joined.ranked = ranked;
                    joined.rating = hostRating;
                    events.push_back(joined);
                }
                else if (type == ERROR_)
                {
                    int errorCode = r.U8();
                    std::string message = r.Str();
                    if (!r.Ok()) return false;
                    if (errorCode == 1 && restarts < 3)
                    {
                        ++restarts;   // the cookie ran out: start again
                        StartHandshake(now);
                        return false;
                    }
                    state = State::Failed;
                    Event failed{Event::Error, message.empty() ? "the relay refused (" + std::to_string(errorCode) + ")" : message, 0};
                    failed.errorCode = errorCode;
                    events.push_back(failed);
                }
                return false;
            }

            // Session packets: header, client id, seq, body, tag.
            if (state != State::InRoom || size < HEADER_SIZE + 8 + TAG_SIZE || !Verify(data, size)) return false;
            uint32_t id = r.U32();
            uint32_t seq = r.U32();
            if (!r.Ok() || id != clientId || !AcceptSeq(seq)) return false;
            lastReceivedMs = now;
            size_t bodySize = size - HEADER_SIZE - 8 - TAG_SIZE;
            const uint8_t *body = data + HEADER_SIZE + 8;
            if (type == RELAYED)
            {
                payload.assign(body, body + bodySize);
                return true;
            }
            Reader rest(body, bodySize);
            if (type == PONG)
            {
                double sent = rest.F64();
                if (rest.Ok() && sent <= now) rttMs = rttMs < 0.0 ? now - sent : rttMs * 0.8 + (now - sent) * 0.2;
            }
            else if (type == EVENT)
            {
                int event = rest.U8();
                uint64_t seed = U64(rest);
                std::string peerName = rest.Str();
                uint8_t flags = rest.U8();
                int rating = rest.U16();
                if (!rest.Ok()) return false;
                if (event == 1)
                {
                    matchSeed = seed;
                    if (flags & ROOM_RANKED) ranked = true;
                    peerRating = rating;
                    Event joined{Event::PeerJoined, peerName, seed};
                    joined.ranked = (flags & ROOM_RANKED) != 0;
                    joined.rating = rating;
                    events.push_back(joined);
                }
                else if (event == 4)
                {
                    events.push_back(Event{Event::ResultRecorded, "", 0});
                }
                else if (event == 2)
                {
                    events.push_back(Event{Event::PeerLeft, peerName, 0});
                }
                else if (event == 3)
                {
                    events.push_back(Event{Event::RoomClosing, "", 0});
                }
            }
            return false;
        }

        void Client::Update(double now, std::vector<std::vector<uint8_t>> &packets, std::vector<Event> &events)
        {
            if (state == State::Handshake)
            {
                if (now - startMs > HANDSHAKE_TIMEOUT_MS)
                {
                    state = State::Failed;
                    Event failed{Event::Error, "no answer from the relay", 0};
                    failed.errorCode = Event::NO_ANSWER;
                    events.push_back(failed);
                    return;
                }
                if (now - lastSendMs >= RETRY_MS)
                {
                    SendHandshake(packets);
                    lastSendMs = now;
                }
                return;
            }
            if (state == State::InRoom && now - lastReceivedMs >= QUIET_MS && now - lastPingMs >= QUIET_MS)
            {
                Writer w;
                Header(w, PING);
                w.U32(clientId);
                w.U32(++sendSeq);
                w.F64(now);
                std::vector<uint8_t> packet = w.data;
                Tag(packet);
                packets.push_back(packet);
                lastPingMs = now;
            }
        }

        bool Client::Wrap(const uint8_t *payload, size_t size, std::vector<uint8_t> &packet)
        {
            if (state != State::InRoom || size == 0 || size > 1300) return false;
            Writer w;
            Header(w, DATA);
            w.U32(clientId);
            w.U32(++sendSeq);
            w.Bytes(payload, size);
            packet = w.data;
            Tag(packet);
            return true;
        }

        bool Client::Result(const MatchResult &result, std::vector<uint8_t> &packet)
        {
            if (state != State::InRoom) return false;
            Writer w;
            Header(w, RESULT);
            w.U32(clientId);
            w.U32(++sendSeq);
            w.U8(result.outcome);
            w.U8(result.halves);
            w.U8(result.peerHalves);
            w.U8(result.rounds);
            w.U8(result.flags);
            w.U32(result.season);
            w.Bytes(result.settings, sizeof(result.settings));
            w.U32(result.seconds);
            w.Str(CutUtf8(result.note, 60));
            packet = w.data;
            Tag(packet);
            return true;
        }

        bool Client::Leave(std::vector<uint8_t> &packet)
        {
            if (state != State::InRoom) return false;
            Writer w;
            Header(w, LEAVE);
            w.U32(clientId);
            w.U32(++sendSeq);
            packet = w.data;
            Tag(packet);
            Reset();
            return true;
        }
    }
}
