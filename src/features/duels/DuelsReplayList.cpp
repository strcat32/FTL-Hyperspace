#include "Global.h"
#include "Duels.h"
#include "DuelsDemo.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsReplayList.h"
#include "DuelsShips.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <functional>

namespace Duels
{
    namespace ReplayList
    {
        static const float BW = 1160.f, BH = 560.f, BX = (1280.f - BW) / 2.f, BY = 80.f;
        static const float LX = BX + 30.f, LW = BW - 60.f, LIST_Y = BY + 64.f, ROW_H = 24.f;
        static const int ROWS = 15;
        static const int FONT = 10, TEXT = 12;

        enum Column { FILE_NAME, DATE, HOST, GUEST, RANKED, HOST_SHIP, GUEST_SHIP, LENGTH, COLUMNS };
        static const char *const HEADS[COLUMNS] = {"FILE", "DATE", "HOST", "GUEST", "RANKED", "HOST'S SHIP", "GUEST'S SHIP", "LENGTH"};
        static const char *const NAMES[COLUMNS] = {"file", "date", "host", "guest", "ranked", "hostship", "guestship", "length"};
        static const float COLUMN_X[COLUMNS] = {10.f, 240.f, 375.f, 500.f, 625.f, 700.f, 860.f, 1020.f};
        static const float COLUMN_W[COLUMNS] = {222.f, 128.f, 118.f, 118.f, 66.f, 152.f, 152.f, 70.f};

        struct State
        {
            bool open = false;
            int mouseX = -1, mouseY = -1;
            std::vector<Demo::DemoInfo> demos;
            int sortColumn = DATE;
            bool descending = true;
            int page = 0;
            std::string picked;                   // the marked demo's file
            std::string message;
            Style::Box heads[COLUMNS];
            std::vector<Style::Box> rows;
            std::vector<std::string> rowFiles;
            Style::Box prev, next, play, close;
        };

        static State g;

        static GL_Color Rgb(int r, int gr, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, gr / 255.f, b / 255.f, a);
        }

        static void Text(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_print(font, x, y, text);
        }

        // The text cut to fit a width ("Captain_Kaz..").
        static std::string Fit(int font, const std::string &text, float width)
        {
            if ((float)freetype::easy_measureWidth(font, text) <= width) return text;
            std::string cut = text;
            while (!cut.empty() && (float)freetype::easy_measureWidth(font, cut + "..") > width) cut.pop_back();
            return cut + "..";
        }

        static bool Hover(const Style::Box &box)
        {
            return box.Contains(g.mouseX, g.mouseY);
        }

