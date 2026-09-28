#include "Global.h"
#include "Duels.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsWin32.h"
#include "Projectile_Extend.h"

#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>

namespace Duels
{
    // -----------------------------------------------------------------------------------------
    // Argument helpers
    // -----------------------------------------------------------------------------------------

    static const char *SYSTEM_NAMES[] = {
        "shields", "engines", "oxygen", "weapons", "drones", "medbay", "pilot", "sensors",
        "doors", "teleporter", "cloaking", "artillery", "battery", "clonebay", "mind", "hacking"};
    static const int SYSTEM_COUNT = 16;

    static int ParseSystem(const std::string &name)
    {
        for (int i = 0; i < SYSTEM_COUNT; ++i)
        {
            if (name == SYSTEM_NAMES[i]) return i;
        }
        char *end = nullptr;
        long value = std::strtol(name.c_str(), &end, 10);
        return (end != name.c_str() && *end == '\0' && value >= 0 && value < SYSTEM_COUNT) ? (int)value : -1;
    }

    static ShipManager *ArgShip(const Command &cmd, size_t index, std::string &message)
    {
        int shipId;
        if (!ArgInt(cmd, index, shipId) || shipId < 0 || shipId > 1)
        {
            message = "ship id must be 0 (player) or 1 (enemy)";
            return nullptr;
        }
        ShipManager *ship = G_->GetShipManager(shipId);
        if (!ship) message = shipId == 1 ? "no enemy ship" : "no player ship";
        return ship;
    }

    static ProjectileFactory *ArgWeapon(ShipManager *ship, const Command &cmd, size_t index, std::string &message)
    {
        int slot;
        if (!ArgInt(cmd, index, slot))
        {
            message = "weapon slot missing";
            return nullptr;
        }
        if (!ship->weaponSystem)
        {
            message = "ship has no weapons system";
            return nullptr;
        }
        std::vector<ProjectileFactory*> weapons = ship->GetWeaponList();
        if (slot < 0 || slot >= (int)weapons.size())
        {
            message = "no weapon in slot " + std::to_string(slot);
            return nullptr;
        }
        return weapons[slot];
    }

    static bool ArgOnOff(const Command &cmd, size_t index, bool &out)
    {
        if (ArgIs(cmd, index, "on") || ArgIs(cmd, index, "open")) { out = true; return true; }
        if (ArgIs(cmd, index, "off") || ArgIs(cmd, index, "close")) { out = false; return true; }
        return false;
    }

    // Living crew owned by `ship`, wherever they are (own ship first, then boarders).
    static std::vector<CrewMember*> OwnCrew(ShipManager *ship)
    {
        std::vector<CrewMember*> crew;
        for (int shipId = 0; shipId < 2; ++shipId)
        {
            ShipManager *where = G_->GetShipManager(shipId);
            if (!where) continue;
            for (CrewMember *member : where->vCrewList)
            {
                if (member->iShipId == ship->iShipId && !member->bDead) crew.push_back(member);
            }
        }
        return crew;
    }

    static std::string Raw(const Command &cmd, size_t index)
    {
        return index < cmd.raw.size() ? cmd.raw[index] : "";
    }

    static long FileSize(const std::string &path)
    {
        std::ifstream file(path.c_str(), std::ios::binary | std::ios::ate);
        return file ? (long)file.tellg() : -1;
    }

    // -----------------------------------------------------------------------------------------
    // Shared helpers
    // -----------------------------------------------------------------------------------------

    bool SetSystemPower(ShipManager *ship, int system, int level)
    {
        // User-style changes, like the power bars in the UI. ForceDecreaseSystemPower is the ion/damage path:
        // the game restores that power on its own later (Step 1 finding).
        ShipSystem *shipSystem = ship->GetSystem(system);
        if (!shipSystem) return false;
        for (int guard = 0; guard < 32; ++guard)
        {
            int power = ship->GetSystemPower(system);
            if (power == level) break;
            bool changed = power < level ? ship->IncreaseSystemPower(system) : shipSystem->DecreasePower(false);
            if (!changed) break;
        }
        return ship->GetSystemPower(system) == level;
    }

    const char *SystemName(int system)
    {
        return system >= 0 && system < SYSTEM_COUNT ? SYSTEM_NAMES[system] : "?";
    }

