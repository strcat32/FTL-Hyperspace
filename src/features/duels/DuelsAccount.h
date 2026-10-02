#pragma once

#include <functional>
#include <string>

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
        // Ranked matches' demos for the master (roadmap BQ): each waits as demos\<file>.upload until the master has it.
        void UploadDemos();
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