        static void ButtonAt(Style::Box &box, float x, float y, float w, float h, const std::string &label, bool enabled = true)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = h;
            Style::Button(x, y, w, h, label, TEXT, !enabled ? Style::Look::Off : Hover(box) ? Style::Look::Hover : Style::Look::Idle);
        }

        static std::string Lower(std::string text)
        {
            for (char &c : text) c = (char)std::tolower((unsigned char)c);
            return text;
        }

        static std::string FileShown(const Demo::DemoInfo &d)
        {
            const std::string &f = d.file;
            return f.size() > 8 && f.compare(f.size() - 8, 8, ".ftldemo") == 0 ? f.substr(0, f.size() - 8) : f;
        }

        static std::string Date(const Demo::DemoInfo &d)
        {
            if (d.startUtc == 0) return "?";
            std::time_t t = (std::time_t)d.startUtc;
            std::tm local = *std::localtime(&t);
            char text[32];
            std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
            return text;
        }

        static std::string Ship(const std::string &blueprint)
        {
            return blueprint.empty() ? std::string("?") : Ships::Title(blueprint);
        }

        static std::string Length(const Demo::DemoInfo &d)
        {
            int seconds = (int)(d.lengthMs / 1000.0);
            char text[16];
            snprintf(text, sizeof(text), "%d:%02d", seconds / 60, seconds % 60);
            return text;
        }

        static std::string Cell(const Demo::DemoInfo &d, int column)
        {
            switch (column)
            {
            case FILE_NAME: return FileShown(d);
            case DATE: return Date(d);
            case HOST: return d.hostName.empty() ? std::string("?") : d.hostName;
            case GUEST: return d.guestName.empty() ? std::string("?") : d.guestName;
            case RANKED: return d.ranked < 0 ? std::string("?") : d.ranked ? std::string("[x]") : std::string("[ ]");
            case HOST_SHIP: return Ship(d.hostShip);
            case GUEST_SHIP: return Ship(d.guestShip);
            default: return Length(d);
            }
        }

        static void Sort()
        {
            int column = g.sortColumn;
            bool down = g.descending;
            std::stable_sort(g.demos.begin(), g.demos.end(), [&](const Demo::DemoInfo &a, const Demo::DemoInfo &b) {
                bool less, more;
                switch (column)
                {
                case DATE:
                    less = a.startUtc < b.startUtc || (a.startUtc == b.startUtc && a.file < b.file);
                    more = b.startUtc < a.startUtc || (a.startUtc == b.startUtc && b.file < a.file);
                    break;
                case RANKED:
                    less = a.ranked < b.ranked;
                    more = b.ranked < a.ranked;
                    break;
                case LENGTH:
                    less = a.lengthMs < b.lengthMs;
                    more = b.lengthMs < a.lengthMs;
                    break;
                default:
                {
                    std::string x = Lower(Cell(a, column)), y = Lower(Cell(b, column));
                    less = x < y;
                    more = y < x;
                    break;
                }
                }
                return down ? more : less;
            });
        }

        static const Demo::DemoInfo *Find(const std::string &file)
        {
            for (const Demo::DemoInfo &d : g.demos)
            {
                if (d.file == file) return &d;
            }
            return nullptr;
        }

        static void Play(const std::string &file)
        {
            const Demo::DemoInfo *d = Find(file);
            if (!d) return;
            if (!d->problem.empty())
            {
                g.message = d->problem;
                return;
            }
            std::string path = d->path;
            Log("Replays: playing %s", path.c_str());
            Close();
            Lobby::PlayReplay(path);
        }

        void Open()
        {
            g.demos = Demo::ListDemos();
            Sort();
            g.page = 0;
            g.picked.clear();
            g.message.clear();
            g.open = true;
            Log("Replays: %u demos in demos\\", (unsigned)g.demos.size());
        }

        void Close()
        {
            g.open = false;
        }

        bool IsOpen()
        {
            return g.open;
        }

        void Render()
        {
            if (!g.open) return;
            const GL_Color soft = Rgb(190, 196, 204), light = Rgb(226, 230, 236), white = Rgb(255, 255, 255), gold = Rgb(255, 235, 170),
                           red = Rgb(255, 140, 120);
            Style::Dialog(BX, BY, BW, BH, "REPLAYS");

            // The heads: a click sorts by that column (again: the other way round).
            float listH = ROW_H * (ROWS + 1) + 8.f;
            Style::Field(LX, LIST_Y, LW, listH, false);
            for (int c = 0; c < COLUMNS; ++c)
            {
                Style::Box &box = g.heads[c];
                box.x = LX + COLUMN_X[c] - 4.f;
                box.y = LIST_Y + 2.f;
                box.w = COLUMN_W[c];
                box.h = ROW_H;
                std::string head = std::string(HEADS[c]) + (g.sortColumn == c ? (g.descending ? " v" : " ^") : "");
                Text(FONT, LX + COLUMN_X[c], LIST_Y + 6.f, head, g.sortColumn == c ? gold : Hover(box) ? white : soft);
            }

            int pages = std::max(1, ((int)g.demos.size() + ROWS - 1) / ROWS);
            g.page = std::max(0, std::min(g.page, pages - 1));
            g.rows.clear();
            g.rowFiles.clear();
            for (int i = 0; i < ROWS && g.page * ROWS + i < (int)g.demos.size(); ++i)
            {
                const Demo::DemoInfo &d = g.demos[g.page * ROWS + i];
                Style::Box box;
                box.x = LX + 4.f;
                box.y = LIST_Y + 4.f + ROW_H * (i + 1);
                box.w = LW - 8.f;
                box.h = ROW_H;
                if (d.file == g.picked) CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, Rgb(255, 230, 94, 0.28f));
                else if (Hover(box)) CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, Rgb(255, 255, 255, 0.08f));
                bool playable = d.problem.empty();
                for (int c = 0; c < COLUMNS; ++c)
                {
                    const GL_Color &colour = !playable ? soft : c == FILE_NAME || c == LENGTH ? light
                                           : c == RANKED && d.ranked == 1 ? Rgb(140, 255, 130) : white;
                    Text(FONT, LX + COLUMN_X[c], box.y + 4.f, Fit(FONT, Cell(d, c), COLUMN_W[c] - 8.f), colour);
                }
                g.rows.push_back(box);
                g.rowFiles.push_back(d.file);
            }

            // Under the list: how many, the pages; the marked demo's problem, if it has one; CLOSE and PLAY.
            float sy = LIST_Y + listH + 8.f;
            std::string count = g.demos.empty() ? std::string("No demos in demos\\ yet: every duel is recorded there (Record a demo, in HOST DUEL and JOIN DUEL).")
                                                : std::to_string(g.demos.size()) + (g.demos.size() == 1 ? " demo" : " demos") + " in demos\\ (all kept).";
            Text(FONT, LX, sy, Fit(FONT, count, LW - 170.f), light);
            ButtonAt(g.prev, LX + LW - 160.f, sy - 2.f, 36.f, 26.f, "<", g.page > 0);
            CSurface::GL_SetColor(light);
            freetype::easy_printCenter(FONT, LX + LW - 80.f, sy + 3.f, std::to_string(g.page + 1) + " / " + std::to_string(pages));
            ButtonAt(g.next, LX + LW - 36.f, sy - 2.f, 36.f, 26.f, ">", g.page + 1 < pages);
            const Demo::DemoInfo *picked = Find(g.picked);
            float by = BY + BH - 58.f;
            std::string note = !g.message.empty() ? g.message
                             : picked ? (picked->problem.empty() ? "PLAY, or a second click on it, plays it." : picked->problem)
                                      : "A click on a row marks it; a click on a column's head sorts by it.";
            Text(FONT, LX, by + 10.f, Fit(FONT, note, LW - 360.f), picked && !picked->problem.empty() ? red : soft);
            ButtonAt(g.close, BX + BW - 30.f - 160.f - 12.f - 130.f, by, 130.f, 34.f, "CLOSE");
            ButtonAt(g.play, BX + BW - 30.f - 160.f, by, 160.f, 34.f, "PLAY", picked && picked->problem.empty());
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        bool MouseClick(int x, int y)
        {
            if (!g.open) return false;
            g.message.clear();
            for (int c = 0; c < COLUMNS; ++c)
            {
                if (!g.heads[c].Contains(x, y)) continue;
                if (g.sortColumn == c) g.descending = !g.descending;
                else
                {
                    g.sortColumn = c;
                    g.descending = c == DATE || c == LENGTH || c == RANKED;   // the newest, longest and ranked first
                }
                Sort();
                return true;
            }
            for (size_t i = 0; i < g.rows.size(); ++i)
            {
                if (!g.rows[i].Contains(x, y)) continue;
                if (g.picked == g.rowFiles[i]) Play(g.picked);
                else g.picked = g.rowFiles[i];
                return true;
            }
            if (g.prev.Contains(x, y) && g.page > 0) --g.page;
            else if (g.next.Contains(x, y)) ++g.page;   // kept to the pages there are when drawn
            else if (g.close.Contains(x, y)) Close();
            else if (g.play.Contains(x, y) && !g.picked.empty()) Play(g.picked);
            return true;
        }

        void MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
        }

        bool KeyDown(int key)
        {
            if (!g.open) return false;
            if (key == SDLK_ESCAPE) Close();
            else if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && !g.picked.empty()) Play(g.picked);
            return true;
        }

        bool RunVerb(const std::vector<std::string> &args, std::string &message)
        {
            // menu replays [close | sort <column> | pick <row> | play [<file>|newest]]
            if (args.size() <= 2)
            {
                Open();
                message = "the demo browser: " + std::to_string(g.demos.size()) + " demos";
                return true;
            }
            const std::string &what = args[2];
            if (what == "close")
            {
                Close();
                message = "the demo browser closed";
                return true;
            }
            if (!g.open) Open();
            if (what == "sort" && args.size() > 3)
            {
                for (int c = 0; c < COLUMNS; ++c)
                {
                    if (args[3] != NAMES[c]) continue;
                    if (g.sortColumn == c) g.descending = !g.descending;
                    else
                    {
                        g.sortColumn = c;
                        g.descending = c == DATE || c == LENGTH || c == RANKED;
                    }
                    Sort();
                    message = std::string("sorted by ") + NAMES[c] + (g.descending ? ", descending" : ", ascending") + ": " +
                              (g.demos.empty() ? std::string("no demos") : FileShown(g.demos.front()) + " first");
                    return true;
                }
                message = "usage: menu replays sort file|date|host|guest|ranked|hostship|guestship|length";
                return false;
            }
            if (what == "pick" && args.size() > 3)
            {
                int row = std::atoi(args[3].c_str());
                if (row < 1 || row > (int)g.demos.size())
                {
                    message = "no row " + args[3];
                    return false;
                }
                g.picked = g.demos[row - 1].file;
                message = "marked " + g.picked;
                return true;
            }
            if (what == "play")
            {
                std::string file = args.size() > 3 ? args[3] : "newest";
                if (file == "newest")
                {
                    file.clear();
                    uint32_t newest = 0;
                    for (const Demo::DemoInfo &d : g.demos)
                    {
                        if (d.problem.empty() && (file.empty() || d.startUtc > newest || (d.startUtc == newest && d.file > file)))
                        {
                            newest = d.startUtc;
                            file = d.file;
                        }
                    }
                }
                if (!Find(file))
                {
                    message = "no demo " + file;
                    return false;
                }
                message = "playing " + file;
                Play(file);
                return true;
            }
            message = "usage: menu replays [close | sort <column> | pick <row> | play [<file>|newest]]";
            return false;
        }
    }
}
