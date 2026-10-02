#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

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
    class Reader;
    struct Command;

    namespace Demo
    {
        static const uint16_t FORMAT = 1;
        static const uint8_t COMPRESSION_DEFLATE = 1;
        static const uint8_t FROM_HOST = 0, FROM_GUEST = 1;
        static const uint8_t KIND_MESSAGE = 0;      // a message between the games, as it went
        static const uint8_t KIND_FULL_STATE = 1;   // a player's own state without the vision (never sent; BA joins the other's)
        static const uint8_t KIND_MARKER = 2;       // MARK_*
        static const uint8_t MARK_HEADER = 0, MARK_END = 1;
        static const uint8_t MARK_STATUS = 2;       // the match's status as it changes (BB): ranked (u8), why not (str)
        static const uint8_t MARK_SWAP = 3;         // the other's full states came after the match (BA): how far their
                                                    // demo's start is after ours (f64 ms), how many (u32)
        static const uint8_t MARK_PEER_COLD = 4;    // the other game came back after a crash (roadmap BR): who (u8,
                                                    // FROM_*); its states count from the start again
        // The swap after a match (BA, part 2): MSG_DEMO_STATES (reliable, either way, once the match is over) carries our
        // full states in pieces: the deflated length (u32), the raw length (u32), the piece's place (u32), our demo's
        // start on our clock (f64; -1 without a demo), the bytes. MSG_DEMO_SAVED (reliable): all of the other's came.
        static const uint8_t MSG_DEMO_STATES = 43, MSG_DEMO_SAVED = 44;

        // A new match's connection (not a return after a lost one): a file opens, if demos are on (duels.cfg
        // record_demos, on by default; the `demo` verb). The end of the connection or leaving closes it.
        void Begin(bool host, const std::string &hostName, const std::string &guestName);
        // The other game came back after a crash (roadmap BR, DuelsRejoin.cpp): its states count from the start again,
        // in a replay of this demo too (MARK_PEER_COLD).
        void NotePeerCold();
        void End(const std::string &why);
        bool Recording();

        // Net::Send and the listener: each game message as it passes (our own states too, as they went: the opponent's
        // view in a replay, BA; in full they come with FullState).
        void Sent(uint8_t type, const uint8_t *data, size_t size);
        void Received(uint8_t type, const uint8_t *data, size_t size);
        // Our own ship's state as the opponent would get it with all in sight (DuelsMatch.cpp writes it after the one
        // that goes, with Vision::FullScope), recorded or not: it is kept for the swap after the match.
        void FullState(const Writer &w);
        // The swap after a match (roadmap BA, part 2; the user, 2026-10-01): when a match is over the two games send each
        // other their full states (nothing is hidden then), and each adds the other's to its demo at the times they went
        // (on its own clock), so that each player's own demo has both sides. Match::OnFrame starts it at the match's end
        // and sends its pieces, one a frame; the listener hands its messages here.
        void StartSwap();
        void SwapFrame(double now);
        void OnSwapMessage(uint8_t type, Reader &r);
        // Ours not all with the other game yet, or theirs not all here (LOBBY waits for it, 10 s at most).
        bool SwapBusy();
        // The match's status (roadmap BB): ranked, or unranked and why; a recording keeps each change (MARK_STATUS), for
        // its replay's line at the top and the demo browser.
        void NoteStatus(bool ranked, const std::string &why);

        std::string Status();
        // "Record a demo" (roadmap AV: HOST DUEL and JOIN DUEL): the next matches recorded or not, kept in duels.cfg
        // (record_demos).
        bool RecordingOn();
        void SetRecordingOn(bool on);
        // The demo browser (roadmap AU, DuelsReplayList.cpp): the demos in demos\ and what each one is, read from it.
        struct DemoInfo
        {
            std::string file, path;
            std::string problem;                 // why it can't be played ("" if it can)
            uint32_t startUtc = 0;               // the match's start
            std::string hostName, guestName;
            std::string hostShip, guestShip;     // the blueprints of their first loadouts
            int ranked = -1;                     // the match's last status (BB): 1, 0, -1 not known
            double lengthMs = 0.0;
        };
        std::vector<DemoInfo> ListDemos();
        // Test verb: demo (its state), demo on|off (record the next matches or not), demo stop (close the file now),
        // demo hold on|off (the swap's pieces wait: LOBBY's wait for the other game, in tests).
        bool RunVerb(const Command &cmd, std::string &message);

        // Replay (roadmap 5.1, docs/design/demos.md, stages 1-5): a demo played back in this game, in Net's replay mode.
        // Its records come at their times, from one player's side (BA): our ship is theirs. What the other player sent
        // goes to the listener as if received (their ship, its states, its crew, their shots and verdicts, the chat; the
        // match's flow when they hosted). Of what the shown player sent: their ship's loadout (our ship becomes their
        // ship, Match::ReplayOwnLoadout), their full states and roster (our ship follows them), their crew going aboard
        // and back, their drones' shots, their shots and verdicts on the other's (Match::ReplayOwnShot, ReplayOwnResult),
        // the match's flow when they hosted, the chat. The recorder's side is shown first; the other player's needs their
        // full states: their demo of the match, found in the same folder (or named: `replay <file> with <file>`), joins
        // its full states at the demo's times. With full sensors the other ship follows the other player's full states,
        // and everything of both ships is in sight (Match::FullSensors).
        bool StartReplay(const std::string &path, std::string &message);
        // A ranked room's match: its ticket's nonce (Net::LastTicketNonce); its demo goes to the master when it is saved
        // (roadmap BQ).
        void SetTicket(const std::string &nonce);
        // A replay that runs ends (FTL's main menu, roadmap BO: it went on unseen behind the menu).
        void StopReplay(const std::string &why);
        void ReplayFrame(double now);   // Net::Update while it replays: a seek's arrival, the replay's end
        // A replay's pause is FTL's pause too: the world stands still (CommandGui::IsPaused), and the replay's clock.
        bool ReplayPaused();
        // A replay's clock is FTL's world time (DuelsTrace.h). Its pace (WorldManager::OnLoop, DuelsHooks.cpp): how
        // many steps of FTL's world a frame takes (2 to 8 for its speed, 32 while a seek runs ahead), and each step's
        // share of FTL's own (half speed); 1 and 1 outside a replay. Before each step its clock moves on by the step and
        // the records whose time has come are played.
        void Pace(int &steps, float &share, double stepMs);
        void BeforeWorldStep(double stepMs);

        // A replay as its screen shows it (roadmap AW: DuelsReplayUi.cpp; BB: DuelsHud.cpp). At the demo's end it stays
        // on its last moment, paused.
        struct ReplayView
        {
            bool active = false;
            bool paused = false, seeking = false, ended = false;
            double positionMs = 0.0, lengthMs = 0.0, speed = 1.0;
            std::string hostName, guestName;
            bool recorderHost = true;
            bool viewedHost = true;          // whose side the screen shows (BA): our ship is theirs
            bool bothSides = false;          // both players' full states are there: the other's view, full sensors
            bool fullSensors = false;
            int ranked = -1;              // the recorded match's status: 1 ranked, 0 unranked, -1 not known (an older demo)
            std::string unrankedWhy;
            // A long seek (a view switch, a step back: from the demo's start) runs behind a cover (roadmap BO): what it
            // says, and how far it is (0 to 1).
            bool covering = false;
            std::string coverText;
            double coverProgress = 0.0;
        };
        ReplayView GetReplayView();
        // Its controls (the replay verbs do the same): play or pause (at its end: from the start again), stop (back to
        // the start, paused, as a media player's stop), to a time of the demo, back or on by ms, the speed one up (1),
        // one down (-1) or round (0: 1/2, 1, 2, 4, 8, 1/2, ...). The start is the demo's first state of our ship (the
        // ships are fitted then); no seek goes further back.
        void ReplayPlayPause();
        void ReplayStop();
        // A seek's world steps stop for the frame once this much real time went into them (WorldManager::OnLoop).
        bool SeekBudgetSpent();
        // True while a replay starts again by itself (stop, a seek back): the lost connection and the new one that this
        // is in Net's replay mode stay out of the feed (DuelsMatch.cpp, DuelsRounds.cpp).
        bool ReplayRestarting();
        // The other player's side (BA): the replay starts again from there and runs to where it was. Full sensors on or
        // off, at once. Both need both players' full states (ReplayView::bothSides); the feed says so otherwise.
        void ReplaySwitchView();
        void ReplaySetFullSensors(bool on);
        bool ReplayFullSensors();
        void ReplaySeekTo(double ms);
        void ReplayStep(double ms);
        void ReplaySpeedStep(int direction);
        // Test verb: replay <file> [with <file>] [view host|guest] | replay pause | replay resume | replay stop |
        // replay speed 0.5|1|2|4|8 | replay seek <s>|+<s>|-<s> | replay view host|guest | replay sensors full|seen |
        // replay (its state).
        bool RunReplayVerb(const Command &cmd, std::string &message);
    }
}
