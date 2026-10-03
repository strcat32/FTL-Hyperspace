#include "Global.h"
#include "CustomShipSelect.h"
#include "CustomStore.h"
#include "Duels.h"
#include "DuelsBays.h"
#include "DuelsConsole.h"
#include "DuelsCrew.h"
#include "DuelsRefit.h"
#include "DuelsRounds.h"
#include "DuelsTrace.h"
#include "DuelsTune.h"
#include "DuelsWire.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <vector>

namespace Duels
{
    namespace Refit
    {
        // A crew member as they were, to bring them back with the same name, race, skills and look.
        struct Member
        {
            CrewBlueprint blueprint;
            std::string species;
            std::string name;
            Slot station;
        };

        struct RefitState
        {
            bool haveCaptain = false;
            Member captain;                 // the first crew member at the start of the match: always returns
            std::vector<Member> crew;       // everyone as the last fight began (Permanent Death off: all return)
            std::vector<bool> weaponsPowered;   // by slot, as the last fight began (powered again in the preparation)
            std::map<int, int> systemsPowered;  // the other systems' power as the last fight began (an ion storm, a hit
                                                // or FTL's power loss took some; powered again in the preparation)
            uint32_t revived = 0, shops = 0;
            // Levels as the match began: a level taken back stops there (roadmap V).
            std::map<int, int> startLevels;
            int startReactor = 0;
            int saleArmed = -1;             // the system whose sale waits for a second right-click
            double saleArmedUntil = 0.0;
            int saleDue = -1, saleDuePrice = 0;   // sold at the second right-click, made at the screen's next loop
            bool tipShown = false;
            // The round's shop as the host stocked it, for building it again with the buy-back page (BJ).
            std::vector<ShopItem> stock;
            int shopRound = 0;
            bool buyBackStale = false;      // a system was sold: the store is built again with it
            std::set<int> buyBackOffered;   // the systems on the buy-back page now
        };

        static RefitState g_refit;
        static Store *g_store = nullptr;   // the round's store, while it is open (its description's place, BY)
        static const char *const STORE_ID = "FTL_DUELS_ROUND";
        static const int PAGE_ITEMS = 6;     // a kind's page in the shop: two sections of three (AP)
        static const int STORE_LOWER = 20;   // px below FTL's place (AQ)
        // FTL's place for the store's tabbed window, taken before the first move (the window keeps ours afterwards).
        static bool g_storePlaced = false;
        static int g_ftlStoreX = 0, g_ftlStoreY = 0;
        static bool g_shipScreensPlaced = false;
        static int g_ftlShipScreensX = 0, g_ftlShipScreensY = 0;

        static CommandGui *Gui()
        {
            CApp *app = G_->GetCApp();
            return app ? app->gui : nullptr;
        }

        // Our crew aboard our own ship: not the opponent's boarders (guests), not drones.
        static std::vector<CrewMember*> CrewAboard(ShipManager *ship)
        {
            std::vector<CrewMember*> crew;
            if (!ship) return crew;
            for (CrewMember *member : ship->vCrewList)
            {
                if (member && !member->IsDrone() && !Crew::IsGuest(member) && member->iShipId == ship->iShipId) crew.push_back(member);
            }
            return crew;
        }

        static Member Remember(CrewMember *crew)
        {
            Member member;
            member.blueprint = crew->blueprint;
            member.species = crew->species;
            member.name = crew->GetName();
            member.station = crew->savedPosition;
            return member;
        }