    // -----------------------------------------------------------------------------------------
    // Verbs
    // -----------------------------------------------------------------------------------------

    static bool DoPower(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int system = cmd.args.size() > 2 ? ParseSystem(cmd.args[2]) : -1;
        int level;
        if (system < 0 || !ArgInt(cmd, 3, level))
        {
            message = "usage: power <ship> <system> <level>";
            return false;
        }
        if (!ship->HasSystem(system))
        {
            message = std::string("ship has no ") + SYSTEM_NAMES[system];
            return false;
        }

        bool reached = SetSystemPower(ship, system, level);
        message = std::string(SYSTEM_NAMES[system]) + " power " + std::to_string(ship->GetSystemPower(system)) + "/" +
                  std::to_string(ship->GetSystemPowerMax(system));
        return reached;
    }

    // arm <ship> <slot> <WEAPON_BLUEPRINT>: puts that weapon into the slot, replacing what is there (tests of weapon
    // types; in a duel, before connecting, so the loadout carries it).
    static bool DoArm(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int slot;
        if (!ArgInt(cmd, 2, slot) || cmd.raw.size() < 4)
        {
            message = "usage: arm <ship> <slot> <WEAPON_BLUEPRINT>";
            return false;
        }
        if (!ship->weaponSystem)
        {
            message = "ship has no weapons system";
            return false;
        }
        const std::string &name = cmd.raw[3];   // original spelling: blueprint names are case-sensitive
        WeaponBlueprint *blueprint = G_->GetBlueprints()->GetWeaponBlueprint(name);
        if (!blueprint || blueprint->name != name)
        {
            message = "no weapon blueprint " + name;
            return false;
        }
        int count = (int)ship->GetWeaponList().size();
        if (slot < 0 || slot > count || slot >= ship->weaponSystem->slot_count)
        {
            message = "slot " + std::to_string(slot) + " is not free or next (" + std::to_string(count) + " weapons, " +
                      std::to_string(ship->weaponSystem->slot_count) + " slots)";
            return false;
        }
        if (slot < count) ship->RemoveWeapon(slot);
        ship->AddWeapon(blueprint, slot);
        std::ostringstream out;
        out << "weapons:";
        for (ProjectileFactory *weapon : ship->GetWeaponList()) out << " " << (weapon->blueprint ? weapon->blueprint->name : "?");
        message = out.str();
        return true;
    }

    // upgrade <ship> <system> <levels>: more system levels, and as much more reactor power (tests).
    static bool DoUpgrade(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int system = cmd.args.size() > 2 ? ParseSystem(cmd.args[2]) : -1;
        int levels;
        if (system < 0 || !ArgInt(cmd, 3, levels) || levels < 1)
        {
            message = "usage: upgrade <ship> <system> <levels>";
            return false;
        }
        ShipSystem *target = ship->GetSystem(system);
        if (!target)
        {
            message = std::string("ship has no ") + SYSTEM_NAMES[system];
            return false;
        }
        target->UpgradeSystem(levels);
        PowerManager *power = PowerManager::GetPowerManager(ship->iShipId);
        if (power) power->currentPower.second += levels;
        message = std::string(SYSTEM_NAMES[system]) + " level " + std::to_string(target->powerState.second) +
                  (power ? ", reactor " + std::to_string(power->currentPower.second) : "");
        return true;
    }

    static bool DoWeapon(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        ProjectileFactory *weapon = ArgWeapon(ship, cmd, 2, message);
        if (!weapon) return false;
        bool on;
        if (!ArgOnOff(cmd, 3, on))
        {
            message = "usage: weapon <ship> <slot> on|off";
            return false;
        }

        if (on) ship->PowerWeapon(weapon, true, false);
        else ship->DePowerWeapon(weapon, true);
        message = weapon->name + (weapon->powered ? " powered" : " unpowered");
        return weapon->powered == on;
    }

