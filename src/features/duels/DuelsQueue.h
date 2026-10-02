#pragma once

#include <string>

// The ranked queue (roadmap BW, the user 2026-10-02: "without needing to decide whether we host or not, we can just join
// the autoqueue from the main menu"): RANKED QUEUE in the main menu's panel asks the master to pair this player with
// another one (POST /api/queue; docs/design/ranked-play.md in the FTL: Duels repository). The master draws which game
// opens the room: that game opens a ranked room (unlisted, with the master's password) at the master's relay and tells
// its code (POST /api/queue/room); the other joins it with the code. Both games start their runs on their own (the
// lobby's ranked room, without its windows); neither player picks a side. A window in the main menu shows how it goes.
namespace Duels
{
    namespace Queue
    {
        // RANKED QUEUE: into the queue (signed in; a ranked season running), the window open.
        void Start();
        // CANCEL (or the window closed): out of the queue.
        void Cancel(const std::string &why);
        // Every frame: the master asked every 2 s, the pair's room opened or joined, its code told.
        void OnFrame();
        bool Active();

        // The window, in the main menu.
        bool IsOpen();
        void Render();
        void MouseMove(int x, int y);
        bool MouseClick(int x, int y);
        bool KeyDown(int key);

        std::string Status();
    }
}
