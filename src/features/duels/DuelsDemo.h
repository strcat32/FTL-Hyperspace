#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Demos (roadmap 5.1; docs/design/demos.md in the FTL: Duels repository): a match recorded as the timed stream of the
// game messages the two games exchange (what Net::Send sends and the listener receives), and our own ship's full
// state with each state we send (the state the opponent gets leaves out what their sensors don't see, roadmap 4.5).
// A file in demos\ next to the game, from a match's connection to its end:
//   "FTLDDEMO", the container's version (u16), the compression (u8: 1 deflate), then the compressed records: the
//   milliseconds since the start (u32), who it came from (u8: 0 host, 1 guest), what it is (u8: KIND_*), the message
//   type (u8), the length (u32) and the bytes. The first record is the header (a marker), the last the end (a marker).
namespace Duels
{
    class Writer;
    struct Command;

    namespace Demo
    {
        static const uint16_t FORMAT = 1;
        static const uint8_t COMPRESSION_DEFLATE = 1;
        static const uint8_t FROM_HOST = 0, FROM_GUEST = 1;
        static const uint8_t KIND_MESSAGE = 0;      // a message between the games, as it went
        static const uint8_t KIND_FULL_STATE = 1;   // the recorder's own state without the vision (never sent)
        static const uint8_t KIND_MARKER = 2;       // MARK_*
        static const uint8_t MARK_HEADER = 0, MARK_END = 1;

        // A new match's connection (not a return after a lost one): a file opens, if demos are on (duels.cfg
        // record_demos, on by default; the `demo` verb). The end of the connection or leaving closes it.
        void Begin(bool host, const std::string &hostName, const std::string &guestName);
        void End(const std::string &why);
        bool Recording();

        // Net::Send and the listener: each game message as it passes (our own states go as full states instead).
        void Sent(uint8_t type, const uint8_t *data, size_t size);
        void Received(uint8_t type, const uint8_t *data, size_t size);
        // Our own ship's state as the opponent would get it with all in sight (DuelsMatch.cpp writes it after the one
        // that goes, with Vision::FullScope).
        void FullState(const Writer &w);

        std::string Status();
        // Test verb: demo (its state), demo on|off (record the next matches or not), demo stop (close the file now).
        bool RunVerb(const Command &cmd, std::string &message);

        // Replay (roadmap 5.1, docs/design/demos.md, stages 1-4): a demo played back in this game, in Net's replay mode.
        // Its records come at their times. What the opponent sent goes to the listener as if received (its ship, its
        // states, its crew, its shots and verdicts, the chat; the match's flow when it hosted). Of what the recorder
        // sent: its ship's loadout (our ship becomes its ship, Match::ReplayOwnLoadout), its full states and roster (our
        // ship follows them), its shots and its verdicts on the opponent's (Match::ReplayOwnShot, ReplayOwnResult), the
        // match's flow when it hosted, the chat. Boarding, hacking, mind control and drones wait for a later stage.
        bool StartReplay(const std::string &path, std::string &message);
        void ReplayFrame(double now);   // Net::Update while it replays
        // A replay's pause is FTL's pause too: the world stands still (CommandGui::IsPaused).
        bool ReplayPaused();
        // Test verb: replay <file> | replay pause | replay resume | replay stop | replay (its state).
        bool RunReplayVerb(const Command &cmd, std::string &message);
    }
}