        static bool Alive(const CrewMember *crew)
        {
            return crew && !crew->bDead && !crew->bOutOfGame && crew->health.first > 0.f;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The shop's stock (the host picks it)
        // ---------------------------------------------------------------------------------------------------------

        // Augments that do nothing in a duel (rules, section 7): no star map, no events, no scrap collected. (The FTL
        // Recharge Booster charges the drive for running away, roadmap AD: it is sold again.)
        static const char *const USELESS_AUGMENTS[] = {"FTL_JAMMER", "FTL_JUMPER", "FLEET_DISTRACTION",
                                                        "ADV_SCANNERS", "STASIS_POD", "SCRAP_COLLECTOR", "REPAIR_ARM",
                                                        "DRONE_RECOVERY"};
        // Systems every player ship has from the start: a shop section of them would sell nothing.
        static const char *const STANDARD_SYSTEMS[] = {"shields", "engines", "oxygen", "weapons", "pilot", "sensors", "doors"};

        struct Candidate
        {
            std::string name;
            int cost;
            int weight;
        };

        static bool Useless(const std::string &augment)
        {
            for (const char *name : USELESS_AUGMENTS)
            {
                if (augment == name) return true;
            }
            return false;
        }

        // Systems a round's shop may sell: not those every ship has, not the weapon and drone bays (the duel's own),
        // and not drone control, whose store box comes with a random free drone (the two games' shops would differ).
        static bool Sellable(const std::string &system)
        {
            for (const char *name : STANDARD_SYSTEMS)
            {
                if (system == name) return false;
            }
            return system.compare(0, 11, "weapon_bay_") != 0 && system.compare(0, 10, "drone_bay_") != 0 && system != "drones";
        }

        template <class Map>
        static std::vector<Candidate> Pool(const Map &blueprints, int cap, uint8_t kind)
        {
            std::vector<Candidate> pool;
            for (const auto &entry : blueprints)
            {
                const Description &desc = entry.second.desc;
                if (desc.rarity <= 0 || desc.rarity > 5) continue;
                if (cap > 0 && desc.cost > cap) continue;
                if (kind == KIND_AUGMENT && Useless(entry.first)) continue;
                if (kind == KIND_SYSTEM && !Sellable(entry.first)) continue;
                pool.push_back(Candidate{entry.first, desc.cost, 6 - desc.rarity});
            }
            return pool;
        }

        // As FTL's stores draw: weight 6 - rarity, no item twice.
        static std::vector<Candidate> Draw(std::vector<Candidate> pool, int count, std::mt19937 &random)
        {
            std::vector<Candidate> chosen;
            while ((int)chosen.size() < count && !pool.empty())
            {
                int total = 0;
                for (const Candidate &candidate : pool) total += candidate.weight;
                int pick = std::uniform_int_distribution<int>(0, std::max(0, total - 1))(random);
                size_t index = 0;
                while (index + 1 < pool.size() && pick >= pool[index].weight)
                {
                    pick -= pool[index].weight;
                    ++index;
                }
                chosen.push_back(pool[index]);
                pool.erase(pool.begin() + index);
            }
            return chosen;
        }

        std::vector<ShopItem> MakeStock(int round, std::mt19937 &random)
        {
            std::vector<ShopItem> stock;
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!blueprints) return stock;
            // The round's price caps (rules, section 7: the tiers; fine settings, roadmap BE): weapons and drones 55, 65,
            // 75, 85, then none; augments 50, 60, 80, 100 and systems 60, 80, 90, 150 (roadmap CP: a 120-scrap Weapon
            // Pre-Igniter was for sale in round 1).
            auto capOf = [round](const char *setting) -> int {
                std::vector<double> caps = Tune::Numbers(setting);
                return round >= 1 && round <= (int)caps.size() ? (int)caps[round - 1] : 0;
            };
            int cap = capOf("shop.price_caps"), augmentCap = capOf("shop.augment_caps"), systemCap = capOf("shop.system_caps");
            // Every kind every round (those shop.kinds names), each on a page of its own (AP), in this order.
            static const uint8_t KINDS[] = {KIND_WEAPON, KIND_DRONE, KIND_AUGMENT, KIND_SYSTEM, KIND_CREW};
            static const char *const KIND_WORDS[] = {"weapons", "drones", "augments", "systems", "crew"};

            for (size_t k = 0; k < sizeof(KINDS) / sizeof(KINDS[0]); ++k)
            {
                const uint8_t kind = KINDS[k];
                if (!Tune::HasWord("shop.kinds", KIND_WORDS[k])) continue;
                std::vector<Candidate> pool;
                switch (kind)
                {
                case KIND_WEAPON: pool = Pool(blueprints->weaponBlueprints, cap, kind); break;
                case KIND_DRONE: pool = Pool(blueprints->droneBlueprints, cap, kind); break;
                case KIND_AUGMENT: pool = Pool(blueprints->augmentBlueprints, augmentCap, kind); break;
                case KIND_SYSTEM: pool = Pool(blueprints->systemBlueprints, systemCap, kind); break;
                case KIND_CREW: pool = Pool(blueprints->crewBlueprints, 0, kind); break;
                default: break;
                }
                // Blueprints the shop never sells (shop.exclude).
                pool.erase(std::remove_if(pool.begin(), pool.end(), [](const Candidate &candidate) { return Tune::HasWord("shop.exclude", candidate.name); }),
                           pool.end());
                for (const Candidate &candidate : Draw(pool, PAGE_ITEMS, random))
                {
                    ShopItem item;
                    item.kind = kind;
                    item.blueprint = candidate.name;
                    item.price = (uint16_t)std::max(0, candidate.cost);
                    stock.push_back(item);
                }
            }
            // Missiles and drone parts don't come back between rounds (rules, section 7): they are sold here, at FTL's
            // prices.
            ShopItem missiles;
            missiles.kind = KIND_MISSILES;
            missiles.count = (uint8_t)Tune::Number("shop.missiles");
            if (missiles.count > 0) stock.push_back(missiles);
            ShopItem parts;
            parts.kind = KIND_DRONE_PARTS;
            parts.count = (uint8_t)Tune::Number("shop.drone_parts");
            if (parts.count > 0) stock.push_back(parts);
            std::string listed;
            for (const ShopItem &item : stock)
            {
                if (!item.blueprint.empty()) listed += " " + item.blueprint + " " + std::to_string(item.price);
            }
            Log("Refit: round %d's stock (caps: weapons and drones %d, augments %d, systems %d; 0 none):%s", round, cap, augmentCap,
                systemCap, listed.c_str());
            return stock;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The match, the fights, the crew
        // ---------------------------------------------------------------------------------------------------------

        void OnMatchStart()
        {
            g_refit = RefitState();
            ShipManager *own = G_->GetShipManager(0);
            if (own)
            {
                for (ShipSystem *system : own->vSystemList)
                {
                    if (system && system->iSystemType >= 0 && system->iSystemType < SYS_ALL)
                        g_refit.startLevels[system->iSystemType] = system->powerState.second;
                }
                g_refit.startReactor = PowerManager::GetPowerManager(0)->currentPower.second;
            }
            std::vector<CrewMember*> crew = CrewAboard(own);
            for (CrewMember *member : crew) g_refit.crew.push_back(Remember(member));
            if (!crew.empty())
            {
                g_refit.haveCaptain = true;
                g_refit.captain = g_refit.crew.front();
                Log("Refit: the captain is %s (%s); %u crew at the start", g_refit.captain.name.c_str(),
                    g_refit.captain.species.c_str(), (unsigned)crew.size());
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // Back after a crash (roadmap BR)
        // ---------------------------------------------------------------------------------------------------------

        static const int SKILLS = 6;   // piloting, engines, shields, weapons, repair, combat
        static const uint8_t REJOIN_FORMAT = 1;

        static void WriteMember(Writer &w, const Member &member)
        {
            w.Str(member.species);
            w.Str(member.name);
            w.Bool(member.blueprint.male);
            for (int skill = 0; skill < SKILLS; ++skill)
            {
                bool known = skill < (int)member.blueprint.skillLevel.size();
                w.U8((uint8_t)std::max(0, known ? member.blueprint.skillLevel[skill].first : 0));
                w.U8((uint8_t)std::max(0, known ? member.blueprint.skillLevel[skill].second : 0));
            }
            w.I16((int16_t)member.station.roomId);
            w.I16((int16_t)member.station.slotId);
        }

        // A member as the file has them: their race's blueprint with their name, sex and skills (the look is the race's
        // first).
        static Member ReadMember(Reader &r)
        {
            Member member;
            member.species = r.Str();
            member.name = r.Str();
            bool male = r.Bool();
            int skills[SKILLS][2];
            for (int skill = 0; skill < SKILLS; ++skill)
            {
                skills[skill][0] = r.U8();
                skills[skill][1] = r.U8();
            }
            member.station.roomId = r.I16();
            member.station.slotId = r.I16();
            if (!r.Ok()) return member;
            if (BlueprintManager *blueprints = G_->GetBlueprints()) member.blueprint = blueprints->GetCrewBlueprint(member.species);
            member.blueprint.name = member.species;
            member.blueprint.crewName = TextString(member.name, true);
            member.blueprint.crewNameLong = TextString(member.name, true);
            member.blueprint.male = male;
            for (int skill = 0; skill < SKILLS && skill < (int)member.blueprint.skillLevel.size(); ++skill)
            {
                member.blueprint.skillLevel[skill].first = skills[skill][0];
                member.blueprint.skillLevel[skill].second = skills[skill][1];
            }
            if (member.station.roomId >= 0 && member.station.slotId >= 0)
            {
                if (ShipGraph *graph = ShipGraph::GetShipInfo(0)) member.station.worldLocation = graph->GetSlotWorldPosition(member.station.slotId, member.station.roomId);
            }
            return member;
        }

        void WriteRejoin(Writer &w)
        {
            const RefitState &s = g_refit;
            w.U8(REJOIN_FORMAT);
            w.U8((uint8_t)std::min<size_t>(s.startLevels.size(), 255));
            for (const std::pair<const int, int> &level : s.startLevels)
            {
                w.U8((uint8_t)level.first);
                w.U8((uint8_t)level.second);
            }
            w.U8((uint8_t)s.startReactor);
            w.Bool(s.haveCaptain);
            if (s.haveCaptain) WriteMember(w, s.captain);
            size_t crew = std::min<size_t>(s.crew.size(), 255);
            w.U8((uint8_t)crew);
            for (size_t i = 0; i < crew; ++i) WriteMember(w, s.crew[i]);
            size_t weapons = std::min<size_t>(s.weaponsPowered.size(), 255);
            w.U8((uint8_t)weapons);
            for (size_t i = 0; i < weapons; ++i) w.Bool(s.weaponsPowered[i]);
            w.U8((uint8_t)std::min<size_t>(s.systemsPowered.size(), 255));
            for (const std::pair<const int, int> &system : s.systemsPowered)
            {
                w.U8((uint8_t)system.first);
                w.U8((uint8_t)system.second);
            }
        }

        void ColdStart(const std::vector<uint8_t> &saved)
        {
            g_refit = RefitState();
            Reader r(saved);
            RefitState s;
            if (r.U8() != REJOIN_FORMAT)
            {
                Log("Refit: back after a crash: the file's part is of another format; as a new match");
                OnMatchStart();
                return;
            }
            int levels = r.U8();
            for (int i = 0; i < levels; ++i)
            {
                int id = r.U8();
                s.startLevels[id] = r.U8();
            }
            s.startReactor = r.U8();
            s.haveCaptain = r.Bool();
            if (s.haveCaptain) s.captain = ReadMember(r);
            s.crew.resize(r.U8());
            for (Member &member : s.crew) member = ReadMember(r);
            s.weaponsPowered.resize(r.U8());
            for (size_t i = 0; i < s.weaponsPowered.size(); ++i) s.weaponsPowered[i] = r.Bool();
            int systems = r.U8();
            for (int i = 0; i < systems; ++i)
            {
                int id = r.U8();
                s.systemsPowered[id] = r.U8();
            }
            if (!r.Ok())
            {
                Log("Refit: back after a crash: the file's part doesn't read; as a new match");
                OnMatchStart();
                return;
            }
            g_refit = s;
            Log("Refit: back after a crash: the captain %s (%s), %u crew as the last fight began, %u systems' levels as the match began",
                s.haveCaptain ? s.captain.name.c_str() : "-", s.haveCaptain ? s.captain.species.c_str() : "-", (unsigned)s.crew.size(),
                (unsigned)s.startLevels.size());
        }

        void OnFightStart()
        {
            std::vector<Member> crew;
            ShipManager *own = G_->GetShipManager(0);
            for (CrewMember *member : CrewAboard(own)) crew.push_back(Remember(member));
            g_refit.crew.swap(crew);
            g_refit.weaponsPowered.clear();
            if (own && own->weaponSystem)
            {
                for (ProjectileFactory *weapon : own->GetWeaponList()) g_refit.weaponsPowered.push_back(weapon->powered);
            }
            g_refit.systemsPowered.clear();
            if (own)
            {
                for (ShipSystem *system : own->vSystemList)
                {
                    int id = system ? system->iSystemType : -1;
                    if (id < 0 || id >= SYS_ALL || id == SYS_WEAPONS || id == SYS_DRONES || !system->bNeedsPower) continue;
                    g_refit.systemsPowered[id] = own->GetSystemPower(id);
                }
            }
            // The captain as they are now (their skills grow).
            for (const Member &member : g_refit.crew)
            {
                if (g_refit.haveCaptain && member.name == g_refit.captain.name && member.species == g_refit.captain.species)
                {
                    g_refit.captain = member;
                }
            }
        }

        bool CrewGone(ShipManager *ship)
        {
            if (!ship) return false;
            for (CrewMember *crew : CrewAboard(ship))
            {
                if (Alive(crew)) return false;
            }
            for (const std::pair<uint16_t, CrewMember*> &entry : Crew::AwayCrew())
            {
                if (Alive(entry.second)) return false;
            }
            // A match against the AI (roadmap 3.6) has no crew registry: FTL's own ship ids tell ours aboard its ship.
            ShipManager *other = G_->GetShipManager(1 - ship->iShipId);
            if (Rounds::IsLocal() && other)
            {
                for (CrewMember *crew : other->vCrewList)
                {
                    if (crew && !crew->IsDrone() && crew->iShipId == ship->iShipId && Alive(crew)) return false;
                }
            }
            // Clones on the way count as alive (rules, section 3).
            CrewMemberFactory *factory = G_->GetCrewFactory();
            return !factory || factory->CountCloneReadyCrew(true) == 0;
        }

        void EndOfRound(bool ownDestroyed, bool theirsDestroyed)
        {
            ShipManager *own = G_->GetShipManager(0);
            ShipManager *replica = G_->GetShipManager(1);
            if (!own) return;

            // Mind control ends: ours lets go of the crew it holds, and theirs of ours (its replica's system).
            if (own->mindSystem) own->mindSystem->ReleaseCrew();
            if (replica && replica->mindSystem) replica->mindSystem->ReleaseCrew();
            for (ShipManager *ship : {own, replica})
            {
                if (!ship) continue;
                for (CrewMember *crew : ship->vCrewList)
                {
                    if (crew && crew->bMindControlled) crew->SetMindControl(false);
                }
            }

            // Nobody fights across the end of the round.
            for (ShipManager *ship : {own, replica})
            {
                if (!ship) continue;
                for (CrewMember *crew : ship->vCrewList)
                {
                    if (!crew) continue;
                    crew->crewTarget = nullptr;
                    crew->bFighting = false;
                }
            }
            if (CommandGui *gui = Gui())
            {
                gui->crewControl.selectedCrew.clear();
                gui->crewControl.potentialSelectedCrew.clear();
            }

            // Ours aboard their ship: home, unless that ship was destroyed (they die with it, as in FTL).
            int home = 0, lost = 0;
            for (const std::pair<uint16_t, CrewMember*> &entry : Crew::AwayCrew())
            {
                CrewMember *crew = entry.second;
                if (!replica) break;
                bool alive = Alive(crew);
                if (!alive || theirsDestroyed)
                {
                    if (alive)
                    {
                        crew->health.first = 0.f;
                        crew->Kill(true);
                    }
                    ++lost;
                    continue;
                }
                crew->EmptySlot();
                replica->vCrewList.erase(std::remove(replica->vCrewList.begin(), replica->vCrewList.end(), crew),
                                         replica->vCrewList.end());
                crew->SetCurrentShip(0);
                int room = own->GetSystemRoom(SYS_TELEPORTER);
                own->AddCrewMember(crew, room >= 0 ? room : 0);
                Crew::CameHome(entry.first);
                ++home;
            }

            // Theirs aboard ours: they leave as FTL's teleport takes crew (onto the replica, which goes with them at the
            // next preparation). Their own game decides whether they live on; aboard our destroyed ship they die.
            int left = 0;
            for (const std::pair<uint16_t, CrewMember*> &entry : Crew::Guests())
            {
                CrewMember *crew = entry.second;
                if (ownDestroyed && !crew->bDead)
                {
                    crew->health.first = 0.f;
                    crew->Kill(true);
                }
                if (replica && !crew->bDead)
                {
                    crew->EmptySlot();
                    own->vCrewList.erase(std::remove(own->vCrewList.begin(), own->vCrewList.end(), crew), own->vCrewList.end());
                    crew->SetCurrentShip(1);
                    replica->AddCrewMember(crew, 0);
                }
                Crew::RemoveGuest(entry.first);
                ++left;
            }
            Log("Refit: the round is over; %d of ours came home, %d died aboard their ship, %d of theirs left ours", home, lost, left);
        }

        // Brings a crew member back as they were: a new crew member with the same blueprint (Hyperspace takes the
        // race from its name); the dead one's clone stays in the clone bay no longer.
        static void Revive(ShipManager *own, const Member &member)
        {
            CrewMemberFactory *factory = G_->GetCrewFactory();
            if (factory)
            {
                for (CrewMember *dead : factory->crewMembers)
                {
                    if (dead && dead->iShipId == 0 && (dead->bDead || dead->bOutOfGame) && dead->species == member.species &&
                        dead->GetName() == member.name)
                    {
                        dead->SetCloneReady(false);
                    }
                }
            }
            CrewBlueprint blueprint = member.blueprint;
            blueprint.name = member.species;
            CrewMember *crew = own->AddCrewMemberFromBlueprint(&blueprint, -1, false, -1, false);
            if (!crew)
            {
                Log("Refit: %s (%s) could not come back", member.name.c_str(), member.species.c_str());
                return;
            }
            crew->health.first = crew->health.second;
            if (member.station.roomId >= 0) crew->SetSavePosition(member.station);
            ++g_refit.revived;
            Log("Refit: %s (%s) is back", member.name.c_str(), member.species.c_str());
        }

        static bool IsAboard(ShipManager *own, const Member &member)
        {
            for (CrewMember *crew : CrewAboard(own))
            {
                if (Alive(crew) && crew->species == member.species && crew->GetName() == member.name) return true;
            }
            return false;
        }

        void Restore(bool permadeath)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;

            // A ship that blew up: FTL's destroyed state undone (the explosion over, the ship drawn again). Its
            // explosion switched every weapon off; the weapons powered when the fight began come back below.
            bool destroyed = own->bDestroyed || own->ship.bDestroyed;
            if (destroyed)
            {
                own->bDestroyed = false;
                own->ship.bDestroyed = false;
                own->ship.explosion.Restart();
                own->ship.explosion.running = false;
                own->ship.explosion.done = true;
                Log("Refit: the ship's destruction is undone");
            }

            // The hull, and every system whole, unlocked, unhacked and without FTL's temporary power loss. (Health is
            // written: repairs through AddDamage don't reach the weapon bays' systems, DuelsHooks.cpp.)
            own->ship.hullIntegrity.first = own->ship.hullIntegrity.second;
            own->ClearStatusAll();
            for (ShipSystem *system : own->vSystemList)
            {
                if (!system) continue;
                system->healthState.first = system->healthState.second;
                Bays::Repaired(system);
                system->fDamageOverTime = 0.f;
                system->fRepairOverTime = 0.f;
                if (system->iLockCount != 0) system->LockSystem(0);
                system->StopHacking();
            }

            // Rooms: no fire, no breach, full air, doors whole and not ioned or hacked, no crystal lockdown.
            for (std::vector<Fire> &column : own->fireSpreader.grid)
            {
                for (Fire &fire : column)
                {
                    if (fire.fDamage > 0.f) fire.Reset();
                    fire.fDamage = 0.f;
                }
            }
            std::fill(own->fireSpreader.roomCount.begin(), own->fireSpreader.roomCount.end(), 0);
            own->fireSpreader.count = 0;
            for (OuterHull *wall : own->ship.vOuterWalls)
            {
                if (wall) wall->fDamage = 0.f;
            }
            if (own->oxygenSystem)
            {
                for (float &level : own->oxygenSystem->oxygenLevels) level = 100.f;
                own->oxygenSystem->fTotalOxygen = 1.f;
            }
            own->ship.ResetDoorHealth();
            for (Door *door : own->ship.vOuterAirlocks)
            {
                if (door) door->ResetHealth();
            }
            own->ship.SetIonedDoors(false);
            for (Door *door : own->ship.vDoorList)
            {
                if (door) door->iHacked = 0;
            }
            for (LockdownShard &shard : own->ship.lockdowns) shard.bDone = true;

            // The crew: healed, awake, their own again; the dead captain back (all the dead with Permanent Death
            // off), then everyone to their stations.
            for (CrewMember *crew : CrewAboard(own))
            {
                if (!Alive(crew)) continue;
                crew->health.first = crew->health.second;
                crew->fStunTime = 0.f;
                if (crew->bMindControlled) crew->SetMindControl(false);
            }
            if (g_refit.haveCaptain && !IsAboard(own, g_refit.captain)) Revive(own, g_refit.captain);
            if (!permadeath)
            {
                for (const Member &member : g_refit.crew)
                {
                    if (!IsAboard(own, member)) Revive(own, member);
                }
            }
            own->RestoreCrewPositions();
            if (CommandGui *gui = Gui()) gui->crewControl.UpdateCrewBoxes();

            // The systems powered as when the last fight began (an ion storm halved the reactor and FTL took power
            // from some, oxygen say; a hit or the ship's explosion took more, and FTL doesn't give it back by itself).
            std::string repowered;
            for (const auto &entry : g_refit.systemsPowered)
            {
                int before = own->GetSystemPower(entry.first);
                while (own->GetSystemPower(entry.first) < entry.second && own->IncreaseSystemPower(entry.first)) continue;
                if (own->GetSystemPower(entry.first) != before)
                    repowered += (repowered.empty() ? "" : ", ") + ShipSystem::SystemIdToName(entry.first) + " " + std::to_string(own->GetSystemPower(entry.first));
            }
            if (!repowered.empty()) Log("Refit: powered again as the last fight began: %s", repowered.c_str());

            // Shields up for the new fight, and the weapons powered as when the last fight began (a hit on a bay, or the
            // ship's explosion, switched some off; FTL doesn't power them again by itself).
            own->InstantPowerShields();
            if (own->weaponSystem)
            {
                std::vector<ProjectileFactory*> weapons = own->GetWeaponList();
                for (size_t slot = 0; slot < weapons.size() && slot < g_refit.weaponsPowered.size(); ++slot)
                {
                    if (g_refit.weaponsPowered[slot] && !weapons[slot]->powered) own->PowerWeapon(weapons[slot], true, false);
                }
            }
            Log("Refit: the ship is repaired (hull %d/%d), %u crew aboard", own->ship.hullIntegrity.first,
                own->ship.hullIntegrity.second, (unsigned)CrewAboard(own).size());
        }

        void KeepAir()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own || !own->oxygenSystem) return;
            // Rooms open to space keep their own air (DH), and the rooms open to them: FTL (Hyperspace's airlock update)
            // empties such a room again at once, with its air-loss sound, so a refill every frame played that sound every
            // frame until the next fight.
            std::set<int> venting;
            for (Door *door : own->ship.vOuterAirlocks)
            {
                if (!door || !door->bOpen) continue;
                if (door->iRoom1 >= 0) venting.insert(door->iRoom1);
                if (door->iRoom2 >= 0) venting.insert(door->iRoom2);
            }
            for (bool grown = !venting.empty(); grown;)
            {
                grown = false;
                for (Door *door : own->ship.vDoorList)
                {
                    if (!door || !door->bOpen || door->iRoom1 < 0 || door->iRoom2 < 0) continue;
                    bool one = venting.count(door->iRoom1) > 0, two = venting.count(door->iRoom2) > 0;
                    if (one == two) continue;
                    venting.insert(one ? door->iRoom2 : door->iRoom1);
                    grown = true;
                }
            }
            std::vector<float> &levels = own->oxygenSystem->oxygenLevels;
            for (size_t room = 0; room < levels.size(); ++room)
            {
                if (!venting.count((int)room)) levels[room] = 100.f;
            }
            own->oxygenSystem->fTotalOxygen = 1.f;
        }

