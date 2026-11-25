#include "TouchControls.h"
#include "MobileStyle.h"
#include "SafeArea.h"

namespace scm
{
namespace numpad
{
namespace
{
// Keys do not take the focus, so the pad keeps it for a hardware keyboard.
class Key : public juce::TextButton
{
public:
    enum class Kind { Text, Backspace, Ok };
    Key (const juce::String& label, Kind k = Kind::Text) : juce::TextButton (label), kind (k)
    {
        setWantsKeyboardFocus (false);
        if (kind == Kind::Ok) setColour (juce::TextButton::buttonColourId, style::accent);
        if (kind == Kind::Backspace) setTitle ("Delete");
    }

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        if (kind != Kind::Backspace)
        {
            juce::TextButton::paintButton (g, over, down);
            return;
        }
        getLookAndFeel().drawButtonBackground (g, *this, findColour (juce::TextButton::buttonColourId), over, down);
        // a key pointing left with a cross in it
        const auto b = getLocalBounds().toFloat().withSizeKeepingCentre (30.0f, 20.0f);
        juce::Path p;
        p.startNewSubPath (b.getX(), b.getCentreY());
        p.lineTo (b.getX() + 9.0f, b.getY());
        p.lineTo (b.getRight(), b.getY());
        p.lineTo (b.getRight(), b.getBottom());
        p.lineTo (b.getX() + 9.0f, b.getBottom());
        p.closeSubPath();
        const auto c = b.withTrimmedLeft (9.0f).getCentre();
        p.startNewSubPath (c.x - 4.0f, c.y - 4.0f); p.lineTo (c.x + 4.0f, c.y + 4.0f);
        p.startNewSubPath (c.x + 4.0f, c.y - 4.0f); p.lineTo (c.x - 4.0f, c.y + 4.0f);
        g.setColour (isEnabled() ? style::text : style::textDim);
        g.strokePath (p, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

private:
    Kind kind;
};

class Pad : public juce::Component
{
public:
    explicit Pad (Request r)
        : request (std::move (r)),
          entry (request.value ? format (*request.value, request.decimals) : juce::String(), request.maximum <= 0.0)
    {
        for (int d = 0; d <= 9; ++d)
        {
            auto* k = keys.add (new Key (juce::String (d)));
            k->onClick = [this, d] { entry.digit (d); repaint(); };
        }
        point.onClick = [this] { entry.point(); repaint(); };
        sign.onClick = [this] { entry.toggleSign(); repaint(); };
        back.onClick = [this] { entry.backspace(); repaint(); };
        ok.onClick = [this] { enter(); };
        point.setEnabled (request.decimals > 0);
        sign.setEnabled (request.minimum < 0.0);
        for (auto* k : keys) addAndMakeVisible (k);
        for (auto* k : { &point, &sign, &back, &ok }) addAndMakeVisible (k);

        setWantsKeyboardFocus (true);
        setSize (kKeyW * 4 + kGap * 3 + kMargin * 2,
                 kMargin + kTitleH + kGap + kDisplayH + kGap + kKeyH * 4 + kGap * 3 + kMargin);
        juce::Component::SafePointer<Pad> safe (this);
        juce::MessageManager::callAsync ([safe] { if (safe != nullptr && safe->isShowing()) safe->grabKeyboardFocus(); });
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (kMargin);
        auto top = r.removeFromTop (kTitleH);
        g.setColour (style::text);
        g.setFont (style::font (15.0f, true));
        g.drawText (request.title, top, juce::Justification::centredLeft, true);
        g.setColour (style::textDim);
        g.setFont (style::font (13.0f));
        g.drawText (format (request.minimum, request.decimals) + " to " + format (request.maximum, request.decimals)
                        + request.suffix,
                    top, juce::Justification::centredRight, true);
        r.removeFromTop (kGap);

        const auto display = r.removeFromTop (kDisplayH).toFloat();
        g.setColour (style::plotBackground);
        g.fillRoundedRectangle (display, 6.0f);
        g.setColour (style::border);
        g.drawRoundedRectangle (display.reduced (0.5f), 6.0f, 1.0f);

        const auto font = style::font (26.0f, true);
        const auto number = entry.text();
        const auto v = entry.value();
        const bool outside = v && (*v < request.minimum || *v > request.maximum);
        const float numberW = juce::GlyphArrangement::getStringWidth (font, number);
        const float unitW = juce::GlyphArrangement::getStringWidth (style::font (18.0f), request.suffix);
        auto text = display.reduced (12.0f, 0.0f);
        auto unitArea = text.removeFromRight (unitW + 2.0f);
        auto numberArea = text.removeFromRight (std::min (text.getWidth(), numberW + 4.0f));
        if (entry.isFresh() && number.isNotEmpty())   // selected: the next key replaces it
        {
            g.setColour (style::selectionFill);
            g.fillRoundedRectangle (numberArea.reduced (0.0f, 8.0f).expanded (3.0f, 0.0f), 4.0f);
        }
        g.setColour (outside ? style::warning : style::text);
        g.setFont (font);
        g.drawText (number, numberArea, juce::Justification::centredRight, false);
        g.setColour (style::textDim);
        g.setFont (style::font (18.0f));
        g.drawText (request.suffix, unitArea, juce::Justification::centredRight, false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (kMargin);
        r.removeFromTop (kTitleH + kGap + kDisplayH + kGap);
        const auto cell = [&] (int col, int row, int cols = 1, int rows = 1)
        {
            return juce::Rectangle<int> (r.getX() + col * (kKeyW + kGap), r.getY() + row * (kKeyH + kGap),
                                         cols * kKeyW + (cols - 1) * kGap, rows * kKeyH + (rows - 1) * kGap);
        };
        // 7 8 9 <x]
        // 4 5 6 +/-
        // 1 2 3 OK
        // 0 0 . OK
        for (int d = 1; d <= 9; ++d)
            keys[d]->setBounds (cell ((d - 1) % 3, 2 - (d - 1) / 3));
        back.setBounds (cell (3, 0));
        sign.setBounds (cell (3, 1));
        ok.setBounds (cell (3, 2, 1, 2));
        keys[0]->setBounds (cell (0, 3, 2));
        point.setBounds (cell (2, 3));
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto c = k.getTextCharacter();
        if (c >= '0' && c <= '9') { entry.digit ((int) (c - '0')); repaint(); return true; }
        if ((c == '.' || c == ',') && point.isEnabled()) { entry.point(); repaint(); return true; }
        if (c == '-' && sign.isEnabled()) { entry.toggleSign(); repaint(); return true; }
        if (k.isKeyCode (juce::KeyPress::backspaceKey)) { entry.backspace(); repaint(); return true; }
        if (k.isKeyCode (juce::KeyPress::returnKey)) { enter(); return true; }
        if (k.isKeyCode (juce::KeyPress::escapeKey)) { close(); return true; }
        return false;
    }

private:
    void enter()
    {
        if (const auto v = entry.value(); v && request.onEnter)
            request.onEnter (juce::jlimit (request.minimum, request.maximum, *v));
        close();
    }

    void close()
    {
        if (auto* box = findParentComponentOfClass<juce::CallOutBox>())
            box->dismiss();
    }

    static constexpr int kKeyW = 62, kKeyH = 50, kGap = 6, kMargin = 8, kTitleH = 22, kDisplayH = 48;
    Request request;
    Entry entry;
    juce::OwnedArray<Key> keys;
    Key point { "." }, sign { "+/-" }, back { {}, Key::Kind::Backspace }, ok { "OK", Key::Kind::Ok };
};
} // namespace

void show (juce::Component& target, Request r, juce::Rectangle<int> areaInTarget)
{
    auto& screen = safearea::screenOf (target);
    const auto area = screen.getLocalArea (&target, areaInTarget.isEmpty() ? target.getLocalBounds() : areaInTarget);
    auto& box = juce::CallOutBox::launchAsynchronously (std::make_unique<Pad> (std::move (r)), area, &screen);
    box.setArrowSize (12.0f);
    box.setDismissalMouseClicksAreAlwaysConsumed (true);   // the tap that closes it moves nothing
    box.updatePosition (area, safearea::safeBoundsOf (target));
}
} // namespace numpad

//==============================================================================
TouchSlider::TouchSlider()
{
    setViewportIgnoreDragFlag (true);
    setSliderSnapsToMousePosition (false);
    addMouseListener (&opener, true);   // the value box too, even when it is made again
}

TouchSlider::~TouchSlider()
{
    removeMouseListener (&opener);
}

void TouchSlider::setup (const juce::String& title, double lo, double hi, double interval,
                         const juce::String& suffix, int decimals, int textBoxWidth)
{
    setTitle (title);
    setSliderStyle (juce::Slider::LinearHorizontal);
    setTextBoxStyle (juce::Slider::TextBoxRight, true, textBoxWidth, 30);   // read-only: the pad types
    setRange (lo, hi, interval);
    setNumDecimalPlacesToDisplay (decimals);
    setTextValueSuffix (suffix);
}

void TouchSlider::Opener::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent == &slider || dynamic_cast<juce::Label*> (e.eventComponent) == nullptr
        || e.mouseWasDraggedSinceMouseDown() || ! slider.isEnabled())
        return;
    numpad::Request r;
    r.title = slider.getTitle();
    r.suffix = slider.getTextValueSuffix();
    r.value = slider.getValue();
    r.minimum = slider.getMinimum();
    r.maximum = slider.getMaximum();
    r.decimals = slider.getNumDecimalPlacesToDisplay();
    juce::Component::SafePointer<juce::Slider> safe (&slider);
    r.onEnter = [safe] (double v) { if (safe != nullptr) safe->setValue (v, juce::sendNotificationSync); };
    numpad::show (*e.eventComponent, std::move (r));
}

//==============================================================================
NumberField::NumberField (const juce::String& title, double lo, double hi, int places, const juce::String& unit)
    : minimum (lo), maximum (hi), decimals (places), suffix (unit)
{
    setTitle (title);
    setWantsKeyboardFocus (false);
}

void NumberField::setValue (std::optional<double> v, juce::NotificationType n)
{
    if (v) v = juce::jlimit (minimum, maximum, *v);
    if (v == value) return;
    value = v;
    repaint();
    if (n != juce::dontSendNotification && onChange) onChange();
}

void NumberField::paint (juce::Graphics& g)
{
    const auto b = getLocalBounds().toFloat().reduced (0.5f);
    g.setColour (juce::Colour (0xff2b2b2b));
    g.fillRoundedRectangle (b, 5.0f);
    g.setColour (style::border);
    g.drawRoundedRectangle (b, 5.0f, 1.0f);
    g.setFont (style::font (16.0f));
    if (value)
    {
        g.setColour (style::text);
        g.drawText (numpad::format (*value, decimals) + suffix, b.reduced (8.0f, 0.0f), juce::Justification::centredLeft, true);
    }
    else
    {
        g.setColour (style::textDim);
        g.drawText ("Tap to type", b.reduced (8.0f, 0.0f), juce::Justification::centredLeft, true);
    }
}

void NumberField::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasDraggedSinceMouseDown() || ! isEnabled()) return;   // a drag scrolls the panel
    numpad::Request r;
    r.title = getTitle();
    r.suffix = suffix;
    r.value = value;
    r.minimum = minimum;
    r.maximum = maximum;
    r.decimals = decimals;
    juce::Component::SafePointer<NumberField> safe (this);
    r.onEnter = [safe] (double v) { if (safe != nullptr) safe->setValue (v, juce::sendNotificationSync); };
    numpad::show (*this, std::move (r));
}

//==============================================================================
void setupScrolling (juce::Viewport& v)
{
    v.setScrollBarsShown (true, false);
    v.setScrollBarThickness (kScrollBarW);
    v.setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::all);
    auto& bar = v.getVerticalScrollBar();
    bar.setColour (juce::ScrollBar::thumbColourId, style::textDim);
    bar.setColour (juce::ScrollBar::trackColourId, style::barBackground);
}
} // namespace scm
