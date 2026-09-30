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

        // Two kinds of bays: one per weapon slot and one per drone slot.
        enum Kind
        {
            WEAPONS = 0,
            DRONES = 1,
            KINDS = 2
        };
        static const int SYSTEM_OF[KINDS] = {SYS_WEAPONS, SYS_DRONES};
        static const char LETTER[KINDS] = {'W', 'D'};
        static const char *const KIND_NAME[KINDS] = {"weapons", "drones"};
        static const char *const BAY_NAMES[KINDS][MAX_BAYS] = {
            {"weapon_bay_1", "weapon_bay_2", "weapon_bay_3", "weapon_bay_4"},
            {"drone_bay_1", "drone_bay_2", "drone_bay_3", nullptr}};

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
            // One system room cut into bays: the weapons room (W1 keeps its id and the gunner console) or the drone
            // control room (D1 keeps its id).
            struct Cut
            {
                bool ok = false;
                int systemRoom = -1;
                int consoleDirection = -1;      // FTL's direction (Globals::GetDirection), -1: FTL chooses
                bool consoleSet = false;        // the console is bay 1's slot 0 (it was set in the blueprint or picture)
                std::vector<int> bayRooms;      // bay 1 .. bay n
                std::vector<int> spareRooms;    // tiles no slot needs: empty rooms
            };
            // Each room that was cut, and the rooms it became: drawn as one room (no walls or doors between them).
            struct Group
            {
                TileRect original;          // in the layout's tiles
                std::vector<int> rooms;
            };
            bool ok = false;
            Cut cuts[KINDS];
            // Rooms cut smaller whose system keeps its picture where the whole room was: room -> how far (tiles) the
            // room's top left corner moved.
            std::map<int, Tile> moved;
            std::vector<Group> groups;
            int xOffset = 0, yOffset = 0;   // the layout's X_OFFSET and Y_OFFSET (FTL's room rectangles include them)
            std::string text;               // the layout, cut
            std::string summary;            // for the log
        };

        struct BaysState
        {
            std::map<std::string, Plan> plans;     // by layout name
            bool typesKnown = false;
            int types[KINDS][MAX_BAYS] = {{-1, -1, -1, -1}, {-1, -1, -1, -1}};
            std::map<std::string, std::pair<Tile, std::string>> glows;   // rooms.xml: picture -> console tile (px) and direction
            bool glowsLoaded = false;
            const std::string *building = nullptr;   // the layout of the ship whose systems are being added
            // Ship::OnInit: the ship and its plan while FTL builds its room graph, walls and grid.
            Ship *initShip = nullptr;
            const Plan *initPlan = nullptr;
            // The doors inside a cut room: out of the ship's door list (not drawn, not clickable, not closed by the
            // doors system), open, and still in its room graph so crew walk through.
            std::map<const Ship*, std::vector<Door*>> hiddenDoors;
            // The kind each bay's icons show, with the room icon they were made for (a new system gets new ones).
            std::map<const ShipSystem*, std::pair<const GL_Primitive*, std::string>> icons;
            uint32_t layoutsCut = 0, blueprintsPatched = 0, switchedOff = 0;
            uint32_t doorsHidden = 0, wallsLeftOut = 0, iconsChanged = 0;
        };

        static BaysState g_bays;

        // ---------------------------------------------------------------------------------------------------------
        // Bay systems
        // ---------------------------------------------------------------------------------------------------------

        static int MaxBays(int kind)
        {
            int count = 0;
            while (count < MAX_BAYS && BAY_NAMES[kind][count]) ++count;
            return count;
        }

        static int BayType(int kind, int number)
        {
            if (!g_bays.typesKnown)
            {
                g_bays.typesKnown = true;
                for (int k = 0; k < KINDS; ++k)
                {
                    for (int i = 0; i < MaxBays(k); ++i)
                    {
                        int type = ShipSystem::NameToSystemId(BAY_NAMES[k][i]);
                        g_bays.types[k][i] = type >= SYS_CUSTOM_FIRST ? type : -1;
                        if (g_bays.types[k][i] < 0) g_bays.typesKnown = false;   // data not loaded yet: ask again later
                    }
                }
            }
            return number >= 1 && number <= MaxBays(kind) ? g_bays.types[kind][number - 1] : -1;
        }

        // The kind and number (1..) of a bay system; false if the system is no bay.
        static bool BayOf(int systemType, int &kind, int &number)
        {
            if (systemType < SYS_CUSTOM_FIRST) return false;
            for (int k = 0; k < KINDS; ++k)
            {
                for (int n = 1; n <= MaxBays(k); ++n)
                {
                    if (BayType(k, n) == systemType)
                    {
                        kind = k;
                        number = n;
                        return true;
                    }
                }
            }
            return false;
        }

        int BayNumber(int systemType)
        {
            int kind, number;
            return BayOf(systemType, kind, number) ? number : 0;
        }

        bool IsBay(const ShipSystem *system)
        {
            return system && BayNumber(system->iSystemType) > 0;
        }

        static ShipSystem *Bay(ShipManager *ship, int kind, int number)
        {
            int type = BayType(kind, number);
            if (!ship || type < 0 || type >= (int)ship->systemKey.size() || ship->systemKey[type] < 0) return nullptr;
            return ship->GetSystem(type);
        }

        static ShipManager *ShipOf(const ShipSystem *system)
        {
            return system ? G_->GetShipManager(system->_shipObj.iShipId) : nullptr;
        }

        // A bay that switches its weapon or drone off: damaged (it needs all its bars), ioned or hacked.
        static bool Disabled(const ShipSystem *bay)
        {
            return bay->healthState.first < bay->healthState.second || bay->iLockCount > 0 ||
                   (bay->bUnderAttack && bay->iHackEffect >= 2);
        }

        // The weapon or drone in a slot (index from 0), what it needs, whether it is on.
        static ProjectileFactory *Weapon(ShipManager *ship, int index)
        {
            if (!ship || !ship->weaponSystem || index < 0) return nullptr;
            const std::vector<ProjectileFactory*> &list = ship->weaponSystem->weapons;
            return index < (int)list.size() ? list[index] : nullptr;
        }

        static Drone *DroneIn(ShipManager *ship, int index)
        {
            if (!ship || !ship->droneSystem || index < 0) return nullptr;
            const std::vector<Drone*> &list = ship->droneSystem->drones;
            return index < (int)list.size() ? list[index] : nullptr;
        }

        static bool HasItem(ShipManager *ship, int kind, int index)
        {
            return kind == WEAPONS ? Weapon(ship, index) != nullptr : DroneIn(ship, index) != nullptr;
        }

        static int ItemPower(ShipManager *ship, int kind, int index)
        {
            if (kind == WEAPONS)
            {
                ProjectileFactory *weapon = Weapon(ship, index);
                return weapon ? std::max(1, weapon->requiredPower) : 1;
            }
            Drone *drone = DroneIn(ship, index);
            return drone ? std::max(1, drone->powerRequired) : 1;
        }

        static bool ItemPowered(ShipManager *ship, int kind, int index)
        {
            if (kind == WEAPONS)
            {
                ProjectileFactory *weapon = Weapon(ship, index);
                return weapon && weapon->powered;
            }
            Drone *drone = DroneIn(ship, index);
            return drone && drone->powered;
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

        // Where a system's console is (tile in its room, row by row), from the blueprint's slot or else from its
        // room picture's glow (the blueprint's picture, or FTL's default "room_<system>"); -1 when there is none.
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

        // The layout while it is cut: its entries, the rooms as they are now, the new rooms, and where each tile of a
        // cut room went.
        struct Cutting
        {
            const ShipBlueprint *bp = nullptr;
            std::vector<Entry> entries;
            std::map<int, TileRect> rooms;       // as they are now (donors shrink)
            std::map<int, TileRect> original;    // every cut room as it was
            int nextId = 0;
            size_t lastRoom = 0;
            std::vector<Entry> newRooms;
            std::map<Tile, int> tileRoom;        // tiles of cut rooms -> the room each is in now
            std::map<Tile, int> donated;         // tiles given by a neighbour -> that room
        };

        // Cuts one system room into 1x1 rooms, one per slot (bay 1 keeps the room's id and its console tile), taking
        // missing tiles from a neighbouring room one tile wide.
        static bool CutRoom(Cutting &c, Plan &plan, int kind, int slots, std::ostringstream &summary)
        {
            auto system = c.bp->systemInfo.find(SYSTEM_OF[kind]);
            if (system == c.bp->systemInfo.end() || system->second.location.empty()) return false;
            Plan::Cut &cut = plan.cuts[kind];
            cut.systemRoom = system->second.location[0];
            auto rect = c.rooms.find(cut.systemRoom);
            if (rect == c.rooms.end() || slots < 1) return false;
            const TileRect sr = rect->second;
            c.original[cut.systemRoom] = sr;

            // The console's tile (weapons) becomes bay 1; then the room's other tiles row by row.
            int direction = -1;
            int consoleSlot = ConsoleSlot(SYSTEM_OF[kind], system->second, sr, direction);
            cut.consoleSet = consoleSlot >= 0;
            cut.consoleDirection = direction;
            if (consoleSlot < 0) consoleSlot = 0;
            Tile first{sr.x + consoleSlot % sr.w, sr.y + consoleSlot / sr.w};
            std::vector<Tile> tiles{first};
            for (int y = sr.y; y < sr.y + sr.h; ++y)
            {
                for (int x = sr.x; x < sr.x + sr.w; ++x)
                {
                    if (!(Tile{x, y} == first)) tiles.push_back(Tile{x, y});
                }
            }

            // Too few tiles (Engi B's weapons: 2 tiles, 3 slots): a neighbouring room one tile wide gives the tile at
            // its end next to the cut room, unless its console is there.
            std::vector<Tile> gifts;
            for (size_t i = 0; (int)tiles.size() < slots && i < tiles.size(); ++i)
            {
                static const int DX[4] = {1, -1, 0, 0}, DY[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4 && (int)tiles.size() < slots; ++d)
                {
                    Tile t{tiles[i].x + DX[d], tiles[i].y + DY[d]};
                    if (sr.Contains(t) || c.donated.count(t) || c.tileRoom.count(t)) continue;
                    for (std::pair<const int, TileRect> &room : c.rooms)
                    {
                        TileRect &r = room.second;
                        if (room.first == cut.systemRoom || !r.Contains(t)) continue;
                        bool thin = (r.w == 1 && r.h >= 2) || (r.h == 1 && r.w >= 2);
                        bool atEnd = (r.w == 1 && (t.y == r.y || t.y == r.y + r.h - 1)) ||
                                     (r.h == 1 && (t.x == r.x || t.x == r.x + r.w - 1));
                        if (!thin || !atEnd || c.original.count(room.first)) break;
                        bool console = false;
                        for (const std::pair<const int, ShipBlueprint::SystemTemplate> &other : c.bp->systemInfo)
                        {
                            if (other.second.location.empty() || other.second.location[0] != room.first) continue;
                            int ignored;
                            int slot = ConsoleSlot(other.first, other.second, r, ignored);
                            if (slot >= 0 && Tile{r.x + slot % r.w, r.y + slot / r.w} == t) console = true;
                        }
                        if (console) break;
                        c.donated[t] = room.first;
                        c.original[room.first] = r;
                        tiles.push_back(t);
                        gifts.push_back(t);
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
                summary << KIND_NAME[kind] << " room " << cut.systemRoom << ": too few tiles for " << slots << " slots; ";
                return false;
            }

            // Bay 1 keeps the room's id; the room's other tiles and donated ones become bays, spare tiles last.
            c.tileRoom[first] = cut.systemRoom;
            cut.bayRooms.push_back(cut.systemRoom);
            plan.moved[cut.systemRoom] = Tile{first.x - sr.x, first.y - sr.y};
            std::vector<Tile> bayTiles, spareTiles;
            for (size_t i = 1; i < tiles.size(); ++i)
            {
                bool own = sr.Contains(tiles[i]);
                bool gift = std::find(gifts.begin(), gifts.end(), tiles[i]) != gifts.end();
                if ((int)(bayTiles.size() + 1) < slots && (own || gift)) bayTiles.push_back(tiles[i]);
                else if (own) spareTiles.push_back(tiles[i]);
            }
            for (const Tile &t : bayTiles)
            {
                c.tileRoom[t] = c.nextId;
                cut.bayRooms.push_back(c.nextId);
                c.newRooms.push_back(Entry{"ROOM", {c.nextId, t.x, t.y, 1, 1}});
                ++c.nextId;
            }
            for (const Tile &t : spareTiles)
            {
                c.tileRoom[t] = c.nextId;
                cut.spareRooms.push_back(c.nextId);
                c.newRooms.push_back(Entry{"ROOM", {c.nextId, t.x, t.y, 1, 1}});
                ++c.nextId;
            }
            Plan::Group group;
            group.original = sr;
            plan.groups.push_back(group);
            cut.ok = true;

            summary << KIND_NAME[kind] << " room " << cut.systemRoom << " (" << sr.w << "x" << sr.h << ", " << slots
                    << " slots, bay 1 at " << first.x << "," << first.y << (cut.consoleSet ? "" : " by default") << "): bays";
            for (int room : cut.bayRooms) summary << ' ' << room;
            if (!cut.spareRooms.empty())
            {
                summary << ", spare";
                for (int room : cut.spareRooms) summary << ' ' << room;
            }
            for (const Tile &t : gifts) summary << ", a tile from room " << c.donated[t];
            summary << "; ";
            return true;
        }

        static Plan MakePlan(const std::string &layout, const char *text)
        {
            Plan plan;
            Cutting c;
            c.bp = PlayerBlueprint(layout);
            if (!c.bp) return plan;
            c.entries = ParseLayout(text);
            for (size_t i = 0; i < c.entries.size(); ++i)
            {
                const Entry &entry = c.entries[i];
                if (entry.key == "X_OFFSET" && !entry.args.empty()) plan.xOffset = entry.args[0];
                if (entry.key == "Y_OFFSET" && !entry.args.empty()) plan.yOffset = entry.args[0];
                if (entry.key != "ROOM" || entry.args.size() < 5) continue;
                c.rooms[entry.args[0]] = TileRect{entry.args[1], entry.args[2], entry.args[3], entry.args[4]};
                c.nextId = std::max(c.nextId, entry.args[0] + 1);
                c.lastRoom = i;
            }

            std::ostringstream summary;
            int weaponSlots = std::max(1, std::min(MaxBays(WEAPONS), c.bp->weaponSlots > 0 ? c.bp->weaponSlots : 4));
            int droneSlots = std::max(1, std::min(MaxBays(DRONES), c.bp->droneSlots > 0 ? c.bp->droneSlots : 2));
            CutRoom(c, plan, WEAPONS, weaponSlots, summary);
            CutRoom(c, plan, DRONES, droneSlots, summary);
            if (!plan.cuts[WEAPONS].ok && !plan.cuts[DRONES].ok)
            {
                plan.summary = summary.str();
                return plan;
            }

            // The rooms' own entries: bay 1 is one tile of the cut room; donors lose their tile.
            for (Entry &entry : c.entries)
            {
                if (entry.key != "ROOM" || entry.args.size() < 5 || !c.original.count(entry.args[0])) continue;
                int room = entry.args[0];
                const TileRect &r = c.rooms[room];
                bool cutRoom = false;
                for (int kind = 0; kind < KINDS; ++kind) cutRoom = cutRoom || (plan.cuts[kind].ok && plan.cuts[kind].systemRoom == room);
                if (cutRoom)
                {
                    const Tile &moved = plan.moved[room];
                    const TileRect &o = c.original[room];
                    entry.args = {room, o.x + moved.x, o.y + moved.y, 1, 1};
                }
                else entry.args = {room, r.x, r.y, r.w, r.h};
            }

            // Doors of the cut rooms open into the room that now has their tile.
            int doorsMoved = 0;
            for (Entry &entry : c.entries)
            {
                if (entry.key != "DOOR" || entry.args.size() < 5) continue;
                for (int side = 2; side <= 3; ++side)
                {
                    int room = entry.args[side];
                    auto original = c.original.find(room);
                    if (original == c.original.end()) continue;
                    int x = entry.args[0], y = entry.args[1];
                    Tile sides[2] = {entry.args[4] != 0 ? Tile{x - 1, y} : Tile{x, y - 1}, Tile{x, y}};
                    for (const Tile &t : sides)
                    {
                        if (!original->second.Contains(t)) continue;
                        auto now = c.tileRoom.find(t);
                        if (now != c.tileRoom.end() && now->second != room)
                        {
                            entry.args[side] = now->second;
                            ++doorsMoved;
                        }
                        break;
                    }
                }
            }

            // Doors inside each cut room, between neighbouring tiles that are now in different rooms: crew walk there
            // as before (the doors are hidden and stay open, OnGraphBuilt).
            std::vector<Entry> newDoors;
            for (Plan::Group &group : plan.groups)
            {
                const TileRect &g = group.original;
                int base = -1;
                for (const std::pair<const int, TileRect> &original : c.original)
                {
                    if (original.second.x == g.x && original.second.y == g.y && original.second.w == g.w &&
                        original.second.h == g.h) base = original.first;
                }
                auto roomOfTile = [&](const Tile &t) -> int {
                    auto found = c.tileRoom.find(t);
                    return found != c.tileRoom.end() ? found->second : base;   // a donor's remaining tiles
                };
                for (int y = g.y; y < g.y + g.h; ++y)
                {
                    for (int x = g.x; x < g.x + g.w; ++x)
                    {
                        int room = roomOfTile(Tile{x, y});
                        if (std::find(group.rooms.begin(), group.rooms.end(), room) == group.rooms.end()) group.rooms.push_back(room);
                        if (x + 1 < g.x + g.w)
                        {
                            int right = roomOfTile(Tile{x + 1, y});
                            if (right != room) newDoors.push_back(Entry{"DOOR", {x + 1, y, room, right, 1}});
                        }
                        if (y + 1 < g.y + g.h)
                        {
                            int down = roomOfTile(Tile{x, y + 1});
                            if (down != room) newDoors.push_back(Entry{"DOOR", {x, y + 1, room, down, 0}});
                        }
                    }
                }
            }

            c.entries.insert(c.entries.begin() + c.lastRoom + 1, c.newRooms.begin(), c.newRooms.end());
            c.entries.insert(c.entries.end(), newDoors.begin(), newDoors.end());
            plan.text = WriteLayout(c.entries);
            plan.ok = true;
            summary << doorsMoved << " doors moved, " << newDoors.size() << " new";
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
            bool patched = false;
            for (int kind = 0; kind < KINDS; ++kind)
            {
                const Plan::Cut &cut = plan.cuts[kind];
                if (!cut.ok || BayType(kind, 1) < 0 || bp->systemInfo.count(BayType(kind, 1))) continue;
                auto system = bp->systemInfo.find(SYSTEM_OF[kind]);
                if (system == bp->systemInfo.end() || system->second.location.empty() ||
                    system->second.location[0] != cut.systemRoom) continue;
                // The gunner console: bay 1's only tile, facing the way it did.
                if (cut.consoleSet)
                {
                    system->second.slot = 0;
                    system->second.direction = cut.consoleDirection;
                }
                for (size_t i = 0; i < cut.bayRooms.size() && (int)i < MaxBays(kind); ++i)
                {
                    int type = BayType(kind, (int)i + 1);
                    ShipBlueprint::SystemTemplate bay = ShipBlueprint::SystemTemplate();
                    bay.systemId = type;
                    bay.powerLevel = 1;
                    bay.location = std::vector<int>{cut.bayRooms[i]};
                    bay.bp = 0;
                    bay.maxPower = MAX_BAYS;
                    bay.slot = -1;
                    bay.direction = -1;
                    bp->systemInfo[type] = bay;
                    if (std::find(bp->systems.begin(), bp->systems.end(), type) == bp->systems.end()) bp->systems.push_back(type);
                }
                patched = true;
            }
            if (patched) ++g_bays.blueprintsPatched;
        }

        void PrepareBlueprint(ShipBlueprint *bp)
        {
            if (!bp || bp->blueprintName.compare(0, 12, "PLAYER_SHIP_") != 0 || BayType(WEAPONS, 1) < 0) return;
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

        // A bay's bars are its weapon's or drone's power (an empty slot's bay has one). Damage stays damage.
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

        // The kind of drone a drone bay shows: what the drone does.
        static const char *DroneKind(const Drone *drone)
        {
            const DroneBlueprint *bp = drone ? drone->blueprint : nullptr;
            if (!bp) return "empty";
            const std::string &type = bp->typeName, &weapon = bp->weaponBlueprint;
            if (type == "COMBAT")
            {
                if (weapon.find("FIRE") != std::string::npos) return "drone_fire";
                if (weapon.find("BEAM") != std::string::npos) return "drone_beam";
                if (weapon.find("ION") != std::string::npos) return "drone_ion";
                if (weapon.find("MISSILE") != std::string::npos) return "drone_missile";
                return "drone_laser";
            }
            if (type == "DEFENSE") return bp->name.find("ANTI_DRONE") != std::string::npos ? "drone_antidrone" : "drone_defense";
            if (type == "SHIELD") return "drone_shield";
            if (type == "SHIP_REPAIR") return "drone_hull";
            if (type == "REPAIR") return "drone_repair";
            if (type == "BATTLE") return "drone_battle";
            if (type == "BOARDER") return "drone_boarder";
            return "drone_laser";
        }

        static std::string ItemKind(ShipManager *ship, int kind, int index)
        {
            return kind == WEAPONS ? WeaponKind(Weapon(ship, index)) : DroneKind(DroneIn(ship, index));
        }

        // A bay's icons for a kind of weapon or drone, made the way FTL makes a system's: the room icon and its
        // outline at the room's centre, the discs (5 states, own and enemy look, and the colour blind slots) at the
        // origin.
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

        // Our own ship, or the AI's outside a duel: the replica's weapons and drones follow their owner's.
        static bool Owned(ShipManager *ship)
        {
            return ship->iShipId == 0 || !GetState().aiOff[1];
        }

        void AfterLoop(ShipManager *ship)
        {
            if (!ship) return;
            bool owned = Owned(ship);
            for (int kind = 0; kind < KINDS; ++kind)
            {
                if (!Bay(ship, kind, 1)) continue;
                for (int number = 1; number <= MaxBays(kind); ++number)
                {
                    ShipSystem *bay = Bay(ship, kind, number);
                    if (!bay) continue;
                    int index = number - 1;
                    SetLevel(bay, ItemPower(ship, kind, index));
                    std::string icon = ItemKind(ship, kind, index);
                    auto shown = g_bays.icons.find(bay);
                    if (shown == g_bays.icons.end() || shown->second.first != bay->iconPrimitive || shown->second.second != icon)
                    {
                        SetIcons(bay, icon);
                    }
                    if (owned && ItemPowered(ship, kind, index) && Disabled(bay))
                    {
                        if (kind == WEAPONS) ship->weaponSystem->DePowerWeapon(Weapon(ship, index), false);
                        else ship->DePowerDrone(DroneIn(ship, index), false);
                        ++g_bays.switchedOff;
                    }
                }
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

        static bool g_selectingRepair = false;

        void SetSelectingRepair(bool on)
        {
            g_selectingRepair = on;
        }

        ShipSystem *InRoom(ShipManager *ship, int roomId, ShipSystem *found)
        {
            if (!found) return found;
            for (int kind = 0; kind < KINDS; ++kind)
            {
                if (found->iSystemType != SYSTEM_OF[kind]) continue;
                ShipSystem *bay = Bay(ship, kind, 1);
                if (!bay || bay->roomId != roomId) return found;
                // Crew repair bay 1 first, then the spare bars (buffer points) of the system beside it.
                if (g_selectingRepair && !bay->NeedsRepairing() && found->NeedsRepairing()) return found;
                return bay;
            }
            return found;
        }

        bool KeepConsole(CrewMember *crew, ShipSystem *system)
        {
            if (!crew || crew->intruder || !IsBay(system)) return false;
            ShipManager *ship = ShipOf(system);
            return ship && ship->weaponSystem && ship->weaponSystem->roomId == system->roomId;
        }

        bool Untouchable(ShipSystem *system)
        {
            if (!system) return false;
            for (int kind = 0; kind < KINDS; ++kind)
            {
                if (system->iSystemType == SYSTEM_OF[kind]) return Bay(ShipOf(system), kind, 1) != nullptr;
            }
            return false;
        }

        // The bay of a weapon or drone of a ship (nullptr if it has none).
        static ShipSystem *BayOfWeapon(ShipManager *ship, const ProjectileFactory *weapon)
        {
            if (!ship || !ship->weaponSystem || !weapon) return nullptr;
            const std::vector<ProjectileFactory*> &list = ship->weaponSystem->weapons;
            auto found = std::find(list.begin(), list.end(), weapon);
            return found == list.end() ? nullptr : Bay(ship, WEAPONS, (int)(found - list.begin()) + 1);
        }

        static ShipSystem *BayOfDrone(ShipManager *ship, const Drone *drone)
        {
            if (!ship || !ship->droneSystem || !drone) return nullptr;
            const std::vector<Drone*> &list = ship->droneSystem->drones;
            auto found = std::find(list.begin(), list.end(), drone);
            return found == list.end() ? nullptr : Bay(ship, DRONES, (int)(found - list.begin()) + 1);
        }

        bool MayPower(ShipManager *ship, ProjectileFactory *weapon)
        {
            if (!ship || !Owned(ship)) return true;
            ShipSystem *bay = BayOfWeapon(ship, weapon);
            return !bay || !Disabled(bay);
        }

        bool MayPowerDrone(ShipManager *ship, Drone *drone)
        {
            if (!ship || !Owned(ship)) return true;
            ShipSystem *bay = BayOfDrone(ship, drone);
            return !bay || !Disabled(bay);
        }

        bool HideRoomIcon(ShipSystem *system)
        {
            return Untouchable(system);
        }

        bool BayOut(const ProjectileFactory *weapon)
        {
            ShipSystem *bay = weapon ? BayOfWeapon(G_->GetShipManager(weapon->iShipId), weapon) : nullptr;
            return bay && Disabled(bay);
        }

        bool DroneBayOut(const Drone *drone)
        {
            ShipSystem *bay = drone ? BayOfDrone(G_->GetShipManager(drone->iShipId), drone) : nullptr;
            return bay && Disabled(bay);
        }

        // Powered bars go to the segments from the bottom in the order FTL stacks them in one bar: the reactor's, then
        // the battery's, then Zoltan bonus power on top.
        static void TakePower(int powered, int &bonus, int &reactor, int &battery, Segment &segment)
        {
            segment.reactor = std::min(powered, reactor);
            reactor -= segment.reactor;
            powered -= segment.reactor;
            segment.battery = std::min(powered, battery);
            battery -= segment.battery;
            powered -= segment.battery;
            segment.bonus = std::min(powered, bonus);
            bonus -= segment.bonus;
        }

        static bool g_bufferHit = false;
        static uint32_t g_bufferPoints = 0;

        bool BufferHit()
        {
            return g_bufferHit;
        }

        // The system a bay belongs to, and its whole spare bars (not damaged, not ioned); nullptr if none of its
        // damage is ours to decide.
        static ShipSystem *BufferOf(ShipSystem *bay, int &spare, int &kind, int &number)
        {
            spare = 0;
            if (!bay || !BayOf(bay->iSystemType, kind, number)) return nullptr;
            ShipManager *ship = ShipOf(bay);
            if (!ship || !Owned(ship)) return nullptr;
            ShipSystem *system = kind == WEAPONS ? (ShipSystem*)ship->weaponSystem : (ShipSystem*)ship->droneSystem;
            if (!system) return nullptr;
            int capacity = system->powerState.second, used = 0;
            for (int n = 1; n <= MaxBays(kind); ++n)
            {
                if (HasItem(ship, kind, n - 1)) used += ItemPower(ship, kind, n - 1);
            }
            int damaged = std::max(0, system->healthState.second - system->healthState.first);
            spare = std::max(0, capacity - used - damaged - std::max(0, system->iLockCount));
            return system;
        }

        int TakeBuffer(ShipSystem *bay, int amount)
        {
            int spare = 0, kind = 0, number = 0;
            ShipSystem *system = amount > 0 ? BufferOf(bay, spare, kind, number) : nullptr;
            int taken = system ? std::min(amount, spare) : 0;
            if (taken <= 0) return 0;
            g_bufferHit = true;
            system->AddDamage(taken);
            g_bufferHit = false;
            g_bufferPoints += (uint32_t)taken;
            Log("Bays: damage on %s bay %d: %d spare bar%s of %s took it (%d left)", kind == WEAPONS ? "weapon" : "drone",
                number, taken, taken == 1 ? "" : "s", kind == WEAPONS ? "the weapons system" : "drone control",
                spare - taken);
            return taken;
        }

        bool BufferPartial(ShipSystem *bay, float amount, bool overTime, bool &result)
        {
            int spare = 0, kind = 0, number = 0;
            ShipSystem *system = amount > 0.f ? BufferOf(bay, spare, kind, number) : nullptr;
            if (!system || spare <= 0) return false;
            // Fire, a beam or sabotage wears on the spare bars first (a whole bar at a time, as FTL counts it).
            g_bufferHit = true;
            result = overTime ? system->DamageOverTime(amount) : system->PartialDamage(amount);
            g_bufferHit = false;
            return true;
        }

        bool PowerSegments(ShipSystem *system, std::vector<Segment> &segments)
        {
            segments.clear();
            int kind = -1;
            for (int k = 0; k < KINDS; ++k)
            {
                if (system && system->iSystemType == SYSTEM_OF[k]) kind = k;
            }
            ShipManager *ship = ShipOf(system);
            if (kind < 0 || !ship || !Bay(ship, kind, 1)) return false;
            int capacity = system->powerState.second;
            int bonus = system->iBonusPower, reactor = system->powerState.first, battery = system->iBatteryPower;
            int used = 0;
            for (int number = 1; number <= MaxBays(kind) && used < capacity; ++number)
            {
                if (!HasItem(ship, kind, number - 1)) continue;
                Segment segment;
                segment.bars = std::min(ItemPower(ship, kind, number - 1), capacity - used);
                TakePower(ItemPowered(ship, kind, number - 1) ? segment.bars : 0, bonus, reactor, battery, segment);
                ShipSystem *bay = Bay(ship, kind, number);
                if (bay)
                {
                    segment.damage = std::min(segment.bars, std::max(0, bay->healthState.second - bay->healthState.first));
                    segment.repair = bay->fRepairOverTime;
                    segment.partial = bay->fDamageOverTime;
                    segment.ioned = bay->iLockCount > 0;
                    segment.hacked = bay->bUnderAttack && bay->iHackEffect >= 2;
                }
                segments.push_back(segment);
                used += segment.bars;
            }
            if (used < capacity)
            {
                // The spare bars: what the system has beyond its weapons' (drones') needs, and its own damage there.
                Segment spare;
                spare.bars = capacity - used;
                spare.damage = std::min(spare.bars, std::max(0, system->healthState.second - system->healthState.first));
                TakePower(std::max(0, std::min(spare.bars, bonus + reactor + battery)), bonus, reactor, battery, spare);
                spare.repair = system->fRepairOverTime;
                spare.partial = system->fDamageOverTime;
                segments.push_back(spare);
            }
            return !segments.empty();
        }

        static bool g_panelLayout = false;

        void SetPanelLayout(bool on)
        {
            g_panelLayout = on;
        }

        bool HiddenFromPanel(const ShipManager *ship, const ShipSystem *system)
        {
            return g_panelLayout && ship && ship->iShipId == 0 && system && IsBay(system);
        }

        bool HideBox(const ShipSystem *system)
        {
            int kind, number;
            return system && BayOf(system->iSystemType, kind, number) && !HasItem(ShipOf(system), kind, number - 1);
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
            if (!ship) return out.str();
            for (int kind = 0; kind < KINDS; ++kind)
            {
                for (int number = 1; number <= MaxBays(kind); ++number)
                {
                    ShipSystem *bay = Bay(ship, kind, number);
                    if (!bay) continue;
                    out << LETTER[kind] << number << ':' << bay->healthState.first << '/' << bay->healthState.second;
                    if (bay->iLockCount > 0) out << 'i';
                    if (bay->bUnderAttack && bay->iHackEffect >= 2) out << 'h';
                    if (HasItem(ship, kind, number - 1)) out << (ItemPowered(ship, kind, number - 1) ? '+' : '-');
                    out << ' ';
                }
            }
            return out.str();
        }

        bool Tooltip(const ShipSystem *system, std::string &text)
        {
            int kind, number;
            if (!system || !BayOf(system->iSystemType, kind, number)) return false;
            ShipManager *ship = ShipOf(system);
            int index = number - 1;
            std::string item;
            if (kind == WEAPONS)
            {
                ProjectileFactory *weapon = Weapon(ship, index);
                if (weapon && weapon->blueprint) item = const_cast<TextString&>(weapon->blueprint->desc.title).GetText();
            }
            else
            {
                Drone *drone = DroneIn(ship, index);
                if (drone && drone->blueprint) item = const_cast<TextString&>(drone->blueprint->desc.title).GetText();
            }
            text = std::string(kind == WEAPONS ? "Weapon bay " : "Drone bay ") + std::to_string(number) + ": " +
                   (item.empty() ? "empty" : item);
            if (item.empty()) return true;
            int bars = system->healthState.second, whole = system->healthState.first;
            std::string state = whole < bars ? "Damaged: " + std::to_string(whole) + " of " + std::to_string(bars) + " bars whole"
                                             : "Whole (" + std::to_string(bars) + (bars == 1 ? " bar)" : " bars)");
            if (system->iLockCount > 0) state += ", ioned";
            if (system->bUnderAttack && system->iHackEffect >= 2) state += ", hacked";
            if (Disabled(system)) state += std::string(": the ") + (kind == WEAPONS ? "weapon" : "drone") + " is off until it is repaired";
            text += "\n" + state;
            return true;
        }

        std::string Status()
        {
            std::ostringstream out;
            out << "bays: layouts cut " << g_bays.layoutsCut << ", blueprints " << g_bays.blueprintsPatched
                << ", weapons and drones switched off " << g_bays.switchedOff << ", icons " << g_bays.iconsChanged
                << ", doors hidden " << g_bays.doorsHidden << ", walls left out " << g_bays.wallsLeftOut
                << ", hits taken by spare bars " << g_bufferPoints;
            return out.str();
        }
    }
}
