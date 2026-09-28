#include "Global.h"
#include "Duels.h"
#include "DuelsRooms.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

namespace Duels
{
    namespace Rooms
    {
        struct Tile
        {
            int x, y, damage;
        };

        struct Lockdown
        {
            int room;
            float x, y;   // where the crystal shards start
        };

        struct Snapshot
        {
            std::vector<int> oxygen;              // per room, 0-100
            std::vector<Tile> fires;              // burning tiles (grid x, y) and how strong
            std::vector<std::pair<int, int>> breaches;   // outer wall index, damage
            std::vector<bool> doors;              // doors, then airlocks: open
            std::vector<Lockdown> lockdowns;
        };

        struct RoomsState
        {
            bool havePending = false;
            Snapshot pending, owner;              // the latest from the owner, held on the replica every frame
            bool haveOwner = false;
            std::set<int> lockedRooms;            // rooms locked down here because the owner's are
            uint32_t firesStarted = 0, breachesMade = 0, doorsMoved = 0, lockdownsMade = 0;
        };

        static RoomsState g_rooms;

        void Reset()
        {
            g_rooms = RoomsState();
        }

        static std::vector<Door*> Doors(ShipManager *ship)
        {
            std::vector<Door*> doors = ship->ship.vDoorList;
            doors.insert(doors.end(), ship->ship.vOuterAirlocks.begin(), ship->ship.vOuterAirlocks.end());
            return doors;
        }

        static Snapshot Take(ShipManager *ship)
        {
            Snapshot s;
            if (ship->oxygenSystem)
            {
                for (float level : ship->oxygenSystem->oxygenLevels) s.oxygen.push_back(std::max(0, std::min(100, (int)std::lround(level))));
            }
            std::vector<std::vector<Fire>> &grid = ship->fireSpreader.grid;
            for (int x = 0; x < (int)grid.size(); ++x)
            {
                for (int y = 0; y < (int)grid[x].size(); ++y)
                {
                    Fire &fire = grid[x][y];
                    if (fire.fDamage > 0.f) s.fires.push_back(Tile{x, y, std::max(1, std::min(255, (int)std::lround(fire.fDamage)))});
                }
            }
            std::vector<OuterHull*> &walls = ship->ship.vOuterWalls;
            for (int i = 0; i < (int)walls.size(); ++i)
            {
                if (walls[i] && walls[i]->fDamage > 0.f) s.breaches.push_back(std::make_pair(i, std::max(1, std::min(255, (int)std::lround(walls[i]->fDamage)))));
            }
            for (Door *door : Doors(ship)) s.doors.push_back(door && door->bOpen);
            for (const LockdownShard &shard : ship->ship.lockdowns)
            {
                if (!shard.bDone) s.lockdowns.push_back(Lockdown{shard.lockingRoom, shard.position.x, shard.position.y});
            }
            return s;
        }

        void WriteState(Writer &w)
        {
            ShipManager *own = G_->GetShipManager(0);
            Snapshot s = own ? Take(own) : Snapshot();
            w.U8((uint8_t)s.oxygen.size());
            for (int level : s.oxygen) w.U8((uint8_t)level);
            w.U8((uint8_t)std::min<size_t>(s.fires.size(), 255));
            for (size_t i = 0; i < s.fires.size() && i < 255; ++i)
            {
                w.U8((uint8_t)s.fires[i].x);
                w.U8((uint8_t)s.fires[i].y);
                w.U8((uint8_t)s.fires[i].damage);
            }
            w.U8((uint8_t)std::min<size_t>(s.breaches.size(), 255));
            for (size_t i = 0; i < s.breaches.size() && i < 255; ++i)
            {
                w.U8((uint8_t)s.breaches[i].first);
                w.U8((uint8_t)s.breaches[i].second);
            }
            w.U8((uint8_t)s.doors.size());
            for (size_t i = 0; i < s.doors.size(); i += 8)
            {
                uint8_t bits = 0;
                for (size_t b = 0; b < 8 && i + b < s.doors.size(); ++b) bits |= s.doors[i + b] ? (uint8_t)(1u << b) : 0;
                w.U8(bits);
            }
            w.U8((uint8_t)std::min<size_t>(s.lockdowns.size(), 255));
            for (size_t i = 0; i < s.lockdowns.size() && i < 255; ++i)
            {
                w.U8((uint8_t)s.lockdowns[i].room);
                w.I16((int16_t)std::lround(s.lockdowns[i].x));
                w.I16((int16_t)std::lround(s.lockdowns[i].y));
            }
        }

