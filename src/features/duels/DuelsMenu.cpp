#include "Global.h"
#include "Duels.h"
#include "DuelsConfig.h"
#include "DuelsLobby.h"
#include "DuelsMatch.h"
#include "DuelsMenu.h"
#include "DuelsStyle.h"
#include "DuelsTrace.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace Duels
{
    namespace Menu
    {
        enum class Window
        {
            None,
            Name,       // the first start: the player's name
            Tutorial,   // a duel in short
            Guide       // the players' guide (USAGE.md)
        };

        struct GuideRow
        {
            int font;
            float indent;
            std::string text;
            GL_Color colour;
            float height;
            float dy;   // where the font draws, from the row's top (font 24 draws 15 px below its y)
        };

        struct MenuState
        {
            Window open = Window::None;
            Window back = Window::None;          // where the guide goes back to
            bool pending = false;                // the menu opened: ours come on its first frame
            int mouseX = -1, mouseY = -1;
            struct TextInput *nameInput = nullptr;
            std::string nameError;
            bool dontShow = false;
            bool tutorialShown = false;          // once per start
            bool nameThenTutorial = false;       // the name prompt of a first start: the tutorial box follows
            std::vector<GuideRow> guide;
            size_t guideTop = 0;
            size_t guideShown = 0;                // rows on the page last drawn
            size_t guideLastTop = 0;              // the top row of the last page
            bool guideLoaded = false;
            Style::Box ok, guideButton, check, up, down, close, field;
            Style::Box panelHost, panelJoin, panelName, panelGuide;   // the title screen's panel
        };

        static MenuState g;

        static const int FONT = 10, TEXT = 12, BIG = 24;
        static const size_t NAME_MAX = Match::NAME_MAX;   // the name prompt takes what the name command takes

        static GL_Color Rgb(int r, int g, int b, float a = 1.f)
        {
            return GL_Color(r / 255.f, g / 255.f, b / 255.f, a);
        }

        static void Text(int font, float x, float y, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            freetype::easy_print(font, x, y, text);
        }

        static float Paragraph(int font, float x, float y, float width, const std::string &text, const GL_Color &colour)
        {
            CSurface::GL_SetColor(colour);
            return freetype::easy_printAutoNewlines(font, x, y, (int)width, text).y - y;
        }

        static void ButtonAt(Style::Box &area, float x, float y, float w, const std::string &label, bool enabled = true)
        {
            area.x = x;
            area.y = y;
            area.w = w;
            area.h = 34.f;
            Style::Look look = !enabled ? Style::Look::Off : area.Contains(g.mouseX, g.mouseY) ? Style::Look::Hover : Style::Look::Idle;
            Style::Button(x, y, w, 34.f, label, TEXT, look);
        }

        static void Sound(const char *name)
        {
            if (G_->GetSoundControl()) G_->GetSoundControl()->PlaySoundMix(name, -1.f, false);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The name
        // ---------------------------------------------------------------------------------------------------------

        static std::string Trimmed(const std::string &text)
        {
            size_t start = text.find_first_not_of(' '), end = text.find_last_not_of(' ');
            return start == std::string::npos ? "" : text.substr(start, end - start + 1);
        }

        static void OpenName(bool thenTutorial)
        {
            g.nameThenTutorial = thenTutorial;
            if (!g.nameInput) g.nameInput = new struct TextInput((int)NAME_MAX, TextInput::ALLOW_ASCII, "");
            Match::Init();   // the saved name, or this start's captain (a crew member's name) to begin with
            std::string current = Match::PlayerName();
            g.nameInput->SetText(current);
            g.nameInput->Start();
            g.nameError.clear();
            g.open = Window::Name;
            Log("Menu: the name prompt (now '%s')", current.c_str());
        }

        static void OpenTutorial()
        {
            g.dontShow = false;
            g.open = Window::Tutorial;
            g.tutorialShown = true;
            Log("Menu: the tutorial box");
        }

        static void AcceptName()
        {
            std::string name = Trimmed(g.nameInput ? g.nameInput->GetText() : "");
            if (name.empty())
            {
                g.nameError = "A name, please: the other player sees it.";
                Sound("powerUpFail");
                return;
            }
            Match::SetPlayerName(name);
            if (SettingsFromConfig()) Config::SavePlayerName(name);   // a test scenario leaves the file alone
            if (g.nameInput) g.nameInput->Stop();
            Log("Menu: the player's name is '%s'", name.c_str());
            g.open = Window::None;
            if (g.nameThenTutorial && Config::Value("tutorial") != "off") OpenTutorial();
            g.nameThenTutorial = false;
        }

        static void RenderName()
        {
            const float w = 540.f, h = 250.f, x = (1280.f - w) / 2.f, y = 230.f;
            Style::Dialog(x, y, w, h, "FTL: DUELS");
            float left = x + 30.f, top = y + 26.f;
            Text(BIG, left, top - 14.f, "WELCOME", Rgb(255, 255, 255));
            Text(TEXT, left, top + 32.f, "Your name, as the other players see it:", Rgb(220, 224, 230));
            Style::Box &field = g.field;
            field.x = left;
            field.y = top + 58.f;
            field.w = w - 60.f;
            field.h = 34.f;
            Style::TextField(field.x, field.y, field.w, field.h, g.nameInput ? g.nameInput->GetText() : std::string(),
                             g.nameInput ? g.nameInput->pos : 0, true, false);
            float note = top + 104.f;
            if (!g.nameError.empty()) Text(FONT, left, note, g.nameError, Rgb(255, 140, 120));
            else Paragraph(FONT, left, note, w - 60.f, "Kept in duels.cfg (the console's name command changes it). Unranked until "
                                                   "the Steam login comes.", Rgb(190, 196, 204));
            ButtonAt(g.ok, x + w - 30.f - 120.f, y + h - 56.f, 120.f, "OK");
        }

        // ---------------------------------------------------------------------------------------------------------
        // The tutorial box
        // ---------------------------------------------------------------------------------------------------------

        // A duel in short points of one line each (AS).
        static const char *const TUTORIAL[] = {
            "- HOST DUEL or JOIN DUEL here in the menu (a code joins any room)",
            "- Pick your ship in the hangar and start",
            "- Before each round: repairs, scrap, the shop; then READY",
            "- A round: destroy their ship or their whole crew for a point",
            "- A draw: half a point each; jumping away: half for them",
            "- Most points wins the match",
            "- The DUELS button: the match, how to win, the icons",
        };

        static void RenderTutorial()
        {
            const float w = 720.f, h = 380.f, x = (1280.f - w) / 2.f, y = 170.f;
            Style::Dialog(x, y, w, h, "FTL: DUELS");
            float left = x + 30.f, top = y + 24.f, width = w - 60.f;
            top += Style::Label(left, top, "A DUEL, IN SHORT") + 14.f;
            for (const char *point : TUTORIAL) top += Paragraph(TEXT, left, top, width, point, Rgb(226, 230, 236)) + 10.f;
            Paragraph(FONT, left, top + 8.f, width, "More in the players' guide: every button and command, tests on this computer.",
                      Rgb(190, 196, 204));

            // "Don't show this again": the box and its label (a click on either).
            Style::Box &check = g.check;
            check.x = left;
            check.y = y + h - 50.f;
            check.w = 32.f + (float)freetype::easy_measureWidth(TEXT, "Don't show this again");
            check.h = Style::CHECK_SIZE;
            Style::CheckBox(check.x, check.y, g.dontShow, check.Contains(g.mouseX, g.mouseY));
            Text(TEXT, check.x + 32.f, check.y + 3.f, "Don't show this again", Rgb(220, 224, 230));

            ButtonAt(g.guideButton, x + w - 30.f - 120.f - 12.f - 190.f, y + h - 58.f, 190.f, "PLAYERS' GUIDE");
            ButtonAt(g.ok, x + w - 30.f - 120.f, y + h - 58.f, 120.f, "OK");
        }

        static void CloseTutorial()
        {
            if (g.dontShow) Config::SaveValue("tutorial", "off");
            Log("Menu: the tutorial box closed%s", g.dontShow ? " (not to be shown again)" : "");
            g.open = Window::None;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The players' guide
        // ---------------------------------------------------------------------------------------------------------

        // Markdown as rows for FTL's fonts: the title big, headings in the light of FTL's buttons, list items indented,
        // a table's rows as its first cell in gold with the others indented under it, the marks (* ` \) gone.
        // Typography FTL's fonts don't have, as plain letters (UTF-8 in the guide).
        static const char *const TYPOGRAPHY[][2] = {
            {"\xE2\x86\x92", "->"}, {"\xE2\x80\x94", "-"}, {"\xE2\x80\x93", "-"}, {"\xE2\x80\xA6", "..."},
            {"\xE2\x80\x98", "'"}, {"\xE2\x80\x99", "'"}, {"\xE2\x80\x9C", "\""}, {"\xE2\x80\x9D", "\""},
            {"\xC2\xA0", " "}};

        static std::string Plain(const std::string &text)
        {
            std::string out;
            for (size_t i = 0; i < text.size(); ++i)
            {
                bool replaced = false;
                for (const auto &pair : TYPOGRAPHY)
                {
                    size_t length = std::char_traits<char>::length(pair[0]);
                    if (text.compare(i, length, pair[0]) == 0)
                    {
                        out += pair[1];
                        i += length - 1;
                        replaced = true;
                        break;
                    }
                }
                if (replaced) continue;
                if (text[i] == '*' || text[i] == '`') continue;
                if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') continue;
                out += text[i];
            }
            return out;
        }

        static void AddRow(int font, float indent, const std::string &text, const GL_Color &colour)
        {
            float height = font == BIG ? 36.f : font == TEXT ? 21.f : 17.f;
            float dy = font == BIG ? -10.f : 0.f;
            g.guide.push_back(GuideRow{font, indent, text, colour, height, dy});
        }

        // A paragraph wrapped as FTL wraps (measured once): each line becomes a row.
        static void AddRows(int font, float indent, const std::string &text, const GL_Color &colour, float width)
        {
            std::istringstream words(text);
            std::string word, row;
            float usable = width - indent;
            while (words >> word)
            {
                std::string longer = row.empty() ? word : row + " " + word;
                if (!row.empty() && (float)freetype::easy_measureWidth(font, longer) > usable)
                {
                    AddRow(font, indent, row, colour);
                    row = word;
                }
                else row = longer;
            }
            AddRow(font, indent, row, colour);
        }

        static void AddGap(float height)
        {
            g.guide.push_back(GuideRow{FONT, 0.f, "", GL_Color(1.f, 1.f, 1.f, 1.f), height, 0.f});
        }

        static std::vector<std::string> Cells(const std::string &line)
        {
            std::vector<std::string> cells;
            std::string cell;
            for (size_t i = 1; i < line.size(); ++i)
            {
                if (line[i] == '|' && line[i - 1] != '\\')
                {
                    cells.push_back(Trimmed(Plain(cell)));
                    cell.clear();
                }
                else cell += line[i];
            }
            return cells;
        }

        static void LoadGuide(float width)
        {
            g.guideLoaded = true;
            g.guide.clear();
            const GL_Color heading = Style::ButtonBody(Style::Look::Idle), body = Rgb(220, 224, 230), soft = Rgb(180, 188, 198),
                           gold = Rgb(255, 235, 170);
            char *data = G_->GetResources()->LoadFile("data/duels_guide.md");
            if (!data)
            {
                AddRows(TEXT, 0.f, "The players' guide isn't in this installation (data/duels_guide.md). It is USAGE.md in "
                                   "FTL: Duels' package.", Rgb(255, 200, 190), width);
                Log("Menu: the players' guide isn't there (data/duels_guide.md)");
                return;
            }
            std::vector<std::string> lines;
            {
                std::istringstream stream(data);
                delete[] data;
                std::string line;
                while (std::getline(stream, line))
                {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    lines.push_back(line);
                }
            }
            for (size_t n = 0; n < lines.size(); ++n)
            {
                const std::string &line = lines[n];
                if (line.empty())
                {
                    AddGap(8.f);
                    continue;
                }
                if (line.compare(0, 2, "# ") == 0) AddRows(BIG, 0.f, Plain(line.substr(2)), gold, width);
                else if (line.compare(0, 3, "## ") == 0) AddRows(TEXT, 0.f, Plain(line.substr(3)), heading, width);
                else if (line.compare(0, 4, "### ") == 0) AddRows(TEXT, 0.f, Plain(line.substr(4)), soft, width);
                else if (line[0] == '|')
                {
                    if (line.find("---") != std::string::npos) continue;   // the header's rule
                    std::vector<std::string> cells = Cells(line);
                    bool header = n + 1 < lines.size() && lines[n + 1].compare(0, 4, "|---") == 0;
                    if (cells.empty()) continue;
                    if (header)
                    {
                        // The header row: the columns' names, as a small heading.
                        std::string names;
                        for (const std::string &cell : cells) names += (names.empty() ? "" : "  /  ") + cell;
                        AddRows(FONT, 0.f, names, soft, width);
                        continue;
                    }
                    AddRows(FONT, 8.f, cells[0], gold, width);
                    for (size_t i = 1; i < cells.size(); ++i)
                    {
                        if (!cells[i].empty()) AddRows(FONT, 28.f, cells[i], body, width);
                    }
                    AddGap(3.f);
                }
                else
                {
                    // A list item keeps its mark and its indent.
                    size_t spaces = line.find_first_not_of(' ');
                    std::string rest = line.substr(spaces == std::string::npos ? 0 : spaces);
                    float indent = (float)(spaces == std::string::npos ? 0 : spaces) * 6.f;
                    if (rest.compare(0, 2, "- ") == 0) AddRows(FONT, indent + 12.f, "- " + Plain(rest.substr(2)), body, width);
                    else AddRows(FONT, indent, Plain(rest), body, width);
                }
            }
            Log("Menu: the players' guide has %u rows from %u lines", (unsigned)g.guide.size(), (unsigned)lines.size());
        }

        static void RenderGuide()
        {
            const float w = 1000.f, h = 620.f, x = (1280.f - w) / 2.f, y = 70.f;
            Style::Dialog(x, y, w, h, "PLAYERS' GUIDE");
            float left = x + 30.f, top = y + 22.f, width = w - 60.f, bottom = y + h - 70.f;
            if (!g.guideLoaded)
            {
                LoadGuide(width);
                // The last page: as many rows from the end as fit.
                float used = 0.f;
                size_t first = g.guide.size();
                while (first > 0 && used + g.guide[first - 1].height <= bottom - top) used += g.guide[--first].height;
                g.guideLastTop = first;
            }
            float at = top;
            size_t row = g.guideTop;
            for (; row < g.guide.size(); ++row)
            {
                const GuideRow &r = g.guide[row];
                if (at + r.height > bottom) break;
                if (!r.text.empty()) Text(r.font, left + r.indent, at + r.dy, r.text, r.colour);
                at += r.height;
            }
            g.guideShown = row - g.guideTop;
            int percent = g.guideLastTop == 0 ? 100 : (int)(100.f * (float)g.guideTop / (float)g.guideLastTop + 0.5f);
            Text(FONT, left, y + h - 46.f, g.guide.empty() ? "" : std::to_string(percent) + "%   (Up, Down, Page Up, Page Down)",
                 Rgb(170, 176, 186));
            ButtonAt(g.up, x + w - 30.f - 120.f - 12.f - 110.f - 12.f - 110.f, y + h - 58.f, 110.f, "UP", g.guideTop > 0);
            ButtonAt(g.down, x + w - 30.f - 120.f - 12.f - 110.f, y + h - 58.f, 110.f, "DOWN", g.guideTop < g.guideLastTop);
            ButtonAt(g.close, x + w - 30.f - 120.f, y + h - 58.f, 120.f, "BACK");
        }

        // A page, less a row that stays in sight.
        static int PageRows()
        {
            return std::max(1, (int)g.guideShown - 1);
        }

        static void Scroll(int rows)
        {
            long top = (long)g.guideTop + rows;
            g.guideTop = (size_t)std::max(0L, std::min(top, (long)g.guideLastTop));
        }

        static void OpenGuide(Window back)
        {
            g.back = back;
            g.guideTop = 0;
            g.open = Window::Guide;
            Log("Menu: the players' guide");
        }

        // ---------------------------------------------------------------------------------------------------------
        // Entry points
        // ---------------------------------------------------------------------------------------------------------

        void OnMenuOpen()
        {
            // On the menu's first frame (the game's own start has run by then: our module, the name of this start, the
            // test harness). Hyperspace's question about an old profile, if it asks one, waits under ours.
            g.pending = true;
        }

        static void Close()
        {
            if (g.nameInput) g.nameInput->Stop();
            g.open = Window::None;
        }

        // FTL's menu buttons, once (their places, for ours).
        static void LogMenuButtons()
        {
            static bool logged = false;
            CApp *app = G_->GetCApp();
            if (logged || !app) return;
            logged = true;
            std::string text;
            for (Button *button : app->menu.buttons)
            {
                if (!button) continue;
                char line[160];
                snprintf(line, sizeof(line), "%s at %d,%d hit %d,%d %dx%d image %dx%d%s", text.empty() ? "" : ";", button->position.x,
                         button->position.y, button->hitbox.x, button->hitbox.y, button->hitbox.w, button->hitbox.h,
                         button->imageSize.x, button->imageSize.y, button->bActive ? "" : " (off)");
                text += line;
            }
            Log("Menu: FTL's buttons%s", text.c_str());
        }

        // ---------------------------------------------------------------------------------------------------------
        // FTL: Duels' panel on the title screen (roadmap 3.5): HOST DUEL, JOIN DUEL, the player's name and the guide.
        // FTL's own column of buttons stays as it is (it fills the right side down to the screen's bottom); the panel
        // takes the empty left side.
        // ---------------------------------------------------------------------------------------------------------

        static const float PX = 70.f, PY = 330.f, PW = 380.f, PH = 222.f;

        // FTL's menu shows its title screen: not the hangar, the options, the stats, the credits or a question.
        static bool OnTitle()
        {
            CApp *app = G_->GetCApp();
            if (!app || !app->menu.bOpen) return false;
            MainMenu &m = app->menu;
            return !m.shipBuilder.bOpen && !m.bScoreScreen && !m.bCreditScreen && !m.optionScreen.bOpen && !m.changelog.bOpen &&
                   !m.confirmNewGame.bOpen && !m.bSelectSave;
        }

        static void BigButton(Style::Box &box, float x, float y, float w, float h, const std::string &label, int font)
        {
            box.x = x;
            box.y = y;
            box.w = w;
            box.h = h;
            bool hover = box.Contains(g.mouseX, g.mouseY) && g.open == Window::None && !Lobby::IsOpen();
            Style::Button(x, y, w, h, label, font, hover ? Style::Look::Hover : Style::Look::Idle);
        }

        static void RenderPanel()
        {
            Style::Dialog(PX, PY, PW, PH, "FTL: DUELS", false);
            BigButton(g.panelHost, PX + 30.f, PY + 30.f, PW - 60.f, 50.f, "HOST DUEL", 63);
            BigButton(g.panelJoin, PX + 30.f, PY + 94.f, PW - 60.f, 50.f, "JOIN DUEL", 63);
            float y = PY + PH - 50.f;
            CSurface::GL_SetColor(Rgb(206, 210, 216));
            freetype::easy_print(TEXT, PX + 30.f, y + 7.f, "You: " + Match::ScreenName(Match::PlayerName()));
            BigButton(g.panelName, PX + PW - 30.f - 84.f - 8.f - 72.f, y, 72.f, 28.f, "NAME", TEXT);
            BigButton(g.panelGuide, PX + PW - 30.f - 84.f, y, 84.f, 28.f, "GUIDE", TEXT);
        }

        static bool ClickPanel(int x, int y)
        {
            if (g.panelHost.Contains(x, y)) Lobby::OpenHost();
            else if (g.panelJoin.Contains(x, y)) Lobby::OpenJoin();
            else if (g.panelName.Contains(x, y)) OpenName(false);
            else if (g.panelGuide.Contains(x, y)) OpenGuide(Window::None);
            else return false;
            return true;
        }

        void Render()
        {
            LogMenuButtons();
            if (g.pending)
            {
                g.pending = false;
                if (g.open != Window::None || !SettingsFromConfig()) {}   // a test scenario opens them itself
                else if (Config::PlayerName().empty()) OpenName(true);
                else if (!g.tutorialShown && Config::Value("tutorial") != "off") OpenTutorial();
            }
            bool title = OnTitle();
            if (title) RenderPanel();
            else
            {
                g.panelHost.w = g.panelJoin.w = g.panelName.w = g.panelGuide.w = 0.f;   // not there: no clicks
            }
            Lobby::Render();
            switch (g.open)
            {
            case Window::Name: RenderName(); break;
            case Window::Tutorial: RenderTutorial(); break;
            case Window::Guide: RenderGuide(); break;
            default: break;
            }
            CSurface::GL_SetColor(COLOR_WHITE);
        }

        // One of our windows is open (ours or the lobby's): FTL's menu gets no input.
        bool IsOpen()
        {
            return g.open != Window::None || Lobby::IsOpen();
        }

        bool MouseMove(int x, int y)
        {
            g.mouseX = x;
            g.mouseY = y;
            Lobby::MouseMove(x, y);
            return IsOpen();
        }

        bool MouseClick(int x, int y)
        {
            if (g.open == Window::None)
            {
                if (Lobby::MouseClick(x, y)) return true;
                if (OnTitle() && ClickPanel(x, y)) return true;
            }
            switch (g.open)
            {
            case Window::Name:
                if (g.ok.Contains(x, y)) AcceptName();
                else if (g.field.Contains(x, y) && g.nameInput) g.nameInput->Start();
                return true;
            case Window::Tutorial:
                if (g.check.Contains(x, y)) g.dontShow = !g.dontShow;
                else if (g.guideButton.Contains(x, y)) OpenGuide(Window::Tutorial);
                else if (g.ok.Contains(x, y)) CloseTutorial();
                return true;
            case Window::Guide:
                if (g.up.Contains(x, y)) Scroll(-PageRows());
                else if (g.down.Contains(x, y)) Scroll(PageRows());
                else if (g.close.Contains(x, y)) g.open = g.back;
                return true;
            default:
                return false;
            }
        }

        bool TextInput(int ch)
        {
            if (g.open == Window::None) return Lobby::TextInput(ch);
            if (g.open != Window::Name || !g.nameInput) return false;
            g.nameInput->OnTextInput(ch);
            g.nameError.clear();
            return true;
        }

        // Enter confirms the name, whether it comes as a key or as the text's confirmation (it can come as both; the
        // tutorial box that follows doesn't take Enter, so the second does nothing). Escape doesn't close the name
        // prompt: a name is needed.
        bool TextEvent(int event)
        {
            if (g.open == Window::None) return Lobby::TextEvent(event);
            if (g.open != Window::Name || !g.nameInput) return IsOpen();
            if (event == CEvent::TEXT_CONFIRM) AcceptName();
            else if (event != CEvent::TEXT_CANCEL)
            {
                g.nameInput->OnTextEvent((CEvent::TextEvent)event);
                g.nameError.clear();
            }
            return true;
        }

        bool KeyDown(int key)
        {
            if (g.open == Window::None) return Lobby::KeyDown(key);
            switch (g.open)
            {
            case Window::Name:
                if (key == SDLK_RETURN || key == SDLK_KP_ENTER) AcceptName();
                return true;
            case Window::Tutorial:
                if (key == SDLK_ESCAPE) CloseTutorial();
                return true;
            case Window::Guide:
                if (key == SDLK_UP) Scroll(-1);
                else if (key == SDLK_DOWN) Scroll(1);
                else if (key == SDLK_PAGEUP) Scroll(-PageRows());
                else if (key == SDLK_PAGEDOWN || key == SDLK_SPACE) Scroll(PageRows());
                else if (key == SDLK_HOME) g.guideTop = 0;
                else if (key == SDLK_ESCAPE) g.open = g.back;
                return true;
            default:
                return false;
            }
        }

        bool RunVerb(const std::vector<std::string> &args, std::string &message)
        {
            if (args.size() >= 2 && args[1] == "name") OpenName(true);   // as on a first start
            else if (args.size() >= 2 && args[1] == "tutorial") OpenTutorial();
            else if (args.size() >= 2 && args[1] == "guide") OpenGuide(Window::None);
            else if (args.size() >= 2 && args[1] == "close") Close();
            else if (args.size() >= 2)
            {
                // The lobby's: host, join, choose, cancel, code, password, start.
                if (Lobby::RunVerb(args, message)) return true;
                if (message.empty())
                {
                    message = "usage: menu name|tutorial|guide|close|host|join|choose|cancel|code <code|@file>|password <password>|start";
                }
                return false;
            }
            const char *names[] = {"nothing", "the name prompt", "the tutorial box", "the players' guide"};
            message = std::string("menu: ") + names[(int)g.open] + " open" + (Lobby::IsOpen() ? ", and a lobby window" : "") +
                      "; the player's name '" + Match::PlayerName() + "'";
            return true;
        }
    }
}

// FTL's main menu with ours over it (outermost, so ours are drawn last and take the input first; Hyperspace's own
// dialog over the menu, SaveFile.cpp, waits under ours).
HOOK_METHOD_PRIORITY(MainMenu, Open, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::Open -> Begin (DuelsMenu.cpp)\n")
    bool ret = super();
    Duels::Menu::OnMenuOpen();
    return ret;
}

HOOK_METHOD_PRIORITY(MainMenu, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::OnRender -> Begin (DuelsMenu.cpp)\n")
    super();
    Duels::Menu::Render();
}

HOOK_METHOD_PRIORITY(MainMenu, MouseMove, -2000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::MouseMove -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::MouseMove(x, y)) return;
    super(x, y);
}

HOOK_METHOD_PRIORITY(MainMenu, MouseClick, -2000, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::MouseClick -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::MouseClick(x, y)) return;
    super(x, y);
}

HOOK_METHOD_PRIORITY(MainMenu, MouseUp, -2000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::MouseUp -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::IsOpen()) return;
    super(mX, mY);
}

HOOK_METHOD_PRIORITY(MainMenu, MouseRClick, -2000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::MouseRClick -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::IsOpen()) return;
    super(mX, mY);
}

HOOK_METHOD_PRIORITY(MainMenu, OnKeyDown, -2000, (SDLKey sym, bool bShift) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::OnKeyDown -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::KeyDown((int)sym)) return;
    super(sym, bShift);
}

HOOK_METHOD_PRIORITY(MainMenu, OnTextInput, -2000, (int ch) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::OnTextInput -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::TextInput(ch)) return;
    super(ch);
}

HOOK_METHOD_PRIORITY(MainMenu, OnTextEvent, -2000, (CEvent::TextEvent event) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> MainMenu::OnTextEvent -> Begin (DuelsMenu.cpp)\n")
    if (Duels::Menu::TextEvent((int)event)) return;
    super(event);
}