        void GiveScrap(bool firstRound, int amount)
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            // The first round's scrap replaces what the ship started with: both players start equal.
            if (firstRound) own->currentScrap = 0;
            own->ModifyScrapCount(amount, false);
            Log("Refit: %d scrap for this round, %d in all", amount, own->currentScrap);
        }

        // ---------------------------------------------------------------------------------------------------------
        // The shop
        // ---------------------------------------------------------------------------------------------------------

        static std::string SystemTitle(int id);

        // The buy-back page (roadmap BJ): the systems our ship had as the match began and has sold since (every system
        // can be sold, AK; the shop's systems page never has those every ship has, oxygen or engines say), at FTL's
        // price for the system, which comes back at level 1. Only this game's: the other player's shop is their own.
        static std::vector<StoreItem> BuyBackItems()
        {
            std::vector<StoreItem> items;
            ShipManager *own = G_->GetShipManager(0);
            BlueprintManager *blueprints = G_->GetBlueprints();
            if (!own || !blueprints) return items;
            for (const auto &entry : g_refit.startLevels)
            {
                int id = entry.first;
                if (id < 0 || id >= SYS_ALL || id == SYS_REACTOR || own->HasSystem(id)) continue;
                std::string name = ShipSystem::SystemIdToName(id);
                SystemBlueprint *blueprint = blueprints->GetSystemBlueprint(name);
                if (!blueprint || blueprint->name != name) continue;
                StoreItem item = StoreItem();
                item.blueprint = name;
                item.price.price = std::max(0, blueprint->desc.cost);
                item.stock = -1;
                items.push_back(item);
            }
            return items;
        }

