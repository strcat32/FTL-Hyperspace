#include "Global.h"
#include "Duels.h"
#include "DuelsCrew.h"
#include "DuelsTrace.h"
#include "DuelsVision.h"
#include "DuelsWire.h"
#include "Drones.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <sstream>
#include <vector>

namespace Duels
{
    namespace Crew
    {
        static const int SKILLS = 6;               // piloting, engines, shields, weapons, repair, combat
        static const float PLACE_DISTANCE = 52.f;   // a puppet this far (1.5 tiles) from its owner's crew member ...
        static const double PLACE_AFTER_MS = 1000.0;   // ... for this long is put where they are

        // State flags per crew member.
        enum : uint8_t
        {
            FLAG_DEAD = 1,
            FLAG_MIND_CONTROLLED = 2,
            FLAG_FIGHTING = 4,    // these three as the crew member's animation shows them
            FLAG_REPAIRING = 8,
            FLAG_MANNING = 16,
            FLAG_IN_FIRE = 32,
            FLAG_TYPING = 64,
            ANIMATION_FLAGS = FLAG_FIGHTING | FLAG_REPAIRING | FLAG_IN_FIRE | FLAG_TYPING
        };

        struct RosterEntry
        {
            uint16_t id = 0;
            std::string species, name;
            bool male = true;
            int droneSlot = -1;           // a crew drone (anti-personnel, system repair): its slot in the drone system
            int skills[SKILLS][2] = {};   // FTL's skill progress and level
        };

        struct Sample
        {
            double t = 0.0;
            float x = 0.f, y = 0.f;
            int room = -1, goalRoom = -1, goalSlot = -1;
            bool still = false;   // at the same place as in the owner's state before
            int health = 0;
            uint8_t flags = 0;
        };

        struct Puppet
        {
            RosterEntry roster;
            CrewMember *crew = nullptr;   // checked against the replica's crew list before every use
            bool haveSample = false;
            Sample sample;
            int movingToRoom = -1, movingToSlot = -1;
            double farSinceMs = -1.0;
            bool placeNow = true;          // a new puppet goes straight to its owner's position
            bool ownerControlled = false;  // under mind control, as the owner last said
            float normalMax = 0.f;         // its own maximum health while a mind control's boost raises it (0: none)
            bool hidden = false;           // left out of the owner's latest state: where we can't see them (roadmap 4.5)
        };

        struct CrewState
        {
            // Ours.
            std::map<const CrewMember*, uint16_t> ownIds;
            uint16_t nextId = 1;
            std::string sentRoster;
            std::map<const CrewAnimation*, uint8_t> ownAnimations;   // what each animation showed last (only compared)
            // Boarding. Theirs aboard our ship, ours to decide, by their owner's ids.
            std::map<uint16_t, CrewMember*> guests;
            // Ours aboard their ship: puppets of the owner's guest state there, by our ids (kept for their return).
            std::map<uint16_t, Puppet> away;
            std::map<const CrewMember*, uint16_t> heldIds;
            std::map<const CrewMember*, uint16_t> prevIds;   // our crew's ids one state earlier (for crew just gone aboard)
            std::vector<uint16_t> deadGuestsSent;            // the guests the latest state reported dead (for a demo's)
            std::vector<std::pair<uint16_t, Sample>> pendingAway;
            uint32_t boarded = 0, guestsMade = 0;
        };

        static CrewState g_crew;

        // Theirs: the puppets of a ship's crew, driven by its owner's roster and states. Side 1 is the opponent's ship;
        // in a replay (roadmap 5.1) side 0 is ours, the recorder's.
        struct PuppetSide
        {
            bool active = false;           // a roster came: the ship's crew are puppets now
            std::map<uint16_t, Puppet> puppets;
            std::vector<std::pair<uint16_t, Sample>> pending;
            bool havePending = false;
            std::map<const CrewAnimation*, uint8_t> puppetAnimations;   // rebuilt after every loop of the ship
            uint32_t rostersApplied = 0, placed = 0, created = 0;
            // A replay: the other side's crew aboard this ship (on ours: the opponent's boarders), by their owner's ids,
            // puppets of this ship's owner's guest entries (its game simulated them).
            std::map<uint16_t, Puppet> guests;
        };
        static PuppetSide g_sides[2];

        static PuppetSide &SideOf(const ShipManager *ship)
        {
            return g_sides[ship && ship->iShipId == 0 ? 0 : 1];
        }

        void Reset()
        {
            g_crew = CrewState();
            g_sides[0] = PuppetSide();
            g_sides[1] = PuppetSide();
        }

        // ---------------------------------------------------------------------------------------------------------
        // Our side
        // ---------------------------------------------------------------------------------------------------------

        int WireHealth(const CrewMember *crew)
        {
            if (!crew || crew->bDead || crew->health.first <= 0.f) return 0;
            return (int)std::max(1L, std::lround(crew->health.first));
        }

        // FTL's crew drones (anti-personnel, system repair) are crew members and drones at once; a drone system's list
        // holds their drone part.
        static CrewMember *CrewOfDrone(Drone *drone)
        {
            if (!drone || (drone->type != DRONE_REPAIR && drone->type != DRONE_BATTLE)) return nullptr;
            return reinterpret_cast<CrewDrone*>(reinterpret_cast<char*>(drone) - offsetof(CrewDrone, _drone));
        }