    static bool DoFire(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        ProjectileFactory *weapon = ArgWeapon(ship, cmd, 2, message);
        if (!weapon) return false;
        ShipManager *target = G_->GetShipManager(1 - ship->iShipId);
        int room;
        if (!target)
        {
            message = "no opponent to fire at";
            return false;
        }
        if (!ArgIs(cmd, 3, "room") || !ArgInt(cmd, 4, room))
        {
            message = "usage: fire <ship> <slot> room <room>";
            return false;
        }

        Pointf center = target->GetRoomCenter(room);
        if (center.x < 0.f)
        {
            message = "opponent has no room " + std::to_string(room);
            return false;
        }

        // Like the enemy AI: one point per required target. Beams take (start, sweep) — sweep one tile to the right.
        int required = weapon->NumTargetsRequired();
        std::vector<Pointf> points(1, center);
        bool beam = weapon->blueprint && weapon->blueprint->type == 2;
        while ((int)points.size() < required)
        {
            Pointf next = center;
            if (beam) next.x += 35.f;
            points.push_back(next);
        }
        weapon->Fire(points, target->iShipId);

        std::ostringstream out;
        out << weapon->name << " aimed at room " << room << " with " << points.size() << "/" << required
            << " target points (charge " << weapon->cooldown.first << "/" << weapon->cooldown.second
            << (weapon->powered ? "" : ", UNPOWERED") << ")";
        message = out.str();
        return true;
    }

    static bool DoAutofire(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        ProjectileFactory *weapon = ArgWeapon(ship, cmd, 2, message);
        if (!weapon) return false;
        bool on;
        if (!ArgOnOff(cmd, 3, on))
        {
            message = "usage: autofire <ship> <slot> on|off";
            return false;
        }
        weapon->SetAutoFire(on);
        message = weapon->name + (on ? " autofire on" : " autofire off");
        return true;
    }

    static bool DoCrew(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int index, room;
        if (!ArgInt(cmd, 2, index) || !ArgIs(cmd, 3, "room") || !ArgInt(cmd, 4, room))
        {
            message = "usage: crew <ship> <index> room <room>";
            return false;
        }
        std::vector<CrewMember*> crew = OwnCrew(ship);
        if (index < 0 || index >= (int)crew.size())
        {
            message = "no living crew member " + std::to_string(index);
            return false;
        }

        CrewMember *member = crew[index];
        ShipManager *where = G_->GetShipManager(member->currentShipId);
        bool ok = where && where->CommandCrewMoveRoom(member, room);
        message = member->species + " #" + std::to_string(index) + (ok ? " moving to room " : " cannot move to room ") +
                  std::to_string(room);
        return ok;
    }

    static bool DoDoor(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int doorId;
        bool open;
        if (!ArgInt(cmd, 2, doorId) || !ArgOnOff(cmd, 3, open))
        {
            message = "usage: door <ship> <door id> open|close";
            return false;
        }
        for (Door *door : ship->ship.vDoorList)
        {
            if (door->iDoorId != doorId) continue;
            if (open) door->Open();
            else door->Close();
            message = "door " + std::to_string(doorId) + (door->bOpen ? " open" : " closed");
            return door->bOpen == open;
        }
        message = "no door " + std::to_string(doorId);
        return false;
    }

    static bool DoCloak(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        if (!ship->cloakSystem)
        {
            message = "ship has no cloaking";
            return false;
        }
        if (!ship->CanCloak())
        {
            message = "cloak not ready";
            return false;
        }
        ship->cloakSystem->SetTurnedOn(true);
        message = "cloak engaged";
        return true;
    }

    bool SpawnEnemy(const std::string &blueprint, std::string &message)
    {
        WorldManager *world = G_->GetWorld();
        CommandGui *gui = world ? world->commandGui : nullptr;
        if (!gui || !world->playerShip)
        {
            message = "not in a game";
            return false;
        }
        if (G_->GetShipManager(1))
        {
            message = "an enemy ship is already present";
            return false;
        }

        ShipEvent shipEvent{};
        shipEvent.present = true;
        shipEvent.name = blueprint;
        shipEvent.blueprint = blueprint;
        shipEvent.hostile = true;
        shipEvent.shipSeed = std::rand();

        CompleteShip *enemy = world->CreateShip(&shipEvent, false);
        if (!enemy)
        {
            message = "CreateShip failed for " + blueprint;
            return false;
        }
        gui->AddEnemyShip(enemy);
        message = "spawned " + blueprint + " as ship 1";
        return true;
    }