        // The weapons the feed named as needing the weapons system's level (WeaponLevelHint), in this match.
        static std::set<std::string> g_levelHinted;
        static double g_levelCheckMs = 0.0;

        void OpenShop(int round, const std::vector<ShopItem> &stock)
        {
            if (round == 1) g_levelHinted.clear();   // (the hints of a match)
            CommandGui *gui = Gui();
            WorldManager *world = G_->GetWorld();
            if (!gui || !world || stock.empty()) return;
            g_refit.stock = stock;
            g_refit.shopRound = round;
            g_refit.buyBackStale = false;

            StoreDefinition definition = StoreDefinition();
            definition.hullRepair.visible = false;   // the preparation repairs the ship anyway
            std::vector<uint8_t> order;
            std::map<uint8_t, StoreCategory> categories;
            for (const ShopItem &item : stock)
            {
                if (item.kind == KIND_MISSILES || item.kind == KIND_DRONE_PARTS)
                {
                    ResourceItem resource = ResourceItem();
                    resource.type = item.kind == KIND_MISSILES ? "missiles" : "drones";
                    resource.minMaxCount = std::make_pair((int)item.count, (int)item.count);
                    resource.price.price = item.price > 0 ? item.price : -1;
                    definition.resources.push_back(resource);
                    continue;
                }
                if (item.kind > KIND_SYSTEM) continue;
                if (!categories.count(item.kind))
                {
                    StoreCategory category = StoreCategory();
                    category.categoryType = (CategoryType)item.kind;
                    category.chance = 100;
                    categories[item.kind] = category;
                    order.push_back(item.kind);
                }
                StoreItem entry = StoreItem();
                entry.blueprint = item.blueprint;
                entry.price.price = item.price;
                entry.stock = -1;
                categories[item.kind].items.push_back(entry);
            }
            // A page of its own for each kind (AP): Hyperspace's store puts two sections on a page and shows three items
            // in a section, so each kind comes as two sections, its items shared out between them.
            for (uint8_t kind : order)
            {
                const StoreCategory &all = categories[kind];
                StoreCategory first = all, second = all;
                size_t half = (all.items.size() + 1) / 2;
                first.items.assign(all.items.begin(), all.items.begin() + half);
                second.items.assign(all.items.begin() + half, all.items.end());
                definition.categories[-1].push_back(first);
                definition.categories[-1].push_back(second);
            }
            // Our sold systems on a sixth page (BJ), two sections as the others.
            std::vector<StoreItem> buyBack = BuyBackItems();
            g_refit.buyBackOffered.clear();
            for (const StoreItem &item : buyBack) g_refit.buyBackOffered.insert(ShipSystem::NameToSystemId(item.blueprint));
            if (!buyBack.empty())
            {
                StoreCategory first = StoreCategory(), second = StoreCategory();
                first.categoryType = second.categoryType = CategoryType::SYSTEMS;
                first.chance = second.chance = 100;
                first.customTitle = second.customTitle = "BUY BACK";
                size_t half = std::min<size_t>(3, buyBack.size());
                first.items.assign(buyBack.begin(), buyBack.begin() + half);
                second.items.assign(buyBack.begin() + half, buyBack.end());
                definition.categories[-1].push_back(first);
                if (!second.items.empty()) definition.categories[-1].push_back(second);
            }

            // Hyperspace's custom store builds it (at most three items per section are shown).
            CustomStore::instance->RegisterStoreDefinition(STORE_ID, definition);
            CustomStore::instance->forceCustomStore = STORE_ID;
            Store *store = gui->CreateNewStore(world->starMap.worldLevel);
            CustomStore::instance->forceCustomStore = "";
            g_store = store;
            // A little lower than FTL has it (roadmap AQ): the score panel (DuelsMatchUi.cpp) covered its BUY tab.
            if (!g_storePlaced)
            {
                g_storePlaced = true;
                g_ftlStoreX = gui->storeScreens.position.x;
                g_ftlStoreY = gui->storeScreens.position.y;
                Log("Refit: FTL's store window at %d,%d; ours %d px lower", g_ftlStoreX, g_ftlStoreY, STORE_LOWER);
            }
            gui->storeScreens.SetPosition(Point(g_ftlStoreX, g_ftlStoreY + STORE_LOWER));
            PlaceShipScreens();
            ++g_refit.shops;
            Log("Refit: round %d's shop is open (%u items, %u to buy back)%s", round, (unsigned)stock.size(), (unsigned)buyBack.size(),
                store ? "" : ", but no store came");
        }

