#include "Global.h"
#include "Duels.h"
#include "DuelsStats.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Duels
{
    namespace Stats
    {
        static const char *const FILE_NAME = "duels-stats.txt";

        // A field as the file keeps it: no tab and no line end in it.
        static std::string Clean(const std::string &text)
        {
            std::string out = text;
            for (char &c : out)
            {
                if (c == '\t' || c == '\n' || c == '\r') c = ' ';
            }
            return out;
        }

        void Record(const MatchLine &line)
        {
            bool fresh = !std::ifstream(FILE_NAME).good();
            std::ofstream file(FILE_NAME, std::ios::app);
            if (!file)
            {
                Log("Stats: can't write %s", FILE_NAME);
                return;
            }
            if (fresh)
            {
                file << "# FTL: Duels: your finished matches, one a line (the main menu's STATS reads them). Tab-separated:\n";
                file << "# when, ranked, hosted, name, opponent, ship, opponent's ship, result (1 won, 0 drawn, -1 lost), points x2,\n";
                file << "# the opponent's points x2, rounds, damage score, the opponent's, seconds, how it ended\n";
            }
            char damage[32], opponentDamage[32];
            std::snprintf(damage, sizeof(damage), "%.2f", line.damage);
            std::snprintf(opponentDamage, sizeof(opponentDamage), "%.2f", line.opponentDamage);
            file << Clean(line.when) << '\t' << (line.ranked ? 1 : 0) << '\t' << (line.host ? 1 : 0) << '\t' << Clean(line.name) << '\t'
                 << Clean(line.opponent) << '\t' << Clean(line.ship) << '\t' << Clean(line.opponentShip) << '\t' << line.result << '\t'
                 << line.halves << '\t' << line.opponentHalves << '\t' << line.rounds << '\t' << damage << '\t' << opponentDamage << '\t'
                 << line.seconds << '\t' << Clean(line.how) << '\n';
            Log("Stats: the match against %s is in %s (%s)", line.opponent.c_str(), FILE_NAME,
                line.result > 0 ? "won" : line.result < 0 ? "lost" : "drawn");
        }

        std::vector<MatchLine> Load()
        {
            std::vector<MatchLine> lines;
            std::ifstream file(FILE_NAME);
            std::string text;
            while (std::getline(file, text))
            {
                if (!text.empty() && text.back() == '\r') text.pop_back();
                if (text.empty() || text[0] == '#') continue;
                std::vector<std::string> f;
                std::stringstream fields(text);
                std::string field;
                while (std::getline(fields, field, '\t')) f.push_back(field);
                if (f.size() < 15) continue;
                MatchLine line;
                line.when = f[0];
                line.ranked = f[1] == "1";
                line.host = f[2] == "1";
                line.name = f[3];
                line.opponent = f[4];
                line.ship = f[5];
                line.opponentShip = f[6];
                line.result = std::atoi(f[7].c_str());
                line.halves = std::atoi(f[8].c_str());
                line.opponentHalves = std::atoi(f[9].c_str());
                line.rounds = std::atoi(f[10].c_str());
                line.damage = std::atof(f[11].c_str());
                line.opponentDamage = std::atof(f[12].c_str());
                line.seconds = std::atoi(f[13].c_str());
                line.how = f[14];
                lines.push_back(line);
            }
            return lines;
        }
    }
}