        bool ReadState(Reader &r)
        {
            Snapshot s;
            s.oxygen.resize(r.U8());
            for (int &level : s.oxygen) level = r.U8();
            s.fires.resize(r.U8());
            for (Tile &tile : s.fires)
            {
                tile.x = r.U8();
                tile.y = r.U8();
                tile.damage = r.U8();
            }
            s.breaches.resize(r.U8());
            for (std::pair<int, int> &breach : s.breaches)
            {
                breach.first = r.U8();
                breach.second = r.U8();
            }
            size_t doorCount = r.U8();
            for (size_t i = 0; i < doorCount; i += 8)
            {
                uint8_t bits = r.U8();
                for (size_t b = 0; b < 8 && i + b < doorCount; ++b) s.doors.push_back((bits >> b) & 1);
            }
            s.lockdowns.resize(r.U8());
            for (Lockdown &lockdown : s.lockdowns)
            {
                lockdown.room = r.U8();
                lockdown.x = r.I16();
                lockdown.y = r.I16();
            }
            if (!r.Ok()) return false;
            g_rooms.pending = s;
            g_rooms.havePending = true;
            return true;
        }

        // Oxygen, fire strength and breach damage: the owner's values over whatever happened here.
        static void Hold(ShipManager *replica)
        {
            const Snapshot &s = g_rooms.owner;
            if (replica->oxygenSystem)
            {
                std::vector<float> &levels = replica->oxygenSystem->oxygenLevels;
                for (size_t i = 0; i < levels.size() && i < s.oxygen.size(); ++i) levels[i] = (float)s.oxygen[i];
            }
            std::vector<std::vector<Fire>> &grid = replica->fireSpreader.grid;
            std::set<std::pair<int, int>> burning;
            for (const Tile &tile : s.fires)
            {
                if (tile.x >= (int)grid.size() || tile.y >= (int)grid[tile.x].size()) continue;
                burning.insert(std::make_pair(tile.x, tile.y));
                Fire &fire = grid[tile.x][tile.y];
                if (fire.fDamage <= 0.f)
                {
                    fire.Spread();   // starts it, with its animation
                    ++g_rooms.firesStarted;
                }
                fire.fDamage = (float)tile.damage;
            }
            std::fill(replica->fireSpreader.roomCount.begin(), replica->fireSpreader.roomCount.end(), 0);
            replica->fireSpreader.count = 0;
            for (int x = 0; x < (int)grid.size(); ++x)
            {
                for (int y = 0; y < (int)grid[x].size(); ++y)
                {
                    Fire &fire = grid[x][y];
                    if (fire.fDamage > 0.f && !burning.count(std::make_pair(x, y))) fire.fDamage = 0.f;
                    if (fire.fDamage <= 0.f) continue;
                    ++replica->fireSpreader.count;
                    if (fire.roomId >= 0 && fire.roomId < (int)replica->fireSpreader.roomCount.size()) ++replica->fireSpreader.roomCount[fire.roomId];
                }
            }
            std::vector<OuterHull*> &walls = replica->ship.vOuterWalls;
            std::vector<int> damage(walls.size(), 0);
            for (const std::pair<int, int> &breach : s.breaches)
            {
                if (breach.first < (int)damage.size()) damage[breach.first] = breach.second;
            }
            for (size_t i = 0; i < walls.size(); ++i)
            {
                OuterHull *wall = walls[i];
                if (!wall) continue;
                if (damage[i] > 0 && wall->fDamage <= 0.f)
                {
                    wall->PartialDamage((float)damage[i]);   // a new breach, with its animation
                    ++g_rooms.breachesMade;
                }
                wall->fDamage = (float)damage[i];
            }
            // Lockdowns only where the owner's are (our own lockdown bomb's copy must not lock anything here).
            std::set<int> ownerLocked;
            for (const Lockdown &lockdown : s.lockdowns) ownerLocked.insert(lockdown.room);
            for (LockdownShard &shard : replica->ship.lockdowns)
            {
                if (!shard.bDone && !ownerLocked.count(shard.lockingRoom)) shard.bDone = true;
            }

            // Rooms light up red with fire or a breach, and their systems count as breached, as FTL's environment
            // update (which doesn't run for the replica) would do.
            for (Room *room : replica->ship.vRoomList)
            {
                if (!room) continue;
                int id = room->iRoomId;
                bool fire = id >= 0 && id < (int)replica->fireSpreader.roomCount.size() && replica->fireSpreader.roomCount[id] > 0;
                std::pair<int, int> drains = replica->ship.ContainsHullBreach(id);
                room->bWarningLight = fire || drains.second > 0;
                ShipSystem *system = replica->GetSystemInRoom(id);
                if (system) system->bBreached = drains.second > 0;
            }
        }

