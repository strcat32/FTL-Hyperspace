#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsMatch.h"
#include "DuelsShipControl.h"

#include <boost/algorithm/string.hpp>

// ---------------------------------------------------------------------------------------------
// Frame tick and loop counters
// ---------------------------------------------------------------------------------------------

// Outermost CApp::OnLoop hook, so the per-frame work sees everything the frame did.
HOOK_METHOD_PRIORITY(CApp, OnLoop, -5000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CApp::OnLoop -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::OnFrame();
}

// Keeps running while the window is unfocused, unlike parts of the main loop.
HOOK_METHOD(CApp, GenInputEvents, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::GenInputEvents -> Begin (DuelsHooks.cpp)\n")
    ++Duels::GetState().inputPumps;
    super();
}

HOOK_METHOD(WorldManager, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::OnLoop -> Begin (DuelsHooks.cpp)\n")
    Duels::State &state = Duels::GetState();
    ++state.worldLoops;
    // Raw field on purpose: GetSpeedFactor() is rescaled by Hyperspace's time dilation hooks.
    state.gameTime += G_->GetCFPS()->SpeedFactor * 0.0625;
    super();
}

HOOK_METHOD(WorldManager, PauseLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::PauseLoop -> Begin (DuelsHooks.cpp)\n")
    ++Duels::GetState().pauseLoops;
    super();
}

HOOK_METHOD(CApp, OnExit, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnExit -> Begin (DuelsHooks.cpp)\n")
    Duels::Log("Game exiting");
    Duels::Shutdown();
    super();
}

// ---------------------------------------------------------------------------------------------
// Console: "DUEL <verb> <args...>"
// ---------------------------------------------------------------------------------------------

// Outer to Hyperspace's own RunCommand hook, so DUEL never reaches the vanilla console.
HOOK_METHOD_PRIORITY(CommandGui, RunCommand, -100, (std::string& command) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RunCommand -> Begin (DuelsHooks.cpp)\n")
    std::string name = command.substr(0, command.find(' '));
    boost::to_upper(name);
    if (name != "DUEL")
    {
        super(command);
        return;
    }

    std::string message;
    Duels::Command cmd;
    if (!Duels::ParseCommand(command.size() > 4 ? command.substr(4) : "", cmd))
    {
        message = "usage: DUEL <verb> <args...>";
    }
    else
    {
        bool ok = Duels::Execute(cmd, message);
        if (message.empty()) message = ok ? "ok" : "failed";
    }
    Duels::Log("console: %s -> %s", command.c_str(), message.c_str());
    PrintHelper::GetInstance()->AddMessage("DUEL: " + message);
}

// ---------------------------------------------------------------------------------------------
// No-pause: every pause source is blocked while Duels::State::noPause is set.
// Layered on purpose: callers may use IsPaused(), SetPaused() or read the flags directly.
// ---------------------------------------------------------------------------------------------

HOOK_METHOD_PRIORITY(CommandGui, IsPaused, -1000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::IsPaused -> Begin (DuelsHooks.cpp)\n")
    if (Duels::GetState().noPause) return false;
    return super();
}

HOOK_METHOD_PRIORITY(CommandGui, SetPaused, -1000, (bool val, bool isAutoPause) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::SetPaused -> Begin (DuelsHooks.cpp)\n")
    if (val && Duels::GetState().noPause)
    {
        ++Duels::GetState().blockedPauses;
        return;
    }
    super(val, isAutoPause);
}

HOOK_METHOD_PRIORITY(CommandGui, OnLoop, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::OnLoop -> Begin (DuelsHooks.cpp)\n")
    super();
    if (!Duels::GetState().noPause) return;

    if (bPaused || bAutoPaused || menu_pause || event_pause || touch_pause)
    {
        ++Duels::GetState().blockedPauses;
        bPaused = false;
        bAutoPaused = false;
        menu_pause = false;
        event_pause = false;
        touch_pause = false;
    }
}