        void PlaceShipScreens()
        {
            CommandGui *gui = Gui();
            if (!gui) return;
            // As much lower as the store (roadmap CW): the score panel covered their tabs.
            if (!g_shipScreensPlaced)
            {
                g_shipScreensPlaced = true;
                g_ftlShipScreensX = gui->shipScreens.position.x;
                g_ftlShipScreensY = gui->shipScreens.position.y;
                Log("Refit: FTL's ship screens at %d,%d; ours %d px lower", g_ftlShipScreensX, g_ftlShipScreensY, STORE_LOWER);
            }
            gui->shipScreens.SetPosition(Point(g_ftlShipScreensX, g_ftlShipScreensY + STORE_LOWER));
        }

        bool StoreDescriptionShown()
        {
            CommandGui *gui = Gui();
            if (!gui || !Rounds::InPreparation()) return false;
            // (Hyperspace's store draws its Store's info box right of it, the item's tip box under it: the right side, from
            // the store's top down to FTL's systems.)
            if (g_store && gui->storeScreens.bOpen && !g_store->infoBox.IsEmpty()) return true;
            // The ship's screens show a system's, a crew member's or an item's description there too (roadmap CW): the tab
            // on screen (another keeps its last one).
            TabbedWindow &screens = gui->shipScreens;
            if (!screens.bOpen || screens.currentTab >= screens.windows.size()) return false;
            FocusWindow *shown = screens.windows[screens.currentTab];
            if (shown == (FocusWindow*)&gui->upgradeScreen) return !gui->upgradeScreen.infoBox.IsEmpty();
            if (shown == (FocusWindow*)&gui->crewScreen) return !gui->crewScreen.infoBox.IsEmpty();
            if (shown == (FocusWindow*)&gui->equipScreen) return !gui->equipScreen.infoBox.IsEmpty();
            return false;
        }

