#pragma once

#include "DuelsNet.h"

#include <string>
#include <vector>

// The way into a duel from FTL's main menu (roadmap 3.5; docs/design/lobby.md in the FTL: Duels repository): the Host
// window (the room: its name, a password, listed or not; the match's settings) and the Join window (the open rooms of
// every relay on the list, a filter by relay, the room picked shown on the right; or a room by its code), then FTL's
// hangar, whose START begins the run and opens (joins) the room, at the first relay of the list that can.
// DuelsMenu.cpp puts the menu's panel up and passes the menu's input on while a window of ours is open.
namespace Duels
{
    namespace Lobby
    {
        void OpenHost();
        void OpenJoin();
        // The demo browser's PLAY (roadmap AU): a run starts as START starts one (no hangar, its cover), and the demo
        // plays with it.
        void PlayReplay(const std::string &path);
        // FTL's CONTINUE (roadmap BR): back into the match the last session lost (DuelsRejoin.cpp): a run starts as START
        // starts one, and once it is there our ship is made as the file has it and the way back is tried.
        void StartRejoin();
        // The ranked queue's pair (roadmap BW, DuelsQueue.cpp): a ranked room at the master's relay, unlisted, with the
        // master's password, opened (host) or joined by its code (guest) without the windows; the run starts as START
        // starts one.
        void StartQueuedRoom(bool host, const Net::RelayAddress &relay, const std::string &code, const std::string &password,
                             const std::string &opponent);
        bool IsOpen();
        void Render();
        void MouseMove(int x, int y);
        bool MouseClick(int x, int y);
        bool TextInput(int ch);
        bool TextEvent(int event);
        bool KeyDown(int key);

        // The end screen's LOBBY (part 5), the Duels window's after STAY: the duel is left, FTL goes to its main menu
        // (as its pause menu's MAIN MENU: CommandGui::GetCommand gives FTL command 5 once), and the room list opens there.
        void ToLobby();
        bool TakeMenuRequest();
        // FTL's main menu with a note there (a way back into a match that didn't work, a room that couldn't be opened or
        // joined), and JOIN DUEL's list under it (listRooms).
        void RequestMenu(const std::string &note, bool listRooms = false);

        // Every frame: a room waits for the run the hangar starts (the menu gone, our ship there), then opens or is
        // joined; back from the hangar to the menu drops it. At a run's start FTL's first message box (its story) is
        // closed: the tutorial box explained the duel instead. FirstBoxAnswered: its choice was given in this run (the
        // box goes then; a match against the AI has no pause from that moment, as FTL answers boxes only in a pause).
        void OnFrame();
        bool FirstBoxAnswered();
        // The run began for a duel whose room is still being opened or joined (HOST DUEL's or JOIN DUEL's START).
        bool OpeningDuel();
        // The title of the room this game opened or joined from the list ("": none known, a room joined by its code).
        std::string RoomTitle();
        // START without the hangar (roadmap 3.9, AL): a dark screen from the click until that box is gone (FTL would
        // show its hangar for a frame, then the box and its PAUSED for another). Over everything, in the menu and the run.
        void RenderCover();

        // Test verbs, through "menu": host, join (the windows), choose (their CHOOSE SHIP), cancel, code <code|@file>
        // and password <password> (the fields), refresh and pick <code|@file> (the list: a click on that room's row),
        // start (the hangar's START, clicked).
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
