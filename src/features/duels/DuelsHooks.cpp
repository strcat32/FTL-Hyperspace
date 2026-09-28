#include "Global.h"
#include "CommandConsole.h"
#include "Duels.h"
#include "DuelsDrones.h"
#include "DuelsMatch.h"
#include "DuelsScreen.h"
#include "DuelsShipControl.h"
#include "DuelsView.h"

#include <boost/algorithm/string.hpp>
#include <cmath>

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
        if (projectile->dead || projectile->startedDeath || projectile->currentSpace != space) break;
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

// Bombs override the collision check; ours in the replica waits there for the defender's verdict.
HOOK_METHOD_PRIORITY(BombProjectile, CollisionCheck, -2000, (Collideable *other) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> BombProjectile::CollisionCheck -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Match::BeginBombCheck(this, other)) return;
    super(other);
    Duels::Match::EndCollisionCheck();
}

// A bomb's dodge roll, when it appears in its target room.
HOOK_METHOD_PRIORITY(ShipManager, GetDodged, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::GetDodged -> Begin (DuelsHooks.cpp)\n")
    bool dodged = false;
    if (Duels::Match::ForcedDodge(this, dodged)) return dodged;
    dodged = super();
    Duels::Match::ObserveDodge(this, dodged);
    return dodged;
}

// A beam's sweep over the rooms behind the shields.
HOOK_METHOD_PRIORITY(ShipManager, DamageBeam, -2000, (Pointf location1, Pointf location2, Damage dmg) -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageBeam -> Begin (DuelsHooks.cpp)\n")
    Duels::Match::MuteBeamDamage(this, dmg);
    int hullBefore = ship.hullIntegrity.first;
    bool hit = super(location1, location2, dmg);
    Duels::Match::ObserveBeam(this, hit, hullBefore);
    return hit;
}

// ---------------------------------------------------------------------------------------------
// Drones in a network duel (DuelsDrones.cpp): our drones act; the replica's drones are puppets that follow their owner.
// ---------------------------------------------------------------------------------------------

// A drone's shot (combat and defense drones): ours at the replica goes like a weapon's shot, a defense drone's in our
// space is shown to the opponent. A puppet never fires on its own.
HOOK_METHOD_PRIORITY(SpaceDrone, GetNextProjectile, -2000, () -> Projectile*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceDrone::GetNextProjectile -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::MayFire(this)) return nullptr;
    Projectile *projectile = super();
    if (projectile) Duels::Match::OnOwnDroneProjectile(this, projectile);
    return projectile;
}

// After every drone moved this frame: the puppets take their owners' positions (the frame is drawn after this).
HOOK_METHOD_PRIORITY(SpaceManager, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceManager::OnLoop -> Begin (DuelsHooks.cpp)\n")
    super();
    Duels::Drones::AfterSpaceLoop();
}

// A projectile runs into a drone: we report hits on the opponent's drones in our space.
HOOK_METHOD_PRIORITY(SpaceDrone, CollisionMoving, -2000, (Pointf start, Pointf finish, Damage damage, bool raytrace) -> CollisionResponse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SpaceDrone::CollisionMoving -> Begin (DuelsHooks.cpp)\n")
    bool exploding = explosion.tracker.running;
    float ionStunBefore = ionStun;
    CollisionResponse response = super(start, finish, damage, raytrace);
    Duels::Drones::ObserveDroneCollision(this, exploding, ionStunBefore);
    return response;
}

// A stunned or hacked drone may explode each second: that roll belongs to the drone's owner.
HOOK_METHOD_PRIORITY(Drone, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Drone::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::RunsOwnLoop(this)) return;
    super();
}

// The replica's hull repair drone repairs nothing here: the owner's hull comes with the state.
HOOK_METHOD_PRIORITY(ShipManager, DamageTarget, -2000, (Pointf location, Damage damage) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipManager::DamageTarget -> Begin (DuelsHooks.cpp)\n")
    if (Duels::Drones::BlocksReplicaRepair(this, damage.iDamage)) return;
    super(location, damage);
}

// Nor does the replica's shield drone add super shields here (Hyperspace rewrites this loop; its pulse still shows).
HOOK_METHOD_PRIORITY(SuperShieldDrone, OnLoop, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> SuperShieldDrone::OnLoop -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::Drones::IsPuppet(this) || !shieldSystem)
    {
        super();
        return;
    }
    std::pair<int, int> superShield = shieldSystem->shields.power.super;
    super();
    shieldSystem->shields.power.super = superShield;
}

// ---------------------------------------------------------------------------------------------
// Duel view (DuelsView.cpp): both ships at one scale, the opponent mirrored in a grown enemy window, mouse input
// mapped back to each ship's coordinates.
// ---------------------------------------------------------------------------------------------