        // A weapon that can't be powered because the weapons system's level is full, while the reactor has bars left: FTL
        // says "not enough power" then too, and a player who bought a weapon and reactor bars but not the system's level
        // couldn't tell why (the user's third test, 2026-10-02). The feed says it, once a match for each weapon.
        static void WeaponLevelHint(ShipManager *own)
        {
            double now = RealMs();
            if (now < g_levelCheckMs || !own->weaponSystem) return;
            g_levelCheckMs = now + 1000.0;
            PowerManager *power = PowerManager::GetPowerManager(0);
            if (!power) return;
            WeaponSystem *system = own->weaponSystem;
            int left = system->powerState.second - system->powerState.first;
            int reactorLeft = power->currentPower.second - power->currentPower.first;
            for (ProjectileFactory *weapon : own->GetWeaponList())
            {
                if (!weapon || weapon->powered || !weapon->blueprint || g_levelHinted.count(weapon->blueprint->name)) continue;
                int need = weapon->requiredPower;
                if (left >= need || reactorLeft < need) continue;   // it fits, or the reactor is what's short
                g_levelHinted.insert(weapon->blueprint->name);
                std::string text = weapon->name + " needs " + std::to_string(need) + " bar" + (need == 1 ? "" : "s") +
                                   " of weapons power: the weapons system (level " + std::to_string(system->powerState.second) + ") has " +
                                   std::to_string(left) + " left. Upgrade it (U) to power it.";
                Log("Refit: a hint: %s", text.c_str());
                Console::Feed(text);
            }
        }

        void OnPrepFrame()
        {
            CommandGui *gui = Gui();
            ShipManager *own = G_->GetShipManager(0);
            if (!gui || !own || !Rounds::InPreparation()) return;
            WeaponLevelHint(own);
            // A system bought back comes at level 1, for its price (FTL builds it at the level the ship's blueprint
            // gives it: the engines came back at level 2 for 1 scrap after a level had been sold for 10).
            for (auto it = g_refit.buyBackOffered.begin(); it != g_refit.buyBackOffered.end();)
            {
                ShipSystem *system = own->HasSystem(*it) ? own->GetSystem(*it) : nullptr;
                if (!system)
                {
                    ++it;
                    continue;
                }
                int level = system->powerState.second;
                if (level > 1) system->UpgradeSystem(1 - level);
                Log("Refit: %s bought back, at level 1 (it came at %d)", SystemTitle(*it).c_str(), level);
                it = g_refit.buyBackOffered.erase(it);
            }
            if (!g_refit.buyBackStale) return;
            // The store built again with the system just sold on its buy-back page. The windows stay as they were: the
            // upgrade screen open (systems are sold there), the store closed (FTL opens a new store's window).
            bool storeOpen = gui->storeScreens.bOpen, shipOpen = gui->shipScreens.bOpen;
            if (storeOpen) gui->storeScreens.Close();
            gui->SetStore(nullptr, false);
            OpenShop(g_refit.shopRound, g_refit.stock);
            if (!storeOpen && gui->storeScreens.bOpen) gui->storeScreens.Close();
            if (shipOpen && !gui->shipScreens.bOpen) gui->shipScreens.Open();
            Log("Refit: the store built again for the buy-back page (the store %s, the ship's screens %s)", gui->storeScreens.bOpen ? "open" : "closed",
                gui->shipScreens.bOpen ? "open" : "closed");
        }

