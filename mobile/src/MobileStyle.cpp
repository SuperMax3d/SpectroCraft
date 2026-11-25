#include "MobileStyle.h"
#include "MobileFonts.h"

namespace scm::style
{
namespace
{
struct Typefaces
{
    juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (
        MobileFonts::SourceSans3Regular_ttf, (size_t) MobileFonts::SourceSans3Regular_ttfSize);
    juce::Typeface::Ptr semibold = juce::Typeface::createSystemTypefaceFor (
        MobileFonts::SourceSans3Semibold_ttf, (size_t) MobileFonts::SourceSans3Semibold_ttfSize);
};

Typefaces& typefaces()
{
    static Typefaces t;
    return t;
}
} // namespace

juce::Font font (float height, bool semibold)
{
    auto tf = semibold ? typefaces().semibold : typefaces().regular;
    return juce::Font (juce::FontOptions (tf).withHeight (height));
}

juce::Colour plotColour (int index)
{
    static const juce::Colour colours[] = {
        juce::Colour (0xff00d4aa), juce::Colour (0xffffb300), juce::Colour (0xffe57373),
        juce::Colour (0xff64b5f6), juce::Colour (0xffba68c8), juce::Colour (0xffaed581),
        juce::Colour (0xfff06292), juce::Colour (0xff4dd0e1) };
    return colours[(size_t) (((index % 8) + 8) % 8)];
}

LookAndFeel::LookAndFeel()
{
    setColourScheme ({ panelBackground, appBackground, panelBackground, border, text,
                       panelBackground, text, accent, text });
    setColour (juce::ResizableWindow::backgroundColourId, appBackground);
    setColour (juce::TextButton::buttonColourId, juce::Colour (0xff333333));
    setColour (juce::TextButton::buttonOnColourId, accent);
    setColour (juce::TextButton::textColourOffId, text);
    setColour (juce::TextButton::textColourOnId, juce::Colours::white);
    setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff2b2b2b));
    setColour (juce::ComboBox::outlineColourId, border);
    setColour (juce::ComboBox::textColourId, text);
    setColour (juce::ComboBox::arrowColourId, textDim);
    setColour (juce::PopupMenu::backgroundColourId, juce::Colour (0xff2b2b2b));
    setColour (juce::PopupMenu::textColourId, text);
    setColour (juce::PopupMenu::headerTextColourId, textDim);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
    setColour (juce::PopupMenu::highlightedTextColourId, juce::Colours::white);
    setColour (juce::Label::textColourId, text);
    setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff2b2b2b));
    setColour (juce::TextEditor::textColourId, text);
    setColour (juce::TextEditor::outlineColourId, border);
    setColour (juce::TextEditor::focusedOutlineColourId, accent);
    setColour (juce::Slider::trackColourId, accent);
    setColour (juce::Slider::thumbColourId, juce::Colour (0xffe8e8e8));
    setColour (juce::Slider::backgroundColourId, juce::Colour (0xff3a3a3a));
    setColour (juce::Slider::textBoxTextColourId, text);
    setColour (juce::Slider::textBoxOutlineColourId, border);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff2b2b2b));
    setColour (juce::ToggleButton::textColourId, text);
    setColour (juce::ToggleButton::tickColourId, accent);
    setColour (juce::ToggleButton::tickDisabledColourId, textDim);
    setColour (juce::ListBox::backgroundColourId, panelBackground);
    setColour (juce::ListBox::textColourId, text);
    setColour (juce::ScrollBar::thumbColourId, juce::Colour (0xff5a5a5a));
    setColour (juce::AlertWindow::backgroundColourId, panelBackground);
    setColour (juce::AlertWindow::textColourId, text);
    setColour (juce::AlertWindow::outlineColourId, border);
}

juce::Typeface::Ptr LookAndFeel::getTypefaceForFont (const juce::Font& f)
{
    if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName()
        || f.getTypefaceName() == "<Sans-Serif>")
        return f.isBold() ? typefaces().semibold : typefaces().regular;
    return LookAndFeel_V4::getTypefaceForFont (f);
}

juce::Font LookAndFeel::getTextButtonFont (juce::TextButton&, int buttonHeight)
{
    return font (juce::jlimit (14.0f, 18.0f, (float) buttonHeight * 0.42f), true);
}

juce::Font LookAndFeel::getLabelFont (juce::Label& l)
{
    return font (l.getFont().getHeight());
}

juce::Font LookAndFeel::getComboBoxFont (juce::ComboBox& b)
{
    return font (juce::jlimit (14.0f, 17.0f, (float) b.getHeight() * 0.42f));
}

juce::Font LookAndFeel::getPopupMenuFont()
{
    return font (18.0f);
}

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b,
                                        const juce::Colour& background, bool highlighted, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    auto c = background;
    if (! b.isEnabled()) c = c.withMultipliedAlpha (0.45f);
    else if (down) c = c.brighter (0.25f);
    else if (highlighted) c = c.brighter (0.1f);
    g.setColour (c);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (border);
    g.drawRoundedRectangle (r, 6.0f, 1.0f);
}
} // namespace scm::style