HOOK_METHOD_PRIORITY(CombatControl, RenderTarget, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::RenderTarget -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginTarget();
    Duels::View::BeginDecorations();
    super();
    Duels::View::EndDecorations();
    Duels::View::EndTarget();
}

// Hyperspace's ship icons and event timers in the enemy window show tooltips from here.
HOOK_METHOD_PRIORITY(CommandGui, MouseMove, -2000, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::MouseMove -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginDecorations();
    super(mX, mY);
    Duels::View::EndDecorations();
}

HOOK_METHOD_PRIORITY(CachedPrimitive, OnRender, -2000, (const GL_Color &color) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CachedPrimitive::OnRender -> Begin (DuelsHooks.cpp)\n")
    float x, y;
    // The teleport, hacking and mind-control markers on the opponent's rooms, the right way round.
    if (Duels::View::TargetMarkerCenter(this, x, y) && Duels::View::BeginUnmirrored(x, y, 0.f))
    {
        super(color);
        Duels::View::EndUnmirrored();
        return;
    }
    if (!Duels::View::HullBarShift(this, x, y)) return super(color);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(x, y, 0.f);
    super(color);
    CSurface::GL_PopMatrix();
}

// The system icons in the opponent's rooms, the right way round and at a readable size. (FTL builds each icon at its
// room's centre, so it turns around that.)
HOOK_METHOD_PRIORITY(ShipSystem, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> ShipSystem::OnRender -> Begin (DuelsHooks.cpp)\n")
    float x, y;
    if (!Duels::View::TargetRoomCenter(_shipObj.iShipId, roomId, x, y) || !Duels::View::BeginUnmirrored(x, y, 0.8f))
    {
        return super();
    }
    super();
    Duels::View::EndUnmirrored();
}

// The crosshairs on the opponent's rooms, numbered by weapon: Hyperspace draws each at the local origin, translated to
// the target point (AdditionalWeaponSlots.cpp). The flak radius and beam lines are drawn otherwise.
HOOK_METHOD_PRIORITY(WeaponControl, RenderAiming, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponControl::RenderAiming -> Begin (DuelsHooks.cpp)\n")
    Duels::View::SetAiming(true);
    super();
    Duels::View::SetAiming(false);
}

HOOK_STATIC_PRIORITY(CSurface, GL_RenderPrimitive, -2000, (GL_Primitive *primitive) -> void)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> CSurface::GL_RenderPrimitive -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Aiming() || !primitive || !primitive->hasTexture || !Duels::View::BeginUnmirrored(0.f, 0.f, 0.f))
    {
        return super(primitive);
    }
    super(primitive);
    Duels::View::EndUnmirrored();
}

HOOK_METHOD_PRIORITY(CommandGui, RenderPlayerShip, -2000, (Point &shipCenter, float jumpScale) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::RenderPlayerShip -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginPlayerShip();
    super(shipCenter, jumpScale);
    Duels::View::EndPlayerShip();
}

// Inside RenderTarget's and RenderPlayerShip's own push and translate: the transform covers the ship, its space
// (projectiles), and the charge bars and aiming marks drawn after it; their pop removes it.
HOOK_METHOD_PRIORITY(CompleteShip, OnRenderShip, -2000, (bool unk1, bool unk2) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CompleteShip::OnRenderShip -> Begin (DuelsHooks.cpp)\n")
    Duels::View::ApplyShipTransform(shipManager);
    super(unk1, unk2);
}

// "MISS", damage numbers: readable, at normal size, although drawn inside a scaled or mirrored ship.
HOOK_METHOD_PRIORITY(DamageMessage, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> DamageMessage::OnRender -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Transforming())
    {
        super();
        return;
    }
    float sx, sy;
    Duels::View::InverseScale(sx, sy);
    CSurface::GL_PushMatrix();
    CSurface::GL_Translate(position.x, position.y, 0.f);
    CSurface::GL_Scale(sx, sy, 1.f);
    CSurface::GL_Translate(-position.x, -position.y, 0.f);
    super();
    CSurface::GL_PopMatrix();
}

extern Point g_enemyShipCorner;   // Hyperspace, CustomWeapons.cpp

// Hyperspace keeps the opponent's weapon charge bars on screen, reckoning with where FTL would draw its ship; ours is
// scaled and mirrored inside the window. Its check passes while our transform is on.
HOOK_METHOD_PRIORITY(WeaponAnimation, RenderChargeBar, -2000, (float alpha) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> WeaponAnimation::RenderChargeBar -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::Transforming()) return super(alpha);
    Point saved = g_enemyShipCorner;
    int width = (int)(anim.info.frameWidth * anim.fScale);
    int barX = bMirrored ? renderPoint.x + mountPoint.x - width - 18 : renderPoint.x - mountPoint.x + width + 10;
    int barY = renderPoint.y - mountPoint.y;
    g_enemyShipCorner = Point(640 - barX, 360 - barY);
    super(alpha);
    g_enemyShipCorner = saved;
}

