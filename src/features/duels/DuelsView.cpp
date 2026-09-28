#include "Global.h"
#include "CustomEvents.h"
#include "Duels.h"
#include "DuelsView.h"
#include "EnemyShipIcons.h"
#include "HullNumbers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace Duels
{
    namespace View
    {
        // FTL's enemy window (combatUI/box_hostiles2.png) and where things are in it.
        static const int BOX_W = 410;
        static const int BOX_H = 555;
        static const float MARGIN_TOP = 62.f;      // title, hull and shield rows
        static const float MARGIN_BOTTOM = 66.f;   // the opponent's system boxes
        static const float MARGIN_LEFT = 12.f;
        static const float MARGIN_RIGHT = 18.f;    // the frame's right border sits further in
        static const float HULL_PADDING = 4.f;
        // Between our ship's hull and the enemy window.
        static const float GAP = 25.f;
        // The jump, ship and upgrade buttons at the top of the screen end here. An enemy window that grows further
        // left moves its top down by BUTTONS_CLEARANCE to pass below them; its bottom stays where it is, above our
        // subsystem panel.
        static const float BUTTONS_RIGHT = 760.f;
        static const int BUTTONS_CLEARANCE = 35;

        struct Layout
        {
            bool active = false;
            CombatControl *combat = nullptr;
            // Our ship: FTL draws its point p at ownOrigin + p, we draw it at ownScreen + ownScale (p - ownPivot).
            // Unless both ships are drawn at one scale ("equal"), that is FTL's own place and size.
            ShipManager *own = nullptr;
            float ownScale = 1.f;
            float ownOriginX = 0.f, ownOriginY = 0.f, ownPivotX = 0.f, ownPivotY = 0.f, ownScreenX = 0.f, ownScreenY = 0.f;
            bool ownMoved = false;
            // The opponent, mirrored: FTL at targetOrigin + p, we at targetScreen + (-scale, scale) (p - targetPivot).
            ShipManager *target = nullptr;
            float scale = 1.f;
            float targetOriginX = 0.f, targetOriginY = 0.f, targetPivotX = 0.f, targetPivotY = 0.f;
            float targetScreenX = 0.f, targetScreenY = 0.f;
            // The enemy window: grown to the left by `grow`, its top moved down by `lower`.
            int grow = 0, lower = 0;
            float boxX = 0.f, boxY = 0.f, boxW = 0.f, boxH = 0.f;
        };

        // We move the enemy window through two fields, CombatControl::position.x and boxPosition.y, which everything
        // in it is drawn and hit-tested from. FTL sets position once (OnInit) and the box position for every new enemy
        // (AddEnemyShip); a value that differs from the one we set is FTL's new one.
        struct Window
        {
            CombatControl *combat = nullptr;
            bool moved = false;
            int ftlPositionX = 0, ftlBoxY = 0;
            int ourPositionX = 0, ourBoxY = 0;
        };

        enum class Drawing
        {
            None,
            Target,
            Own
        };

        static Mode g_mode = Mode::Auto;
        static bool g_equal = false;
        static bool g_duelOpponent = false;
        static Layout g_layout;
        static Window g_window;
        static Drawing g_drawing = Drawing::None;        // whose drawing we are inside
        static Drawing g_transforming = Drawing::None;   // whose transform is applied now

        void SetMode(Mode mode) { g_mode = mode; }
        Mode GetMode() { return g_mode; }
        void SetEqualSize(bool equal) { g_equal = equal; }
        bool EqualSize() { return g_equal; }
        void SetDuelOpponent(bool present) { g_duelOpponent = present; }
        bool Active() { return g_layout.active; }

        static void TakeFtlValues(CombatControl *combat)
        {
            Window &w = g_window;
            if (w.combat != combat)
            {
                w = Window();
                w.combat = combat;
            }
            if (!w.moved || combat->position.x != w.ourPositionX) w.ftlPositionX = combat->position.x;
            if (!w.moved || combat->boxPosition.y != w.ourBoxY) w.ftlBoxY = combat->boxPosition.y;
        }

        static void MoveWindow(CombatControl *combat, int grow, int lower)
        {
            Window &w = g_window;
            combat->position.x = w.ftlPositionX - grow;
            combat->boxPosition.y = w.ftlBoxY + lower;
            w.ourPositionX = combat->position.x;
            w.ourBoxY = combat->boxPosition.y;
            w.moved = true;
        }

        static void RestoreWindow()
        {
            Window &w = g_window;
            if (!w.moved || !w.combat) return;
            // Only what is still ours: FTL may have set new values meanwhile.
            if (w.combat->position.x == w.ourPositionX) w.combat->position.x = w.ftlPositionX;
            if (w.combat->boxPosition.y == w.ourBoxY) w.combat->boxPosition.y = w.ftlBoxY;
            w.moved = false;
        }

        struct Hull
        {
            float x, y, w, h;   // ship coordinates, with a little room around the hull image
        };

        static Hull HullOf(ShipManager *ship)
        {
            // FTL draws the hull image at its offset from the corner of the ship's rooms (Ship::OnRenderBase).
            const ImageDesc &image = ship->ship.shipImage;
            ShipGraph *graph = ShipGraph::GetShipInfo(ship->iShipId);
            float x = (float)image.x + (graph ? (float)graph->shipBox.x : 0.f);
            float y = (float)image.y + (graph ? (float)graph->shipBox.y : 0.f);
            return {x - HULL_PADDING, y - HULL_PADDING, std::max(1.f, (float)image.w + 2.f * HULL_PADDING),
                    std::max(1.f, (float)image.h + 2.f * HULL_PADDING)};
        }

        static bool Compute(CommandGui *gui, Layout &l)
        {
            CombatControl *combat = &gui->combatControl;
            if (g_mode == Mode::Off || combat->boss_visual || !combat->currentTarget) return false;
            ShipManager *target = combat->currentTarget->shipManager;
            ShipManager *own = gui->shipComplete ? gui->shipComplete->shipManager : nullptr;
            if (!target || target->iShipId != 1 || !own || own->iShipId != 0) return false;
            if (g_mode == Mode::Auto && !g_duelOpponent) return false;

            TakeFtlValues(combat);
            const Window &w = g_window;
            float boxLeft = (float)(w.ftlPositionX + combat->boxPosition.x);   // FTL's window
            float boxTop = (float)(combat->position.y + w.ftlBoxY);
            float boxRight = boxLeft + BOX_W;
            float boxBottom = boxTop + BOX_H;
            float fitRight = boxRight - MARGIN_RIGHT;

            Hull ours = HullOf(own), theirs = HullOf(target);
            float ownLeft = (float)gui->shipPosition.x + ours.x;
            float ownCenterY = (float)gui->shipPosition.y + ours.y + ours.h * 0.5f;
            auto heightFit = [&](int lower) { return (BOX_H - lower - MARGIN_TOP - MARGIN_BOTTOM) / theirs.h; };
            auto widthFit = [&](float left) { return (fitRight - MARGIN_LEFT - left) / theirs.w; };

            // The opponent's scale (at most FTL's own): its hull within the window's height, and within its width as
            // far as the window can grow left. A window at its own height can't grow past the buttons; one moved down
            // can, but has less height.
            float ownScale = 1.f, keepTop, lowerTop;
            if (g_equal)
            {
                // Both ships at one scale: both hulls side by side with a gap up to the window's right edge, ours
                // ending left of FTL's window position at least.
                float sideBySide = (fitRight - MARGIN_LEFT - GAP - ownLeft) / (ours.w + theirs.w);
                float ownRoom = (boxLeft - GAP - ownLeft) / ours.w;
                float common = std::min(1.f, std::min(sideBySide, ownRoom));
                keepTop = std::min(common, std::min(heightFit(0), widthFit(BUTTONS_RIGHT)));
                lowerTop = std::min(common, heightFit(BUTTONS_CLEARANCE));
            }
            else
            {
                // Our ship as FTL draws it: the window grows up to a gap after its hull (never narrower than FTL's).
                float edge = std::min(boxLeft, ownLeft + ours.w + GAP);
                keepTop = std::min(1.f, std::min(heightFit(0), widthFit(std::max(edge, BUTTONS_RIGHT))));
                lowerTop = std::min(1.f, std::min(heightFit(BUTTONS_CLEARANCE), widthFit(edge)));
            }
            int lower = lowerTop > keepTop + 0.01f ? BUTTONS_CLEARANCE : 0;
            float scale = std::max(0.2f, lower ? lowerTop : keepTop);
            if (g_equal) ownScale = scale;

            // The window grows only as far as the opponent needs (never narrower than FTL's).
            float left = std::min(boxLeft, std::floor(fitRight - MARGIN_LEFT - theirs.w * scale));
            if (left >= BUTTONS_RIGHT) lower = 0;
            int grow = (int)(boxLeft - left);

            // One center line for both ships: ours where FTL has it, as far as the opponent fits in its window there.
            // (At one scale our ship moves to the opponent's line if that has to move.)
            float fitTop = boxTop + lower + MARGIN_TOP, fitBottom = boxBottom - MARGIN_BOTTOM;
            float half = theirs.h * scale * 0.5f;
            float centerY = fitTop + half <= fitBottom - half
                                ? std::max(fitTop + half, std::min(fitBottom - half, ownCenterY))
                                : (fitTop + fitBottom) * 0.5f;

            l.active = true;
            l.combat = combat;
            l.scale = scale;
            l.own = own;
            l.ownScale = ownScale;
            l.ownOriginX = (float)gui->shipPosition.x;
            l.ownOriginY = (float)gui->shipPosition.y;
            l.ownPivotX = ours.x;
            l.ownPivotY = ours.y + ours.h * 0.5f;
            l.ownScreenX = ownLeft;
            l.ownScreenY = g_equal ? centerY : ownCenterY;
            l.ownMoved = g_equal && (ownScale != 1.f || centerY != ownCenterY);
            l.target = target;
            l.targetOriginX = (float)(w.ftlPositionX - grow + combat->targetPosition.x);
            l.targetOriginY = (float)(combat->position.y + combat->targetPosition.y);
            l.targetPivotX = theirs.x + theirs.w * 0.5f;
            l.targetPivotY = theirs.y + theirs.h * 0.5f;
            l.targetScreenX = (boxLeft - grow + MARGIN_LEFT + fitRight) * 0.5f;
            l.targetScreenY = centerY;
            l.grow = grow;
            l.lower = lower;
            l.boxX = boxLeft - grow;
            l.boxY = boxTop + lower;
            l.boxW = (float)(BOX_W + grow);
            l.boxH = (float)(BOX_H - lower);
            return true;
        }

        static bool g_playerShields = false;

        void OnFrame()
        {
            if (g_playerShields && !G_->GetShipManager(1)) g_playerShields = false;

            CApp *app = G_->GetCApp();
            Layout layout;
            if (app && app->gui && Compute(app->gui, layout))
            {
                MoveWindow(layout.combat, layout.grow, layout.lower);
            }
            else
            {
                RestoreWindow();
            }
            g_layout = layout;
        }

        void BeginTarget() { g_drawing = Drawing::Target; }
        void BeginPlayerShip() { g_drawing = Drawing::Own; }

        // FTL picks nearest or smooth texture filtering per draw; a shrunk ship drawn with nearest filtering loses rows
        // and columns of pixels and looks grainy. CSurface::GL_ForceAntialias (never used by FTL itself) makes every
        // draw smooth: on while a scaled ship is drawn, and for whole frames drawn finer than FTL's (DuelsScreen).
        static bool g_frameSmooth = false;
        static bool g_shipSmooth = false;

        static void UpdateSmoothing()
        {
            CSurface::GL_ForceAntialias(g_frameSmooth || g_shipSmooth);
        }

        void SetFrameSmoothing(bool on)
        {
            if (g_frameSmooth == on) return;
            g_frameSmooth = on;
            UpdateSmoothing();
        }

        static void EndShip()
        {
            g_drawing = Drawing::None;
            g_transforming = Drawing::None;
            if (g_shipSmooth)
            {
                g_shipSmooth = false;
                UpdateSmoothing();
            }
        }

        void EndTarget() { EndShip(); }
        void EndPlayerShip() { EndShip(); }

        void ApplyShipTransform(ShipManager *ship)
        {
            const Layout &l = g_layout;
            if (!l.active || !ship) return;
            // No push: RenderTarget and RenderPlayerShip pop their matrix after the aiming marks, which belong to the
            // same transform.
            float scale = 1.f;
            if (g_drawing == Drawing::Target && ship == l.target)
            {
                CSurface::GL_Translate(l.targetScreenX - l.targetOriginX, l.targetScreenY - l.targetOriginY, 0.f);
                CSurface::GL_Scale(-l.scale, l.scale, 1.f);
                CSurface::GL_Translate(-l.targetPivotX, -l.targetPivotY, 0.f);
                g_transforming = Drawing::Target;
                scale = l.scale;
            }
            else if (g_drawing == Drawing::Own && ship == l.own && l.ownMoved)
            {
                CSurface::GL_Translate(l.ownScreenX - l.ownOriginX, l.ownScreenY - l.ownOriginY, 0.f);
                CSurface::GL_Scale(l.ownScale, l.ownScale, 1.f);
                CSurface::GL_Translate(-l.ownPivotX, -l.ownPivotY, 0.f);
                g_transforming = Drawing::Own;
                scale = l.ownScale;
            }
            if (scale != 1.f)
            {
                g_shipSmooth = true;
                UpdateSmoothing();
            }
        }

        bool Transforming() { return g_transforming != Drawing::None; }

        void InverseScale(float &sx, float &sy)
        {
            float scale = g_transforming == Drawing::Target ? g_layout.scale : g_layout.ownScale;
            if (scale <= 0.f) scale = 1.f;
            sx = (g_transforming == Drawing::Target ? -1.f : 1.f) / scale;
            sy = 1.f / scale;
        }

        static bool g_unmirror = true;
        void SetUnmirrorIcons(bool on) { g_unmirror = on; }

        bool BeginUnmirrored(float x, float y, float minScale)
        {
            if (!g_unmirror || g_transforming != Drawing::Target || !g_layout.active) return false;
            float grow = g_layout.scale > 0.f ? std::max(1.f, minScale / g_layout.scale) : 1.f;
            CSurface::GL_PushMatrix();
            CSurface::GL_Translate(x, y, 0.f);
            CSurface::GL_Scale(-grow, grow, 1.f);
            CSurface::GL_Translate(-x, -y, 0.f);
            return true;
        }

        void EndUnmirrored()
        {
            CSurface::GL_PopMatrix();
        }

        bool TargetRoomCenter(int shipId, int roomId, float &x, float &y)
        {
            const Layout &l = g_layout;
            if (g_transforming != Drawing::Target || !l.active || !l.target || shipId != l.target->iShipId) return false;
            if (roomId < 0 || roomId >= (int)l.target->ship.vRoomList.size()) return false;
            Pointf center = l.target->GetRoomCenter(roomId);
            x = center.x;
            y = center.y;
            return true;
        }

        bool TargetMarkerCenter(const CachedPrimitive *image, float &x, float &y)
        {
            const Layout &l = g_layout;
            if (g_transforming != Drawing::Target || !l.active || !l.combat) return false;
            CombatControl *combat = l.combat;
            const CachedImage *markers[4] = {&combat->teleportTarget_send, &combat->teleportTarget_return, &combat->hackTarget,
                                             &combat->mindTarget};
            for (const CachedImage *marker : markers)
            {
                if (image != marker || !marker->texture) continue;
                x = marker->x + marker->texture->width_ * marker->wScale * 0.5f;
                y = marker->y + marker->texture->height_ * marker->hScale * 0.5f;
                return true;
            }
            return false;
        }

        static bool g_aiming = false;
        void SetAiming(bool aiming) { g_aiming = aiming; }
        bool Aiming() { return g_aiming; }

        void AdjustHostileBoxSize(const CombatControl *combat, Point &size)
        {
            const Layout &l = g_layout;
            if (!l.active || combat != l.combat || combat->boss_visual) return;
            size.x += l.grow;
            size.y -= l.lower;
        }

        // The frame image drawn at another size without seams: in three bands (title, interior, bottom edge), each
        // with its left and right part at their own size and the part between stretched; only the interior band
        // stretches vertically. The kept parts hold the chamfered corners, the notch at the bottom right, and in the
        // title band the slot for the hull number (in Hyperspace's variant of the image). The interior is a faint
        // dot pattern and gradient, which stretches without a visible change; the title band's lower rows are even
        // along their length, so its narrower stretched part meets the interior's without a seam.
        static void DrawFrame(GL_Texture *texture, float x, float y, float w, float h, GL_Color color)
        {
            struct Band
            {
                float top, bottom, left, right;   // image pixels (410 x 555)
                bool stretches;
            };
            const Band bands[3] = {{0.f, 34.f, 140.f, 50.f, false}, {34.f, 480.f, 60.f, 50.f, true}, {480.f, 555.f, 60.f, 50.f, false}};
            float tw = (float)texture->width_, th = (float)texture->height_;
            float bandY = y;
            for (const Band &band : bands)
            {
                float bandH = band.bottom - band.top + (band.stretches ? h - th : 0.f);
                float xs[4] = {x, x + band.left, x + w - band.right, x + w};
                float us[4] = {0.f, band.left / tw, (tw - band.right) / tw, 1.f};
                for (int col = 0; col < 3; ++col)
                {
                    CSurface::GL_BlitImagePartial(texture, xs[col], bandY, xs[col + 1] - xs[col], bandH, us[col], us[col + 1],
                                                  band.top / th, band.bottom / th, 1.f, color, false);
                }
                bandY += bandH;
            }
        }

        bool DrawHostileBox(CombatControl *combat, GL_Color color, int stencilBit)
        {
            const Layout &l = g_layout;
            if (!l.active || combat != l.combat || combat->boss_visual || (!l.grow && !l.lower)) return false;
            ResourceControl *resources = G_->GetResources();
            GL_Texture *box = resources->GetImageId("combatUI/box_hostiles2.png");
            GL_Texture *mask = resources->GetImageId("combatUI/box_hostiles_mask.png");
            if (!box || !mask || box->width_ != BOX_W || box->height_ != BOX_H) return false;
            float x = (float)(combat->position.x + combat->boxPosition.x);
            float y = (float)(combat->position.y + combat->boxPosition.y);
            // As FTL: the frame, then its mask into the stencil buffer (the window's content is clipped to it).
            DrawFrame(box, x, y, l.boxW, l.boxH, color);
            CSurface::GL_SetStencilMode(STENCIL_SET, stencilBit, stencilBit);
            DrawFrame(mask, x, y, l.boxW, l.boxH, COLOR_WHITE);
            CSurface::GL_SetStencilMode(STENCIL_IGNORE, 0, 0);
            return true;
        }

        void BeginSysBoxes(CombatControl *combat)
        {
            if (g_window.moved && combat == g_window.combat) combat->boxPosition.y = g_window.ftlBoxY;
        }

        void EndSysBoxes(CombatControl *combat)
        {
            if (g_window.moved && combat == g_window.combat) combat->boxPosition.y = g_window.ourBoxY;
        }

        // Where a decoration placed at a fixed point of FTL's window goes in the grown one: those in its left half
        // follow its left edge, those in its top half its top edge. False if it stays.
        static bool DecorationShift(int x, int y, int &dx, int &dy)
        {
            const Layout &l = g_layout;
            if (!l.active) return false;
            float ftlLeft = l.boxX + l.grow, ftlTop = l.boxY - l.lower;
            if (x < ftlLeft - 20.f || x > ftlLeft + BOX_W + 20.f || y < ftlTop - 60.f || y > ftlTop + BOX_H + 20.f) return false;
            dx = x < ftlLeft + BOX_W * 0.5f ? -l.grow : 0;
            dy = y < ftlTop + BOX_H * 0.5f ? l.lower : 0;
            return dx || dy;
        }

        struct Shifted
        {
            int *x, *y;
            int dx, dy;
        };
        static std::vector<Shifted> g_shifted;

        static void ShiftDecoration(int &x, int &y)
        {
            int dx = 0, dy = 0;
            if (!DecorationShift(x, y, dx, dy)) return;
            x += dx;
            y += dy;
            g_shifted.push_back({&x, &y, dx, dy});
        }

        void BeginDecorations()
        {
            EndDecorations();
            if (!g_layout.active) return;
            HullNumbers *numbers = HullNumbers::GetInstance();
            ShiftDecoration(numbers->enemyIndicator.x, numbers->enemyIndicator.y);
            for (auto &entry : numbers->enemyIndicatorLoc) ShiftDecoration(entry.second.x, entry.second.y);
            if (ShipIconManager::instance)
            {
                ShiftDecoration(ShipIconManager::instance->normalBoxPos.x, ShipIconManager::instance->normalBoxPos.y);
            }
            TriggeredEventGui *timers = TriggeredEventGui::GetInstance();
            ShiftDecoration(timers->normalBoxPos.x, timers->normalBoxPos.y);
        }

        void EndDecorations()
        {
            for (const Shifted &shifted : g_shifted)
            {
                *shifted.x -= shifted.dx;
                *shifted.y -= shifted.dy;
            }
            g_shifted.clear();
        }

        bool HullBarShift(const CachedPrimitive *image, float &dx, float &dy)
        {
            CachedImage *bar = HullBars::GetInstance()->hullBarImage;
            int x = 0, y = 0;
            if (g_drawing != Drawing::Target || !bar || image != bar || !DecorationShift(bar->x, bar->y, x, y)) return false;
            dx = (float)x;
            dy = (float)y;
            return true;
        }

        // FTL's hull bar for enemies is an image of 22 segments, drawn as far as hull / 22 of it. A player ship's 30
        // hull point bars would run past the image's end, where its last column is stretched into a solid bar; the
        // extra segments are drawn from the image again instead (its right-hand part), after the whole image.
        static float g_hullBarRest = 0.f;

        static bool IsTargetHullBar(const CachedImage *image)
        {
            CApp *app = G_->GetCApp();
            return g_drawing == Drawing::Target && g_layout.active && app && app->gui && image == &app->gui->combatControl.healthMask;
        }

        bool LimitHullBar(const CachedImage *image, float &xSize)
        {
            if (xSize <= 1.f || !IsTargetHullBar(image)) return false;
            g_hullBarRest = xSize - 1.f;
            xSize = 1.f;
            return true;
        }

        float TakeHullBarRest(const CachedPrimitive *image)
        {
            if (g_hullBarRest <= 0.f || !IsTargetHullBar(static_cast<const CachedImage*>(image))) return 0.f;
            float rest = g_hullBarRest;
            g_hullBarRest = 0.f;
            return rest;
        }

        bool AdjustHeaderText(int fontSize, float &x, float &y, const std::string &text)
        {
            const Layout &l = g_layout;
            if (g_drawing != Drawing::Target || !l.active) return false;
            // FTL right-aligns them at its window's right edge, in the header.
            float ftlRight = l.boxX + BOX_W;
            if (y > l.boxY + MARGIN_TOP || x < ftlRight - 40.f || x > ftlRight + 10.f) return false;
            x += (float)l.grow;
            // The hull bar starts at the window's left edge, about 11 px per hull point. (Both lines use the same
            // width unless one is very long, so they stay together.)
            float barRight = l.boxX + 17.f + 11.f * (float)std::max(1, l.target->ship.hullIntegrity.second) + 8.f;
            float width = std::max(170.f, (float)freetype::easy_measureWidth(fontSize, text));
            if (x - width < barRight) y += 30.f;
            return true;
        }

        bool TargetShipPoint(float x, float y, float &shipX, float &shipY, bool &inside)
        {
            const Layout &l = g_layout;
            inside = false;
            if (!l.active) return false;
            inside = x >= l.boxX && x < l.boxX + l.boxW && y >= l.boxY && y < l.boxY + l.boxH;
            shipX = l.targetPivotX - (x - l.targetScreenX) / l.scale;
            shipY = l.targetPivotY + (y - l.targetScreenY) / l.scale;
            return true;
        }

        bool OwnShipPoint(float x, float y, float &shipX, float &shipY)
        {
            const Layout &l = g_layout;
            if (!l.active) return false;
            shipX = l.ownPivotX + (x - l.ownScreenX) / l.ownScale;
            shipY = l.ownPivotY + (y - l.ownScreenY) / l.ownScale;
            return true;
        }

        void UsePlayerShieldPosition(ShipManager *ship)
        {
            g_playerShields = true;
            // The shields were placed when the ship was built; place them again (GetBaseEllipse is hooked).
            if (ship && ship->iShipId == 1 && ship->shieldSystem) ship->shieldSystem->SetBaseEllipse(ship->ship.GetBaseEllipse());
        }

        bool PlayerShieldPosition(int shipId)
        {
            return g_playerShields && shipId == 1;
        }

        bool ShipToScreen(int shipId, float shipX, float shipY, float &screenX, float &screenY)
        {
            const Layout &l = g_layout;
            CApp *app = G_->GetCApp();
            if (!app || !app->gui) return false;
            if (shipId == 0)
            {
                if (l.active)
                {
                    screenX = l.ownScreenX + l.ownScale * (shipX - l.ownPivotX);
                    screenY = l.ownScreenY + l.ownScale * (shipY - l.ownPivotY);
                }
                else
                {
                    screenX = app->gui->shipPosition.x + shipX;
                    screenY = app->gui->shipPosition.y + shipY;
                }
                return true;
            }
            if (l.active)
            {
                screenX = l.targetScreenX - l.scale * (shipX - l.targetPivotX);
                screenY = l.targetScreenY + l.scale * (shipY - l.targetPivotY);
            }
            else
            {
                const CombatControl &combat = app->gui->combatControl;
                screenX = combat.position.x + combat.targetPosition.x + shipX;
                screenY = combat.position.y + combat.targetPosition.y + shipY;
            }
            return true;
        }

        std::string Describe()
        {
            const char *mode = g_mode == Mode::Off ? "off" : g_mode == Mode::Auto ? "auto" : "fit";
            const Layout &l = g_layout;
            if (!l.active) return std::string("duel view: vanilla (mode ") + mode + ")";
            // The hull images as drawn now (with the room around them that the layout keeps free).
            Hull ours = HullOf(l.own), theirs = HullOf(l.target);
            char buffer[640];
            snprintf(buffer, sizeof(buffer),
                     "duel view (%s%s): scale %.3f | ours %s at %.3f, hull %.0f,%.0f to %.0f,%.0f | theirs %s, hull %.0f,%.0f "
                     "to %.0f,%.0f, mirrored | window %.0f,%.0f %.0fx%.0f (grown %d, top down %d) | centre lines y %.0f / %.0f",
                     mode, g_equal ? ", equal size" : "", l.scale, l.own->myBlueprint.blueprintName.c_str(), l.ownScale,
                     l.ownScreenX + l.ownScale * (ours.x - l.ownPivotX), l.ownScreenY + l.ownScale * (ours.y - l.ownPivotY),
                     l.ownScreenX + l.ownScale * (ours.x + ours.w - l.ownPivotX),
                     l.ownScreenY + l.ownScale * (ours.y + ours.h - l.ownPivotY), l.target->myBlueprint.blueprintName.c_str(),
                     l.targetScreenX - l.scale * (theirs.x + theirs.w - l.targetPivotX),
                     l.targetScreenY + l.scale * (theirs.y - l.targetPivotY),
                     l.targetScreenX - l.scale * (theirs.x - l.targetPivotX),
                     l.targetScreenY + l.scale * (theirs.y + theirs.h - l.targetPivotY), l.boxX, l.boxY, l.boxW, l.boxH,
                     l.grow, l.lower, l.ownScreenY, l.targetScreenY);
            return buffer;
        }
    }
}