        // A crew drone's slot in that ship's drone system (-1: not one of its crew drones).
        static int DroneSlotOf(ShipManager *ship, const CrewMember *crew)
        {
            if (!ship || !ship->droneSystem || !crew) return -1;
            std::vector<Drone*> &drones = ship->droneSystem->drones;
            for (size_t slot = 0; slot < drones.size(); ++slot)
            {
                if (CrewOfDrone(drones[slot]) == crew) return (int)slot;
            }
            return -1;
        }

        // Our crew members on our ship, with their ids: the crew drones that are out count too.
        static std::vector<std::pair<uint16_t, CrewMember*>> OwnCrew()
        {
            std::vector<std::pair<uint16_t, CrewMember*>> list;
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return list;
            std::map<const CrewMember*, uint16_t> ids;
            for (CrewMember *crew : own->vCrewList)
            {
                if (!crew || crew->iShipId != 0) continue;
                if (crew->IsDrone())
                {
                    int slot = DroneSlotOf(own, crew);
                    if (slot < 0 || crew->bDead || !own->droneSystem->drones[slot]->deployed) continue;
                }
                auto found = g_crew.ownIds.find(crew);
                auto held = g_crew.heldIds.find(crew);
                uint16_t id = found != g_crew.ownIds.end() ? found->second
                              : held != g_crew.heldIds.end() ? held->second : g_crew.nextId++;
                if (held != g_crew.heldIds.end()) g_crew.heldIds.erase(held);
                ids[crew] = id;
                list.push_back(std::make_pair(id, crew));
            }
            g_crew.ownIds.swap(ids);
            g_crew.prevIds.swap(ids);
            std::sort(list.begin(), list.end(),
                      [](const std::pair<uint16_t, CrewMember*> &a, const std::pair<uint16_t, CrewMember*> &b) { return a.first < b.first; });
            return list;
        }

        // Our crew with the ids the latest state gave them, without changing them (a demo's full state).
        static std::vector<std::pair<uint16_t, CrewMember*>> KnownOwnCrew()
        {
            std::vector<std::pair<uint16_t, CrewMember*>> list;
            for (const std::pair<const CrewMember*, uint16_t> &entry : g_crew.ownIds) list.push_back(std::make_pair(entry.second, const_cast<CrewMember*>(entry.first)));
            std::sort(list.begin(), list.end(),
                      [](const std::pair<uint16_t, CrewMember*> &a, const std::pair<uint16_t, CrewMember*> &b) { return a.first < b.first; });
            return list;
        }

        static std::string BuildRoster()
        {
            Writer w;
            std::vector<std::pair<uint16_t, CrewMember*>> crew = OwnCrew();
            w.U8((uint8_t)crew.size());
            for (const std::pair<uint16_t, CrewMember*> &entry : crew)
            {
                CrewMember *member = entry.second;
                w.U16(entry.first);
                w.Str(member->species);
                w.Str(member->GetName());
                w.Bool(member->blueprint.male);
                w.I8((int8_t)DroneSlotOf(G_->GetShipManager(0), member));
                for (int skill = 0; skill < SKILLS; ++skill)
                {
                    bool known = skill < (int)member->blueprint.skillLevel.size();
                    w.U8((uint8_t)std::max(0, known ? member->blueprint.skillLevel[skill].first : 0));
                    w.U8((uint8_t)std::max(0, known ? member->blueprint.skillLevel[skill].second : 0));
                }
            }
            return std::string(w.data.begin(), w.data.end());
        }

        bool RosterChanged()
        {
            return BuildRoster() != g_crew.sentRoster;
        }

        void SendRosterAgain()
        {
            g_crew.sentRoster.clear();
        }

        void WriteRoster(Writer &w)
        {
            g_crew.sentRoster = BuildRoster();
            w.data.insert(w.data.end(), g_crew.sentRoster.begin(), g_crew.sentRoster.end());
            Log("Crew: roster sent (%u crew)", (unsigned)(g_crew.sentRoster.empty() ? 0 : (uint8_t)g_crew.sentRoster[0]));
        }

        static void WriteEntry(Writer &w, uint16_t id, CrewMember *member)
        {
            {
                uint8_t flags = 0;
                if (member->bDead) flags |= FLAG_DEAD;
                if (member->bMindControlled) flags |= FLAG_MIND_CONTROLLED;
                if (member->bActiveManning) flags |= FLAG_MANNING;
                auto shown = g_crew.ownAnimations.find(member->crewAnim);
                if (shown != g_crew.ownAnimations.end()) flags |= shown->second;
                // Where they are heading: the final goal while walking, else where they stand.
                int goalRoom = member->finalGoal.roomId >= 0 ? member->finalGoal.roomId : member->currentSlot.roomId;
                int goalSlot = member->finalGoal.roomId >= 0 ? member->finalGoal.slotId : member->currentSlot.slotId;
                w.U16(id);
                w.U8(flags);
                w.I8((int8_t)member->iRoomId);
                w.I16((int16_t)std::lround(member->x));
                w.I16((int16_t)std::lround(member->y));
                w.I8((int8_t)goalRoom);
                w.I8((int8_t)goalSlot);
                w.U16((uint16_t)WireHealth(member));
            }
        }

        // A guest's crew member while it is still on our ship (FTL cleans up the dead).
        static CrewMember *LiveGuest(uint16_t id)
        {
            auto found = g_crew.guests.find(id);
            ShipManager *own = G_->GetShipManager(0);
            if (found == g_crew.guests.end() || !own) return nullptr;
            for (CrewMember *crew : own->vCrewList)
            {
                if (crew == found->second) return crew;
            }
            return nullptr;
        }