    static bool DoSpawn(const Command &cmd, std::string &message)
    {
        if (cmd.args.size() < 2)
        {
            message = "usage: spawn <SHIP_BLUEPRINT>";
            return false;
        }
        return SpawnEnemy(Raw(cmd, 1), message);
    }

    static bool DoExport(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        std::string path = Raw(cmd, 2);
        if (path.empty())
        {
            message = "usage: export <ship> <file>";
            return false;
        }
        int file = FileHelper::createBinaryFile(path);
        ship->ExportShip(file);
        FileHelper::closeBinaryFile(file);
        long size = FileSize(path);
        message = "exported ship " + std::to_string(ship->iShipId) + " to " + path + " (" + std::to_string(size) + " bytes)";
        return size > 0;
    }

    static bool DoImport(const Command &, std::string &message)
    {
        // Step 1 finding: ShipManager::ImportShip on the enemy crashes inside ShipSystem::DecreasePower when run
        // from CreateShip's OnInit. Loadouts will be transferred explicitly instead (see docs/dev/step1-results.md).
        message = "import is disabled (ImportShip on ship 1 crashes); loadouts will be transferred explicitly";
        return false;
    }

    static bool DoDescribe(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;

        Log("--- ship %d: %s, hull %d/%d, reactor %d/%d ---", ship->iShipId, ship->myBlueprint.blueprintName.c_str(),
            ship->ship.hullIntegrity.first, ship->ship.hullIntegrity.second,
            PowerManager::GetPowerManager(ship->iShipId)->currentPower.first,
            PowerManager::GetPowerManager(ship->iShipId)->currentPower.second);
        for (int system = 0; system < SYSTEM_COUNT; ++system)
        {
            if (!ship->HasSystem(system)) continue;
            ShipSystem *shipSystem = ship->GetSystem(system);
            Log("  system %-10s room %2d power %d/%d health %d/%d", SYSTEM_NAMES[system], ship->GetSystemRoom(system),
                ship->GetSystemPower(system), ship->GetSystemPowerMax(system), shipSystem->healthState.first,
                shipSystem->healthState.second);
        }
        if (ship->shieldSystem)
        {
            Log("  shield layers %d charge %.2f", ship->shieldSystem->shields.power.first, ship->shieldSystem->shields.charger);
        }
        if (ship->weaponSystem)
        {
            std::vector<ProjectileFactory*> weapons = ship->GetWeaponList();
            for (size_t slot = 0; slot < weapons.size(); ++slot)
            {
                ProjectileFactory *weapon = weapons[slot];
                Log("  weapon %u %-18s type %d power %d %s charge %.2f/%.2f autofire %d", (unsigned)slot,
                    weapon->name.c_str(), weapon->blueprint ? weapon->blueprint->type : -1, weapon->requiredPower,
                    weapon->powered ? "on " : "off", weapon->cooldown.first, weapon->cooldown.second,
                    (int)weapon->autoFiring);
            }
        }
        std::vector<CrewMember*> crew = OwnCrew(ship);
        for (size_t index = 0; index < crew.size(); ++index)
        {
            CrewMember *member = crew[index];
            Log("  crew %u %-8s on ship %d room %2d health %.0f/%.0f", (unsigned)index, member->species.c_str(),
                member->currentShipId, member->iRoomId, member->health.first, member->health.second);
        }
        for (Door *door : ship->ship.vDoorList)
        {
            Log("  door %2d rooms %2d-%2d %s", door->iDoorId, door->iRoom1, door->iRoom2, door->bOpen ? "open" : "closed");
        }
        message = "described ship " + std::to_string(ship->iShipId) + " in duels_log.txt";
        return true;
    }

