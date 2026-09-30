#include "Global.h"
#include "CustomStore.h"
#include "Duels.h"
#include "DuelsCrew.h"
#include "DuelsRefit.h"
#include "DuelsRounds.h"

#include <algorithm>
#include <map>
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
            uint32_t revived = 0, shops = 0;
        };

        static RefitState g_refit;
        static const char *const STORE_ID = "FTL_DUELS_ROUND";

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

        // Augments that do nothing in a duel (rules, section 7): no jumps, no star map, no events, no scrap collected.
        static const char *const USELESS_AUGMENTS[] = {"FTL_JAMMER", "FTL_BOOSTER", "FTL_JUMPER", "FLEET_DISTRACTION",
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
            // The price cap for weapons and drones (rules, section 7): 55, 65, 75, 85, then none.
            static const int CAPS[] = {55, 65, 75, 85};
            int cap = round >= 1 && round <= 4 ? CAPS[round - 1] : 0;
            std::vector<uint8_t> others = {KIND_DRONE, KIND_AUGMENT, KIND_SYSTEM, KIND_CREW};
            std::shuffle(others.begin(), others.end(), random);
            std::vector<uint8_t> kinds = {KIND_WEAPON};
            int sections = round <= 2 ? 3 : 4;
            for (int i = 0; i < sections - 1 && i < (int)others.size(); ++i) kinds.push_back(others[i]);

            for (uint8_t kind : kinds)
            {
                std::vector<Candidate> pool;
                switch (kind)
                {
                case KIND_WEAPON: pool = Pool(blueprints->weaponBlueprints, cap, kind); break;
                case KIND_DRONE: pool = Pool(blueprints->droneBlueprints, cap, kind); break;
                case KIND_AUGMENT: pool = Pool(blueprints->augmentBlueprints, 0, kind); break;
                case KIND_SYSTEM: pool = Pool(blueprints->systemBlueprints, 0, kind); break;
                case KIND_CREW: pool = Pool(blueprints->crewBlueprints, 0, kind); break;
                default: break;
                }
                for (const Candidate &candidate : Draw(pool, 3, random))
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
            missiles.count = 8;
            stock.push_back(missiles);
            ShopItem parts;
            parts.kind = KIND_DRONE_PARTS;
            parts.count = 4;
            stock.push_back(parts);
            return stock;
        }

        // ---------------------------------------------------------------------------------------------------------
        // The match, the fights, the crew
        // ---------------------------------------------------------------------------------------------------------

        void OnMatchStart()
        {
            g_refit = RefitState();
            std::vector<CrewMember*> crew = CrewAboard(G_->GetShipManager(0));
            for (CrewMember *member : crew) g_refit.crew.push_back(Remember(member));
            if (!crew.empty())
            {
                g_refit.haveCaptain = true;
                g_refit.captain = g_refit.crew.front();
                Log("Refit: the captain is %s (%s); %u crew at the start", g_refit.captain.name.c_str(),
                    g_refit.captain.species.c_str(), (unsigned)crew.size());
            }
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

        void OpenShop(int round, const std::vector<ShopItem> &stock)
        {
            CommandGui *gui = Gui();
            WorldManager *world = G_->GetWorld();
            if (!gui || !world || stock.empty()) return;

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
            for (uint8_t kind : order) definition.categories[-1].push_back(categories[kind]);

            // Hyperspace's custom store builds it (at most three items per section are shown).
            CustomStore::instance->RegisterStoreDefinition(STORE_ID, definition);
            CustomStore::instance->forceCustomStore = STORE_ID;
            Store *store = gui->CreateNewStore(world->starMap.worldLevel);
            CustomStore::instance->forceCustomStore = "";
            ++g_refit.shops;
            Log("Refit: round %d's shop is open (%u items)%s", round, (unsigned)stock.size(), store ? "" : ", but no store came");
        }

        void CloseShop()
        {
            CommandGui *gui = Gui();
            if (!gui) return;
            // The window first: closing it looks at its current tab.
            if (gui->storeScreens.bOpen) gui->storeScreens.Close();
            gui->SetStore(nullptr, false);
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

        void ResetWeaponCharge()
        {
            ShipManager *own = G_->GetShipManager(0);
            if (!own) return;
            if (own->weaponSystem)
            {
                for (ProjectileFactory *weapon : own->GetWeaponList()) weapon->cooldown.first = 0.f;
            }
            for (ArtillerySystem *artillery : own->artillerySystems)
            {
                if (artillery && artillery->projectileFactory) artillery->projectileFactory->cooldown.first = 0.f;
            }
        }
    }
}
