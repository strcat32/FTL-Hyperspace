#pragma once

#include <string>

struct CachedPrimitive;
struct CombatControl;
struct GL_Color;
struct Point;
struct ShipManager;

// How a duel looks: both ships at the same scale, facing each other, in FTL's own 1280 x 720 screen.
//
// FTL draws the player's ship at full size on the left and the enemy in a window on the right, made for enemy ships,
// which FTL draws upright. A duel opponent flies a player ship, which is wide and would be cut off. So in a duel both
// ships are drawn at one common scale, as large as the pair fits (1.0 for small ships, about 0.84 for two Kestrels):
// our ship shrinks towards its left edge, the enemy window grows to the left into the room that frees (and moves
// down below the jump and menu buttons when it reaches them; its bottom stays), and the opponent is drawn in it
// mirrored, so it faces us and its shots leave towards us. Both ships share one horizontal center line.
//
// Mouse input goes through the inverse transforms, so aiming, crew orders and doors work as usual.
//
// Modes: "auto" (default) for the duel opponent only; "fit" for any enemy (to check ships in single player); "off"
// is vanilla.
namespace Duels
{
    namespace View
    {
        enum class Mode
        {
            Off,
            Auto,
            Fit
        };

        void SetMode(Mode mode);
        Mode GetMode();
        // Set by the match layer: ship 1 is a duel opponent's replica.
        void SetDuelOpponent(bool present);

        // Once per frame, before input and drawing: computes the layout and moves and sizes the enemy window.
        void OnFrame();
        // A duel layout is in use.
        bool Active();

        // Drawing. CombatControl::RenderTarget and CommandGui::RenderPlayerShip bracket the two ships' drawing;
        // CompleteShip::OnRenderShip applies the ship's transform inside their own push and pop, so it also covers
        // the aiming marks drawn after the ship.
        void BeginTarget();
        void EndTarget();
        void BeginPlayerShip();
        void EndPlayerShip();
        void ApplyShipTransform(ShipManager *ship);
        // True while a scaled or mirrored ship is drawn; InverseScale undoes that locally (for text drawn there).
        bool Transforming();
        void InverseScale(float &sx, float &sy);

        // The enemy window: its size (CombatControl::GetHostileBoxSize), its frame (DrawHostileBox, false: draw
        // FTL's), and FTL's box position while it places the opponent's system boxes (UpdateSysBoxes: those keep
        // their place when the window's top moves down).
        void AdjustHostileBoxSize(const CombatControl *combat, Point &size);
        bool DrawHostileBox(CombatControl *combat, GL_Color color, int stencilBit);
        void BeginSysBoxes(CombatControl *combat);
        void EndSysBoxes(CombatControl *combat);
        // Right-aligned text in the enemy window's header (ship class, relationship): at the grown window's right
        // edge, and below the hull and shield rows where a player ship's hull bar (30 points) would run under it.
        bool AdjustHeaderText(int fontSize, float &x, float &y, const std::string &text);
        // Hyperspace draws some decorations of the enemy window at fixed screen positions made for FTL's window: the
        // hull number, ship icons and event timers (moved between Begin and EndDecorations, around drawing and
        // mouse handling), and extra hull bars (HullBarShift: a translation for that image).
        void BeginDecorations();
        void EndDecorations();
        bool HullBarShift(const CachedPrimitive *image, float &dx, float &dy);

        // Mouse: a screen point in the opponent's and in our ship's coordinates, as the ships are drawn now.
        // `inside` tells whether the point is in the enemy window. Both return false without a duel layout.
        bool TargetShipPoint(float x, float y, float &shipX, float &shipY, bool &inside);
        bool OwnShipPoint(float x, float y, float &shipX, float &shipY);

        // FTL shifts an enemy ship's shield ellipse 110 px down (Ship::GetBaseEllipse), which suits its upright enemy
        // layouts; a player ship as ship 1 gets its shields where the player's own ship would have them.
        // UsePlayerShieldPosition switches that on for ship 1 (and re-places the shields of a ship already there);
        // it switches off again when ship 1 is gone.
        void UsePlayerShieldPosition(ShipManager *ship);
        bool PlayerShieldPosition(int shipId);

        // For tests: where a point in a ship's coordinates is drawn now (ship 0 ours, 1 the opponent), and a
        // description of the layout.
        bool ShipToScreen(int shipId, float shipX, float shipY, float &screenX, float &screenY);
        std::string Describe();
    }
}