        void WriteState(Writer &w, bool record)
        {
            // Only those the opponent can see (roadmap 4.5): their puppets of the others stay as last seen (FTL draws
            // none of them where its player can't see).
            std::vector<std::pair<uint16_t, CrewMember*>> crew = record ? KnownOwnCrew() : OwnCrew();
            const Vision::Seen &seen = Vision::Current();
            crew.erase(std::remove_if(crew.begin(), crew.end(),
                                      [&](const std::pair<uint16_t, CrewMember*> &entry) { return !seen.Crew(entry.second->iRoomId); }),
                       crew.end());
            w.U8((uint8_t)crew.size());
            for (const std::pair<uint16_t, CrewMember*> &entry : crew) WriteEntry(w, entry.first, entry.second);
            // Guests: their crew aboard our ship, by their ids. One that died and is gone says so once more.
            std::vector<std::pair<uint16_t, CrewMember*>> guests;
            if (record)
            {
                for (const std::pair<const uint16_t, CrewMember*> &entry : g_crew.guests)
                {
                    CrewMember *live = LiveGuest(entry.first);
                    if (live) guests.push_back(std::make_pair(entry.first, live));
                }
                for (uint16_t id : g_crew.deadGuestsSent) guests.push_back(std::make_pair(id, (CrewMember*)nullptr));
            }
            else
            {
                g_crew.deadGuestsSent.clear();
                for (auto it = g_crew.guests.begin(); it != g_crew.guests.end();)
                {
                    CrewMember *live = LiveGuest(it->first);
                    if (live)
                    {
                        guests.push_back(std::make_pair(it->first, live));
                        ++it;
                    }
                    else
                    {
                        guests.push_back(std::make_pair(it->first, (CrewMember*)nullptr));
                        g_crew.deadGuestsSent.push_back(it->first);
                        it = g_crew.guests.erase(it);
                    }
                }
            }
            w.U8((uint8_t)guests.size());
            for (const std::pair<uint16_t, CrewMember*> &entry : guests)
            {
                if (entry.second)
                {
                    WriteEntry(w, entry.first, entry.second);
                    continue;
                }
                w.U16(entry.first);
                w.U8(FLAG_DEAD);
                w.I8(-1);
                w.I16(0);
                w.I16(0);
                w.I8(-1);
                w.I8(-1);
                w.U16(0);
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Their side
        // ---------------------------------------------------------------------------------------------------------

        // The puppet's crew member if it is still on the replica (FTL cleans up dead crew itself).
        static CrewMember *LiveCrew(ShipManager *replica, Puppet &puppet)
        {
            if (!puppet.crew || !replica) return puppet.crew = nullptr;
            for (CrewMember *crew : replica->vCrewList)
            {
                if (crew == puppet.crew) return crew;
            }
            return puppet.crew = nullptr;
        }

        static void ApplySkills(CrewMember *crew, const RosterEntry &roster)
        {
            for (int skill = 0; skill < SKILLS && skill < (int)crew->blueprint.skillLevel.size(); ++skill)
            {
                crew->blueprint.skillLevel[skill].first = roster.skills[skill][0];
                crew->blueprint.skillLevel[skill].second = roster.skills[skill][1];
            }
        }

        // A crew member for a puppet, in the room its owner is in (room 0 until the first state).
        static void CreateCrew(ShipManager *replica, Puppet &puppet)
        {
            int room = puppet.haveSample && puppet.sample.room >= 0 ? puppet.sample.room : 0;
            CrewMember *crew = replica->AddCrewMemberFromString(puppet.roster.name, puppet.roster.species, false, room, false,
                                                                puppet.roster.male);
            if (!crew) return;
            // Its owner's name as it is: FTL shortens one that is too wide ("Frederi."), its owner's game didn't.
            TextString name(puppet.roster.name, true);
            crew->SetName(&name, true);
            puppet.crew = crew;
            puppet.placeNow = true;
            puppet.movingToRoom = puppet.movingToSlot = -1;
            ApplySkills(crew, puppet.roster);
            ++SideOf(replica).created;
        }

        // A crew drone's puppet is the replica's own drone in that slot while it is out (DuelsDrones.cpp launches it as
        // the owner's is launched); never a crew member made here.
        static void BindDrone(ShipManager *replica, Puppet &puppet)
        {
            int slot = puppet.roster.droneSlot;
            Drone *drone = replica && replica->droneSystem && slot >= 0 && slot < (int)replica->droneSystem->drones.size()
                           ? replica->droneSystem->drones[slot] : nullptr;
            CrewMember *crew = CrewOfDrone(drone);
            if (!crew || !drone->deployed || drone->bDead)
            {
                puppet.crew = nullptr;
                return;
            }
            if (puppet.crew != crew)
            {
                puppet.crew = crew;
                puppet.placeNow = true;
                puppet.movingToRoom = puppet.movingToSlot = -1;
            }
        }

        static bool IsPuppetCrew(const CrewMember *crew)
        {
            for (const PuppetSide &side : g_sides)
            {
                for (const std::pair<const uint16_t, Puppet> &entry : side.guests)
                {
                    if (entry.second.crew == crew) return true;
                }
                if (!side.active) continue;
                for (const std::pair<const uint16_t, Puppet> &entry : side.puppets)
                {
                    if (entry.second.crew == crew) return true;
                }
            }
            return false;
        }

        static void ApplyRosterTo(PuppetSide &side, ShipManager *replica, Reader &r)
        {
            std::vector<RosterEntry> roster(r.U8());
            for (RosterEntry &entry : roster)
            {
                entry.id = r.U16();
                entry.species = r.Str();
                entry.name = r.Str();
                entry.male = r.Bool();
                entry.droneSlot = r.I8();
                for (int skill = 0; skill < SKILLS; ++skill)
                {
                    entry.skills[skill][0] = r.U8();
                    entry.skills[skill][1] = r.U8();
                }
            }
            if (!r.Ok() || !replica) return;

            // Crew no longer on board leave the replica (the dead ones FTL cleans up itself).
            for (auto it = side.puppets.begin(); it != side.puppets.end();)
            {
                bool kept = std::any_of(roster.begin(), roster.end(), [&](const RosterEntry &entry) { return entry.id == it->first; });
                if (kept)
                {
                    ++it;
                    continue;
                }
                CrewMember *crew = LiveCrew(replica, it->second);
                // A crew drone's puppet stays with the replica's drone system (DuelsDrones.cpp takes it back).
                if (crew && !crew->bDead && it->second.roster.droneSlot < 0) replica->RemoveCrewmember(crew);
                it = side.puppets.erase(it);
            }
            // The replica's own crew (its blueprint's, or anyone else not from the roster) leaves too.
            std::vector<CrewMember*> others;
            for (CrewMember *crew : replica->vCrewList)
            {
                if (crew && crew->iShipId == replica->iShipId && !crew->IsDrone() && !crew->bDead && !IsPuppetCrew(crew)) others.push_back(crew);
            }
            for (CrewMember *crew : others) replica->RemoveCrewmember(crew);

            for (const RosterEntry &entry : roster)
            {
                Puppet &puppet = side.puppets[entry.id];
                bool fresh = puppet.roster.id == 0;
                if (fresh) puppet.hidden = true;   // until a state has them
                bool sameMember = !fresh && puppet.roster.species == entry.species && puppet.roster.droneSlot == entry.droneSlot;
                puppet.roster = entry;
                if (entry.droneSlot >= 0)
                {
                    BindDrone(replica, puppet);
                    continue;
                }
                CrewMember *crew = LiveCrew(replica, puppet);
                if (crew && !sameMember)
                {
                    replica->RemoveCrewmember(crew);
                    puppet.crew = crew = nullptr;
                }
                if (!crew) CreateCrew(replica, puppet);
                else ApplySkills(crew, entry);
            }
            side.active = true;
            ++side.rostersApplied;
            Log("Crew: roster applied (%u crew; %u removed from the replica's own)", (unsigned)roster.size(), (unsigned)others.size());
        }

        void ApplyRoster(Reader &r)
        {
            ApplyRosterTo(g_sides[1], G_->GetShipManager(1), r);
        }

        void ReplayOwnRoster(const uint8_t *data, size_t size)
        {
            Reader r(data, size);
            ApplyRosterTo(g_sides[0], G_->GetShipManager(0), r);
        }

        static void ReadEntries(Reader &r, std::vector<std::pair<uint16_t, Sample>> &samples)
        {
            samples.resize(r.U8());
            for (std::pair<uint16_t, Sample> &entry : samples)
            {
                entry.first = r.U16();
                Sample &s = entry.second;
                s.flags = r.U8();
                s.room = r.I8();
                s.x = r.I16();
                s.y = r.I16();
                s.goalRoom = r.I8();
                s.goalSlot = r.I8();
                s.health = r.U16();
            }
        }

        bool ReadState(Reader &r)
        {
            std::vector<std::pair<uint16_t, Sample>> samples, away;
            ReadEntries(r, samples);
            ReadEntries(r, away);
            if (!r.Ok()) return false;
            g_sides[1].pending.swap(samples);
            g_crew.pendingAway.swap(away);
            g_sides[1].havePending = true;
            return true;
        }

        // A puppet takes its owner's sample: their crew (made again when they come back to life), or ours away
        // (never made here: they are our crew members).
        // The slot at a point of a room (FTL numbers a room's tiles row by row), or -1.
        static int SlotAt(ShipManager *ship, int roomId, float x, float y)
        {
            ShipGraph *graph = ship ? ShipGraph::GetShipInfo(ship->iShipId) : nullptr;
            if (!graph || roomId < 0 || roomId >= (int)graph->rooms.size() || !graph->rooms[roomId]) return -1;
            const Globals::Rect &rect = graph->rooms[roomId]->rect;
            int columns = rect.w / 35, rows = rect.h / 35;
            int column = (int)std::floor((x - rect.x) / 35.f), row = (int)std::floor((y - rect.y) / 35.f);
            if (column < 0 || row < 0 || column >= columns || row >= rows) return -1;
            return row * columns + column;
        }

        // Where a puppet walks: to its owner's goal while the owner walks there, but to where the owner stands while
        // it stands still with its goal elsewhere (an intruder breaking a door on its way, crew held up by one, a
        // drone switched off on its way).
        static void Target(ShipManager *replica, const Sample &s, int &room, int &slot)
        {
            room = s.goalRoom;
            slot = s.goalSlot;
            if (s.still && s.room >= 0 && s.goalRoom != s.room)
            {
                room = s.room;
                slot = SlotAt(replica, s.room, s.x, s.y);
            }
        }

        static void ApplySample(ShipManager *replica, Puppet &puppet, const Sample &sample, bool mayCreate)
        {
            {
                bool still = puppet.haveSample && std::fabs(puppet.sample.x - sample.x) < 0.5f &&
                             std::fabs(puppet.sample.y - sample.y) < 0.5f;
                puppet.sample = sample;
                puppet.sample.still = still;
                puppet.haveSample = true;
                const Sample &s = puppet.sample;
                CrewMember *crew = LiveCrew(replica, puppet);

                // Alive again (a clone), or never made: a new crew member in their room.
                if (mayCreate && !(s.flags & FLAG_DEAD) && (!crew || crew->bDead))
                {
                    CreateCrew(replica, puppet);
                    crew = puppet.crew;
                }
                if (!crew) return;
                if ((s.flags & FLAG_DEAD) && !crew->bDead)
                {
                    crew->health.first = 0.f;
                    crew->Kill(true);
                    return;
                }
                if (crew->bDead) return;
                // A drone switched off stands where its owner's does: FTL would walk it to the nearest free slot on its
                // own (not for puppets, DuelsHooks.cpp), and the two drones stand a few pixels apart when it happens.
                if (puppet.roster.droneSlot >= 0 && crew->bFrozen && s.room >= 0 && crew->iRoomId != s.room)
                {
                    crew->SetRoom(s.room);
                }

                // A mind control's boost (its levels 2 and 3 raise the health above the maximum, roadmap 3.8): the puppet
                // has the owner's higher maximum while it is controlled, and its own back after.
                bool controlled = (s.flags & FLAG_MIND_CONTROLLED) != 0;
                if ((controlled || crew->bMindControlled) && (float)s.health > crew->health.second)
                {
                    if (puppet.normalMax <= 0.f) puppet.normalMax = crew->health.second;
                    crew->health.second = (float)s.health;
                }
                else if (!controlled && !crew->bMindControlled && puppet.normalMax > 0.f)
                {
                    crew->health.second = puppet.normalMax;
                    puppet.normalMax = 0.f;
                }
                crew->health.first = std::min((float)s.health, crew->health.second);
                // The owner's mind control, when it changes there: our own mind control takes and releases puppets here
                // first (DuelsMind.cpp), and the owner's older states must not undo that.
                if (controlled != puppet.ownerControlled)
                {
                    puppet.ownerControlled = controlled;
                    if (crew->bMindControlled != controlled) crew->SetMindControl(controlled);
                }
                if (puppet.placeNow)
                {
                    crew->SetPosition(Point((int)s.x, (int)s.y));
                    puppet.placeNow = false;
                    puppet.movingToRoom = puppet.movingToSlot = -1;
                }
                int room, slot;
                Target(replica, s, room, slot);
                if (room >= 0 && (room != puppet.movingToRoom || slot != puppet.movingToSlot))
                {
                    crew->MoveToRoom(room, slot, true);
                    puppet.movingToRoom = room;
                    puppet.movingToSlot = slot;
                }
            }
        }

        static void ApplyStateFor(PuppetSide &side, ShipManager *replica, double localTime, bool withAway)
        {
            if (!side.havePending) return;
            side.havePending = false;
            if (!replica || !side.active) return;
            // Those the owner left out are where we can't see them (roadmap 4.5): they stand where they were last seen,
            // undrawn (FTL would draw them in a room our crew light up), and go straight to where they are when seen.
            for (std::pair<const uint16_t, Puppet> &entry : side.puppets)
            {
                bool hidden = std::none_of(side.pending.begin(), side.pending.end(),
                                           [&](const std::pair<uint16_t, Sample> &sent) { return sent.first == entry.first; });
                if (entry.second.hidden && !hidden) entry.second.placeNow = true;
                entry.second.hidden = hidden;
            }
            for (std::pair<uint16_t, Sample> &entry : side.pending)
            {
                auto found = side.puppets.find(entry.first);
                if (found == side.puppets.end()) continue;   // not in a roster yet
                entry.second.t = localTime;
                bool drone = found->second.roster.droneSlot >= 0;
                if (drone) BindDrone(replica, found->second);
                ApplySample(replica, found->second, entry.second, !drone);
            }
            if (!withAway) return;
            for (std::pair<uint16_t, Sample> &entry : g_crew.pendingAway)
            {
                auto found = g_crew.away.find(entry.first);
                if (found == g_crew.away.end()) continue;   // not (or no longer) ours aboard
                entry.second.t = localTime;
                ApplySample(replica, found->second, entry.second, false);
            }
        }

        void ApplyState(double localTime)
        {
            ApplyStateFor(g_sides[1], G_->GetShipManager(1), localTime, true);
        }

        bool ReplayOwnState(Reader &r, double localTime)
        {
            // The recorder's crew on its ship, and its guests (the opponent's boarders there) as its game had them.
            std::vector<std::pair<uint16_t, Sample>> samples, guests;
            ReadEntries(r, samples);
            ReadEntries(r, guests);
            if (!r.Ok()) return false;
            ShipManager *own = G_->GetShipManager(0);
            g_sides[0].pending.swap(samples);
            g_sides[0].havePending = true;
            ApplyStateFor(g_sides[0], own, localTime, false);
            for (std::pair<uint16_t, Sample> &entry : guests)
            {
                auto found = g_sides[0].guests.find(entry.first);
                if (found == g_sides[0].guests.end() || !own) continue;   // aboard before the replay knew of them
                entry.second.t = localTime;
                ApplySample(own, found->second, entry.second, false);
            }
            return true;
        }

        void AfterReplicaLoop(ShipManager *replica)
        {
            if (!replica || (replica->iShipId != 0 && replica->iShipId != 1) || replica != G_->GetShipManager(replica->iShipId)) return;
            PuppetSide &side = SideOf(replica);
            if (!side.active) return;
            bool theirs = replica->iShipId == 1;
            double now = WallMs();
            side.puppetAnimations.clear();
            std::vector<Puppet*> all;
            for (std::pair<const uint16_t, Puppet> &entry : side.puppets) all.push_back(&entry.second);
            for (std::pair<const uint16_t, Puppet> &entry : side.guests) all.push_back(&entry.second);
            for (std::pair<const uint16_t, Puppet> &entry : g_crew.away)
            {
                if (!theirs) break;
                if (entry.second.crew && !LiveCrew(replica, entry.second)) Log("Crew: our crew member %u left the replica", (unsigned)entry.first);
                all.push_back(&entry.second);
            }
            for (Puppet *each : all)
            {
                Puppet &puppet = *each;
                CrewMember *crew = LiveCrew(replica, puppet);
                if (!crew || crew->bDead || !puppet.haveSample) continue;
                const Sample &s = puppet.sample;
                // What they are doing shows as their owner's crew member shows it (the next frame's animation).
                if (crew->crewAnim)
                {
                    side.puppetAnimations[crew->crewAnim] = s.flags & ANIMATION_FLAGS;
                    // FTL sets typing again from the replica's own manning after the animation update: for the frame
                    // drawn now it is the owner's.
                    crew->crewAnim->bTyping = (s.flags & FLAG_TYPING) != 0;
                }
                // Their health is their owner's, whatever happened here.
                crew->health.first = std::min((float)s.health, crew->health.second);
                // Too far off for too long (a door closed on one side only, a lost update): put them there.
                float dx = crew->x - s.x, dy = crew->y - s.y;
                if (dx * dx + dy * dy <= PLACE_DISTANCE * PLACE_DISTANCE)
                {
                    puppet.farSinceMs = -1.0;
                    continue;
                }
                if (puppet.farSinceMs < 0.0)
                {
                    puppet.farSinceMs = now;
                    continue;
                }
                if (now - puppet.farSinceMs < PLACE_AFTER_MS) continue;
                crew->SetPosition(Point((int)s.x, (int)s.y));
                puppet.farSinceMs = -1.0;
                puppet.movingToRoom = puppet.movingToSlot = -1;
                int room, slot;
                Target(replica, s, room, slot);
                if (room >= 0)
                {
                    crew->MoveToRoom(room, slot, true);
                    puppet.movingToRoom = room;
                    puppet.movingToSlot = slot;
                }
                ++side.placed;
            }
        }

        enum class Describing { HEALTH, ROOMS, ANIMATIONS };

        static std::string Describe(ShipManager *ship, Describing what)
        {
            // Ours by our ids; the replica's by the owner's ids (the puppets').
            // Guests (the other ship's crew aboard) by their owner's ids, marked "g", after the ship's own.
            std::vector<std::pair<uint16_t, CrewMember*>> list, guests;
            if (ship && ship == G_->GetShipManager(0) && g_sides[0].active)
            {
                // A replay's ship 0: the recorder's crew, its puppets by its ids (roadmap 5.1), and its guests.
                for (std::pair<const uint16_t, Puppet> &entry : g_sides[0].puppets)
                {
                    CrewMember *crew = LiveCrew(ship, entry.second);
                    if (crew) list.push_back(std::make_pair(entry.first, crew));
                }
                for (std::pair<const uint16_t, Puppet> &entry : g_sides[0].guests)
                {
                    CrewMember *crew = LiveCrew(ship, entry.second);
                    if (crew) guests.push_back(std::make_pair(entry.first, crew));
                }
            }
            else if (ship && ship == G_->GetShipManager(0))
            {
                for (const std::pair<const CrewMember*, uint16_t> &entry : g_crew.ownIds) list.push_back(std::make_pair(entry.second, const_cast<CrewMember*>(entry.first)));
                for (const std::pair<const uint16_t, CrewMember*> &entry : g_crew.guests)
                {
                    CrewMember *crew = LiveGuest(entry.first);
                    if (crew) guests.push_back(std::make_pair(entry.first, crew));
                }
            }
            else if (ship)
            {
                for (std::pair<const uint16_t, Puppet> &entry : g_sides[1].puppets)
                {
                    CrewMember *crew = LiveCrew(ship, entry.second);
                    if (crew) list.push_back(std::make_pair(entry.first, crew));
                }
                for (std::pair<const uint16_t, Puppet> &entry : g_crew.away)
                {
                    CrewMember *crew = LiveCrew(ship, entry.second);
                    if (crew) guests.push_back(std::make_pair(entry.first, crew));
                }
            }
            size_t ownCount = list.size();
            list.insert(list.end(), guests.begin(), guests.end());
            auto byId = [](const std::pair<uint16_t, CrewMember*> &a, const std::pair<uint16_t, CrewMember*> &b) { return a.first < b.first; };
            std::sort(list.begin(), list.begin() + ownCount, byId);
            std::sort(list.begin() + ownCount, list.end(), byId);
            std::ostringstream out;
            for (size_t i = 0; i < list.size(); ++i)
            {
                const std::pair<uint16_t, CrewMember*> &entry = list[i];
                const CrewMember *crew = entry.second;
                if (i >= ownCount) out << 'g';
                switch (what)
                {
                    case Describing::HEALTH:
                        out << entry.first << ':' << WireHealth(crew) << (crew->bDead ? "x" : "") << ' ';
                        break;
                    case Describing::ROOMS:
                        out << entry.first << ':' << (crew->bDead ? -1 : crew->iRoomId) << ' ';
                        break;
                    case Describing::ANIMATIONS:
                        out << entry.first << ':' << (crew->bDead || !crew->crewAnim ? -1 : crew->crewAnim->status)
                            << (!crew->bDead && crew->crewAnim && crew->crewAnim->bTyping ? "t" : "") << ' ';
                        break;
                }
            }
            return out.str();
        }

        std::string Signature(ShipManager *ship)
        {
            return Describe(ship, Describing::HEALTH);
        }

        std::string RoomSignature(ShipManager *ship)
        {
            return Describe(ship, Describing::ROOMS);
        }

        std::string AnimationSignature(ShipManager *ship)
        {
            return Describe(ship, Describing::ANIMATIONS);
        }

        std::string Status()
        {
            std::ostringstream out;
            const PuppetSide &side = g_sides[1];
            out << "crew: rosters " << side.rostersApplied << ", puppets " << side.puppets.size() << ", made "
                << side.created << ", put in place " << side.placed << ", boarded " << g_crew.boarded << " (away "
                << g_crew.away.size() << "), guests made " << g_crew.guestsMade << " (aboard " << g_crew.guests.size() << ")";
            if (g_sides[0].active) out << "; our ship's (a replay) " << g_sides[0].puppets.size() << " puppets, " << g_sides[0].guests.size() << " boarders";
            return out.str();
        }

        int AwayId(const CrewMember *crew)
        {
            for (const std::pair<const uint16_t, Puppet> &entry : g_crew.away)
            {
                if (crew && entry.second.crew == crew) return entry.first;
            }
            return -1;
        }

        bool IsPuppet(const CrewMember *crew)
        {
            return crew && (IsPuppetCrew(crew) || (g_sides[1].active && AwayId(crew) >= 0));
        }

        bool IsHidden(const CrewMember *crew)
        {
            if (!crew) return false;
            for (const PuppetSide &side : g_sides)
            {
                if (!side.active) continue;
                for (const std::pair<const uint16_t, Puppet> &entry : side.puppets)
                {
                    if (entry.second.crew == crew) return entry.second.hidden;
                }
            }
            return false;
        }

        CrewMember *PuppetById(uint16_t id)
        {
            auto found = g_sides[1].puppets.find(id);
            ShipManager *replica = G_->GetShipManager(1);
            return found == g_sides[1].puppets.end() || !replica ? nullptr : LiveCrew(replica, found->second);
        }

        int BoardAway(CrewMember *crew)
        {
            // Its id from the last state (or the one before, if a state was sent while it teleported).
            int id = OwnId(crew);
            if (id < 0)
            {
                auto prev = g_crew.prevIds.find(crew);
                auto held = g_crew.heldIds.find(crew);
                id = prev != g_crew.prevIds.end() ? prev->second : held != g_crew.heldIds.end() ? held->second : -1;
            }
            if (id < 0) return -1;
            Puppet &puppet = g_crew.away[(uint16_t)id];
            puppet = Puppet();
            puppet.crew = crew;
            puppet.roster.id = (uint16_t)id;
            puppet.roster.species = crew->species;
            puppet.roster.name = crew->GetName();
            puppet.roster.male = crew->blueprint.male;
            puppet.placeNow = false;   // FTL's teleport put them there
            g_crew.heldIds[crew] = (uint16_t)id;
            g_crew.ownIds.erase(crew);
            ++g_crew.boarded;
            return id;
        }

        void CameHome(uint16_t id)
        {
            g_crew.away.erase(id);
        }

        uint16_t NewRobotId()
        {
            return g_crew.nextId++;
        }

        // A boarder made aboard a ship (a robot by its pod, a crew member by the teleport): the puppet for its id.
        static void BoarderPuppet(Puppet &puppet, uint16_t id, CrewMember *crew)
        {
            puppet = Puppet();
            puppet.crew = crew;
            puppet.roster.id = id;
            puppet.roster.species = crew->species;
            puppet.roster.name = crew->GetName();
            puppet.roster.male = crew->blueprint.male;
            puppet.placeNow = false;   // its pod (the teleport) put it there
        }

        void RobotAway(CrewMember *robot, uint16_t id)
        {
            BoarderPuppet(g_crew.away[id], id, robot);
            ++g_crew.boarded;
        }

        void ReplayAwayAboard(uint16_t id, CrewMember *crew)
        {
            BoarderPuppet(g_crew.away[id], id, crew);
            ++g_crew.boarded;
        }

        void ReplayAwayGone(uint16_t id)
        {
            auto found = g_crew.away.find(id);
            if (found == g_crew.away.end()) return;
            ShipManager *replica = G_->GetShipManager(1);
            CrewMember *crew = LiveCrew(replica, found->second);
            if (crew && !crew->bDead) replica->RemoveCrewmember(crew);
            g_crew.away.erase(found);
        }

        void ReplayGuestAboard(uint16_t id, CrewMember *crew)
        {
            BoarderPuppet(g_sides[0].guests[id], id, crew);
            ++g_crew.guestsMade;
        }

        CrewMember *ReplayGuest(uint16_t id)
        {
            auto found = g_sides[0].guests.find(id);
            return found == g_sides[0].guests.end() ? nullptr : LiveCrew(G_->GetShipManager(0), found->second);
        }

        void AddGuest(uint16_t id, CrewMember *crew)
        {
            g_crew.guests[id] = crew;
            ++g_crew.guestsMade;
        }

        CrewMember *Guest(uint16_t id)
        {
            return LiveGuest(id);
        }

        int GuestIdOf(const CrewMember *crew)
        {
            for (const std::pair<const uint16_t, CrewMember*> &entry : g_crew.guests)
            {
                if (crew && entry.second == crew) return entry.first;
            }
            return -1;
        }

        bool IsGuest(const CrewMember *crew)
        {
            for (const std::pair<const uint16_t, CrewMember*> &entry : g_crew.guests)
            {
                if (crew && entry.second == crew) return true;
            }
            return false;
        }

        void RemoveGuest(uint16_t id)
        {
            g_crew.guests.erase(id);
            g_sides[0].guests.erase(id);   // a replay's
        }

        std::vector<std::pair<uint16_t, CrewMember*>> AwayCrew()
        {
            // Only those still aboard the replica (FTL deletes the dead once their death is over).
            std::vector<std::pair<uint16_t, CrewMember*>> crew;
            ShipManager *replica = G_->GetShipManager(1);
            if (!replica) return crew;
            for (const std::pair<const uint16_t, Puppet> &entry : g_crew.away)
            {
                const std::vector<CrewMember*> &list = replica->vCrewList;
                if (entry.second.crew && std::find(list.begin(), list.end(), entry.second.crew) != list.end())
                {
                    crew.push_back(std::make_pair(entry.first, entry.second.crew));
                }
            }
            return crew;
        }

        std::vector<std::pair<uint16_t, CrewMember*>> Guests()
        {
            // Only those still aboard (FTL deletes the dead once their death is over).
            std::vector<std::pair<uint16_t, CrewMember*>> crew;
            for (const std::pair<const uint16_t, CrewMember*> &entry : g_crew.guests)
            {
                if (CrewMember *guest = LiveGuest(entry.first)) crew.push_back(std::make_pair(entry.first, guest));
            }
            return crew;
        }

        void AdoptPuppet(uint16_t id, CrewMember *crew)
        {
            g_crew.guests.erase(id);
            g_sides[0].guests.erase(id);   // a replay's
            Puppet &puppet = g_sides[1].puppets[id];
            puppet = Puppet();
            puppet.crew = crew;
            puppet.roster.id = id;
            puppet.roster.species = crew->species;
            puppet.roster.name = crew->GetName();
            puppet.roster.male = crew->blueprint.male;
            puppet.placeNow = false;
        }

        int PuppetId(const CrewMember *crew)
        {
            if (!g_sides[1].active || !crew) return -1;
            for (const std::pair<const uint16_t, Puppet> &entry : g_sides[1].puppets)
            {
                if (entry.second.crew == crew) return entry.first;
            }
            return -1;
        }

        int OwnId(const CrewMember *crew)
        {
            auto found = g_crew.ownIds.find(crew);
            return found != g_crew.ownIds.end() ? found->second : -1;
        }

        CrewMember *OwnById(uint16_t id)
        {
            ShipManager *own = G_->GetShipManager(0);
            for (const std::pair<const CrewMember*, uint16_t> &entry : g_crew.ownIds)
            {
                if (entry.second != id || !own) continue;
                // Still one of ours (the map is rebuilt with each state).
                for (CrewMember *crew : own->vCrewList)
                {
                    if (crew == entry.first) return crew;
                }
            }
            return nullptr;
        }

        bool MayRepair(const ShipSystem *system)
        {
            int id = system ? system->_shipObj.iShipId : -1;
            return !((id == 0 || id == 1) && g_sides[id].active);
        }

        void Animate(CrewAnimation *anim, bool &fighting, bool &repairing, bool &onFire)
        {
            if (!anim) return;
            for (PuppetSide &side : g_sides)
            {
                auto puppet = side.puppetAnimations.find(anim);
                if (puppet != side.puppetAnimations.end())
                {
                    fighting = (puppet->second & FLAG_FIGHTING) != 0;
                    repairing = (puppet->second & FLAG_REPAIRING) != 0;
                    onFire = (puppet->second & FLAG_IN_FIRE) != 0;
                    // At a console the replica's own manning would decide (it differs while the owner's repairs).
                    anim->bTyping = (puppet->second & FLAG_TYPING) != 0;
                    return;
                }
            }
            // Ours, in a duel (the ids are made with the first state). Animations of crew that are gone stay in the
            // map until it is cleared; they are only compared with live crew's.
            if (g_crew.ownIds.empty()) return;
            if (g_crew.ownAnimations.size() > 64) g_crew.ownAnimations.clear();
            g_crew.ownAnimations[anim] = (fighting ? FLAG_FIGHTING : 0) | (repairing ? FLAG_REPAIRING : 0) |
                                         (onFire ? FLAG_IN_FIRE : 0) | (anim->bTyping ? FLAG_TYPING : 0);
        }
    }
}
