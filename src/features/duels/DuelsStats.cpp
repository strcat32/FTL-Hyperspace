#include "Global.h"
#include "Duels.h"
#include "DuelsStats.h"
#include "DuelsAccount.h"
#include "DuelsConfig.h"
#include "DuelsHttp.h"
#include "DuelsNet.h"
#include "DuelsShips.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
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
            // A ranked match's record is the master's (DK): kept here too it was a second count that could differ.
            if (line.ranked && !line.ai)
            {
                Log("Stats: the ranked match against %s is the master's record (not kept on this computer)", line.opponent.c_str());
                return;
            }
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
                file << "# the opponent's points x2, rounds, damage score, the opponent's, seconds, how it ended, against the AI (1)\n";
            }
            char damage[32], opponentDamage[32];
            std::snprintf(damage, sizeof(damage), "%.2f", line.damage);
            std::snprintf(opponentDamage, sizeof(opponentDamage), "%.2f", line.opponentDamage);
            file << Clean(line.when) << '\t' << (line.ranked ? 1 : 0) << '\t' << (line.host ? 1 : 0) << '\t' << Clean(line.name) << '\t'
                 << Clean(line.opponent) << '\t' << Clean(line.ship) << '\t' << Clean(line.opponentShip) << '\t' << line.result << '\t'
                 << line.halves << '\t' << line.opponentHalves << '\t' << line.rounds << '\t' << damage << '\t' << opponentDamage << '\t'
                 << line.seconds << '\t' << Clean(line.how) << '\t' << (line.ai ? 1 : 0) << '\n';
            Log("Stats: the match against %s is in %s (%s, %s)", line.opponent.c_str(), FILE_NAME,
                line.result > 0 ? "won" : line.result < 0 ? "lost" : "drawn", line.ai ? "against the AI" : line.ranked ? "ranked" : "unranked");
        }

        // A line's fields, or none for a comment.
        static std::vector<std::string> Fields(std::string text)
        {
            std::vector<std::string> f;
            if (!text.empty() && text.back() == '\r') text.pop_back();
            if (text.empty() || text[0] == '#') return f;
            std::stringstream fields(text);
            std::string field;
            while (std::getline(fields, field, '\t')) f.push_back(field);
            return f;
        }

        int Clear(bool ai)
        {
            std::vector<std::string> kept;
            int gone = 0;
            {
                std::ifstream file(FILE_NAME);
                std::string text;
                while (std::getline(file, text))
                {
                    std::vector<std::string> f = Fields(text);
                    const bool isAi = f.size() > 15 && f[15] == "1";
                    const bool ranked = f.size() > 1 && f[1] == "1";
                    if (f.size() >= 15 && (ai ? isAi : (!isAi && !ranked)))
                    {
                        ++gone;
                        continue;
                    }
                    if (!text.empty() && text.back() == '\r') text.pop_back();
                    kept.push_back(text);
                }
            }
            if (gone == 0) return 0;
            std::ofstream file(FILE_NAME, std::ios::trunc);
            if (!file)
            {
                Log("Stats: can't write %s", FILE_NAME);
                return 0;
            }
            for (const std::string &line : kept) file << line << '\n';
            Log("Stats: %d %s match(es) cleared from %s", gone, ai ? "AI" : "unranked", FILE_NAME);
            return gone;
        }

        std::vector<MatchLine> Load()
        {
            std::vector<MatchLine> lines;
            std::ifstream file(FILE_NAME);
            std::string text;
            while (std::getline(file, text))
            {
                std::vector<std::string> f = Fields(text);
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
                line.ai = f.size() > 15 && f[15] == "1";
                lines.push_back(line);
            }
            return lines;
        }

        // -------------------------------------------------------------------------------------------------------------
        // The master's statistics (roadmap CJ)
        // -------------------------------------------------------------------------------------------------------------

        using Inventory = std::map<std::string, int>;
        static Inventory g_before, g_bought;
        static bool g_snapshot = false;
        static const int FTL_SYSTEMS = 16;   // FTL's own systems (the bays, Duels' own, stay out)

        // A name as the master takes it: upper case, letters, digits and '_', 48 at most.
        static std::string Key(std::string text)
        {
            for (char &c : text)
            {
                c = (char)std::toupper((unsigned char)c);
                if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
            }
            if (text.size() > 48) text.resize(48);
            return text;
        }

        // What our ship has: its weapons, drones, augments and cargo by blueprint, its crew by species, its systems
        // by level, the reactor's bars.
        static Inventory Snapshot()
        {
            Inventory have;
            auto add = [&have](const std::string &key, int count) { if (!key.empty() && count > 0) have[Key(key)] += count; };
            ShipManager *ship = G_->GetShipManager(0);
            if (!ship) return have;
            if (ship->weaponSystem)
                for (ProjectileFactory *weapon : ship->GetWeaponList())
                    if (weapon && weapon->blueprint) add(weapon->blueprint->name, 1);
            if (ship->droneSystem)
                for (Drone *drone : ship->GetDroneList())
                    if (drone && drone->blueprint) add(drone->blueprint->name, 1);
            for (const std::string &augment : ship->GetAugmentationList()) add(augment, 1);
            CApp *app = G_->GetCApp();
            if (app && app->gui)
                for (const std::string &item : app->gui->equipScreen.GetCargoHold()) add(item, 1);
            for (CrewMember *crew : ship->vCrewList)
                if (crew && crew->iShipId == 0 && !crew->bDead) add("CREW_" + crew->species, 1);
            for (ShipSystem *system : ship->vSystemList)
                if (system && system->iSystemType < FTL_SYSTEMS) add("SYSTEM_" + ShipSystem::SystemIdToName(system->iSystemType), system->powerState.second);
            PowerManager *power = PowerManager::GetPowerManager(0);
            if (power) add("REACTOR", power->currentPower.second);
            return have;
        }

        void PrepStarted(bool firstRound)
        {
            if (firstRound) g_bought.clear();
            g_before = Snapshot();
            g_snapshot = true;
        }

        void PrepEnded()
        {
            if (!g_snapshot) return;
            g_snapshot = false;
            for (const auto &item : Snapshot())
            {
                auto before = g_before.find(item.first);
                int more = item.second - (before == g_before.end() ? 0 : before->second);
                if (more > 0) g_bought[item.first] += more;
            }
        }

        // A random number of this installation for the statistics' "players a day" (not linked to anyone; kept in
        // duels.cfg as install_id).
        static std::string InstallId()
        {
            std::string id = Config::Value("install_id");
            if (id.size() == 16 && id.find_first_not_of("0123456789abcdef") == std::string::npos) return id;
            std::random_device device;
            char text[20];
            std::snprintf(text, sizeof(text), "%08x%08x", (unsigned)device(), (unsigned)device());
            id = text;
            Config::SaveValue("install_id", id);
            return id;
        }

        static std::string Hex(const std::string &bytes)
        {
            static const char *const DIGITS = "0123456789abcdef";
            std::string out;
            for (unsigned char c : bytes)
            {
                out += DIGITS[c >> 4];
                out += DIGITS[c & 15];
            }
            return out;
        }

        void SendSummary(const MatchLine &line, uint64_t matchToken)
        {
            // Test duels never count: debug mode on either side, a scripted run (only a test master on this computer gets
            // its summaries, tools/ranked-env.py); and a player may say no.
            if (GetState().debug || Net::PeerDebug() || (AutotestActive() && !Account::MasterIsLocal()) || Config::Value("send_stats") == "off")
                return;
            std::string ship = Ships::Title(line.ship);
            ship.erase(std::remove_if(ship.begin(), ship.end(), [](char c) { return !(std::isalnum((unsigned char)c) || c == ' ' || c == '-'); }),
                       ship.end());
            if (ship.empty()) return;
            if (ship.size() > 32) ship.resize(32);
            if (matchToken == 0)
            {
                std::random_device device;
                matchToken = ((uint64_t)device() << 32) | device();
            }
            char match[20];
            std::snprintf(match, sizeof(match), "%016llx", (unsigned long long)matchToken);
            std::string nonce = line.ranked ? Net::LastTicketNonce() : std::string();
            bool ranked = nonce.size() == 16;
            auto clamp = [](double value, double most) { return std::to_string((long long)std::llround(std::max(0.0, std::min(value, most)))); };
            std::string body = std::string("{\"match\": \"") + match + "\", \"ranked\": " + (ranked ? "true" : "false");
            if (ranked) body += ", \"ticket\": \"" + Hex(nonce) + "\"";
            body += ", \"install\": \"" + InstallId() + "\", " + Account::GameFields() + ", \"ship\": " + Http::Quote(ship) +
                    ", \"result\": \"" + (line.result > 0 ? "won" : line.result < 0 ? "lost" : "drawn") + "\"" +
                    ", \"rounds\": " + clamp(line.rounds, 100) + ", \"seconds\": " + clamp(line.seconds, 36000) +
                    ", \"damage_dealt\": " + clamp(line.damage, 1000000) + ", \"damage_taken\": " + clamp(line.opponentDamage, 1000000) +
                    ", \"bought\": {";
            int items = 0;
            for (const auto &item : g_bought)
            {
                if (items == 64) break;
                body += (items++ ? ", " : "") + Http::Quote(item.first) + ": " + std::to_string(std::min(item.second, 99));
            }
            body += "}}";
            Log("Stats: the match's summary to the master (%s, %d items bought)", ranked ? "ranked" : "unranked", items);
            Account::Request("POST", "/api/summary", body, [](const Http::Response &r)
                             {
                                 Log("Stats: the master %s the summary (%d)", r.status == 200 ? "took" : "didn't take", r.status);
                             });
        }
    }
}
