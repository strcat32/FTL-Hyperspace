#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Back into a match after the game crashed (roadmap BR; the user, 2026-10-02: "If our game crashed and we still saved/know
// where we did connect to, we can continue"). While a match against another player runs, this game keeps a file of it
// next to the game (duels-rejoin.dat): the way to the match (the relay's room or the host's address, the role, the
// match's token, a ranked room), our ship (its loadout and hull, the systems' damage and power, scrap, missiles), our crew
// (with the ids the other game knows them by), and the refit's and the match flow's own parts (the host's game: the whole
// match, and what it keeps to itself). The file goes when the match ends or is left (the other game is told then); a
// crash leaves it. FTL's CONTINUE in the main menu then starts a run, makes our ship as the file has it and goes back the
// way the file says, as a game cut off from its match does (DuelsNet.cpp), while the other game still waits for it
// (Net::REJOIN_GRACE_MS from when it noticed). The handshake says this game comes back cold: the other game sends what such
// a game needs (DuelsMatch.cpp, DuelsRounds.cpp).
namespace Duels
{
    namespace Rejoin
    {
        // While a match runs (DuelsMatch.cpp's frame): the file every second, and in the frame after the host's match
        // changed (OnSaveNeeded); a way back being tried.
        void OnFrame(double now);
        void OnSaveNeeded();
        // The match was left (the other game is told, or nothing waits any more): this session's file goes.
        void Clear(const char *why);
        // The connection ended (DuelsMatch.cpp): this session's file goes; a way back that didn't work says why and goes
        // back to the main menu.
        void OnDisconnected(const std::string &reason);

        // The main menu (DuelsMenu.cpp): a match to go back to (a file of this version, and the other game may still wait
        // for this one), and a line about it ("Round 2 of 5 against Bob, 1 : 0.5 (about 95 s left)").
        bool Available();
        std::string Describe();
        // CONTINUE (DuelsLobby.cpp starts a run without the hangar): our ship and crew as the file has them, then the way
        // back. False and why when it can't start.
        bool Begin(std::string &message);
        // From Begin until the match is joined again or the way back is given up.
        bool Trying();

        // Back cold (Net::Cold during DuelsMatch.cpp's OnConnected): the file's parts.
        float MatchXp();
        std::vector<std::pair<std::string, std::string>> MatchFine();
        const std::vector<uint8_t> &RoundsPart();
        const std::vector<uint8_t> &RefitPart();
        void OnColdConnected();

        // Over the game while the way back is tried.
        void Render();
        std::string Status();
    }
}