        void CloseShop()
        {
            CommandGui *gui = Gui();
            if (!gui) return;
            // The window first: closing it looks at its current tab.
            if (gui->storeScreens.bOpen) gui->storeScreens.Close();
            gui->SetStore(nullptr, false);
            g_store = nullptr;
            // The upgrade (crew, equipment) screens close too; upgrades already paid for are made.
            if (gui->shipScreens.bOpen) gui->shipScreens.Close();
        }

        static bool Inside(const Globals::Rect &r, int x, int y)
        {
            return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
        }

        bool SwitchScreensClick(int x, int y)
        {
            CommandGui *gui = Gui();
            if (!gui || !Rounds::InPreparation()) return false;
            if (gui->storeScreens.bOpen && gui->upgradeButton.bActive && Inside(gui->upgradeButton.hitbox, x, y))
            {
                gui->storeScreens.Close();
                gui->shipScreens.Open();   // as FTL's upgrade button does
                Log("Refit: the store closes, the ship's screens open (upgrade button)");
                return true;
            }
            if (gui->shipScreens.bOpen && gui->storeButton.bActive && Inside(gui->storeButton.hitbox, x, y) && gui->storeScreens.GetWindow(0))
            {
                gui->shipScreens.Close();
                gui->storeScreens.Open();   // as FTL's STORE button does
                Log("Refit: the ship's screens close, the store opens (STORE button)");
                return true;
            }
            return false;
        }

        void SwitchScreensKey(int key)
        {
            CommandGui *gui = Gui();
            if (!gui || key <= 0 || !Rounds::InPreparation()) return;   // 0: a control with no key set
            bool shipKey = key == (int)Settings::GetHotkey("ship_info") || key == (int)Settings::GetHotkey("ship_crew") ||
                           key == (int)Settings::GetHotkey("ship_inv");
            if (gui->storeScreens.bOpen && shipKey)
            {
                gui->storeScreens.Close();
                Log("Refit: the store closes for the ship's screens (key %d)", key);
            }
            else if (gui->shipScreens.bOpen && key == (int)Settings::GetHotkey("store") && gui->storeButton.bActive)
            {
                gui->shipScreens.Close();
                Log("Refit: the ship's screens close for the store (key %d)", key);
            }
        }

        // ---------------------------------------------------------------------------------------------------------
        // The shop buys back: levels taken back, systems sold (roadmap V), for all they cost (AJ), every system (AK)
        // ---------------------------------------------------------------------------------------------------------

        // Every system of FTL's can be sold, down to nothing (AK): a ship can do without its shields, and a player who
        // sells the engines or the piloting gives up dodging and running away. Not the weapon and drone bays (they
        // follow their weapons and drones).
        static bool Sellable(int id)
        {
            return id >= 0 && id < SYS_ALL && id != SYS_REACTOR;
        }

        static std::string SystemTitle(int id)
        {
            switch (id)
            {
            case SYS_SHIELDS: return "Shields";
            case SYS_ENGINES: return "Engines";
            case SYS_OXYGEN: return "Oxygen";
            case SYS_WEAPONS: return "Weapons";
            case SYS_DRONES: return "Drone control";
            case SYS_MEDBAY: return "Medbay";
            case SYS_PILOT: return "Piloting";
            case SYS_SENSORS: return "Sensors";
            case SYS_DOORS: return "Doors";
            case SYS_TELEPORTER: return "Teleporter";
            case SYS_CLOAKING: return "Cloaking";
            case SYS_ARTILLERY: return "Artillery";
            case SYS_BATTERY: return "Backup battery";
            case SYS_CLONEBAY: return "Clone bay";
            case SYS_MIND: return "Mind control";
            case SYS_HACKING: return "Hacking";
            default: return ShipSystem::SystemIdToName(id);
            }
        }

        static void Sound(const char *name)
        {
            if (G_->GetSoundControl()) G_->GetSoundControl()->PlaySoundMix(name, -1.f, false);
        }

        static void Say(const std::string &text)
        {
            Log("Refit: %s", text.c_str());
            Console::Feed(text);
        }

        // Levels go back down to 1 (AK: the levels a ship began the match with have their price too), then the system
        // itself is sold.
        static int LowestLevel(int)
        {
            return 1;
        }

        // What a level cost (FTL's upgrade screen charges upgradeCosts[level - 2] for it).
        static int LevelPrice(const SystemBlueprint *blueprint, int level)
        {
            if (!blueprint || level < 2 || level - 2 >= (int)blueprint->upgradeCosts.size()) return 0;
            return blueprint->upgradeCosts[level - 2];
        }

        // What the system and its levels up to `level` cost, all of it (AJ: rebuilding the ship for the next round
        // shouldn't be punished).
        static int SalePrice(const SystemBlueprint *blueprint, int level)
        {
            if (!blueprint) return 0;
            int paid = blueprint->desc.cost;
            for (int l = 2; l <= level; ++l) paid += LevelPrice(blueprint, l);
            return paid;
        }

        bool TakeBackLevel(UpgradeBox *box, int mouseX, int mouseY)
        {
            if (!box || !box->system || !box->ship || box->tempUpgrade != 0 || !Rounds::InPreparation()) return false;
            // The box under the mouse: its button's place (roadmap CV: not FTL's hover, which a button switched off,
            // a system at its top level or one the scrap can't raise, never has).
            Button *button = box->currentButton;
            if (!button) return false;
            const Globals::Rect &r = button->hitbox;
            if (mouseX < r.x || mouseX >= r.x + r.w || mouseY < r.y || mouseY >= r.y + r.h) return false;
            ShipSystem *system = box->system;
            ShipManager *ship = box->ship;
            int id = system->iSystemType;
            if (id < 0 || id >= SYS_ALL || ship->iShipId != 0) return false;   // FTL's own systems (not the bays)
            int level = system->powerState.second, lowest = LowestLevel(id);
            if (level > lowest)
            {
                int refund = LevelPrice(box->blueprint, level);
                // Its power first: a system holds no more power than its level (FTL takes it as damage does).
                if (system->powerState.first > level - 1) system->ForceDecreasePower(system->powerState.first - (level - 1));
                system->UpgradeSystem(-1);
                ship->ModifyScrapCount(refund, false);
                g_refit.saleArmed = -1;
                Sound("downgradeSystem");
                Say(SystemTitle(id) + " down to level " + std::to_string(level - 1) + ": +" + std::to_string(refund) + " scrap");
                return true;
            }
            if (!Sellable(id)) return false;
            int price = SalePrice(box->blueprint, level);
            double now = WallMs();
            if (g_refit.saleArmed != id || now > g_refit.saleArmedUntil)
            {
                g_refit.saleArmed = id;
                g_refit.saleArmedUntil = now + 3000.0;
                Sound("powerUpFail");
                Say("Right-click again to sell " + SystemTitle(id) + " for " + std::to_string(price) + " scrap");
                return true;
            }
            g_refit.saleArmed = -1;
            g_refit.saleDue = id;
            g_refit.saleDuePrice = price;
            return true;
        }

