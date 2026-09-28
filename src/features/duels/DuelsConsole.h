#pragma once

#include <string>

struct CommandGui;

namespace Duels
{
    // The console (F1): an input line at the top left, where messages are printed, under the last lines printed. While
    // it is open it has the keyboard: Enter runs the line and keeps it open, F1 or Escape closes it, Up and Down go
    // through earlier lines. "DUEL " may be left out. It replaces Hyperspace's console box for F1; Hyperspace's own
    // key ("\") still opens that.
    namespace Console
    {
        // A message: on screen for a few seconds (Hyperspace's PrintHelper), and in the console's list.
        void Print(const std::string &line);

        bool IsOpen();

        // Opens it with that text typed in (the "console" verb, for tests and screenshots).
        bool OpenWith(CommandGui *gui, const std::string &text);

        // Hook entry points. Each returns true when the console took the key, character or text event.
        bool KeyDown(CommandGui *gui, int key);

        // Once at start: an old saved console key that many keyboards can't type (backslash) becomes Tab.
        void MigrateKeys();
        bool TextInput(int ch);
        bool TextEvent(CommandGui *gui, int event);

        // On top of the game, before the mouse cursor (MouseControl::OnRender). Returns true while it is open: the
        // console then shows the recent messages itself.
        bool Render();
    }
}
