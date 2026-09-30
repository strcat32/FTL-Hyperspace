#include "Global.h"
#include "Duels.h"
#include "DuelsHud.h"
#include "DuelsNet.h"
#include "DuelsScreen.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <string>

namespace Duels
{
    namespace Hud
    {
        static const double SAMPLE_MS = 1000.0;   // the numbers change once a second, so they can be read
        static const size_t WINDOW = 5;           // rates and loss over the last 5 seconds

        struct Sample
        {
            double atMs;
            Net::Numbers numbers;
        };

        struct NetStatsState
        {
            bool on = false;
            std::deque<Sample> samples;
            std::string lines[3];
        };

        static NetStatsState g_net;

        void SetNetStats(bool on)
        {
            g_net.on = on;
            g_net.samples.clear();
            for (std::string &line : g_net.lines) line.clear();
        }

        bool NetStats()
        {
            return g_net.on;
        }

        static bool InGame()
        {
            WorldManager *world = G_->GetWorld();
            CApp *app = G_->GetCApp();
            return world && world->playerShip && world->commandGui && app && !app->menu.bOpen;
        }

        // The lines from the samples: rates over the window, loss as the share of the other side's packets that never
        // arrived.
        static void UpdateLines()
        {
            const Net::Numbers &now = g_net.samples.back().numbers;
            if (!now.connected)
            {
                g_net.lines[0] = "Network: not connected";
                g_net.lines[1].clear();
                g_net.lines[2].clear();
                return;
            }
            const Sample &first = g_net.samples.front();
            const Sample &last = g_net.samples.back();
            double seconds = (last.atMs - first.atMs) / 1000.0;
            const Net::Numbers &then = first.numbers;
            char buffer[160];
            if (seconds <= 0.0 || then.packetsReceived > now.packetsReceived)
            {
                snprintf(buffer, sizeof(buffer), "Ping %.0f ms", now.rttMs);
                g_net.lines[0] = buffer;
                g_net.lines[1] = "Traffic: measuring...";
            }
            else
            {
                double received = now.packetsReceived - then.packetsReceived;
                double missed = now.packetsMissed >= then.packetsMissed ? now.packetsMissed - then.packetsMissed : 0.0;
                double loss = received + missed > 0.0 ? 100.0 * missed / (received + missed) : 0.0;
                snprintf(buffer, sizeof(buffer), "Ping %.0f ms (best %.0f)   Loss %.1f%%   Resent %u", now.rttMs,
                         now.bestRttMs, loss, now.reliableResent - then.reliableResent);
                g_net.lines[0] = buffer;
                snprintf(buffer, sizeof(buffer), "Up %.0f pkt/s %.1f KB/s   Down %.0f pkt/s %.1f KB/s",
                         (now.packetsSent - then.packetsSent) / seconds, (now.bytesSent - then.bytesSent) / seconds / 1024.0,
                         received / seconds, (now.bytesReceived - then.bytesReceived) / seconds / 1024.0);
                g_net.lines[1] = buffer;
            }
            if (now.relay)
            {
                snprintf(buffer, sizeof(buffer), "Through the relay, room %s   Waiting for acks: %u", now.relayCode.c_str(),
                         (unsigned)now.pendingReliable);
            }
            else
            {
                snprintf(buffer, sizeof(buffer), "Direct connection   Waiting for acks: %u", (unsigned)now.pendingReliable);
            }
            g_net.lines[2] = buffer;
        }

        static void RenderNetStats()
        {
            double now = WallMs();
            if (g_net.samples.empty() || now - g_net.samples.back().atMs >= SAMPLE_MS)
            {
                Net::Numbers numbers = Net::GetNumbers();
                // A new connection starts its counters again.
                if (!g_net.samples.empty() && numbers.packetsSent < g_net.samples.back().numbers.packetsSent) g_net.samples.clear();
                g_net.samples.push_back(Sample{now, numbers});
                while (g_net.samples.size() > WINDOW + 1) g_net.samples.pop_front();
                UpdateLines();
            }

            // At the top right, under the version label, in its tiny font (roadmap L).
            int font = Screen::VersionFont();
            if (font < 0) font = 10;
            const float right = 1277.f, top = 13.f, lineHeight = 11.f;
            int count = 0;
            float width = 0.f;
            for (const std::string &line : g_net.lines)
            {
                if (line.empty()) continue;
                ++count;
                width = std::max(width, (float)freetype::easy_measureWidth(font, line));
            }
            if (count == 0) return;
            CSurface::GL_DrawRect(right - width - 4.f, top - 1.f, width + 6.f, lineHeight * count + 3.f, GL_Color(0.f, 0.f, 0.f, 0.5f));
            CSurface::GL_SetColor(GL_Color(0.75f, 0.9f, 1.f, 1.f));
            float y = top;
            for (const std::string &line : g_net.lines)
            {
                if (line.empty()) continue;
                freetype::easy_printRightAlign(font, right, y, line);
                y += lineHeight;
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // Test: the match display's text in each of FTL's fonts (the ids the game and Hyperspace use), for a screenshot.
        static double g_fontTestUntil = 0.0;

        void FontTest(double seconds)
        {
            g_fontTestUntil = WallMs() + seconds * 1000.0;
        }

        static void RenderFontTest()
        {
            static const int FONTS[] = {0, 10, 12, 13, 24, 51, 52, 62, 63};
            CSurface::GL_DrawRect(90.f, 40.f, 760.f, 640.f, GL_Color(0.f, 0.f, 0.f, 0.8f));
            float y = 50.f;
            for (int font : FONTS)
            {
                CSurface::GL_SetColor(GL_Color(1.f, 0.95f, 0.8f, 1.f));
                freetype::easy_print(font, 100.f, y, std::to_string(font) + ":  Round 2 of 5   Preparation 0:42");
                freetype::easy_print(font, 100.f, y + 28.f, "Rounds 1 : 0   Damage 34.5 : 12.0");
                y += 68.f;
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // Debug mode in sight (roadmap T): a red "DEBUG MODE!" at the top middle, above the jump and ship buttons, in
        // both players' games (either one's debug mode gives both the test commands; such a match is unranked).
        static void RenderDebugMode()
        {
            const int font = 12;
            const std::string text = "DEBUG MODE!";
            float width = (float)freetype::easy_measureWidth(font, text);
            const float x = 640.f, y = 1.f;
            CSurface::GL_DrawRect(x - width / 2.f - 6.f, y, width + 12.f, 15.f, GL_Color(0.f, 0.f, 0.f, 0.7f));
            CSurface::GL_SetColor(GL_Color(1.f, 0.2f, 0.15f, 1.f));
            freetype::easy_printCenter(font, x, y, text);
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        static float g_shakeX = 0.f, g_shakeY = 0.f;

        void SetShake(float x, float y)
        {
            g_shakeX = x;
            g_shakeY = y;
        }

        float ShakeX()
        {
            return g_shakeX;
        }

        float ShakeY()
        {
            return g_shakeY;
        }

        void EndFrame()
        {
            g_shakeX = g_shakeY = 0.f;
        }

        void Render()
        {
            if (GetState().debug && InGame()) RenderDebugMode();
            if (g_net.on && InGame()) RenderNetStats();
            if (WallMs() < g_fontTestUntil) RenderFontTest();
        }
    }
}
