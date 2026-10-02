#pragma once

#include <string>

// A replay's screen (roadmap AW; docs/design/demos.md, stage 5, in the FTL: Duels repository): the controls over the
// weapons and drone bars (stop, 10 s back, play or pause, 10 s on, as a media player's symbols (roadmap BC); the speed,
// a time line to click on, the time and the demo's length), under the enemy window whose side is shown and full sensors
// (roadmap BA), and their keys. The replay itself is DuelsDemo.cpp's; this shows it and turns clicks and
// keys into its controls.
namespace Duels
{
    namespace ReplayUi
    {
        void Render();
        // A long seek's cover over the whole screen (roadmap BO): what the seek is for and how far it is.
        void RenderSeekCover();
        // The controls take their clicks before the game does; true when one did.
        bool LButtonDown(int x, int y);
        void MouseMove(int x, int y);
        // While a replay runs: Space plays or pauses, Left and Right go 10 s back or on, Up and Down change the speed,
        // Home goes to the start, V shows the other player's side, F turns full sensors on or off. True when the key was taken: one of those, or any other key but Escape (the menu), as
        // no other key may reach the game in a replay (the console's and the chat's are taken before this).
        bool KeyDown(int key);
        // Tests ("click replay <control>"): the middle of a control as last drawn (stop, back, play, on, speed, line, view,
        // sensors),
        // false when it isn't shown.
        bool ControlCentre(const std::string &name, int &x, int &y);
    }
}
