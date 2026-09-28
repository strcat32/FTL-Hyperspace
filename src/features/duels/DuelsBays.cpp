#include "Global.h"
#include "Duels.h"
#include "DuelsBays.h"

#include <rapidxml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Bays
    {
        static const int TILE = 35;
        static const char *const BAY_NAMES[MAX_BAYS] = {"weapon_bay_1", "weapon_bay_2", "weapon_bay_3", "weapon_bay_4"};

        struct Tile
        {
            int x, y;
            Tile() : x(0), y(0) {}
            Tile(int tx, int ty) : x(tx), y(ty) {}
            bool operator==(const Tile &o) const { return x == o.x && y == o.y; }
            bool operator<(const Tile &o) const { return y != o.y ? y < o.y : x < o.x; }
        };

        struct TileRect
        {
            int x, y, w, h;
            TileRect() : x(0), y(0), w(0), h(0) {}
            TileRect(int rx, int ry, int rw, int rh) : x(rx), y(ry), w(rw), h(rh) {}
            bool Contains(const Tile &t) const { return t.x >= x && t.x < x + w && t.y >= y && t.y < y + h; }
        };

        // How one layout is cut. The layout's room ids stay; new rooms get the next free ids.
        struct Plan
        {
            bool ok = false;
            int weaponsRoom = -1;           // keeps its id: now the 1x1 room W1, with the gunner console
            int consoleDirection = -1;      // FTL's direction (Globals::GetDirection), -1: FTL chooses
            bool consoleSet = false;        // the console is W1's slot 0 (it was set in the blueprint or its picture)
            std::vector<int> bayRooms;      // bay 1 (W1) .. bay n
            std::vector<int> spareRooms;    // tiles no weapon slot needs: empty rooms
            // Rooms cut smaller whose system keeps its picture where the whole room was: room -> how far (tiles) the
            // room's top left corner moved.
            std::map<int, Tile> moved;
            // Each room that was cut, and the rooms it became: drawn as one room (no walls or doors between them).
            struct Group
            {
                TileRect original;          // in the layout's tiles
                std::vector<int> rooms;
            };
            std::vector<Group> groups;
            int xOffset = 0, yOffset = 0;   // the layout's X_OFFSET and Y_OFFSET (FTL's room rectangles include them)
            std::string text;               // the layout, cut
            std::string summary;            // for the log
        };

        struct BaysState
        {
            std::map<std::string, Plan> plans;     // by layout name
            bool typesKnown = false;
            int types[MAX_BAYS] = {-1, -1, -1, -1};
            std::map<std::string, std::pair<Tile, std::string>> glows;   // rooms.xml: picture -> console tile (px) and direction
            bool glowsLoaded = false;
            const std::string *building = nullptr;   // the layout of the ship whose systems are being added
            // Ship::OnInit: the ship and its plan while FTL builds its room graph, walls and grid.
            Ship *initShip = nullptr;
            const Plan *initPlan = nullptr;
            // The doors inside a cut room: out of the ship's door list (not drawn, not clickable, not closed by the
            // doors system), open, and still in its room graph so crew walk through.
            std::map<const Ship*, std::vector<Door*>> hiddenDoors;
            // The kind of weapon each bay's icons show, with the room icon they were made for (a new system gets new ones).
            std::map<const ShipSystem*, std::pair<const GL_Primitive*, std::string>> icons;
            uint32_t layoutsCut = 0, blueprintsPatched = 0, weaponsRestored = 0, weaponsSwitchedOff = 0;
            uint32_t doorsHidden = 0, wallsLeftOut = 0, iconsChanged = 0;
        };

        static BaysState g_bays;

        // ---------------------------------------------------------------------------------------------------------
        // Bay systems
        // ---------------------------------------------------------------------------------------------------------

        static int BayType(int number)
        {
            if (!g_bays.typesKnown)
            {
                g_bays.typesKnown = true;
                for (int i = 0; i < MAX_BAYS; ++i)
                {
                    int type = ShipSystem::NameToSystemId(BAY_NAMES[i]);
                    g_bays.types[i] = type >= SYS_CUSTOM_FIRST ? type : -1;
                    if (g_bays.types[i] < 0) g_bays.typesKnown = false;   // data not loaded yet: ask again later
                }
            }
            return number >= 1 && number <= MAX_BAYS ? g_bays.types[number - 1] : -1;
        }

        int BayNumber(int systemType)
        {
            if (systemType < SYS_CUSTOM_FIRST) return 0;
            for (int number = 1; number <= MAX_BAYS; ++number)
            {
                if (BayType(number) == systemType) return number;
            }
            return 0;
        }

        bool IsBay(const ShipSystem *system)
        {
            return system && BayNumber(system->iSystemType) > 0;
        }

        static ShipSystem *Bay(ShipManager *ship, int number)
        {
            int type = BayType(number);
            if (!ship || type < 0 || type >= (int)ship->systemKey.size() || ship->systemKey[type] < 0) return nullptr;
            return ship->GetSystem(type);
        }

        static ShipManager *ShipOf(const ShipSystem *system)
        {
            return system ? G_->GetShipManager(system->_shipObj.iShipId) : nullptr;
        }

        // A bay that switches its weapon off: damaged (a weapon needs all its bars), ioned or hacked.
        static bool Disabled(const ShipSystem *bay)
        {
            return bay->healthState.first < bay->healthState.second || bay->iLockCount > 0 ||
                   (bay->bUnderAttack && bay->iHackEffect >= 2);
        }

        // ---------------------------------------------------------------------------------------------------------
        // Cutting the layout
        // ---------------------------------------------------------------------------------------------------------

        struct Entry
        {
            std::string key;
            std::vector<int> args;
        };

        static bool IsNumber(const std::string &token)
        {
            size_t start = !token.empty() && token[0] == '-' ? 1 : 0;
            if (start >= token.size()) return false;
            for (size_t i = start; i < token.size(); ++i)
            {
                if (!std::isdigit((unsigned char)token[i])) return false;
            }
            return true;
        }

        // FTL's layout text: keywords (X_OFFSET, ELLIPSE, ROOM, DOOR, ...), each followed by its numbers.
        static std::vector<Entry> ParseLayout(const char *text)
        {
            std::vector<Entry> entries;
            std::istringstream in(text);
            std::string token;
            while (in >> token)
            {
                if (!IsNumber(token)) entries.push_back(Entry{token, {}});
                else if (!entries.empty()) entries.back().args.push_back(std::atoi(token.c_str()));
            }
            return entries;
        }

        // Each keyword and number on a line of its own, with Windows line ends like FTL's files (its reader expects them).
        static std::string WriteLayout(const std::vector<Entry> &entries)
        {
            std::ostringstream out;
            for (const Entry &entry : entries)
            {
                out << entry.key << "\r\n";
                for (int value : entry.args) out << value << "\r\n";
            }
            return out.str();
        }

        // data/rooms.xml: where each room picture draws its console (its glow), in pixels from the picture's corner.
        static void LoadGlows()
        {
            g_bays.glowsLoaded = true;
            char *text = G_->GetResources()->LoadFile("data/rooms.xml");
            if (!text) return;
            try
            {
                rapidxml::xml_document<> doc;
                doc.parse<0>(text);
                rapidxml::xml_node<> *root = doc.first_node("FTL");
                for (rapidxml::xml_node<> *node = root ? root->first_node("roomLayout") : nullptr; node;
                     node = node->next_sibling("roomLayout"))
                {
                    rapidxml::xml_attribute<> *name = node->first_attribute("name");
                    rapidxml::xml_node<> *glow = node->first_node("computerGlow");
                    if (!name || !glow || !glow->first_attribute("x") || !glow->first_attribute("y")) continue;
                    Tile at{std::atoi(glow->first_attribute("x")->value()), std::atoi(glow->first_attribute("y")->value())};
                    std::string dir = glow->first_attribute("dir") ? glow->first_attribute("dir")->value() : "";
                    std::transform(dir.begin(), dir.end(), dir.begin(), [](unsigned char c) { return (char)std::tolower(c); });
                    g_bays.glows[name->value()] = std::make_pair(at, dir);
                }
            }
            catch (std::exception &)
            {
                Log("Bays: data/rooms.xml could not be read");
            }
            delete[] text;
        }

        // The player ship blueprint that uses this layout (player ships have a layout each).
        static const ShipBlueprint *PlayerBlueprint(const std::string &layout)
        {
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!blueprints) return nullptr;
            for (const std::pair<const std::string, ShipBlueprint> &entry : blueprints->shipBlueprints)
            {
                if (entry.first.compare(0, 12, "PLAYER_SHIP_") == 0 && entry.second.layoutFile == layout) return &entry.second;
            }
            return nullptr;
        }

        static int SystemRoom(const ShipBlueprint *bp, int system)
        {
            auto found = bp->systemInfo.find(system);
            return found != bp->systemInfo.end() && !found->second.location.empty() ? found->second.location[0] : -1;
        }

        // Where a system's console is (tile in its room, row by row), from the blueprint's slot or else from its
        // room picture's glow (the blueprint's picture, or FTL's default "room_<system>"); -1 when FTL chooses.
        static int ConsoleSlot(int type, const ShipBlueprint::SystemTemplate &system, const TileRect &room, int &direction)
        {
            direction = -1;
            if (system.slot >= 0 && system.slot < room.w * room.h)
            {
                direction = system.direction;
                return system.slot;
            }
            if (!g_bays.glowsLoaded) LoadGlows();
            std::string image = system.image.empty() ? "room_" + ShipSystem::SystemIdToName(type) : system.image;
            std::string name = image.compare(0, 5, "room_") == 0 ? image.substr(5) : image;
            auto glow = g_bays.glows.find(name);
            if (glow == g_bays.glows.end()) return -1;
            int column = std::min(room.w - 1, std::max(0, glow->second.first.x / TILE));
            int row = std::min(room.h - 1, std::max(0, glow->second.first.y / TILE));
            direction = glow->second.second.empty() ? -1 : Globals::GetDirection(glow->second.second);
            return row * room.w + column;
        }

        static Plan MakePlan(const std::string &layout, const char *text)
        {
            Plan plan;
            const ShipBlueprint *bp = PlayerBlueprint(layout);
            if (!bp) return plan;
            auto weapons = bp->systemInfo.find(SYS_WEAPONS);
            if (weapons == bp->systemInfo.end() || weapons->second.location.empty()) return plan;
            int slots = std::max(1, std::min(MAX_BAYS, bp->weaponSlots > 0 ? bp->weaponSlots : 4));
            plan.weaponsRoom = weapons->second.location[0];

            std::vector<Entry> entries = ParseLayout(text);
            std::map<int, TileRect> rooms;
            int nextId = 0;
            size_t lastRoom = 0;
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const Entry &entry = entries[i];
                if (entry.key != "ROOM" || entry.args.size() < 5) continue;
                rooms[entry.args[0]] = TileRect{entry.args[1], entry.args[2], entry.args[3], entry.args[4]};
                nextId = std::max(nextId, entry.args[0] + 1);
                lastRoom = i;
            }
            auto weaponsRect = rooms.find(plan.weaponsRoom);
            if (weaponsRect == rooms.end()) return plan;
            const TileRect wr = weaponsRect->second;

            // The console's tile becomes W1; then the room's other tiles row by row.
            int direction = -1;
            int consoleSlot = ConsoleSlot(SYS_WEAPONS, weapons->second, wr, direction);
            plan.consoleSet = consoleSlot >= 0;
            plan.consoleDirection = direction;
            if (consoleSlot < 0) consoleSlot = 0;
            Tile console{wr.x + consoleSlot % wr.w, wr.y + consoleSlot / wr.w};
            std::vector<Tile> tiles{console};
            for (int y = wr.y; y < wr.y + wr.h; ++y)
            {
                for (int x = wr.x; x < wr.x + wr.w; ++x)
                {
                    if (!(Tile{x, y} == console)) tiles.push_back(Tile{x, y});
                }
            }

            // Too few tiles (Engi B: 2 tiles, 3 slots): a neighbouring room one tile wide gives the tile at its end
            // next to the weapons room, unless its console is there.
            std::map<Tile, int> donated;   // tile -> the room that gave it
            for (size_t i = 0; (int)tiles.size() < slots && i < tiles.size(); ++i)
            {
                static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4 && (int)tiles.size() < slots; ++d)
                {
                    Tile t{tiles[i].x + DX[d], tiles[i].y + DY[d]};
                    if (wr.Contains(t) || donated.count(t)) continue;
                    for (std::pair<const int, TileRect> &room : rooms)
                    {
                        TileRect &r = room.second;
                        if (room.first == plan.weaponsRoom || !r.Contains(t)) continue;
                        bool thin = (r.w == 1 && r.h >= 2) || (r.h == 1 && r.w >= 2);
                        bool atEnd = (r.w == 1 && (t.y == r.y || t.y == r.y + r.h - 1)) ||
                                     (r.h == 1 && (t.x == r.x || t.x == r.x + r.w - 1));
                        if (!thin || !atEnd) break;
                        bool console = false;
                        for (const std::pair<const int, ShipBlueprint::SystemTemplate> &system : bp->systemInfo)
                        {
                            if (system.second.location.empty() || system.second.location[0] != room.first) continue;
                            int ignored;
                            int slot = ConsoleSlot(system.first, system.second, r, ignored);
                            if (slot >= 0 && Tile{r.x + slot % r.w, r.y + slot / r.w} == t) console = true;
                        }
                        if (console) break;
                        donated[t] = room.first;
                        tiles.push_back(t);
                        Plan::Group group;
                        group.original = r;
                        group.rooms.push_back(room.first);
                        plan.groups.push_back(group);
                        // The rest of the room stays a rectangle; its system's picture stays where it was.
                        Tile corner{r.x, r.y};
                        if (r.w == 1 && t.y == r.y) ++r.y;
                        if (r.h == 1 && t.x == r.x) ++r.x;
                        if (r.w == 1) --r.h;
                        else --r.w;
                        Tile &moved = plan.moved[room.first];
                        moved.x += r.x - corner.x;
                        moved.y += r.y - corner.y;
                        break;
                    }
                }
            }
            if ((int)tiles.size() < slots)
            {
                plan.summary = "too few tiles for the weapon slots";
                return plan;
            }

            // Rooms: W1 keeps the weapons room's id, the rest are new.
            std::map<Tile, int> tileRoom;
            tileRoom[console] = plan.weaponsRoom;
            plan.bayRooms.push_back(plan.weaponsRoom);
            plan.moved[plan.weaponsRoom] = Tile{console.x - wr.x, console.y - wr.y};
            // Bays: the weapons room's tiles first, then donated ones; spare tiles last.
            std::vector<Tile> bayTiles, spareTiles;
            for (size_t i = 1; i < tiles.size(); ++i)
            {
                bool own = wr.Contains(tiles[i]);
                if ((int)(bayTiles.size() + 1) < slots && (own || donated.count(tiles[i]))) bayTiles.push_back(tiles[i]);
                else if (own) spareTiles.push_back(tiles[i]);
            }
            std::vector<Entry> newRooms;
            for (const Tile &t : bayTiles)
            {
                tileRoom[t] = nextId;
                plan.bayRooms.push_back(nextId);
                newRooms.push_back(Entry{"ROOM", {nextId, t.x, t.y, 1, 1}});
                ++nextId;
            }
            for (const Tile &t : spareTiles)
            {
                tileRoom[t] = nextId;
                plan.spareRooms.push_back(nextId);
                newRooms.push_back(Entry{"ROOM", {nextId, t.x, t.y, 1, 1}});
                ++nextId;
            }

            // The rooms' own entries: W1 is the console's tile; donors lose their tile.
            for (Entry &entry : entries)
            {
                if (entry.key != "ROOM" || entry.args.size() < 5) continue;
                if (entry.args[0] == plan.weaponsRoom) entry.args = {plan.weaponsRoom, console.x, console.y, 1, 1};
                else if (plan.moved.count(entry.args[0]))
                {
                    const TileRect &r = rooms[entry.args[0]];
                    entry.args = {entry.args[0], r.x, r.y, r.w, r.h};
                }
            }

            // Doors of the cut rooms open into the room that now has their tile.
            auto sideTile = [&](int x, int y, bool vertical, int room, Tile &out) {
                Tile sides[2] = {vertical ? Tile{x - 1, y} : Tile{x, y - 1}, Tile{x, y}};
                for (const Tile &side : sides)
                {
                    auto found = tileRoom.find(side);
                    bool inRoom = room == plan.weaponsRoom ? wr.Contains(side) : (found != tileRoom.end() && donated.count(side) && donated[side] == room);
                    if (inRoom && found != tileRoom.end())
                    {
                        out = side;
                        return true;
                    }
                }
                return false;
            };
            int doorsMoved = 0;
            for (Entry &entry : entries)
            {
                if (entry.key != "DOOR" || entry.args.size() < 5) continue;
                for (int side = 2; side <= 3; ++side)
                {
                    int room = entry.args[side];
                    if (room != plan.weaponsRoom && !plan.moved.count(room)) continue;
                    Tile t;
                    if (!sideTile(entry.args[0], entry.args[1], entry.args[4] != 0, room, t)) continue;
                    if (tileRoom[t] != room)
                    {
                        entry.args[side] = tileRoom[t];
                        ++doorsMoved;
                    }
                }
            }

            // The weapons room is a group too (first); each group's rooms.
            Plan::Group weaponsGroup;
            weaponsGroup.original = wr;
            plan.groups.insert(plan.groups.begin(), weaponsGroup);
            auto roomOfTile = [&](const Plan::Group &group, const Tile &t) -> int {
                auto found = tileRoom.find(t);
                if (found != tileRoom.end() && (group.original.Contains(t))) return found->second;
                return group.rooms.empty() ? -1 : group.rooms[0];   // a donor's remaining tiles
            };
            // Doors inside each cut room, between neighbouring tiles that are now in different rooms: crew walk there
            // as before (the doors are hidden and stay open, OnGraphBuilt).
            std::vector<Entry> newDoors;
            for (Plan::Group &group : plan.groups)
            {
                const TileRect &g = group.original;
                for (int y = g.y; y < g.y + g.h; ++y)
                {
                    for (int x = g.x; x < g.x + g.w; ++x)
                    {
                        Tile t{x, y};
                        int room = roomOfTile(group, t);
                        if (std::find(group.rooms.begin(), group.rooms.end(), room) == group.rooms.end()) group.rooms.push_back(room);
                        if (x + 1 < g.x + g.w)
                        {
                            int right = roomOfTile(group, Tile{x + 1, y});
                            if (right != room) newDoors.push_back(Entry{"DOOR", {x + 1, y, room, right, 1}});
                        }
                        if (y + 1 < g.y + g.h)
                        {
                            int down = roomOfTile(group, Tile{x, y + 1});
                            if (down != room) newDoors.push_back(Entry{"DOOR", {x, y + 1, room, down, 0}});
                        }
                    }
                }
            }
            for (const Entry &entry : entries)
            {
                if (entry.key == "X_OFFSET" && !entry.args.empty()) plan.xOffset = entry.args[0];
                if (entry.key == "Y_OFFSET" && !entry.args.empty()) plan.yOffset = entry.args[0];
            }

            entries.insert(entries.begin() + lastRoom + 1, newRooms.begin(), newRooms.end());
            entries.insert(entries.end(), newDoors.begin(), newDoors.end());
            plan.text = WriteLayout(entries);
            plan.ok = true;

            std::ostringstream summary;
            summary << "weapons room " << plan.weaponsRoom << " (" << wr.w << "x" << wr.h << ", " << slots << " slots, console at "
                    << console.x << "," << console.y << (plan.consoleSet ? "" : " by default") << ") cut: bays";
            for (int room : plan.bayRooms) summary << ' ' << room;
            if (!plan.spareRooms.empty())
            {
                summary << ", spare";
                for (int room : plan.spareRooms) summary << ' ' << room;
            }
            for (const std::pair<const Tile, int> &gift : donated) summary << ", tile from room " << gift.second;
            summary << "; " << doorsMoved << " doors moved, " << newDoors.size() << " new";
            plan.summary = summary.str();
            return plan;
        }

        static const Plan *GetPlan(const std::string &layout, const char *text)
        {
            auto found = g_bays.plans.find(layout);
            if (found != g_bays.plans.end()) return &found->second;
            if (!text || !PlayerBlueprint(layout)) return nullptr;
            Plan &plan = g_bays.plans[layout];
            plan = MakePlan(layout, text);
            if (plan.ok)
            {
                ++g_bays.layoutsCut;
                // In debug mode the cut layout goes to a file for checking (duels_layout_<layout>.txt).
                FILE *dump = GetState().debug ? std::fopen(("duels_layout_" + layout + ".txt").c_str(), "wb") : nullptr;
                if (dump)
                {
                    std::fwrite(plan.text.data(), 1, plan.text.size(), dump);
                    std::fclose(dump);
                }
            }
            Log("Bays: layout %s: %s", layout.c_str(), plan.ok ? plan.summary.c_str() : ("not cut: " + plan.summary).c_str());
            return &plan;
        }

        static bool LayoutName(const std::string &fileName, std::string &layout)
        {
            static const std::string prefix = "data/", suffix = ".txt";
            if (fileName.size() <= prefix.size() + suffix.size() || fileName.compare(0, prefix.size(), prefix) != 0 ||
                fileName.compare(fileName.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
            layout = fileName.substr(prefix.size(), fileName.size() - prefix.size() - suffix.size());
            return layout.find('/') == std::string::npos;
        }

        char *OnLoadFile(const std::string &fileName, char *text)
        {
            std::string layout;
            if (!text || !LayoutName(fileName, layout)) return text;
            const Plan *plan = GetPlan(layout, text);
            if (!plan || !plan->ok) return text;
            char *cut = new char[plan->text.size() + 1];
            std::memcpy(cut, plan->text.c_str(), plan->text.size() + 1);
            delete[] text;
            return cut;
        }

        // ---------------------------------------------------------------------------------------------------------
        // Blueprints
        // ---------------------------------------------------------------------------------------------------------

        static void Patch(ShipBlueprint *bp, const Plan &plan)
        {
            if (BayType(1) < 0 || bp->systemInfo.count(BayType(1))) return;
            auto weapons = bp->systemInfo.find(SYS_WEAPONS);
            if (weapons == bp->systemInfo.end() || weapons->second.location.empty() ||
                weapons->second.location[0] != plan.weaponsRoom) return;
            // The gunner console: W1's only tile, facing the way it did.
            if (plan.consoleSet)
            {
                weapons->second.slot = 0;
                weapons->second.direction = plan.consoleDirection;
            }
            for (size_t i = 0; i < plan.bayRooms.size() && (int)i < MAX_BAYS; ++i)
            {
                int type = BayType((int)i + 1);
                ShipBlueprint::SystemTemplate bay = ShipBlueprint::SystemTemplate();
                bay.systemId = type;
                bay.powerLevel = 1;
                bay.location = std::vector<int>{plan.bayRooms[i]};
                bay.bp = 0;
                bay.maxPower = MAX_BAYS;
                bay.slot = -1;
                bay.direction = -1;
                bp->systemInfo[type] = bay;
                if (std::find(bp->systems.begin(), bp->systems.end(), type) == bp->systems.end()) bp->systems.push_back(type);
            }
            ++g_bays.blueprintsPatched;
        }

        void PrepareBlueprint(ShipBlueprint *bp)
        {
            if (!bp || bp->blueprintName.compare(0, 12, "PLAYER_SHIP_") != 0 || BayType(1) < 0) return;
            const Plan *plan = GetPlan(bp->layoutFile, nullptr);
            if (!plan)
            {
                // Not loaded yet: loading it makes the plan (OnLoadFile).
                char *text = G_->GetResources()->LoadFile("data/" + bp->layoutFile + ".txt");
                delete[] text;
                plan = GetPlan(bp->layoutFile, nullptr);
            }
            if (!plan || !plan->ok) return;
            ShipBlueprint *global = G_->GetBlueprints()->GetShipBlueprint(bp->blueprintName, -1);
            if (global && global != bp) Patch(global, *plan);
            Patch(bp, *plan);
        }

        // ---------------------------------------------------------------------------------------------------------
        // In play
        // ---------------------------------------------------------------------------------------------------------

        // A bay's bars are its weapon's power (an empty slot's bay has one). Damage stays damage.
        static void SetLevel(ShipSystem *bay, int level)
        {
            if (bay->healthState.second == level && bay->powerState.second == level) return;
            int damage = std::max(0, bay->healthState.second - bay->healthState.first);
            bay->healthState.second = level;
            bay->healthState.first = std::max(0, level - damage);
            bay->powerState.second = level;
            bay->powerState.first = bay->healthState.first;
        }

        // The kind of weapon a bay shows (its icons s_bay_<kind>_*.png): what the weapon does to the target.
        static const char *WeaponKind(const ProjectileFactory *weapon)
        {
            const WeaponBlueprint *bp = weapon ? weapon->blueprint : nullptr;
            if (!bp) return "empty";
            const Damage &d = bp->damage;
            if (bp->name.find("CRYSTAL") != std::string::npos) return "crystal";
            if (bp->typeName == "BEAM") return d.iDamage <= 0 && d.iPersDamage <= 0 && d.fireChance > 0 ? "fire" : "beam";
            if (bp->typeName == "BOMB")
            {
                if (d.iDamage <= 0 && d.iIonDamage <= 0 && d.fireChance >= 5) return "fire";
                if (d.iDamage <= 0 && d.iIonDamage > 0) return "ion";
                return "bomb";
            }
            if (bp->typeName == "MISSILES") return "missile";
            if (bp->typeName == "BURST") return bp->name.find("MISSILE") != std::string::npos ? "missile" : "flak";
            return d.iDamage <= 0 && d.iIonDamage > 0 ? "ion" : "laser";
        }

        // A bay's icons for a kind of weapon, made the way FTL makes a system's: the room icon and its outline at the
        // room's centre, the discs (5 states, own and enemy look, and the colour blind slots) at the origin.
        static void SetIcons(ShipSystem *bay, const std::string &kind)
        {
            ResourceControl *resources = G_->GetResources();
            const std::string base = "icons/s_bay_" + kind;
            GL_Texture *overlay = resources->GetImageId(base + "_overlay.png");
            GL_Texture *outline = resources->GetImageId(base + "_overlay2.png");
            if (!overlay || !outline) return;
            const GL_Color white(1.f, 1.f, 1.f, 1.f);
            CSurface::GL_DestroyPrimitive(bay->iconPrimitive);
            bay->iconPrimitive = resources->CreateImagePrimitive(overlay, (int)bay->location.x - overlay->width_ / 2,
                                                                 (int)bay->location.y - overlay->height_ / 2, 0, white, 1.f, false);
            CSurface::GL_DestroyPrimitive(bay->iconBorderPrimitive);
            bay->iconBorderPrimitive = resources->CreateImagePrimitive(outline, (int)bay->location.x - outline->width_ / 2,
                                                                       (int)bay->location.y - outline->height_ / 2, 0, white, 1.f, false);
            bay->imageIcon = resources->GetImageId(base + ".png");
            static const char *const STATES[5] = {"green", "grey", "orange", "red", "blue"};
            for (int state = 0; state < 5; ++state)
            {
                for (int look = 0; look < 2; ++look)
                {
                    GL_Texture *disc = resources->GetImageId(base + "_" + STATES[state] + (state == 4 || look == 0 ? "1" : "2") + ".png");
                    for (int colourBlind = 0; colourBlind < 2; ++colourBlind)
                    {
                        GL_Primitive *&primitive = bay->iconPrimitives[state * 4 + look * 2 + colourBlind];
                        CSurface::GL_DestroyPrimitive(primitive);
                        primitive = resources->CreateImagePrimitive(disc, 0, 0, 0, white, 1.f, false);
                    }
                }
            }
            g_bays.icons[bay] = std::make_pair(bay->iconPrimitive, kind);
            ++g_bays.iconsChanged;
        }

        // Our own ship, or the AI's outside a duel: the replica's weapons follow their owner's.
        static bool Owned(ShipManager *ship)
        {
            return ship->iShipId == 0 || !GetState().aiOff[1];
        }

        void AfterLoop(ShipManager *ship)
        {
            if (!ship || !ship->weaponSystem || !Bay(ship, 1)) return;
            WeaponSystem *weapons = ship->weaponSystem;
            bool owned = Owned(ship);
            for (int number = 1; number <= MAX_BAYS; ++number)
            {
                ShipSystem *bay = Bay(ship, number);
                if (!bay) continue;
                ProjectileFactory *weapon = number - 1 < (int)weapons->weapons.size() ? weapons->weapons[number - 1] : nullptr;
                SetLevel(bay, weapon ? std::max(1, weapon->requiredPower) : 1);
                std::string kind = WeaponKind(weapon);
                auto shown = g_bays.icons.find(bay);
                if (shown == g_bays.icons.end() || shown->second.first != bay->iconPrimitive || shown->second.second != kind)
                {
                    SetIcons(bay, kind);
                }
                if (owned && weapon && weapon->powered && Disabled(bay))
                {
                    weapons->DePowerWeapon(weapon, false);
                    ++g_bays.weaponsSwitchedOff;
                }
            }
            if (weapons->healthState.first < weapons->healthState.second)
            {
                weapons->healthState.first = weapons->healthState.second;
                ++g_bays.weaponsRestored;
            }
            // The doors inside cut rooms stay open (nothing should close them, but a lockdown would).
            auto hidden = g_bays.hiddenDoors.find(&ship->ship);
            if (hidden != g_bays.hiddenDoors.end())
            {
                for (Door *door : hidden->second) door->bOpen = true;
            }
        }

        void BuildingShip(Ship *ship, const std::string *layout)
        {
            g_bays.initShip = layout ? ship : nullptr;
            g_bays.initPlan = nullptr;
            if (!layout) return;
            auto plan = g_bays.plans.find(*layout);
            if (plan != g_bays.plans.end() && plan->second.ok) g_bays.initPlan = &plan->second;
        }

        // Which cut room (group) a room belongs to, -1 if none.
        static int GroupOf(const Plan &plan, int room)
        {
            for (size_t i = 0; i < plan.groups.size(); ++i)
            {
                const std::vector<int> &rooms = plan.groups[i].rooms;
                if (std::find(rooms.begin(), rooms.end(), room) != rooms.end()) return (int)i;
            }
            return -1;
        }

        void OnGraphBuilt()
        {
            Ship *ship = g_bays.initShip;
            const Plan *plan = g_bays.initPlan;
            if (!ship || !plan) return;
            std::vector<Door*> &hidden = g_bays.hiddenDoors[ship];
            hidden.clear();
            std::vector<Door*> kept;
            for (Door *door : ship->vDoorList)
            {
                int group = door ? GroupOf(*plan, door->iRoom1) : -1;
                if (group >= 0 && group == GroupOf(*plan, door->iRoom2))
                {
                    door->bOpen = true;
                    hidden.push_back(door);
                }
                else kept.push_back(door);
            }
            ship->vDoorList.swap(kept);
            g_bays.doorsHidden += (uint32_t)hidden.size();
        }

        // A group's rectangle in FTL's ship pixels (room rectangles are (tile + offset) * 35).
        static void GroupPixels(const Plan &plan, const Plan::Group &group, float &x0, float &y0, float &x1, float &y1)
        {
            x0 = (float)((group.original.x + plan.xOffset) * TILE);
            y0 = (float)((group.original.y + plan.yOffset) * TILE);
            x1 = x0 + group.original.w * TILE;
            y1 = y0 + group.original.h * TILE;
        }

        bool OnLines(std::vector<GL_Line> &lines, float thickness)
        {
            const Plan *plan = g_bays.initPlan;
            if (!g_bays.initShip || !plan || plan->groups.empty()) return false;
            static const float SLACK = 3.f;
            bool changed = false;
            if (thickness > 1.5f)
            {
                // The walls: none inside a cut room (only its outline stays).
                std::vector<GL_Line> kept;
                for (const GL_Line &line : lines)
                {
                    bool inside = false;
                    for (const Plan::Group &group : plan->groups)
                    {
                        float x0, y0, x1, y1;
                        GroupPixels(*plan, group, x0, y0, x1, y1);
                        float lx0 = std::min(line.start.x, line.end.x), lx1 = std::max(line.start.x, line.end.x);
                        float ly0 = std::min(line.start.y, line.end.y), ly1 = std::max(line.start.y, line.end.y);
                        bool vertical = lx1 - lx0 < 0.5f, horizontal = ly1 - ly0 < 0.5f;
                        if (vertical && lx0 > x0 + SLACK && lx0 < x1 - SLACK && ly0 >= y0 - SLACK && ly1 <= y1 + SLACK) inside = true;
                        if (horizontal && ly0 > y0 + SLACK && ly0 < y1 - SLACK && lx0 >= x0 - SLACK && lx1 <= x1 + SLACK) inside = true;
                    }
                    if (inside) ++g_bays.wallsLeftOut;
                    else kept.push_back(line);
                }
                changed = kept.size() != lines.size();
                lines.swap(kept);
            }
            else
            {
                // The floor grid: the lines between the tiles of a cut room, as FTL draws them inside a room.
                for (const Plan::Group &group : plan->groups)
                {
                    float x0, y0, x1, y1;
                    GroupPixels(*plan, group, x0, y0, x1, y1);
                    for (int k = 1; k < group.original.w; ++k) lines.push_back(GL_Line(x0 + k * TILE, y0, x0 + k * TILE, y1));
                    for (int k = 1; k < group.original.h; ++k) lines.push_back(GL_Line(x0, y0 + k * TILE, x1, y0 + k * TILE));
                    changed = true;
                }
            }
            return changed;
        }

        ShipSystem *InRoom(ShipManager *ship, int roomId, ShipSystem *found)
        {
            if (!found || found->iSystemType != SYS_WEAPONS) return found;
            ShipSystem *bay = Bay(ship, 1);
            return bay && bay->roomId == roomId ? bay : found;
        }

        bool KeepConsole(CrewMember *crew, ShipSystem *system)
        {
            if (!crew || crew->intruder || !IsBay(system)) return false;
            ShipManager *ship = ShipOf(system);
            return ship && ship->weaponSystem && ship->weaponSystem->roomId == system->roomId;
        }

        bool Untouchable(ShipSystem *system)
        {
            return system && system->iSystemType == SYS_WEAPONS && Bay(ShipOf(system), 1);
        }

        bool MayPower(ShipManager *ship, ProjectileFactory *weapon)
        {
            if (!ship || !weapon || !ship->weaponSystem || !Owned(ship)) return true;
            const std::vector<ProjectileFactory*> &list = ship->weaponSystem->weapons;
            auto found = std::find(list.begin(), list.end(), weapon);
            if (found == list.end()) return true;
            ShipSystem *bay = Bay(ship, (int)(found - list.begin()) + 1);
            return !bay || !Disabled(bay);
        }

        bool HideRoomIcon(ShipSystem *system)
        {
            return Untouchable(system);
        }

        bool OriginalRoomCorner(ShipSystem *system, int &x, int &y)
        {
            if (!system || !g_bays.building) return false;
            auto plan = g_bays.plans.find(*g_bays.building);
            if (plan == g_bays.plans.end() || !plan->second.ok) return false;
            auto moved = plan->second.moved.find(system->roomId);
            if (moved == plan->second.moved.end() || IsBay(system)) return false;
            x = system->roomShape.x - moved->second.x * TILE;
            y = system->roomShape.y - moved->second.y * TILE;
            return true;
        }

        void Building(const std::string *layout)
        {
            g_bays.building = layout;
        }

        std::string Describe(ShipManager *ship)
        {
            std::ostringstream out;
            if (!ship) return "no such ship";
            out << ship->myBlueprint.blueprintName << " (" << ship->myBlueprint.layoutFile << "):";
            for (Room *room : ship->ship.vRoomList)
            {
                out << "\n  room " << room->iRoomId << " at " << room->rect.x / TILE << "," << room->rect.y / TILE << " "
                    << room->rect.w / TILE << "x" << room->rect.h / TILE << ", console slot " << room->primarySlot
                    << " dir " << room->primaryDirection;
                for (ShipSystem *system : ship->vSystemList)
                {
                    if (system->roomId != room->iRoomId) continue;
                    out << ", " << ShipSystem::SystemIdToName(system->iSystemType) << " " << system->healthState.first << "/"
                        << system->healthState.second;
                    if (system->iLockCount) out << " lock " << system->iLockCount;
                }
            }
            return out.str();
        }

        std::string Signature(ShipManager *ship)
        {
            std::ostringstream out;
            if (!ship || !ship->weaponSystem) return out.str();
            const std::vector<ProjectileFactory*> &weapons = ship->weaponSystem->weapons;
            for (int number = 1; number <= MAX_BAYS; ++number)
            {
                ShipSystem *bay = Bay(ship, number);
                if (!bay) continue;
                out << number << ':' << bay->healthState.first << '/' << bay->healthState.second;
                if (bay->iLockCount > 0) out << 'i';
                if (bay->bUnderAttack && bay->iHackEffect >= 2) out << 'h';
                if (number - 1 < (int)weapons.size()) out << (weapons[number - 1]->powered ? '+' : '-');
                out << ' ';
            }
            return out.str();
        }

        std::string Status()
        {
            std::ostringstream out;
            out << "bays: layouts cut " << g_bays.layoutsCut << ", blueprints " << g_bays.blueprintsPatched
                << ", weapons switched off " << g_bays.weaponsSwitchedOff << ", weapons system restored "
                << g_bays.weaponsRestored << ", icons " << g_bays.iconsChanged << ", doors hidden " << g_bays.doorsHidden
                << ", walls left out " << g_bays.wallsLeftOut;
            return out.str();
        }
    }
}
