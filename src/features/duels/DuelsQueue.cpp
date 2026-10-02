#include "Global.h"
#include "Duels.h"
#include "DuelsAccount.h"
#include "DuelsConsole.h"
#include "DuelsHttp.h"
#include "DuelsLobby.h"
#include "DuelsNet.h"
#include "DuelsQueue.h"
#include "DuelsRounds.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace Duels
{
    namespace Queue
    {
        enum class Stage
        {
            Off,
            Joining,     // POST /api/queue sent
            Waiting,     // in the queue: the master asked every 2 s
            Paired,      // an opponent: the host's game opens the room, the guest's waits for its code
            Started,     // the run began (the lobby opens or joins the room); the host tells the code once it has it
            Failed       // why, in the window
        };

        struct QueueState
        {
            Stage stage = Stage::Off;
            bool open = false;              // the window
            bool asking = false;            // a request to the master on its way
            double askedMs = -1.0e12;
            int64_t since = 0;              // in the queue since (Unix time, the master's)
            int waiting = 0;                // players in the queue, this one too
            int64_t pair = 0;
            bool host = false;
            std::string opponent, password, code, relayHost;
            int opponentRating = 0;
            uint16_t relayPort = 0;
            bool reported = false;          // the host told the room's code
            int rejoins = 0;                // into the queue again after the master forgot this game (at most 3)
            bool rejoin = false;            // ... to be asked next (POST /api/queue again)
            double answeredMs = 0.0;        // the master's last answer (it may be out of reach for a moment)
            double startedMs = 0.0;
            std::string message;
            int mouseX = -1, mouseY = -1;
            Style::Box button;
        };

        static QueueState g;
        static const double POLL_MS = 2000.0;

        static GL_Color Rgb(int r, int gr, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, gr / 255.f, b / 255.f, a);
        }

        static void Fail(const std::string &why)
        {
            g.stage = Stage::Failed;
            g.message = why;
            g.open = true;
            Log("Queue: %s", why.c_str());
        }

        // The master's answer about this player's place (POST and GET /api/queue).
        static void Take(const Http::Response &r)
        {
            g.asking = false;
            if (g.stage != Stage::Joining && g.stage != Stage::Waiting && g.stage != Stage::Paired) return;   // cancelled meanwhile
            Http::Object answer;
            if (r.status != 200 || !Http::ReadObject(r.body, answer))
            {
                // The master out of reach for a moment (restarting: Caddy answers 502 meanwhile, or no answer at all):
                // the queue goes on asking for half a minute, as long as this game was in it.
                bool unreachable = r.status == 0 || r.status == 502 || r.status == 504;
                bool wasIn = g.stage == Stage::Waiting || g.stage == Stage::Paired || g.rejoins > 0;
                if (unreachable && wasIn && WallMs() - g.answeredMs < 30000.0)
                {
                    if (g.stage == Stage::Joining) g.rejoin = true;
                    Log("Queue: the master didn't answer (%d); asking again", r.status);
                    return;
                }
                Http::Object error;
                std::string why = r.status == 0 ? "the master doesn't answer (" + r.error + ")"
                                  : Http::ReadObject(r.body, error) && !error["error"].empty() ? error["error"] : "the master answered " + std::to_string(r.status);
                Fail("No ranked queue: " + why + ".");
                return;
            }
            g.answeredMs = WallMs();
            const std::string &status = answer["status"];
            if (status == "waiting")
            {
                if (g.stage != Stage::Waiting) Log("Queue: waiting for an opponent (%s in the queue)", answer["waiting"].c_str());
                g.stage = Stage::Waiting;
                g.since = std::atoll(answer["since"].c_str());
                g.waiting = std::atoi(answer["waiting"].c_str());
            }
            else if (status == "paired")
            {
                bool fresh = g.stage != Stage::Paired;
                g.stage = Stage::Paired;
                g.pair = std::atoll(answer["pair"].c_str());
                g.host = answer["role"] == "host";
                g.opponent = answer["opponent"];
                g.opponentRating = (int)std::lround(std::atof(answer["opponent_rating"].c_str()));
                g.relayHost = answer["relay_host"];
                g.relayPort = (uint16_t)std::atoi(answer["relay_port"].c_str());
                g.password = answer["password"];
                g.code = answer["code"] == "null" ? std::string() : answer["code"];
                if (fresh)
                {
                    Log("Queue: paired with %s (rating %d): this game %s the room at %s:%u", g.opponent.c_str(), g.opponentRating,
                        g.host ? "opens" : "joins", g.relayHost.c_str(), (unsigned)g.relayPort);
                    Console::Feed("Ranked queue: your opponent is " + g.opponent);
                }
            }
            else if (status == "out" && (g.stage == Stage::Waiting || (g.stage == Stage::Paired && !g.host)) && g.rejoins < 3)
            {
                // The master forgot this game's place (it restarted: its queue is in memory only, or it didn't hear from
                // the game for a while): into the queue again, in a moment (OnFrame).
                ++g.rejoins;
                g.stage = Stage::Joining;
                g.rejoin = true;
                Log("Queue: the master lost this game's place; into the queue again");
            }
            else Fail("The master took this game out of the queue.");
        }

        static void Ask(const std::string &method, const std::string &path, const std::string &body)
        {
            g.asking = true;
            g.askedMs = WallMs();
            Account::Request(method, path, body, Take);
        }

        void Start()
        {
            if (!Account::SignedIn())
            {
                Fail("The ranked queue is for players signed in through Steam: SIGN IN first (the FTL: DUELS panel).");
                return;
            }
            g = QueueState();
            g.open = true;
            g.stage = Stage::Joining;
            g.answeredMs = WallMs();
            if (!Rounds::SeasonKnown()) Account::FetchSeason();   // the match plays by the season's settings
            Log("Queue: into the ranked queue");
            Ask("POST", "/api/queue", "{" + Account::GameFields() + "}");
        }

        void Cancel(const std::string &why)
        {
            if (g.stage == Stage::Off) return;
            bool inQueue = g.stage == Stage::Joining || g.stage == Stage::Waiting || g.stage == Stage::Paired || g.stage == Stage::Started;
            Log("Queue: out of the queue (%s)", why.c_str());
            if (inQueue) Account::Request("POST", "/api/queue/leave", "{}", [](const Http::Response &) {});
            g = QueueState();
        }

        bool Active()
        {
            return g.stage != Stage::Off && g.stage != Stage::Failed;
        }

        // The pair's room: opened by the host's game, joined by the guest's once its code came; the run begins at once.
        static void StartRoom()
        {
            Net::RelayAddress relay;
            relay.server = g.relayHost;
            relay.port = g.relayPort;
            relay.name = g.relayPort == Relay::DEFAULT_PORT ? g.relayHost : g.relayHost + ":" + std::to_string(g.relayPort);
            g.stage = Stage::Started;
            g.startedMs = WallMs();
            g.open = false;
            Lobby::StartQueuedRoom(g.host, relay, g.code, g.password, g.opponent);
        }

        void OnFrame()
        {
            double now = WallMs();
            switch (g.stage)
            {
            case Stage::Joining:
                if (g.rejoin && !g.asking && now - g.askedMs >= POLL_MS)
                {
                    g.rejoin = false;
                    Ask("POST", "/api/queue", "{" + Account::GameFields() + "}");
                }
                break;
            case Stage::Waiting:
                if (!g.asking && now - g.askedMs >= POLL_MS) Ask("GET", "/api/queue", "");
                break;
            case Stage::Paired:
                // The season's settings first (the ranked room plays by them); the guest waits for the room's code.
                if (!Rounds::SeasonKnown())
                {
                    if (!Account::GetSeason().fetching) Account::FetchSeason();
                    break;
                }
                if (g.host || !g.code.empty()) StartRoom();
                else if (!g.asking && now - g.askedMs >= POLL_MS) Ask("GET", "/api/queue", "");
                break;
            case Stage::Started:
            {
                // The host tells the master its room's code (once): the guest's game joins with it.
                if (g.host && !g.reported && Net::GetPhase() == Net::Phase::Hosting && !Net::RelayCode().empty())
                {
                    g.reported = true;
                    std::string body = "{\"pair\": " + std::to_string(g.pair) + ", \"code\": " + Http::Quote(Net::RelayCode()) + "}";
                    Log("Queue: the room %s is open; its code to the master for %s", Net::RelayCode().c_str(), g.opponent.c_str());
                    Account::Request("POST", "/api/queue/room", body, [](const Http::Response &r)
                                     {
                                         if (r.status == 200) return;
                                         Log("Queue: the master didn't take the room's code (%d)", r.status);
                                         // The pair is gone (the master restarted meanwhile): nobody comes to this room.
                                         if (r.status == 404) Console::Feed("Ranked queue: the master lost this pair; leave the room (ESC, MAIN MENU) and join the queue again");
                                     });
                }
                // The match began (both games in it): the queue's part is done. A room that never opened (the lobby said
                // why in a note): out of the queue.
                if (Net::IsConnected()) g = QueueState();
                else if (!Lobby::OpeningDuel() && Net::GetPhase() == Net::Phase::Idle && now - g.startedMs > 3000.0) Cancel("the room didn't open");
                break;
            }
            default:
                break;
            }
        }

        // -------------------------------------------------------------------------------------------------------------
        // The window
        // -------------------------------------------------------------------------------------------------------------

        static const float W = 560.f, H = 230.f, X = (1280.f - W) / 2.f, Y = 220.f;

        bool IsOpen()
        {
            return g.open;
        }

        static std::string Clock(int64_t seconds)
        {
            if (seconds < 0) seconds = 0;
            char text[16];
            std::snprintf(text, sizeof(text), "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
            return text;
        }

        void Render()
        {
            if (!g.open) return;
            Style::Dialog(X, Y, W, H, "RANKED QUEUE");
            float left = X + 30.f, top = Y + 28.f, width = W - 60.f;
            const GL_Color white = Rgb(255, 255, 255), soft = Rgb(206, 210, 216), gold = Rgb(255, 235, 170);
            auto line = [&](int font, const std::string &text, const GL_Color &colour)
            {
                CSurface::GL_SetColor(colour);
                top += freetype::easy_printAutoNewlines(font, left, top, (int)width, text).y - top + 8.f;
            };
            std::string season = Rounds::SeasonKnown() ? Rounds::SeasonName() + "'s settings" : std::string("the season's settings");
            switch (g.stage)
            {
            case Stage::Joining:
                line(13, "Asking the master...", white);
                break;
            case Stage::Waiting:
                line(13, "Looking for an opponent:  " + Clock((int64_t)std::time(nullptr) - g.since), white);
                line(12, std::to_string(g.waiting) + (g.waiting == 1 ? " player" : " players") + " in the queue.", soft);
                line(10, "The master pairs two players and opens a ranked room for them; the match plays by " + season + ".", soft);
                break;
            case Stage::Paired:
            case Stage::Started:
                line(13, "Opponent found: " + g.opponent + (g.opponentRating > 0 ? " (" + std::to_string(g.opponentRating) + ")" : std::string()), gold);
                line(12, g.host ? std::string("Your game opens the room...") : g.code.empty() ? g.opponent + "'s game opens the room..." : "Joining the room...", soft);
                break;
            case Stage::Failed:
                line(12, g.message, Rgb(255, 160, 150));
                break;
            default:
                break;
            }
            g.button.x = X + W - 30.f - 130.f;
            g.button.y = Y + H - 58.f;
            g.button.w = 130.f;
            g.button.h = 34.f;
            bool hover = g.button.Contains(g.mouseX, g.mouseY);
            Style::Button(g.button.x, g.button.y, g.button.w, g.button.h, g.stage == Stage::Failed ? "CLOSE" : "CANCEL", 12,
                          hover ? Style::Look::Hover : Style::Look::Idle);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        static void CloseWindow()
        {
            if (g.stage == Stage::Failed) g = QueueState();
            else Cancel("cancelled");
            g.open = false;
        }

        bool MouseClick(int x, int y)
        {
            if (!g.open) return false;
            if (g.button.Contains(x, y)) CloseWindow();
            return true;
        }

        bool KeyDown(int key)
        {
            if (!g.open) return false;
            if (key == SDLK_ESCAPE) CloseWindow();
            return true;
        }

        std::string Status()
        {
            static const char *const STAGES[] = {"off", "joining", "waiting", "paired", "started", "failed"};
            std::string text = std::string("queue: ") + STAGES[(int)g.stage];
            if (g.stage == Stage::Waiting) text += ", " + std::to_string(g.waiting) + " waiting";
            if (g.stage == Stage::Paired || g.stage == Stage::Started)
            {
                text += ", pair " + std::to_string(g.pair) + " with " + g.opponent + " (" + (g.host ? "we open the room" : "we join") + ")" +
                        (g.code.empty() ? "" : ", room " + g.code);
            }
            if (!g.message.empty()) text += ": " + g.message;
            return text;
        }
    }
}