        void OnUpgradesLoop()
        {
            int id = g_refit.saleDue, price = g_refit.saleDuePrice;
            if (id < 0) return;
            g_refit.saleDue = -1;
            ShipManager *ship = G_->GetShipManager(0);
            CommandGui *gui = Gui();
            if (!ship || !gui || !ship->HasSystem(id) || !Rounds::InPreparation()) return;
            // Upgrades waiting in the other boxes are made first (FTL's ACCEPT): the screen is built anew without the
            // sold system, and its boxes (with what they wait for) go.
            gui->upgradeScreen.ConfirmUpgrades();
            ship->RemoveSystem(id);
            ship->ModifyScrapCount(price, false);
            gui->upgradeScreen.OnInit(ship);
            g_refit.buyBackStale = true;
            Sound("downgradeSystem");
            Say(SystemTitle(id) + " sold: +" + std::to_string(price) + " scrap");
        }

        // What the reactor's bar `level` cost (Hyperspace's reactor prices, ReactorButton::OnRightClick).
        int ReactorPrice(const std::string &blueprint, int level)
        {
            const CustomShipDefinition &def = CustomShipSelect::GetInstance()->GetDefinition(blueprint);
            const std::vector<int> &costs = def.reactorPrices;
            int column = (int)std::floor((level - 1) / 5) + 1;
            if (column >= 0 && column < (int)costs.size() && costs[column] >= 0) return costs[column];
            return costs.empty() ? 0 : costs[0] + (column - 1) * def.reactorPriceIncrement;
        }

        int ReactorMax(const std::string &blueprint)
        {
            return CustomShipSelect::GetInstance()->GetDefinition(blueprint).maxReactorLevel;
        }

        bool TakeBackReactor(ReactorButton *button)
        {
            if (!button || !button->ship || button->tempUpgrade != 0 || !button->bHover || !Rounds::InPreparation()) return false;
            ShipManager *ship = button->ship;
            PowerManager *power = PowerManager::GetPowerManager(0);
            if (!power || ship->iShipId != 0) return false;
            int level = power->currentPower.second;
            if (level <= 1)
            {
                Sound("powerUpFail");
                Say("The reactor keeps its last bar");
                return true;
            }
            if (power->GetAvailablePower() < 1)
            {
                Sound("powerUpFail");
                Say("Every reactor bar is in use: take power off a system first");
                return true;
            }
            int refund = ReactorPrice(ship->myBlueprint.blueprintName, level);
            power->currentPower.second -= 1;
            ship->ModifyScrapCount(refund, false);
            Sound("downgradeSystem");
            Say("Reactor down to " + std::to_string(level - 1) + " bars: +" + std::to_string(refund) + " scrap");
            return true;
        }

        static std::map<int, std::pair<Globals::Rect, double>> g_boxPlaces;   // system id: the button's hit box, when

        bool BoxPlace(int systemId, int &x, int &y)
        {
            auto place = g_boxPlaces.find(systemId);
            if (place == g_boxPlaces.end() || WallMs() - place->second.second > 1000.0) return false;
            const Globals::Rect &r = place->second.first;
            x = r.x + r.w / 2;
            y = r.y + r.h / 2;
            return true;
        }

        void RenderSaleMark(UpgradeBox *box)
        {
            if (box && box->system && box->currentButton)
            {
                g_boxPlaces[box->system->iSystemType] = std::make_pair(box->currentButton->hitbox, WallMs());
            }
            if (!box || !box->system || g_refit.saleArmed != box->system->iSystemType || WallMs() > g_refit.saleArmedUntil) return;
            if (!Rounds::InPreparation() || !box->currentButton) return;
            const Globals::Rect &r = box->currentButton->hitbox;
            CSurface::GL_DrawRect((float)r.x, (float)r.y, (float)r.w, (float)r.h, GL_Color(0.75f, 0.12f, 0.1f, 0.72f));
            CSurface::GL_SetColor(GL_Color(1.f, 1.f, 1.f, 1.f));
            freetype::easy_printCenter(12, r.x + r.w / 2.f, r.y + r.h / 2.f - 18.f, "SELL?");
            freetype::easy_printCenter(12, r.x + r.w / 2.f, r.y + r.h / 2.f + 2.f, "+" + std::to_string(SalePrice(box->blueprint, box->system->powerState.second)));
        }

        void OnUpgradesOpen()
        {
            if (g_refit.tipShown || !Rounds::InPreparation()) return;
            g_refit.tipShown = true;
            Console::Feed("Upgrades: a right-click takes a level back for what it cost; a system at level 1 sells for "
                          "what it cost at a second right-click");
        }

        // A weapon empty: its bar and its charges (FTL fires by the charges, ProjectileFactory::ReadyToFire: emptying the
        // bar alone left a charged artillery to fire at once).
        static void Empty(ProjectileFactory *weapon)
        {
            if (!weapon) return;
            weapon->cooldown.first = 0.f;
            weapon->chargeLevel = 0;
        }

        void HoldCharges()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            if (own->weaponSystem)
            {
                for (ProjectileFactory *weapon : own->GetWeaponList()) Empty(weapon);
            }
            for (ArtillerySystem *artillery : own->artillerySystems)
            {
                if (artillery) Empty(artillery->projectileFactory);
            }
        }

        void ResetWeaponCharge()
        {
            HoldCharges();
            ShipManager *own = G_->GetShipManager(0);
            if (!own || !own->weaponSystem || own->HasAugmentation("WEAPON_PREIGNITE") <= 0) return;
            // As FTL's WeaponSystem::Jump: every powered weapon full (ForceCoolup; a charger's charges too).
            int filled = 0;
            for (ProjectileFactory *weapon : own->GetWeaponList())
            {
                if (!weapon || !weapon->powered) continue;
                weapon->ForceCoolup();
                ++filled;
            }
            Log("Refit: the Weapon Pre-Igniter: %d weapon(s) charged as the fight begins", filled);
        }
    }
}
