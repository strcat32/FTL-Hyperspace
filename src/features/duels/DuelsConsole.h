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
        // A message for the console's log: seen when the console is open (Tab). In debug mode it also shows at the top
        // left for a few seconds, on a dark backdrop (roadmap L: the console is quiet in a normal duel).
        void Print(const std::string &line);

        // The feed at the bottom left, above the power bars and systems (roadmap L): the few lines that matter (a
        // player joins or leaves, a round starts or ends, a draw offer, a lost connection) and the chat. They fade
        // after a while; the chat key opens the feed with more lines and an input line (Enter says it, Escape closes).
        // Both go into the console's log too.
        void Feed(const std::string &line);
        void Chat(const std::string &from, const std::string &text);

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
