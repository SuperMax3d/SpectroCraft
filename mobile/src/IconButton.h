// SpectroCraft Mobile - a touch button drawn as a vector icon (no image files),
// in the app's button style. The name is the button's accessible title
// (VoiceOver / TalkBack read it).
#pragma once
#include <JuceHeader.h>

namespace scm
{
class IconButton : public juce::Button
{
public:
    enum class Icon { Open, Save, Share, Delete, Undo, Redo, Panels };

    IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i)
    {
        setTitle (name);
        setTooltip (name);
    }

    /** The icon's outline in a 24 x 24 box (stroked, not filled). */
    static juce::Path iconPath (Icon icon)
    {
        juce::Path p;
        switch (icon)
        {
            case Icon::Open:      // a folder
                p.startNewSubPath (2.5f, 19.5f);
                p.lineTo (2.5f, 5.0f);
                p.lineTo (9.0f, 5.0f);
                p.lineTo (11.0f, 7.5f);
                p.lineTo (21.5f, 7.5f);
                p.lineTo (21.5f, 19.5f);
                p.closeSubPath();
                p.startNewSubPath (2.5f, 10.5f);
                p.lineTo (21.5f, 10.5f);
                break;
            case Icon::Save:      // an arrow down into a tray
                p.startNewSubPath (12.0f, 3.0f);
                p.lineTo (12.0f, 15.0f);
                p.startNewSubPath (7.0f, 10.0f);
                p.lineTo (12.0f, 15.0f);
                p.lineTo (17.0f, 10.0f);
                p.startNewSubPath (4.0f, 14.5f);
                p.lineTo (4.0f, 20.5f);
                p.lineTo (20.0f, 20.5f);
                p.lineTo (20.0f, 14.5f);
                break;
            case Icon::Share:     // an arrow up out of an open box (as on iOS)
                p.startNewSubPath (12.0f, 15.0f);
                p.lineTo (12.0f, 2.5f);
                p.startNewSubPath (7.5f, 7.0f);
                p.lineTo (12.0f, 2.5f);
                p.lineTo (16.5f, 7.0f);
                p.startNewSubPath (8.5f, 10.0f);
                p.lineTo (5.0f, 10.0f);
                p.lineTo (5.0f, 21.5f);
                p.lineTo (19.0f, 21.5f);
                p.lineTo (19.0f, 10.0f);
                p.lineTo (15.5f, 10.0f);
                break;
            case Icon::Delete:    // a trash can
                p.startNewSubPath (3.0f, 6.0f);
                p.lineTo (21.0f, 6.0f);
                p.startNewSubPath (9.0f, 6.0f);
                p.lineTo (9.0f, 3.0f);
                p.lineTo (15.0f, 3.0f);
                p.lineTo (15.0f, 6.0f);
                p.startNewSubPath (5.5f, 6.0f);
                p.lineTo (6.5f, 21.0f);
                p.lineTo (17.5f, 21.0f);
                p.lineTo (18.5f, 6.0f);
                p.startNewSubPath (10.0f, 10.0f);
                p.lineTo (10.0f, 17.5f);
                p.startNewSubPath (14.0f, 10.0f);
                p.lineTo (14.0f, 17.5f);
                break;
            case Icon::Undo:      // an arrow back to the left, curling down
            case Icon::Redo:      // the same, mirrored
                p.startNewSubPath (8.5f, 5.5f);
                p.lineTo (4.0f, 10.0f);
                p.lineTo (8.5f, 14.5f);
                p.startNewSubPath (4.0f, 10.0f);
                p.lineTo (14.5f, 10.0f);
                p.cubicTo (18.5f, 10.0f, 20.5f, 12.5f, 20.5f, 15.5f);
                p.cubicTo (20.5f, 18.5f, 18.5f, 21.0f, 14.5f, 21.0f);
                p.lineTo (10.0f, 21.0f);
                if (icon == Icon::Redo)
                    p.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f).translated (24.0f, 0.0f));
                break;
            case Icon::Panels:    // a window with a side column
                p.addRoundedRectangle (2.5f, 4.5f, 19.0f, 15.0f, 2.0f);
                p.startNewSubPath (9.0f, 4.5f);
                p.lineTo (9.0f, 19.5f);
                p.startNewSubPath (4.8f, 8.5f);
                p.lineTo (6.8f, 8.5f);
                p.startNewSubPath (4.8f, 11.5f);
                p.lineTo (6.8f, 11.5f);
                break;
        }
        return p;
    }

    void paintButton (juce::Graphics& g, bool highlighted, bool down) override
    {
        getLookAndFeel().drawButtonBackground (g, *this, findColour (getToggleState() ? juce::TextButton::buttonOnColourId
                                                                                      : juce::TextButton::buttonColourId),
                                               highlighted, down);
        const float side = std::min (26.0f, (float) std::min (getWidth(), getHeight()) - 14.0f);
        auto box = getLocalBounds().toFloat().withSizeKeepingCentre (side, side);
        auto path = iconPath (icon);
        path.applyTransform (juce::AffineTransform::scale (side / 24.0f).translated (box.getX(), box.getY()));
        auto colour = findColour (juce::TextButton::textColourOffId);
        if (! isEnabled()) colour = colour.withMultipliedAlpha (0.35f);
        else if (down) colour = juce::Colours::white;
        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (std::max (1.6f, side / 13.0f), juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

private:
    Icon icon;
};
} // namespace scm
