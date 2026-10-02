#pragma once

#include "DuelsHttp.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The player's account at the master server (roadmap BG, 4.3; docs/design/ranked-play.md in the FTL: Duels repository):
// signing in through Steam like a TV app's login (the game asks the master for a code, the player signs in through
// Steam in the browser on the master's link page, the game asks until the code is signed in and gets the account
// key), the key kept in its own file next to the game (duels-account.cfg: never duels.cfg), the Steam name and the
// rating from the master, and tickets for ranked rooms. The master is duels.cfg's (roadmap AZ).
namespace Duels
{
    struct Command;

    namespace Account
    {
        enum class State
        {
            SignedOut,
            Linking,     // the code is out: the player signs in through Steam in the browser
            SignedIn
        };

        struct View
        {
            State state = State::SignedOut;
            std::string code, url;          // while linking
            std::string name, steamId;      // signed in
            bool ratingKnown = false;
            int rating = 0, rd = 0, matches = 0;
            int wins = 0, losses = 0, draws = 0, rank = 0;   // the season now's
            std::string seasonName;
            std::string message;            // the last news ("the code expired", "the master doesn't answer")
        };
        View GetView();
        bool SignedIn();

        // SIGN IN: the code from the master, the browser opened on its page (not in a test), then the polls.
        void StartSignIn();
        void CancelSignIn();
        // SIGN OUT: the account file goes (the master's key stays valid until the site signs it out).
        void SignOut();
        // The name and rating again from the master.
        void Refresh();
        // A request to the account's master with the account's key (the ranked queue, DuelsQueue.cpp): path from "/api".
        void Request(const std::string &method, const std::string &path, const std::string &body, Http::Callback done);
        // This game for the master's ranked requests (roadmap CE): "\"protocol\": 18, \"version\": \"0.8.1\"" (fields of
        // a JSON object), so that an older game is told to update.
        std::string GameFields();
        // The account's master runs on this computer (a test's: tools/ranked-env.py).
        bool MasterIsLocal();
        // The master's newest version and the protocol ranked play needs (GET /api/client, asked once a session): a
        // line for the main menu when this game is older ("" if not), and whether ranked play needs the update.
        void FetchClient();
        std::string UpdateNote(bool &needed);

        // The player's last ranked matches at the master (roadmap BR, the main menu's STATS: GET /api/matches?player=).
        struct RecentMatch
        {
            int64_t ended = 0;              // Unix time
            std::string opponent;
            int halves = 0, opponentHalves = 0;
            int result = 0;                 // 1 won, 0 drawn, -1 lost
            bool rated = false;
            bool ratingKnown = false;
            int before = 0, after = 0;
        };
        struct RecentView
        {
            bool asked = false, known = false;
            std::vector<RecentMatch> matches;   // the newest first
            std::string error;
        };
        void FetchRecent();
        RecentView GetRecent();
        void Frame();

        // A ticket for a ranked room on a relay (the master's id of it, or "server:port"): its bytes and its key (both
        // decoded), or why not.
        using TicketDone = std::function<void(bool ok, const std::string &ticket, const std::string &key, const std::string &why)>;
        void Ticket(const std::string &relayId, TicketDone done);

        // Ranked play's season (roadmap BG): its settings from the master (GET /api/season), handed to
        // Rounds::SetSeason; asked for when HOST DUEL's Ranked is ticked or a ranked room is joined.
        struct SeasonView
        {
            bool known = false, fetching = false;
            int id = 0;
            std::string name, error;
        };
        SeasonView GetSeason();
        void FetchSeason();
        // A ranked match's result reached the relay: the rating is asked for again (twice, a few seconds apart), and
        // the change kept for the end screen.
        void AfterRankedMatch();
        // After a ranked match: the rating before it and now (false until the master's new one came).
        bool RatingChange(int &before, int &after);

        // Test verb: account | account signin | account cancel | account signout | account refresh | account ticket <relay>
        // | account season
        bool RunVerb(const Command &cmd, std::string &message);
    }
}
