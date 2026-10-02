#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// Fine settings and presets (roadmap BE; docs/design/settings.md in the FTL: Duels repository). A duel is tuned by many
// numbers the HOST DUEL window doesn't show (the shop's scrap and stock, the phases' times, the anti-stall timer, the
// hazards' timing): each has a name, a default and a line about what it does. The host's own come from duels.cfg (the
// same "name value" lines, below the match's settings) and from presets; a match plays by the host's, which it sends
// with its settings (MSG_SETTINGS), and the guest's own wait until the match is over. A preset is a named cfg file in
// presets\ next to the game, in two tiers: on top the match's settings as HOST DUEL sets them, below them the fine
// settings (the changed ones as lines, the others as comments with their defaults, so that a text editor shows what
// there is).
namespace Duels
{
    class Writer;
    class Reader;
    struct Command;

    namespace Tune
    {
        struct Setting
        {
            const char *name;
            const char *value;      // the default
            char kind;              // 'n' a number, 'l' numbers, 'r' two numbers (from, to), 'w' words (a comma list)
            double min, max;        // for each number
            const char *help;
        };
        const std::vector<Setting> &All();

        // The value now (the match's in a match, else our own): as text, a number, numbers, words, a word among them.
        std::string Text(const std::string &name);
        double Number(const std::string &name);
        std::vector<double> Numbers(const std::string &name);
        bool HasWord(const std::string &name, const std::string &word);

        // Ours (the host's for the next duel; kept in duels.cfg when the settings come from the file): set one (false
        // and why when the name or the value isn't one), back to its default.
        bool Set(const std::string &name, const std::string &value, std::string &why);
        void Reset(const std::string &name);
        // Ours that differ from the defaults, by name.
        std::vector<std::pair<std::string, std::string>> Changed();
        // The match's differ from the defaults (ours outside a match): a custom match, unranked (rules, section 4,
        // proposed).
        bool Custom();

        // A match: the host sends its own with its settings and plays by them; the guest reads them and plays by them;
        // both go back to their own when it ends (the connection does).
        void WriteMatch(Writer &w);
        bool ReadMatch(Reader &r, std::string &note);
        void EndMatch();
        // Back in a match after a crash (roadmap BR, DuelsRejoin.cpp): the match's as the host's game kept them (those
        // off their defaults, as MatchChanged gives them).
        void UseMatch(const std::vector<std::pair<std::string, std::string>> &changed);

        // Ranked play (roadmap BG): a season's fine setting as it is kept ("" and why when the name or the value isn't
        // one; isDefault when it is the default, which a season's list leaves out); the season's fine settings in
        // place of ours for the matches we host in a ranked room (nullptr: ours again); whether the match's are
        // exactly these (those off their defaults); the match's off their defaults (for the settings' hash).
        std::string SeasonValue(const std::string &name, const std::string &value, bool &isDefault, std::string &why);
        void UseSeason(const std::map<std::string, std::string> *fine);
        bool MatchIs(const std::map<std::string, std::string> &fine);
        std::vector<std::pair<std::string, std::string>> MatchChanged();

        // Verbs: tune | tune <name> | tune <name> <value>|default; preset list | preset save <name> | preset load <name>.
        bool RunVerb(const Command &cmd, std::string &message);
        bool RunPresetVerb(const Command &cmd, std::string &message);
    }
}