// The enemy window: its size, its frame, and the opponent's system boxes at their usual place.
HOOK_METHOD_PRIORITY(CombatControl, GetHostileBoxSize, -2000, () -> Point)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetHostileBoxSize -> Begin (DuelsHooks.cpp)\n")
    Point size = super();
    Duels::View::AdjustHostileBoxSize(this, size);
    return size;
}

HOOK_METHOD_PRIORITY(CombatControl, DrawHostileBox, -2000, (GL_Color color, int stencilBit) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::DrawHostileBox -> Begin (DuelsHooks.cpp)\n")
    if (!Duels::View::DrawHostileBox(this, color, stencilBit)) super(color, stencilBit);
}

HOOK_METHOD_PRIORITY(CombatControl, UpdateSysBoxes, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::UpdateSysBoxes -> Begin (DuelsHooks.cpp)\n")
    Duels::View::BeginSysBoxes(this);
    super();
    Duels::View::EndSysBoxes(this);
}

namespace
{
    // A beam weapon whose first point is placed: its line follows the mouse, in or out of the enemy window.
    bool AimingBeam(CombatControl *combat)
    {
        ProjectileFactory *weapon = combat->weapControl.armedWeapon;
        return weapon && weapon->blueprint && weapon->blueprint->type == 2 && !combat->aimingPoints.empty();
    }

    // A point in the opponent's ship coordinates as the screen point that FTL's own "minus the ship's origin" expects.
    Point TargetScreenPoint(CombatControl *combat, float shipX, float shipY)
    {
        return Point(combat->position.x + combat->targetPosition.x + (int)std::floor(shipX),
                     combat->position.y + combat->targetPosition.y + (int)std::floor(shipY));
    }
}

// FTL reads lastMouse minus a ship's position here: the opponent's rooms and the beam line (UpdateAiming), and our
// drones and rooms (weapons aimed at our own ship). The weapon buttons and system boxes got the real point before.
HOOK_METHOD_PRIORITY(CombatControl, UpdateTarget, -2000, () -> bool)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::UpdateTarget -> Begin (DuelsHooks.cpp)\n")
    Pointf real = lastMouse;
    float targetX, targetY, ownX, ownY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint(real.x, real.y, targetX, targetY, inside) ||
        !Duels::View::OwnShipPoint(real.x, real.y, ownX, ownY))
    {
        return super();
    }

    bool atTarget = inside || AimingBeam(this);
    lastMouse = atTarget ? Pointf(position.x + targetPosition.x + targetX, position.y + targetPosition.y + targetY)
                         : Pointf(playerShipPosition.x + ownX, playerShipPosition.y + ownY);
    bool result = super();
    lastMouse = real;

    // The other ship's arithmetic ran on a point that isn't over that ship: nothing of it is under the mouse.
    if (atTarget)
    {
        currentDrone = nullptr;
        if (selectedSelfRoom != -1 && shipManager) shipManager->ship.SetSelectedRoom(-1);
        selectedSelfRoom = -1;
    }
    else
    {
        if (selectedRoom != -1 && currentTarget && currentTarget->shipManager) currentTarget->shipManager->ship.SetSelectedRoom(-1);
        selectedRoom = -1;
    }
    return result;
}

HOOK_METHOD_PRIORITY(CombatControl, GetSelectedCrew, -2000, (int mX, int mY) -> CrewMember*)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetSelectedCrew -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint((float)mX, (float)mY, shipX, shipY, inside)) return super(mX, mY);
    if (!inside) return nullptr;
    Point point = TargetScreenPoint(this, shipX, shipY);
    return super(point.x, point.y);
}

HOOK_METHOD_PRIORITY(CombatControl, GetSelectedCrew, -2000, (int x, int y, int firstX, int firstY) -> std::vector<CrewMember*>)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetSelectedCrew (area) -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY, firstShipX, firstShipY;
    bool inside = false, firstInside = false;
    if (!Duels::View::TargetShipPoint((float)x, (float)y, shipX, shipY, inside) ||
        !Duels::View::TargetShipPoint((float)firstX, (float)firstY, firstShipX, firstShipY, firstInside))
    {
        return super(x, y, firstX, firstY);
    }
    Point point = TargetScreenPoint(this, shipX, shipY);
    Point first = TargetScreenPoint(this, firstShipX, firstShipY);
    return super(point.x, point.y, first.x, first.y);
}

