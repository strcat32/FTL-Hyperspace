#pragma once

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
        bool IsOpen();
        void Render();
        void MouseMove(int x, int y);
        bool MouseClick(int x, int y);
        bool TextInput(int ch);
        bool TextEvent(int event);
        bool KeyDown(int key);

        // Every frame: a room waits for the run the hangar starts (the menu gone, our ship there), then opens or is
        // joined; back from the hangar to the menu drops it. At a run's start FTL's first message box (its story) is
        // closed: the tutorial box explained the duel instead.
        void OnFrame();

        // Test verbs, through "menu": host, join (the windows), choose (their CHOOSE SHIP), cancel, code <code|@file>
        // and password <password> (the fields), refresh and pick <code|@file> (the list: a click on that room's row),
        // start (the hangar's START, clicked).
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
