#include "Global.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConfig.h"
#include "DuelsHttp.h"
#include "DuelsNet.h"
#include "DuelsRounds.h"
#include "DuelsScript.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace Duels
{
    namespace Account
    {
        static const char *const FILE_NAME = "duels-account.cfg";

        struct AccountState
        {
            bool loaded = false;
            State state = State::SignedOut;
            std::string key, name, steamId;
            std::string master;          // the master that issued the key (its address); "": duels.cfg's
            std::string linkBase;        // the master a sign-in under way asks
            std::string code, url;
            double nextPollMs = 0.0, deadlineMs = 0.0, intervalMs = 3000.0;
            bool polling = false;            // a poll is on its way
            uint32_t linkRun = 0;            // a cancelled sign-in's late answers are dropped
            bool ratingKnown = false;
            int rating = 0, rd = 0, matches = 0;
            std::string message;
            // The season (roadmap BG).
            SeasonView season;
            // After a ranked match: the rating asked for again at these times; the one before, the one after.
            std::vector<double> refreshAt;
            int ratingBefore = 0, matchesBefore = 0, ratingAfter = 0;
            bool changeKnown = false, awaitingChange = false;
            // Ranked matches' demos on their way to the master (roadmap BQ): one at a time.
            int wins = 0, losses = 0, draws = 0, rank = 0;
            std::string seasonName;
            RecentView recent;              // the last ranked matches (roadmap BR)
            // The master's newest version and the protocol ranked play needs (roadmap CE).
            bool clientAsked = false, clientKnown = false;
            std::string newest;
            int rankedProtocol = 0;
        };

        static AccountState g;

        // The account's master: the one that issued the key (a test's local master, or after duels.cfg names another,
        // it stays the key's); signed out, duels.cfg's.
        static std::string Base()
        {
            if (g.state == State::SignedIn && !g.master.empty()) return g.master;
            if (g.state == State::Linking && !g.linkBase.empty()) return g.linkBase;
            return Config::MasterUrl();
        }

        // duels-account.cfg: the key, the Steam ID, the name (its own file: the key is the game's one secret).
        static void Load()
        {
            if (g.loaded) return;
            g.loaded = true;
            std::ifstream file(FILE_NAME);
            std::string line;
            while (std::getline(file, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                size_t space = line.find(' ');
                if (space == std::string::npos) continue;
                std::string key = line.substr(0, space), value = line.substr(space + 1);
                if (key == "key") g.key = value;
                else if (key == "steam_id") g.steamId = value;
                else if (key == "name") g.name = value;
                else if (key == "master") g.master = value;
            }
            if (!g.key.empty())
            {
                g.state = State::SignedIn;
                Log("Account: signed in as %s (from %s)", g.name.empty() ? "(no name)" : g.name.c_str(), FILE_NAME);
            }
        }

        static void Save()
        {
            std::ofstream file(FILE_NAME, std::ios::trunc);
            file << "# FTL: Duels account: the master server's key for your Steam account. Keep it to yourself;\n";
            file << "# SIGN OUT in the FTL: DUELS panel deletes this file.\n";
            file << "key " << g.key << "\n";
            file << "steam_id " << g.steamId << "\n";
            file << "name " << g.name << "\n";
            file << "master " << g.master << "\n";
            if (!file) Log("Account: can't write %s", FILE_NAME);
        }

        View GetView()
        {
            Load();
            View v;
            v.state = g.state;
            v.code = g.code;
            v.url = g.url;
            v.name = g.name;
            v.steamId = g.steamId;
            v.ratingKnown = g.ratingKnown;
            v.rating = g.rating;
            v.rd = g.rd;
            v.matches = g.matches;
            v.wins = g.wins;
            v.losses = g.losses;
            v.draws = g.draws;
            v.rank = g.rank;
            v.seasonName = g.seasonName;
            v.message = g.message;
            return v;
        }

        bool SignedIn()
        {
            Load();
            return g.state == State::SignedIn;
        }

        static std::string ErrorOf(const Http::Response &r)
        {
            if (r.status == 0) return "the master doesn't answer (" + r.error + ")";
            if (r.status == 404) return "the master doesn't offer it yet (404)";
            Http::Object object;
            if (Http::ReadObject(r.body, object) && !object["error"].empty()) return object["error"] + " (" + std::to_string(r.status) + ")";
            return "the master's answer " + std::to_string(r.status);
        }

        void Refresh()
        {
            Load();
            if (g.state != State::SignedIn) return;
            Http::Send("GET", Base() + "/api/me", "", g.key,
                       [](const Http::Response &r)
                       {
                           if (g.state != State::SignedIn) return;
                           Http::Object me;
                           if (r.status == 200 && Http::ReadObject(r.body, me))
                           {
                               if (!me["name"].empty()) g.name = me["name"];
                               g.ratingKnown = !me["rating"].empty();
                               g.rating = (int)(std::atof(me["rating"].c_str()) + 0.5);
                               g.rd = (int)(std::atof(me["rd"].c_str()) + 0.5);
                               g.matches = std::atoi(me["matches"].c_str());
                               g.wins = std::atoi(me["wins"].c_str());
                               g.losses = std::atoi(me["losses"].c_str());
                               g.draws = std::atoi(me["draws"].c_str());
                               g.rank = std::atoi(me["rank"].c_str());
                               g.seasonName = me["season_name"];
                               g.message.clear();
                               // A ranked match counted: the new rating, for the end screen.
                               if (g.awaitingChange && g.matches != g.matchesBefore)
                               {
                                   g.awaitingChange = false;
                                   g.changeKnown = true;
                                   g.ratingAfter = g.rating;
                                   Log("Account: the ranked match counted: rating %d -> %d", g.ratingBefore, g.ratingAfter);
                               }
                               Log("Account: %s, rating %d (RD %d), %d match(es)", g.name.c_str(), g.rating, g.rd, g.matches);
                           }
                           else if (r.status == 401)
                           {
                               // The key is no more (signed out on the site, the data deleted): sign in again.
                               Log("Account: the master doesn't know the key any more: signed out");
                               SignOut();
                               g.message = "Signed out by the master: SIGN IN again";
                           }
                           else
                           {
                               g.message = ErrorOf(r);
                               Log("Account: no rating: %s", g.message.c_str());
                           }
                       });
        }

        void FetchRecent()
        {
            Load();
            g.recent.asked = true;
            if (g.state != State::SignedIn || g.steamId.empty())
            {
                g.recent.known = false;
                g.recent.error = "not signed in";
                return;
            }
            const std::string own = g.steamId;
            Http::Send("GET", Base() + "/api/matches?player=" + own, "", "",
                       [own](const Http::Response &r)
                       {
                           std::vector<Http::Object> list;
                           if (r.status != 200 || !Http::ReadList(r.body, list))
                           {
                               g.recent.known = false;
                               g.recent.error = ErrorOf(r);
                               Log("Account: no recent matches: %s", g.recent.error.c_str());
                               return;
                           }
                           g.recent.matches.clear();
                           for (Http::Object &m : list)
                           {
                               bool host = m["host_steam_id"] == own;
                               RecentMatch match;
                               match.ended = std::atoll(m["ended"].c_str());
                               match.opponent = host ? m["guest"] : m["host"];
                               match.halves = std::atoi((host ? m["host_halves"] : m["guest_halves"]).c_str());
                               match.opponentHalves = std::atoi((host ? m["guest_halves"] : m["host_halves"]).c_str());
                               const std::string &winner = m["winner"];
                               match.result = winner == "draw" || winner == "none" ? 0 : (winner == "host") == host ? 1 : -1;
                               match.rated = m["rated"] == "true";
                               const std::string &before = host ? m["host_before"] : m["guest_before"];
                               const std::string &after = host ? m["host_after"] : m["guest_after"];
                               match.ratingKnown = !before.empty() && before != "null" && !after.empty() && after != "null";
                               match.before = (int)(std::atof(before.c_str()) + 0.5);
                               match.after = (int)(std::atof(after.c_str()) + 0.5);
                               g.recent.matches.push_back(match);
                           }
                           g.recent.known = true;
                           g.recent.error.clear();
                           Log("Account: %u recent ranked match(es)", (unsigned)g.recent.matches.size());
                       });
        }

        RecentView GetRecent()
        {
            return g.recent;
        }

        void StartSignIn()
        {
            Load();
            if (g.state != State::SignedOut) return;
            g.linkBase = Config::MasterUrl();
            g.state = State::Linking;
            g.code.clear();
            g.url.clear();
            g.message = "Asking the master for a code";
            const uint32_t run = ++g.linkRun;
            Http::Send("POST", Base() + "/api/link", "{}", "",
                       [run](const Http::Response &r)
                       {
                           if (run != g.linkRun || g.state != State::Linking) return;
                           Http::Object link;
                           if (r.status != 200 || !Http::ReadObject(r.body, link) || link["code"].empty() || link["url"].empty())
                           {
                               g.state = State::SignedOut;
                               g.message = "No sign-in: " + ErrorOf(r);
                               Log("Account: %s", g.message.c_str());
                               return;
                           }
                           g.code = link["code"];
                           g.url = link["url"];
                           double interval = std::atof(link["interval"].c_str()), expires = std::atof(link["expires_in"].c_str());
                           g.intervalMs = std::max(1.0, interval > 0.0 ? interval : 3.0) * 1000.0;
                           g.nextPollMs = WallMs() + g.intervalMs;
                           g.deadlineMs = WallMs() + (expires > 0.0 ? expires : 600.0) * 1000.0;
                           g.message = "Sign in through Steam in your browser (code " + g.code + ")";
                           Log("Account: the code %s; the browser on %s", g.code.c_str(), g.url.c_str());
#ifdef _WIN32
                           // The player's browser on the master's page (a test's game doesn't open one).
                           if (!AutotestActive() && (g.url.compare(0, 8, "https://") == 0 || g.url.compare(0, 7, "http://") == 0))
                               ShellExecuteA(nullptr, "open", g.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
                       });
        }

        void CancelSignIn()
        {
            if (g.state != State::Linking) return;
            ++g.linkRun;
            g.state = State::SignedOut;
            g.code.clear();
            g.url.clear();
            g.polling = false;
            g.message = "Sign-in cancelled";
            Log("Account: the sign-in cancelled");
        }

        void Request(const std::string &method, const std::string &path, const std::string &body, Http::Callback done)
        {
            Load();
            Http::Send(method, Base() + path, body, g.key, done);
        }

        void SignOut()
        {
            Load();
            ++g.linkRun;
            g.state = State::SignedOut;
            g.key.clear();
            g.name.clear();
            g.steamId.clear();
            g.master.clear();
            g.ratingKnown = false;
            g.code.clear();
            g.url.clear();
            g.polling = false;
            std::remove(FILE_NAME);
            g.message = "Signed out";
            Log("Account: signed out (%s deleted)", FILE_NAME);
        }

        SeasonView GetSeason()
        {
            return g.season;
        }

        void FetchSeason()
        {
            if (g.season.fetching) return;
            g.season.fetching = true;
            Http::Send("GET", Base() + "/api/season", "", "",
                       [](const Http::Response &r)
                       {
                           g.season.fetching = false;
                           Http::Object season;
                           if (r.status != 200 || !Http::ReadObject(r.body, season) || season["id"].empty())
                           {
                               g.season.error = "no season: " + ErrorOf(r);
                               Log("Account: %s", g.season.error.c_str());
                               return;
                           }
                           int id = std::atoi(season["id"].c_str());
                           std::string why;
                           if (Rounds::SetSeason(id, season["name"], season["config"], why))
                           {
                               g.season.known = true;
                               g.season.id = id;
                               g.season.name = season["name"];
                               g.season.error.clear();
                           }
                           else
                           {
                               g.season.known = false;
                               g.season.error = why;
                           }
                       });
        }

        void AfterRankedMatch()
        {
            Load();
            if (g.state != State::SignedIn) return;
            g.ratingBefore = g.ratingKnown ? g.rating : 1500;
            g.matchesBefore = g.matches;
            g.changeKnown = false;
            g.awaitingChange = true;
            const double now = WallMs();
            g.refreshAt = {now + 2500.0, now + 8000.0, now + 20000.0};
            Log("Account: the ranked match's result is at the relay: the rating is asked for again");
        }

        bool RatingChange(int &before, int &after)
        {
            if (!g.changeKnown) return false;
            before = g.ratingBefore;
            after = g.ratingAfter;
            return true;
        }

        void Frame()
        {
            Http::Frame();
            if (!g.refreshAt.empty() && WallMs() >= g.refreshAt.front())
            {
                g.refreshAt.erase(g.refreshAt.begin());
                if (g.awaitingChange) Refresh();
            }
            if (g.state != State::Linking || g.code.empty() || g.polling) return;
            const double now = WallMs();
            if (now >= g.deadlineMs)
            {
                CancelSignIn();
                g.message = "The code expired: SIGN IN again";
                return;
            }
            if (now < g.nextPollMs) return;
            g.polling = true;
            const uint32_t run = g.linkRun;
            Http::Send("GET", Base() + "/api/link/" + g.code, "", "",
                       [run](const Http::Response &r)
                       {
                           if (run != g.linkRun || g.state != State::Linking) return;
                           g.polling = false;
                           g.nextPollMs = WallMs() + g.intervalMs;
                           Http::Object link;
                           bool read = Http::ReadObject(r.body, link);
                           if (r.status == 200 && read && link["status"] == "linked" && !link["account_key"].empty())
                           {
                               g.master = g.linkBase;
                               g.state = State::SignedIn;
                               g.key = link["account_key"];
                               g.steamId = link["steam_id"];
                               g.name = link["name"];
                               g.code.clear();
                               g.url.clear();
                               g.message.clear();
                               Save();
                               Log("Account: signed in as %s", g.name.c_str());
                               Refresh();
                           }
                           else if (r.status == 410 || (read && link["status"] == "expired"))
                           {
                               CancelSignIn();
                               g.message = "The code expired: SIGN IN again";
                           }
                           else if (r.status != 200)
                           {
                               g.message = "Waiting for the sign-in (" + ErrorOf(r) + ")";
                           }
                       });
        }

        // Base64 (a ticket's key).
        static std::string Unbase64(const std::string &text)
        {
            std::string out;
            int bits = 0, value = 0;
            for (char c : text)
            {
                int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52
                          : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
                if (digit < 0) continue;   // padding, spaces
                value = (value << 6) | digit;
                bits += 6;
                if (bits >= 8)
                {
                    bits -= 8;
                    out += (char)((value >> bits) & 0xFF);
                }
            }
            return out;
        }

        bool MasterIsLocal()
        {
            Load();
            std::string base = Base();
            return base.compare(0, 17, "http://127.0.0.1:") == 0 || base.compare(0, 17, "http://localhost:") == 0;
        }

        std::string GameFields()
        {
            return "\"protocol\": " + std::to_string(Net::Protocol()) + ", \"version\": " + Http::Quote(VERSION);
        }

        void FetchClient()
        {
            if (g.clientAsked) return;
            g.clientAsked = true;
            Http::Send("GET", Base() + "/api/client", "", "",
                       [](const Http::Response &r)
                       {
                           Http::Object client;
                           if (r.status != 200 || !Http::ReadObject(r.body, client)) return;   // (an older master: nothing to say)
                           g.clientKnown = true;
                           g.newest = client["newest"] == "null" ? std::string() : client["newest"];
                           g.rankedProtocol = client["protocol"] == "null" ? 0 : std::atoi(client["protocol"].c_str());
                           Log("Account: the master's newest version %s, ranked play needs protocol %d (ours %s, %d)",
                               g.newest.empty() ? "-" : g.newest.c_str(), g.rankedProtocol, VERSION, (int)Net::PROTOCOL_VERSION);
                       });
        }

        // "0.8.1" against "0.8.1-dev": by their numbers, then a release after its development builds. Negative: a is older.
        static int CompareVersions(const std::string &a, const std::string &b)
        {
            auto split = [](const std::string &text, std::vector<int> &numbers, std::string &suffix)
            {
                size_t dash = text.find('-');
                std::string head = text.substr(0, dash);
                suffix = dash == std::string::npos ? std::string() : text.substr(dash + 1);
                size_t start = 0;
                while (start <= head.size())
                {
                    size_t dot = head.find('.', start);
                    numbers.push_back(std::atoi(head.substr(start, dot == std::string::npos ? std::string::npos : dot - start).c_str()));
                    if (dot == std::string::npos) break;
                    start = dot + 1;
                }
                while (numbers.size() < 3) numbers.push_back(0);
            };
            std::vector<int> na, nb;
            std::string sa, sb;
            split(a, na, sa);
            split(b, nb, sb);
            for (size_t i = 0; i < std::max(na.size(), nb.size()); ++i)
            {
                int x = i < na.size() ? na[i] : 0, y = i < nb.size() ? nb[i] : 0;
                if (x != y) return x < y ? -1 : 1;
            }
            if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;
            return 0;
        }

        std::string UpdateNote(bool &needed)
        {
            needed = false;
            if (!g.clientKnown) return "";
            if (g.rankedProtocol > (int)Net::Protocol())
            {
                needed = true;
                return "Ranked play needs " + (g.newest.empty() ? std::string("a newer FTL: Duels") : "FTL: Duels " + g.newest) + " (yours: " + VERSION + ").";
            }
            if (!g.newest.empty() && CompareVersions(VERSION, g.newest) < 0) return "FTL: Duels " + g.newest + " is out (yours: " + VERSION + ").";
            return "";
        }

        void Ticket(const std::string &relayId, TicketDone done)
        {
            Load();
            if (g.state != State::SignedIn)
            {
                if (done) done(false, "", "", "not signed in");
                return;
            }
            Http::Send("POST", Base() + "/api/ticket", "{\"relay\": " + Http::Quote(relayId) + ", " + GameFields() + "}", g.key,
                       [done](const Http::Response &r)
                       {
                           Http::Object ticket;
                           if (r.status == 200 && Http::ReadObject(r.body, ticket) && !ticket["ticket"].empty() && !ticket["ticket_key"].empty())
                           {
                               if (done) done(true, Unbase64(ticket["ticket"]), Unbase64(ticket["ticket_key"]), "");
                               return;
                           }
                           if (r.status == 401) Refresh();   // signs out if the key is gone
                           if (done) done(false, "", "", ErrorOf(r));
                       });
        }

        bool RunVerb(const Command &cmd, std::string &message)
        {
            Load();
            if (cmd.args.size() == 1)
            {
                View v = GetView();
                message = v.state == State::SignedIn ? "signed in as " + v.name + " (" + v.steamId + ")" +
                                                           (v.ratingKnown ? ", rating " + std::to_string(v.rating) + " (RD " + std::to_string(v.rd) +
                                                                                "), " + std::to_string(v.matches) + " match(es)"
                                                                          : std::string())
                          : v.state == State::Linking ? "signing in: code " + v.code + " at " + v.url
                                                      : std::string("signed out");
                if (!v.message.empty()) message += " (" + v.message + ")";
                message += "; the master: " + Base();
                return true;
            }
            if (ArgIs(cmd, 1, "signin"))
            {
                if (g.state != State::SignedOut) { message = "signed in or signing in already"; return false; }
                StartSignIn();
                message = "signing in at " + Base();
                return true;
            }
            if (ArgIs(cmd, 1, "cancel"))
            {
                CancelSignIn();
                message = "sign-in cancelled";
                return true;
            }
            if (ArgIs(cmd, 1, "signout"))
            {
                SignOut();
                message = "signed out";
                return true;
            }
            if (ArgIs(cmd, 1, "refresh"))
            {
                if (g.state != State::SignedIn) { message = "not signed in"; return false; }
                Refresh();
                message = "asking the master for the rating";
                return true;
            }
            if (ArgIs(cmd, 1, "ticket") && cmd.raw.size() > 2)
            {
                std::string relay = cmd.raw[2];
                Ticket(relay, [relay](bool ok, const std::string &ticket, const std::string &key, const std::string &why)
                       {
                           if (ok) Log("Account: a ticket for %s (%u bytes, key %u bytes)", relay.c_str(), (unsigned)ticket.size(), (unsigned)key.size());
                           else Log("Account: no ticket for %s: %s", relay.c_str(), why.c_str());
                       });
                message = "asking the master for a ticket for " + relay;
                return true;
            }
            if (ArgIs(cmd, 1, "season"))
            {
                FetchSeason();
                SeasonView season = GetSeason();
                message = season.known ? "season " + std::to_string(season.id) + " (" + season.name + "); asking the master again"
                                       : "asking the master for the season" + (season.error.empty() ? std::string() : " (" + season.error + ")");
                return true;
            }
            message = "usage: account | account signin | account cancel | account signout | account refresh | account ticket <relay> | "
                      "account season";
            return false;
        }
    }
}
