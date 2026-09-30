#pragma once

#include <string>

struct GL_Color;

// FTL's own look for what FTL: Duels draws (roadmap S; docs/design/match-ui.md in the FTL: Duels repository): FTL's
// button colours and fonts, its button frame, window outline and title tab. The frames are the game's own images,
// loaded from its resources at run time (no image of the game is shipped); the rest is drawn to match them.
namespace Duels
{
    namespace Style
    {
        // FTL's STORE button frame (statusUI/top_store_base.png, 102 x 55): a glow of 7 px, a light border of 3 px and a
        // dark line of 2 px around the field where FTL draws the button's body (78 x 31 at 12, 12).
        const float TOP_FRAME_HEIGHT = 55.f;
        const float TOP_FIELD_INSET = 12.f;
        const float TOP_FIELD_HEIGHT = 31.f;
        const float TOP_FRAME_GLOW = 7.f;

        enum class Look
        {
            Idle,    // FTL's light body
            Hover,   // FTL's yellow, under the mouse
            Off,     // FTL's grey: can't be used now
            Pressed  // held down: a darker body, the letters a pixel lower, no glow (our draw offer, roadmap AH)
        };

        // FTL's colours for text buttons (TextButton's own: the body idle, under the mouse and off; the letters) and the
        // dark line of its frames; the players' colours (the host red, the guest blue).
        GL_Color ButtonBody(Look look);
        GL_Color ButtonText();
        GL_Color FrameLine();
        GL_Color Red(float alpha = 1.f);
        GL_Color Blue(float alpha = 1.f);
        GL_Color Mix(const GL_Color &a, const GL_Color &b, float t);

        // A rectangle with its corners cut at 45 degrees, in steps of a pixel (FTL's buttons and windows).
        void CutRect(float x, float y, float w, float h, float cut, const GL_Color &colour);
        // A blend of two colours from left to right, in vertical strips; with `cut`, its corners are cut.
        void Blend(float x, float y, float w, float h, const GL_Color &left, const GL_Color &right, float cut = 0.f);

        // A text button in the look of FTL's (the STORE button's frame: a faint glow, the light border, the dark line,
        // the body), at any size; x, y, w, h is its outer edge. `body`, if given, replaces the body's colour (a
        // highlight: a draw offer's gold, "Sure?"'s red); the label is in FTL's dark button letters.
        void Button(float x, float y, float w, float h, const std::string &label, int font, Look look, const GL_Color *body = nullptr);

        // FTL's STORE button frame, three-sliced to the width w (its middle stretched): x, y is the image's corner.
        // False when the image isn't there.
        bool TopFrame(float x, float y, float w, const GL_Color &tint);

        // FTL's window outline (the cut corners, the light border, the glow) around x, y, w, h (the border's outer
        // edge), and a title tab on its top left as FTL's options screens have: the title in dark tab letters
        // (font 63) on the light tab (box_options_configure_tab.png).
        void WindowOutline(int x, int y, int w, int h);
        void TitleTab(float frameX, float frameY, const std::string &title);

        // A section's title as FTL labels its boxes (WEAPONS, DRONES): dark letters (font 62) on a light band with a
        // slanted end, its top left at x, y; returns the band's height.
        float Label(float x, float y, const std::string &title);

        // A place on the screen that takes clicks (a button, a field, a check box and its label).
        struct Box
        {
            float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
            bool Contains(int px, int py) const { return w > 0.f && px >= x && px < x + w && py >= y && py < y + h; }
        };

        // A window over the main menu (roadmap 3.5): the screen behind it dimmed, FTL: Duels' red and blue, FTL's
        // outline and a title tab, as the Duels window.
        void Dialog(float x, float y, float w, float h, const std::string &title, bool dim = true);
        // A check box (22 x 22 at x, y): FTL's light border and dark inside, the light square when it is on; the
        // border yellow under the mouse.
        const float CHECK_SIZE = 22.f;
        void CheckBox(float x, float y, bool on, bool hover);
        // A text field's frame: the light border (yellow while it has the keyboard) around a dark field.
        void Field(float x, float y, float w, float h, bool focus);
        // A text field with its text in font 12 (a password as stars) and, while it has the keyboard, a blinking caret
        // before the character `caret` (FTL's TextInput keeps the text and the caret; it draws the text itself at an
        // offset of its own, over the field's border).
        void TextField(float x, float y, float w, float h, const std::string &text, int caret, bool focus, bool stars);

        // The height of one line of a font (FTL's measure).
        float LineHeight(int font);
    }
}