// ---------------------------------------------------------------------------------------------
// Enemy ship driven by commands instead of its AI
// ---------------------------------------------------------------------------------------------

// Outermost, so no AI decision (power, weapons, crew tasks, surrender, escape) runs for a replaced ship.
// ShipAI also runs for the player ship, hence the ship id filter.
HOOK_METHOD_PRIORITY(ShipAI, OnLoop, -10000, (bool hostile) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipAI::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!playerShip && ship && ship->iShipId == 1 && Duels::GetState().aiOff[1])
    {
        Duels::OnAiTakeover(ship);
        return;
    }
    super(hostile);
}

// ShipManager::OnLoop re-powers enemy systems every frame through CheckForRepower, which undid every power
// change we made (Step 1 finding). A replaced ship's power belongs to its owner, so skip it for ship 1.
HOOK_METHOD_PRIORITY(ShipSystem, CheckForRepower, -10000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::CheckForRepower -> Begin (DuelsHooks.cpp)\n")
    if (_shipObj.iShipId == 1 && Duels::GetState().aiOff[1]) return;
    super();
}

// Game speed must stay at normal: under split authority a faster client would charge weapons faster.
HOOK_METHOD_PRIORITY(CFPS, OnLoop, -1000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CFPS::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (Duels::GetState().noPause)
    {
        speedEnabled = false;
        speedLevel = 0;
    }
    super();
}

// ---------------------------------------------------------------------------------------------
// Network duel (DuelsMatch.cpp): shots, their timing, and hits decided by the defender.
// All outermost, so the game's and Hyperspace's own code runs inside, unchanged, whenever the duel isn't involved.
// ---------------------------------------------------------------------------------------------

// A projectile leaves one of our weapons: tell the opponent (exact target point, spawn time).
HOOK_METHOD_PRIORITY(ProjectileFactory, GetProjectile, -2000, () -> Projectile*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ProjectileFactory::GetProjectile -> Begin (DuelsHooks.cpp)\n")
    Projectile *projectile = super();
    if (projectile && iShipId == 0) Duels::Match::OnOwnProjectile(this, projectile);
    return projectile;
}

// Timing of the opponent's shots on our screen: 0 updates = wait, more than 1 = catch up.
HOOK_METHOD_PRIORITY(SpaceManager, UpdateProjectile, -2000, (Projectile *projectile) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::UpdateProjectile -> Begin (DuelsHooks.cpp)\n")
    int runs = Duels::Match::ProjectileUpdates(projectile);
    int space = projectile->currentSpace;
    for (int run = 0; run < runs; ++run)
    {
        super(projectile);
        if (projectile->dead || projectile->currentSpace != space) break;
    }
}

// Our shot at the replica waits for the defender's verdict, then plays out that verdict.
HOOK_METHOD_PRIORITY(Projectile, CollisionCheck, -2000, (Collideable *other) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Projectile::CollisionCheck -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::BeginCollisionCheck(this, other)) return;
    super(other);
    Duels::Match::EndCollisionCheck();
}

HOOK_METHOD_PRIORITY(ShipManager, CollisionShield, -2000, (Pointf start, Pointf finish, Damage damage, bool raytrace) -> CollisionResponse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::CollisionShield -> Begin (DuelsHooks.cpp)\n")
    CollisionResponse forced;
    if (Duels::Match::ForcedShieldResponse(this, start, finish, damage, forced)) return forced;
    CollisionResponse response = super(start, finish, damage, raytrace);
    Duels::Match::ObserveShield(this, response);
    return response;
}

HOOK_METHOD_PRIORITY(ShipManager, DamageArea, -2000, (Pointf location, Damage dmg, bool forceHit) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageArea -> Begin (DuelsHooks.cpp)\n")
    bool hit = false;
    if (Duels::Match::ForcedDamageArea(this, location, hit)) return hit;
    int hullBefore = ship.hullIntegrity.first;
    hit = super(location, dmg, forceHit);
    Duels::Match::ObserveDamageArea(this, hit, hullBefore);
    return hit;
}
