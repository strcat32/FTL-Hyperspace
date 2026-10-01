#include "Global.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConfig.h"
#include "DuelsHttp.h"
#include "DuelsScript.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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
            std::string code, url;
            double nextPollMs = 0.0, deadlineMs = 0.0, intervalMs = 3000.0;
            bool polling = false;            // a poll is on its way
            uint32_t linkRun = 0;            // a cancelled sign-in's late answers are dropped
            bool ratingKnown = false;
            int rating = 0, rd = 0, matches = 0;
            std::string message;
        };

        static AccountState g;

        static std::string Base()
        {
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
                               g.message.clear();
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

        void StartSignIn()
        {
            Load();
            if (g.state != State::SignedOut) return;
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

        void SignOut()
        {
            Load();
            ++g.linkRun;
            g.state = State::SignedOut;
            g.key.clear();
            g.name.clear();
            g.steamId.clear();
            g.ratingKnown = false;
            g.code.clear();
            g.url.clear();
            g.polling = false;
            std::remove(FILE_NAME);
            g.message = "Signed out";
            Log("Account: signed out (%s deleted)", FILE_NAME);
        }

        void Frame()
        {
            Http::Frame();
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

        void Ticket(const std::string &relayId, TicketDone done)
        {
            Load();
            if (g.state != State::SignedIn)
            {
                if (done) done(false, "", "", "not signed in");
                return;
            }
            Http::Send("POST", Base() + "/api/ticket", "{\"relay\": " + Http::Quote(relayId) + "}", g.key,
                       [done](const Http::Response &r)
                       {
                           Http::Object ticket;
                           if (r.status == 200 && Http::ReadObject(r.body, ticket) && !ticket["ticket"].empty() && !ticket["ticket_key"].empty())
                           {
                               if (done) done(true, ticket["ticket"], Unbase64(ticket["ticket_key"]), "");
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
            message = "usage: account | account signin | account cancel | account signout | account refresh | account ticket <relay>";
            return false;
        }
    }
}
