#pragma once

#include <string>
#include <vector>

// FTL: Duels on FTL's main menu (roadmap 3.5; docs/design/lobby.md in the FTL: Duels repository): the title screen's
// panel (HOST DUEL, JOIN DUEL, the player's name, the guide; their windows are DuelsLobby.cpp's), the first start's
// name prompt, the tutorial box that explains a duel from hosting to the rounds (instead of FTL's first message box),
// and the players' guide (USAGE.md, in the data mod as data/duels_guide.md) in a scrolling window. The windows wear
// FTL's look (DuelsStyle.cpp).
namespace Duels
{
    namespace Menu
    {
        // MainMenu::Open: the name prompt on a first start (no name in duels.cfg), then the tutorial box unless the
        // player switched it off. A test scenario starts without them (it opens them with the test verb).
        void OnMenuOpen();
        // Over FTL's menu (MainMenu::OnRender, after FTL's own).
        void Render();
        // Before FTL draws its main menu: its NEW GAME and CONTINUE greyed (roadmap BR).
        void BeforeRender();
        // The menu's input while one of our windows is open: true when ours took it (FTL's menu doesn't see it).
        bool MouseMove(int x, int y);
        bool MouseClick(int x, int y);
        bool TextInput(int ch);
        bool TextEvent(int event);
        bool KeyDown(int key);
        bool IsOpen();
        // Test verb "menu name|tutorial|guide|close" and what's open ("menu").
        bool RunVerb(const std::vector<std::string> &args, std::string &message);
    }
}