        void ApplyState()
        {
            if (!g_rooms.havePending) return;
            g_rooms.havePending = false;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica) return;
            const Snapshot &s = g_rooms.pending;

            // Doors open and close with their animation.
            std::vector<Door*> doors = Doors(replica);
            for (size_t i = 0; i < doors.size() && i < s.doors.size(); ++i)
            {
                Door *door = doors[i];
                if (!door || door->bOpen == s.doors[i]) continue;
                if (s.doors[i]) door->Open();
                else door->Close();
                ++g_rooms.doorsMoved;
            }

            // A room the owner has locked down (its crystal shards fly to the doors, one per door) is locked down here
            // too, once, from where the owner's shards start; the lockdown runs out on its own, as the owner's does.
            std::set<int> ownerRooms;
            for (const Lockdown &lockdown : s.lockdowns)
            {
                if (!ownerRooms.insert(lockdown.room).second || g_rooms.lockedRooms.count(lockdown.room)) continue;
                replica->ship.LockdownRoom(lockdown.room, Pointf(lockdown.x, lockdown.y));
                g_rooms.lockedRooms.insert(lockdown.room);
                ++g_rooms.lockdownsMade;
            }
            for (auto it = g_rooms.lockedRooms.begin(); it != g_rooms.lockedRooms.end();)
            {
                if (ownerRooms.count(*it)) ++it;
                else it = g_rooms.lockedRooms.erase(it);
            }

            g_rooms.owner = s;
            g_rooms.haveOwner = true;
            Hold(replica);
        }

        void AfterReplicaLoop(ShipManager *replica)
        {
            if (g_rooms.haveOwner && replica && replica == G_->GetShipManager(1)) Hold(replica);
        }

        std::string Signature(ShipManager *ship)
        {
            if (!ship) return "";
            Snapshot s = Take(ship);
            std::ostringstream out;
            out << "o2";
            for (int level : s.oxygen) out << ' ' << (level + 5) / 10 * 10;
            out << " fires " << s.fires.size() << " breaches " << s.breaches.size() << " doors ";
            for (bool open : s.doors) out << (open ? '1' : '0');
            // Locked rooms, not shards: a lockdown sends one shard per door, and their number isn't the point.
            std::set<int> locked;
            for (const Lockdown &lockdown : s.lockdowns) locked.insert(lockdown.room);
            out << " lockdowns";
            for (int room : locked) out << ' ' << room;
            return out.str();
        }

        std::string Status()
        {
            std::ostringstream out;
            out << "rooms: fires started " << g_rooms.firesStarted << ", breaches made " << g_rooms.breachesMade << ", doors moved "
                << g_rooms.doorsMoved << ", lockdowns " << g_rooms.lockdownsMade;
            return out.str();
        }

        bool RunsEnvironment(ShipManager *ship)
        {
            return !(g_rooms.haveOwner && ship && ship->iShipId == 1 && ship == G_->GetShipManager(1));
        }
    }
}