    static bool DoPauseTest(const Command &cmd, std::string &message)
    {
        CApp *app = G_->GetCApp();
        CommandGui *gui = app ? app->gui : nullptr;
        std::string kind = cmd.args.size() > 1 ? cmd.args[1] : "";
        if (!gui)
        {
            message = "no game UI";
            return false;
        }

        if (kind == "f1") gui->KeyDown(SDLK_F1, false);
        else if (kind == "escape") gui->KeyDown(SDLK_ESCAPE, false);
        else if (kind == "space") gui->KeyDown(SDLK_SPACE, false);
        else if (kind == "setpaused") gui->SetPaused(true, false);
        else if (kind == "autopause") gui->SetPaused(true, true);
        else if (kind == "menu") gui->KeyDown(SDLK_ESCAPE, false);
        else if (kind == "upgrades") gui->KeyDown(SDLK_u, false);
        else if (kind == "blur") app->OnInputBlur();
        else if (kind == "focus") app->OnInputFocus();
        else if (kind == "minimize" || kind == "restore")
        {
            std::string details;
            bool found = kind == "minimize" ? MinimizeGameWindow(details) : RestoreGameWindow(details);
            message = found ? kind + ": " + details : "game window not found";
            return found;
        }
        else
        {
            message = "usage: pausetest f1|escape|space|setpaused|autopause|menu|upgrades|blur|focus|minimize|restore";
            return false;
        }
        message = "pause trigger '" + kind + "' sent";
        return true;
    }

    static bool DoWindow(const Command &cmd, std::string &message)
    {
        int x, y, width = 0, height = 0;
        if (!ArgInt(cmd, 1, x) || !ArgInt(cmd, 2, y) || (cmd.args.size() > 3 && (!ArgInt(cmd, 3, width) || !ArgInt(cmd, 4, height))))
        {
            message = "usage: window <x> <y> [width height]";
            return false;
        }
        std::string details;
        bool found = MoveGameWindow(x, y, width, height, details);
        message = found ? "window moved: " + details : "game window not found";
        return found;
    }

    static bool DoScreenshot(const Command &cmd, std::string &message)
    {
        std::string path = Raw(cmd, 1);
        if (path.empty())
        {
            message = "usage: screenshot <file.bmp>";
            return false;
        }
        Screen::RequestCapture(path);
        message = "saving the next frame as " + path;
        return true;
    }

    bool ExecuteShipCommand(const Command &cmd, std::string &message)
    {
        const std::string &verb = cmd.args[0];
        if (verb == "window") return DoWindow(cmd, message);
        if (verb == "screenshot") return DoScreenshot(cmd, message);
        if (verb == "power") return DoPower(cmd, message);
        if (verb == "weapon") return DoWeapon(cmd, message);
        if (verb == "arm") return DoArm(cmd, message);
        if (verb == "upgrade") return DoUpgrade(cmd, message);
        if (verb == "fire") return DoFire(cmd, message);
        if (verb == "autofire") return DoAutofire(cmd, message);
        if (verb == "crew") return DoCrew(cmd, message);
        if (verb == "door") return DoDoor(cmd, message);
        if (verb == "cloak") return DoCloak(cmd, message);
        if (verb == "spawn") return DoSpawn(cmd, message);
        if (verb == "export") return DoExport(cmd, message);
        if (verb == "import") return DoImport(cmd, message);
        if (verb == "describe") return DoDescribe(cmd, message);
        if (verb == "pausetest") return DoPauseTest(cmd, message);
        if (verb == "quit")
        {
            GetState().quitRequested = true;
            message = "quit requested";
            return true;
        }
        message = "unknown verb '" + verb + "'";
        return false;
    }

    // -----------------------------------------------------------------------------------------
    // AI takeover
    // -----------------------------------------------------------------------------------------

    static ShipManager *g_aiTakenOver = nullptr;

    void OnAiTakeover(ShipManager *ship)
    {
        if (ship == g_aiTakenOver) return;
        g_aiTakenOver = ship;

        unsigned cleared = 0;
        if (ship->weaponSystem)
        {
            for (ProjectileFactory *weapon : ship->GetWeaponList())
            {
                weapon->ClearAiming();
                weapon->SetAutoFire(false);
                ++cleared;
            }
        }
        Log("Ship %d AI replaced by commands (%u weapons cleared)", ship->iShipId, cleared);
    }

    // -----------------------------------------------------------------------------------------
    // Power tracing: every change of a replaced ship's system power, with the frame it happened in
    // -----------------------------------------------------------------------------------------

