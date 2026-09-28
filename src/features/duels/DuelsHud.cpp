#include "Global.h"
#include "Duels.h"
#include "DuelsHud.h"
#include "DuelsNet.h"
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

            // Next to the reactor bar, above the systems' power bars (they grow up to about y 585).
            const float x = 52.f, top = 522.f, lineHeight = 13.f;
            const int font = 10;
            int count = 0;
            float width = 0.f;
            for (const std::string &line : g_net.lines)
            {
                if (line.empty()) continue;
                ++count;
                width = std::max(width, (float)freetype::easy_measureWidth(font, line));
            }
            if (count == 0) return;
            CSurface::GL_DrawRect(x - 5.f, top - 3.f, width + 10.f, lineHeight * count + 6.f, GL_Color(0.f, 0.f, 0.f, 0.55f));
            CSurface::GL_SetColor(GL_Color(0.75f, 0.9f, 1.f, 1.f));
            float y = top;
            for (const std::string &line : g_net.lines)
            {
                if (line.empty()) continue;
                freetype::easy_print(font, x, y, line);
                y += lineHeight;
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        void Render()
        {
            if (g_net.on && InGame()) RenderNetStats();
        }
    }
}
