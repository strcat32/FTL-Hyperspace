#include "Global.h"
#include "CrewMember_Extend.h"
#include "Duels.h"
#include "DuelsBays.h"
#include "DuelsBoarding.h"
#include "DuelsHacking.h"
#include "DuelsMind.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsTrace.h"
#include "DuelsWin32.h"
#include "Projectile_Extend.h"

#include <cmath>
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

    // install <ship> <system>: adds a system the ship's layout has room for, at level 1 (tests, e.g. a battery).
    static bool DoInstall(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int system = cmd.args.size() > 2 ? ParseSystem(cmd.args[2]) : -1;
        if (system < 0)
        {
            message = "usage: install <ship> <system>";
            return false;
        }
        if (!ship->HasSystem(system))
        {
            ship->AddSystem(system);
            if (!ship->HasSystem(system))
            {
                message = std::string("the ship's layout has no room for ") + SYSTEM_NAMES[system];
                return false;
            }
            // The player's system bar gets a box for it, as after buying one.
            if (ship->iShipId == 0 && G_->GetWorld() && G_->GetWorld()->commandGui)
            {
                G_->GetWorld()->commandGui->sysControl.CreateSystemBoxes();
            }
        }
        message = std::string(SYSTEM_NAMES[system]) + " level " + std::to_string(ship->GetSystem(system)->powerState.second);
        return true;
    }

    // augment <ship> <AUGMENT> [off]: gives the ship an augment (or takes it away), e.g. CLOAK_FIRE (Stealth Weapons).
    static bool DoAugment(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        if (cmd.raw.size() < 3)
        {
            message = "usage: augment <ship> <AUGMENT> [off]";
            return false;
        }
        const std::string &name = cmd.raw[2];   // original spelling: blueprint names are case-sensitive
        AugmentBlueprint *blueprint = G_->GetBlueprints()->GetAugmentBlueprint(name);
        if (!blueprint || blueprint->name != name)
        {
            message = "no augment blueprint " + name;
            return false;
        }
        if (ArgIs(cmd, 3, "off"))
        {
            ship->RemoveAugmentation(name);
        }
        else if (!ship->HasAugmentation(name) && !ship->AddAugmentation(name))
        {
            message = "cannot add " + name + " (no free augment slot, or no such augment)";
            return false;
        }
        message = name + (ship->HasAugmentation(name) ? " on board" : " not on board");
        return true;
    }

    // hack <system>|room <room>|pulse|stop: our hacking drone goes for the enemy's system (in that room); the pulse.
    static bool DoHack(const Command &cmd, std::string &message)
    {
        std::string what;
        for (size_t i = 1; i < cmd.args.size(); ++i) what += (i > 1 ? " " : "") + cmd.args[i];
        if (what.empty())
        {
            message = "usage: hack <system>|room <room>|pulse|stop";
            return false;
        }
        return Hacking::RunVerb(what, message);
    }

    // mind room <room>: our mind control on the enemy's crew in that room.
    static bool DoMind(const Command &cmd, std::string &message)
    {
        std::string what;
        for (size_t i = 1; i < cmd.args.size(); ++i) what += (i > 1 ? " " : "") + cmd.args[i];
        return Mind::RunVerb(what, message);
    }

    // teleport send <room> | teleport recall <room>: our teleporter, the enemy's room.
    static bool DoTeleport(const Command &cmd, std::string &message)
    {
        std::string what;
        for (size_t i = 1; i < cmd.args.size(); ++i) what += (i > 1 ? " " : "") + cmd.args[i];
        return Boarding::RunVerb(what, message);
    }

    // battery <ship> on|off: the backup battery's button.
    static bool DoBattery(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        bool on;
        if (!ArgOnOff(cmd, 2, on))
        {
            message = "usage: battery <ship> on|off";
            return false;
        }
        BatterySystem *battery = ship->batterySystem;
        if (!battery)
        {
            message = "ship has no battery";
            return false;
        }
        battery->SetTurnedOn(on, false);
        message = std::string("battery ") + (battery->bTurnedOn ? "on" : "off") + ", extra power " +
                  std::to_string(PowerManager::GetPowerManager(ship->iShipId)->batteryPower.second);
        return battery->bTurnedOn == on;
    }

    // drone <ship> <slot> <DRONE_BLUEPRINT>: puts that drone into the slot, replacing what is there (tests; in a duel,
    // before connecting, so the loadout carries it).
    static bool DoDrone(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int slot;
        if (!ArgInt(cmd, 2, slot) || cmd.raw.size() < 4)
        {
            message = "usage: drone <ship> <slot> <DRONE_BLUEPRINT>";
            return false;
        }
        if (!ship->droneSystem)
        {
            message = "ship has no drone control (install <ship> drones)";
            return false;
        }
        const std::string &name = cmd.raw[3];
        DroneBlueprint *blueprint = G_->GetBlueprints()->GetDroneBlueprint(name);
        if (!blueprint || blueprint->name != name)
        {
            message = "no drone blueprint " + name;
            return false;
        }
        std::vector<Drone*> drones = ship->GetDroneList();
        int count = (int)drones.size();
        if (slot < 0 || slot > count || slot >= ship->droneSystem->slot_count)
        {
            message = "slot " + std::to_string(slot) + " is not free or next (" + std::to_string(count) + " drones, " +
                      std::to_string(ship->droneSystem->slot_count) + " slots)";
            return false;
        }
        if (slot < count) ship->RemoveDrone(slot);
        ship->AddDrone(blueprint, slot);
        std::ostringstream out;
        out << "drones:";
        for (Drone *drone : ship->GetDroneList()) out << " " << (drone->blueprint ? drone->blueprint->name : "?");
        out << ", " << ship->GetDroneCount() << " drone parts";
        message = out.str();
        return true;
    }

    // droneparts <ship> <count>: how many drone parts the ship has.
    static bool DoDroneParts(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int count;
        if (!ArgInt(cmd, 2, count) || count < 0)
        {
            message = "usage: droneparts <ship> <count>";
            return false;
        }
        ship->ModifyDroneCount(count - ship->GetDroneCount());
        message = std::to_string(ship->GetDroneCount()) + " drone parts";
        return ship->GetDroneCount() == count;
    }

    // dronepower <ship> <slot> on|off: the drone's button (launching it costs a drone part, as in the game).
    static bool DoDronePower(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int slot;
        bool on;
        if (!ArgInt(cmd, 2, slot) || !ArgOnOff(cmd, 3, on))
        {
            message = "usage: dronepower <ship> <slot> on|off";
            return false;
        }
        std::vector<Drone*> drones = ship->droneSystem ? ship->GetDroneList() : std::vector<Drone*>();
        if (slot < 0 || slot >= (int)drones.size())
        {
            message = "no drone in slot " + std::to_string(slot);
            return false;
        }
        Drone *drone = drones[slot];
        if (on) ship->PowerDrone(drone, -1, true, false);
        else ship->DePowerDrone(drone, true);
        message = (drone->blueprint ? drone->blueprint->name : std::string("drone")) + (drone->powered ? " powered" : " unpowered") +
                  (drone->deployed ? ", deployed" : "") + ", " + std::to_string(ship->GetDroneCount()) + " drone parts";
        return drone->powered == on;
    }

    // supershield <ship> <layers>: a Zoltan super shield of that many layers (tests; Zoltan ships start with one).
    static bool DoSuperShield(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int layers;
        if (!ArgInt(cmd, 2, layers) || layers < 0)
        {
            message = "usage: supershield <ship> <layers>";
            return false;
        }
        if (!ship->shieldSystem)
        {
            message = "ship has no shields";
            return false;
        }
        ShieldPower &power = ship->shieldSystem->shields.power;
        power.super.second = std::max(power.super.second, layers);
        power.super.first = layers;
        message = "super shield " + std::to_string(power.super.first) + "/" + std::to_string(power.super.second);
        return true;
    }

    // nebula on|off: our ship as at a nebula beacon, or not (tests, in this game only). The beacon's event switches
    // the sensors off with a status effect (<status type="loss" system="sensors">), which this applies or clears.
    static bool DoNebula(const Command &cmd, std::string &message)
    {
        WorldManager *world = G_->GetWorld();
        ShipManager *ship = G_->GetShipManager(0);
        std::string mode = cmd.args.size() > 1 ? cmd.args[1] : "";
        if (!world || !ship || (mode != "on" && mode != "off"))
        {
            message = "usage: nebula on|off";
            return false;
        }
        world->space.bNebula = mode == "on";
        if (world->space.bNebula) ship->SetSystemPowerLoss(SYS_SENSORS, 1);
        else ship->ClearStatusSystem(SYS_SENSORS);
        ShipSystem *sensors = ship->GetSystem(SYS_SENSORS);
        message = std::string("nebula ") + (world->space.bNebula ? "on" : "off") +
                  (sensors ? ", sensors power " + std::to_string(sensors->powerState.first) : ", no sensors");
        return true;
    }

    // keys <text>: types into the game as the keyboard does (tests of the console). {f1}, {enter}, {esc}, {up},
    // {down} and {back} are those keys; everything else is typed as characters.
    static bool DoKeys(const Command &cmd, std::string &message)
    {
        CApp *app = G_->GetCApp();
        CommandGui *gui = app ? app->gui : nullptr;
        size_t at = cmd.text.find("keys");
        if (!gui || at == std::string::npos)
        {
            message = "usage: keys <text with {f1} {tab} {console} {chat} {enter} {esc} {up} {down} {back}>";
            return false;
        }
        std::string text = cmd.text.substr(at + 4);
        if (!text.empty() && text[0] == ' ') text.erase(0, 1);
        int typed = 0;
        for (size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] == '{')
            {
                size_t end = text.find('}', i);
                std::string key = end == std::string::npos ? "" : text.substr(i + 1, end - i - 1);
                if (key == "f1") gui->KeyDown(SDLK_F1, false);
                else if (key == "tab") gui->KeyDown(SDLK_TAB, false);
                else if (key == "console" || key == "chat")
                {
                    // The hotkey as set in Options > Controls; a letter key also arrives as a typed character.
                    SDLKey hotkey = Settings::GetHotkey(key == "chat" ? "duels_chat" : "console");
                    gui->KeyDown(hotkey, false);
                    if (hotkey >= 32 && hotkey < 127) gui->OnTextInput((int)hotkey);
                }
                else if (key == "esc") gui->KeyDown(SDLK_ESCAPE, false);
                else if (key == "up") gui->KeyDown(SDLK_UP, false);
                else if (key == "down") gui->KeyDown(SDLK_DOWN, false);
                else if (key == "enter") gui->OnTextEvent(CEvent::TEXT_CONFIRM);
                else if (key == "back") gui->OnTextEvent(CEvent::TEXT_BACKSPACE);
                else
                {
                    message = "unknown key {" + key + "}";
                    return false;
                }
                i = end;
                continue;
            }
            gui->OnTextInput((unsigned char)text[i]);
            ++typed;
        }
        message = std::to_string(typed) + " characters typed";
        return true;
    }

    // ionize <ship> <system> <amount>: ion damage to a system, as an ion shot does (tests of the ion lock). The system
    // may also be a custom one by its name (weapon_bay_2).
    static bool DoIonize(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int system = cmd.args.size() > 2 ? ParseSystem(cmd.args[2]) : -1;
        if (system < 0 && cmd.args.size() > 2 && ShipSystem::NameToSystemId(cmd.args[2]) >= SYS_CUSTOM_FIRST)
        {
            system = ShipSystem::NameToSystemId(cmd.args[2]);
        }
        int amount;
        if (system < 0 || !ArgInt(cmd, 3, amount) || amount < 1)
        {
            message = "usage: ionize <ship> <system> <amount>";
            return false;
        }
        const std::string name = ShipSystem::SystemIdToName(system);
        bool has = system < (int)ship->systemKey.size() && ship->systemKey[system] >= 0;
        ShipSystem *target = has ? ship->GetSystem(system) : nullptr;
        if (!target)
        {
            message = "ship has no " + name;
            return false;
        }
        target->IonDamage(amount);
        message = name + " lock " + std::to_string(target->iLockCount) + ", power " + std::to_string(target->powerState.first);
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

    // crewpower <index> [power]: our crew member (as "crew" counts them) uses a Hyperspace crew power (the crystal
    // crew's lockdown), as its button would, if it is ready.
    static bool DoCrewPower(const Command &cmd, std::string &message)
    {
        ShipManager *ship = G_->GetShipManager(0);
        int index, power = 0;
        if (!ship || !ArgInt(cmd, 1, index) || (cmd.args.size() > 2 && !ArgInt(cmd, 2, power)))
        {
            message = "usage: crewpower <index> [power]";
            return false;
        }
        std::vector<CrewMember*> crew = OwnCrew(ship);
        if (index < 0 || index >= (int)crew.size())
        {
            message = "no living crew member " + std::to_string(index);
            return false;
        }
        CrewMember *member = crew[index];
        const std::vector<ActivatedPower*> &powers = CM_EX(member)->crewPowers;
        if (power < 0 || power >= (int)powers.size())
        {
            message = member->species + " #" + std::to_string(index) + " has no power " + std::to_string(power);
            return false;
        }
        PowerReadyState ready = powers[power]->PowerReady();
        if (ready != POWER_READY)
        {
            message = member->species + " #" + std::to_string(index) + " power " + std::to_string(power) + " not ready (" +
                      std::to_string((int)ready) + ")";
            return false;
        }
        powers[power]->PreparePower();
        message = member->species + " #" + std::to_string(index) + " used power " + std::to_string(power) + " in room " +
                  std::to_string(member->iRoomId) + " of ship " + std::to_string(member->currentShipId);
        return true;
    }

    static bool DoDoor(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        int doorId;
        bool open;
        bool all = ArgIs(cmd, 2, "all");
        if ((!all && !ArgInt(cmd, 2, doorId)) || !ArgOnOff(cmd, 3, open))
        {
            message = "usage: door <ship> <door id>|all open|close";
            return false;
        }
        if (all)
        {
            // Every door, the airlocks too (a ship losing its air, for tests).
            int count = 0;
            for (const std::vector<Door*> *list : {&ship->ship.vDoorList, &ship->ship.vOuterAirlocks})
            {
                for (Door *door : *list)
                {
                    if (open) door->Open();
                    else door->Close();
                    ++count;
                }
            }
            message = std::to_string(count) + (open ? " doors opened" : " doors closed") + " (" +
                      std::to_string(ship->ship.vOuterAirlocks.size()) + " airlocks)";
            return true;
        }
        // Inner doors, then the airlocks (FTL keeps those to space apart).
        for (const std::vector<Door*> *list : {&ship->ship.vDoorList, &ship->ship.vOuterAirlocks})
        {
            for (Door *door : *list)
            {
                if (door->iDoorId != doorId) continue;
                if (open) door->Open();
                else door->Close();
                message = (list == &ship->ship.vOuterAirlocks ? "airlock " : "door ") + std::to_string(doorId) +
                          (door->bOpen ? " open" : " closed");
                return door->bOpen == open;
            }
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

    // swap <slot> <slot>: two of our weapons change places, as when the player drags one onto the other.
    static bool DoSwap(const Command &cmd, std::string &message)
    {
        int a, b;
        ShipManager *ship = G_->GetShipManager(0);
        if (!ArgInt(cmd, 1, a) || !ArgInt(cmd, 2, b))
        {
            message = "usage: swap <slot> <slot>";
            return false;
        }
        int count = ship && ship->weaponSystem ? (int)ship->GetWeaponList().size() : 0;
        if (a < 0 || b < 0 || a >= count || b >= count || a == b)
        {
            message = "no such weapon slots (" + std::to_string(count) + " weapons)";
            return false;
        }
        G_->GetCApp()->gui->combatControl.weapControl.SwapArmaments((unsigned)a, (unsigned)b);
        message = "weapons:";
        for (ProjectileFactory *weapon : ship->GetWeaponList()) message += " " + (weapon->blueprint ? weapon->blueprint->name : "?");
        return true;
    }

    // rooms <ship>: the ship's rooms (tiles), their consoles and systems (the weapon bays' cut, DuelsBays.cpp).
    static bool DoRooms(const Command &cmd, std::string &message)
    {
        ShipManager *ship = ArgShip(cmd, 1, message);
        if (!ship) return false;
        std::string text = Bays::Describe(ship);
        size_t start = 0;
        while (start <= text.size())
        {
            size_t end = text.find('\n', start);
            Log("%s", text.substr(start, end == std::string::npos ? std::string::npos : end - start).c_str());
            if (end == std::string::npos) break;
            start = end + 1;
        }
        message = "rooms of ship " + std::to_string(ship->iShipId) + " in the log";
        return true;
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
            Log("  shield layers %d charge %.2f super %d/%d", ship->shieldSystem->shields.power.first,
                ship->shieldSystem->shields.charger, ship->shieldSystem->shields.power.super.first,
                ship->shieldSystem->shields.power.super.second);
        }
        if (ship->droneSystem)
        {
            std::vector<Drone*> drones = ship->GetDroneList();
            Log("  drone parts %d, %u drones", ship->GetDroneCount(), (unsigned)drones.size());
            for (size_t slot = 0; slot < drones.size(); ++slot)
            {
                Drone *drone = drones[slot];
                Log("  drone %u %-18s type %d power %d %s%s%s", (unsigned)slot,
                    drone->blueprint ? drone->blueprint->name.c_str() : "?", drone->type, drone->powerRequired,
                    drone->powered ? "on " : "off", drone->deployed ? " deployed" : "", drone->bDead ? " dead" : "");
            }
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
        for (size_t index = 0; index < ship->artillerySystems.size(); ++index)
        {
            ProjectileFactory *weapon = ship->artillerySystems[index] ? ship->artillerySystems[index]->projectileFactory : nullptr;
            if (!weapon) continue;
            Log("  artillery %u %-15s type %d %s charge %.2f/%.2f", (unsigned)index, weapon->name.c_str(),
                weapon->blueprint ? weapon->blueprint->type : -1, weapon->powered ? "on " : "off", weapon->cooldown.first,
                weapon->cooldown.second);
        }
        std::vector<CrewMember*> crew = OwnCrew(ship);
        for (size_t index = 0; index < crew.size(); ++index)
        {
            CrewMember *member = crew[index];
            Log("  crew %u %-8s on ship %d room %2d health %.0f/%.0f%s", (unsigned)index, member->species.c_str(),
                member->currentShipId, member->iRoomId, member->health.first, member->health.second,
                member->fStunTime > 0.f ? (" stunned " + std::to_string((int)std::ceil(member->fStunTime)) + " s").c_str() : "");
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
        MapToTestDisplay(x, y, width, height);   // test runs on another monitor
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
        if (verb == "install") return DoInstall(cmd, message);
        if (verb == "battery") return DoBattery(cmd, message);
        if (verb == "augment") return DoAugment(cmd, message);
        if (verb == "hack") return DoHack(cmd, message);
        if (verb == "mind") return DoMind(cmd, message);
        if (verb == "teleport") return DoTeleport(cmd, message);
        if (verb == "ionize") return DoIonize(cmd, message);
        if (verb == "drone") return DoDrone(cmd, message);
        if (verb == "droneparts") return DoDroneParts(cmd, message);
        if (verb == "supershield") return DoSuperShield(cmd, message);
        if (verb == "nebula") return DoNebula(cmd, message);
        if (verb == "keys") return DoKeys(cmd, message);
        if (verb == "dronepower") return DoDronePower(cmd, message);
        if (verb == "fire") return DoFire(cmd, message);
        if (verb == "autofire") return DoAutofire(cmd, message);
        if (verb == "crew") return DoCrew(cmd, message);
        if (verb == "crewpower") return DoCrewPower(cmd, message);
        if (verb == "door") return DoDoor(cmd, message);
        if (verb == "cloak") return DoCloak(cmd, message);
        if (verb == "spawn") return DoSpawn(cmd, message);
        if (verb == "export") return DoExport(cmd, message);
        if (verb == "import") return DoImport(cmd, message);
        if (verb == "describe") return DoDescribe(cmd, message);
        if (verb == "rooms") return DoRooms(cmd, message);
        if (verb == "swap") return DoSwap(cmd, message);
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