    void TracePowerChanges(int frame)
    {
        static ShipManager *watched = nullptr;
        static int last[SYSTEM_COUNT];

        const State &state = GetState();
        ShipManager *ship = G_->GetShipManager(1);
        if (!state.tracePower || !ship) return;

        if (ship != watched)
        {
            watched = ship;
            for (int system = 0; system < SYSTEM_COUNT; ++system) last[system] = -1;
        }
        for (int system = 0; system < SYSTEM_COUNT; ++system)
        {
            int power = ship->HasSystem(system) ? ship->GetSystemPower(system) : -1;
            if (power != last[system] && last[system] != -1)
            {
                Log("power change ship 1 %s %d -> %d (frame %d, game %.3f s)", SYSTEM_NAMES[system], last[system], power,
                    frame, state.gameTime);
            }
            last[system] = power;
        }
    }

    // -----------------------------------------------------------------------------------------
    // Projectile tracing
    // -----------------------------------------------------------------------------------------

    struct TrackedProjectile
    {
        unsigned int selfId = 0;
        int type = 0;
        std::string weapon;
        int owner = 0;
        int space = 0;
        int destination = 0;
        bool hitTarget = false;
        bool missed = false;
        bool startedDeath = false;
        bool passedTarget = false;
        double spawnWallMs = 0.0;
        double spawnGameS = 0.0;
        float x = 0.f;
        float y = 0.f;
        bool seen = false;
    };

    static CsvFile g_projectileCsv;
    static std::map<Projectile*, TrackedProjectile> g_tracked;

    static void Emit(const char *event, const TrackedProjectile &p)
    {
        const State &state = GetState();
        double now = WallMs();
        Row row;
        row << now << state.gameTime << event << p.selfId << p.type << p.weapon << p.owner << p.space << p.destination
            << p.x << p.y << (now - p.spawnWallMs) << (state.gameTime - p.spawnGameS);
        g_projectileCsv.WriteRow(row.str());
    }

    static void Snapshot(Projectile *projectile, TrackedProjectile &p)
    {
        p.space = projectile->currentSpace;
        p.destination = projectile->destinationSpace;
        p.hitTarget = projectile->hitTarget;
        p.missed = projectile->missed;
        p.startedDeath = projectile->startedDeath;
        p.passedTarget = projectile->passedTarget;
        p.x = projectile->position.x;
        p.y = projectile->position.y;
    }

    void TraceProjectiles()
    {
        WorldManager *world = G_->GetWorld();
        if (!GetState().trace || !world || !world->commandGui || G_->GetCApp()->menu.bOpen) return;

        if (!g_projectileCsv.IsOpen())
        {
            g_projectileCsv.Open("duels_projectiles.csv",
                                 "wall_ms,game_s,event,self_id,type,weapon,owner,space,destination,x,y,age_wall_ms,age_game_s");
        }

        for (auto &entry : g_tracked) entry.second.seen = false;

        for (Projectile *projectile : world->space.projectiles)
        {
            auto found = g_tracked.find(projectile);
            if (found != g_tracked.end() && found->second.selfId != projectile->selfId)
            {
                // The address was reused by a new projectile.
                Emit("gone", found->second);
                g_tracked.erase(found);
                found = g_tracked.end();
            }

            if (found == g_tracked.end())
            {
                TrackedProjectile p;
                p.selfId = projectile->selfId;
                p.type = projectile->GetType();
                p.weapon = PR_EX(projectile)->name;
                p.owner = projectile->ownerId;
                p.spawnWallMs = WallMs();
                p.spawnGameS = GetState().gameTime;
                Snapshot(projectile, p);
                p.seen = true;
                g_tracked[projectile] = p;
                Emit("spawn", p);
                continue;
            }

            TrackedProjectile &p = found->second;
            TrackedProjectile before = p;
            Snapshot(projectile, p);
            p.seen = true;
            if (p.space != before.space) Emit("transfer", p);
            if (p.hitTarget && !before.hitTarget) Emit("hit", p);
            if (p.missed && !before.missed) Emit("miss", p);
            if (p.passedTarget && !before.passedTarget) Emit("passed", p);
            if (p.startedDeath && !before.startedDeath) Emit("death", p);
        }

        for (auto it = g_tracked.begin(); it != g_tracked.end();)
        {
            if (it->second.seen)
            {
                ++it;
                continue;
            }
            Emit("gone", it->second);
            it = g_tracked.erase(it);
        }
    }

    void CloseProjectileTrace()
    {
        g_projectileCsv.Close();
        g_tracked.clear();
    }
}