HOOK_METHOD_PRIORITY(CombatControl, GetCrewTooltip, -2000, (int x, int y) -> std::string)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CombatControl::GetCrewTooltip -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (!Duels::View::TargetShipPoint((float)x, (float)y, shipX, shipY, inside)) return super(x, y);
    if (!inside) return std::string();
    Point point = TargetScreenPoint(this, shipX, shipY);
    return super(point.x, point.y);
}

// Our ship: crew selection, crew orders, doors and tooltips work in its coordinates, which FTL computes as the mouse
// minus the ship's position, in CommandGui::MouseMove (passed on here) and GetWorldCoordinates (for clicks).
HOOK_METHOD_PRIORITY(CrewControl, MouseMove, -2000, (int mX, int mY, int wX, int wY) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CrewControl::MouseMove -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    if (Duels::View::OwnShipPoint((float)mX, (float)mY, shipX, shipY))
    {
        wX = (int)std::floor(shipX);
        wY = (int)std::floor(shipY);
    }
    super(mX, mY, wX, wY);
}

HOOK_METHOD_PRIORITY(CommandGui, GetWorldCoordinates, -2000, (Point point, bool fromTarget) -> Point)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::GetWorldCoordinates -> Begin (DuelsHooks.cpp)\n")
    float shipX, shipY;
    bool inside = false;
    if (fromTarget && combatControl.currentTarget)
    {
        if (Duels::View::TargetShipPoint((float)point.x, (float)point.y, shipX, shipY, inside))
        {
            return Point((int)std::floor(shipX), (int)std::floor(shipY));
        }
    }
    else if (Duels::View::OwnShipPoint((float)point.x, (float)point.y, shipX, shipY))
    {
        return Point((int)std::floor(shipX), (int)std::floor(shipY));
    }
    return super(point, fromTarget);
}

// ---------------------------------------------------------------------------------------------
// Console: F1 opens it on every keyboard. Hyperspace's key is "\", which German and many other layouts only
// produce with AltGr, and FTL doesn't see it there. A ">" marks the input line.
// ---------------------------------------------------------------------------------------------

namespace Duels
{
    // The same conditions as Hyperspace's own console key (CommandConsole.cpp).
    bool OpenConsole(CommandGui *gui)
    {
        CommandConsole *console = CommandConsole::GetInstance();
        if (!console->enabled || gui->inputBox.bOpen) return false;
        if (gui->writeErrorDialog.bOpen || gui->menuBox.bOpen || gui->gameOverScreen.bOpen) return false;
        if (gui->shipComplete && gui->shipComplete->shipManager && gui->shipComplete->shipManager->bJumping) return false;
        for (FocusWindow *window : gui->focusWindows)
        {
            if (window->bOpen) return false;
        }
        gui->inputBox.StartInput();
        return true;
    }
}

HOOK_METHOD_PRIORITY(CommandGui, KeyDown, -2000, (SDLKey key, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> CommandGui::KeyDown -> Begin (DuelsHooks.cpp)\n")
    if (key == SDLK_F1 && Duels::OpenConsole(this)) return;
    super(key, shiftHeld);
}

// The console's text line (Hyperspace creates it without a prompt) starts with "> ".
HOOK_METHOD_PRIORITY(InputBox, OnRender, -2000, () -> void)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> InputBox::OnRender -> Begin (DuelsHooks.cpp)\n")
    struct TextInput *input = CommandConsole::GetInstance()->textInput;   // "struct": InputBox has a TextInput() method
    if (input && input->prompt.empty()) input->prompt = "> ";
    super();
}

// A player ship as the duel opponent keeps the player's shield position (vanilla adds 110 px for enemies).
HOOK_METHOD_PRIORITY(Ship, GetBaseEllipse, -2000, () -> Globals::Ellipse)
{
    LOG_HOOK("HOOK_METHOD_PRIORITY -> Ship::GetBaseEllipse -> Begin (DuelsHooks.cpp)\n")
    Globals::Ellipse ellipse = super();
    if (Duels::View::PlayerShieldPosition(iShipId)) ellipse.center.y -= 110;
    return ellipse;
}

// The enemy window's header texts (ship class, relationship) clear the hull bar of a player ship.
HOOK_STATIC_PRIORITY(freetype, easy_printRightAlign, -2000, (int fontSize, float x, float y, const std::string &text) -> Pointf)
{
    LOG_HOOK("HOOK_STATIC_PRIORITY -> freetype::easy_printRightAlign -> Begin (DuelsHooks.cpp)\n")
    std::string label;
    if (Duels::Screen::VersionLabel(x, y, text, label)) return super(fontSize, x, y, label);
    Duels::View::AdjustHeaderText(fontSize, x, y, text);
    return super(fontSize, x, y, text);
}
