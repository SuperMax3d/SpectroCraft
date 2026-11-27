#include "StepSlider.h"
#include "MobileStyle.h"
#include "SafeArea.h"

namespace scm
{
namespace
{
constexpr float kInset = 14.0f;       // from the edges to the first and last steps
constexpr float kGroupPitch = 22.0f;  // the group's steps, and the gap before them (easy to hit)
constexpr float kNameH = 18.0f;       // the current step's name, above the thumb
constexpr float kTickLabelH = 14.0f;  // the tick labels, under the track
constexpr float kThumbR = 9.0f;
constexpr float kHintH = 13.0f;       // the readout's second line

float textWidth (const juce::Font& f, const juce::String& s)
{
    return juce::GlyphArrangement::getStringWidth (f, s);
}
} // namespace

//==============================================================================
/** Above the finger, on the screen (over the other views): the step's name
    between its neighbours', and how to make the drag fine. */
class StepSlider::Readout : public juce::Component
{
public:
    Readout() { setInterceptsMouseClicks (false, false); }

    juce::String previous, name, next;
    bool fine = false;

    static juce::Font nameFont() { return style::font (17.0f, true); }
    static juce::Font sideFont() { return style::font (13.0f); }
    static juce::Font hintFont() { return style::font (11.0f); }
    static juce::String hint (bool fine) { return fine ? "fine: 4x slower" : "hold still for finer steps"; }

    float sideWidth() const { return std::max (textWidth (sideFont(), previous), textWidth (sideFont(), next)) + 8.0f; }

    juce::Point<int> wantedSize() const
    {
        const float w = std::max (textWidth (nameFont(), name) + 2.0f * sideWidth() + 28.0f,
                                  textWidth (hintFont(), hint (fine)) + 24.0f);
        return { (int) std::ceil (w), (int) std::ceil (24.0f + kHintH + 10.0f) };
    }

    void paint (juce::Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (style::barBackground.withAlpha (0.94f));
        g.fillRoundedRectangle (b, 6.0f);
        g.setColour (fine ? style::selectionEdge : style::border);
        g.drawRoundedRectangle (b, 6.0f, fine ? 2.0f : 1.0f);

        auto r = b.reduced (10.0f, 4.0f);
        const auto hintArea = r.removeFromBottom (kHintH);
        const float side = sideWidth();
        g.setFont (sideFont());
        g.setColour (style::textDim);
        g.drawText (previous, r.removeFromLeft (side), juce::Justification::centredLeft, false);
        g.drawText (next, r.removeFromRight (side), juce::Justification::centredRight, false);
        g.setFont (nameFont());
        g.setColour (fine ? style::selectionEdge : style::text);
        g.drawText (name, r, juce::Justification::centred, false);
        g.setFont (hintFont());
        g.setColour (style::textDim);
        g.drawText (hint (fine), hintArea, juce::Justification::centred, false);
    }
};

//==============================================================================
StepSlider::StepSlider()
{
    setSliderStyle (juce::Slider::LinearHorizontal);
    setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    setRange (0.0, 1.0, 1.0);
    setWantsKeyboardFocus (false);
}

StepSlider::~StepSlider()
{
    stopTimer();
    readout.reset();
}

void StepSlider::setSteps (const juce::StringArray& stepNames, const juce::StringArray& tickLabels, int groupFrom)
{
    const int keep = getStep();
    names = stepNames;
    ticks = tickLabels;
    while (ticks.size() < names.size()) ticks.add ({});
    group = groupFrom;
    setRange (0.0, (double) std::max (1, names.size() - 1), 1.0);
    setValue ((double) juce::jlimit (0, std::max (0, names.size() - 1), keep), juce::dontSendNotification);
    syncTrack();
    repaint();
}

void StepSlider::setStep (int step, juce::NotificationType n)
{
    setValue ((double) juce::jlimit (0, std::max (0, names.size() - 1), step), n);
}

float StepSlider::xOfStep (int i) const
{
    return drag.xOf (i);
}

void StepSlider::syncTrack()
{
    const int n = names.size();
    const int inGroup = group > 0 && group < n ? n - group : 0;
    const float x0 = kInset, x1 = std::max (x0, (float) getWidth() - kInset);
    std::vector<float> xs;
    if (n == 1)
        xs.push_back ((x0 + x1) * 0.5f);
    else if (n > 1)
    {
        const int main = n - inGroup;
        const float mainW = std::max (0.0f, x1 - x0 - kGroupPitch * (float) inGroup);
        for (int i = 0; i < main; ++i)
            xs.push_back (main > 1 ? x0 + mainW * (float) i / (float) (main - 1) : x0);
        for (int j = 0; j < inGroup; ++j)
            xs.push_back (x0 + mainW + kGroupPitch * (float) (j + 1));
    }
    drag.setPositions (std::move (xs));
    if (! drag.fingerDown()) drag.setStep (getStep());
}

void StepSlider::resized()
{
    juce::Slider::resized();
    syncTrack();
}

float StepSlider::trackY() const
{
    return kNameH + ((float) getHeight() - kNameH - kTickLabelH) * 0.5f;
}

juce::String StepSlider::getTextFromValue (double v)
{
    const int i = juce::roundToInt (v);
    return juce::isPositiveAndBelow (i, names.size()) ? names[i] : juce::String();
}

// ---------------------------------------------------------------- drawing

void StepSlider::paint (juce::Graphics& g)
{
    const int n = names.size();
    if (n == 0) return;
    const bool enabled = isEnabled();
    if (! enabled) g.beginTransparencyLayer (0.4f);

    const float y = trackY();
    const int groupStart = group > 0 && group < n ? group : n;

    // the track: the main steps, then the group a little apart
    g.setColour (style::textDim.withAlpha (0.35f));
    const auto segment = [&] (int a, int b)
    {
        const float l = xOfStep (a) - 4.0f, r = xOfStep (b) + 4.0f;
        g.fillRoundedRectangle (l, y - 2.0f, r - l, 4.0f, 2.0f);
    };
    segment (0, groupStart - 1);
    if (groupStart < n) segment (groupStart, n - 1);

    // a tick under every step, longer where it has a label
    for (int i = 0; i < n; ++i)
    {
        const bool labelled = ticks[i].isNotEmpty();
        g.setColour (labelled ? style::textDim : style::textDim.withAlpha (0.55f));
        g.fillRect (xOfStep (i) - 0.5f, y + 4.0f, 1.0f, labelled ? 6.0f : 3.0f);
    }

    // tick labels, left to right, leaving out any that would touch the one before
    const auto tickFont = style::font (11.0f);
    g.setFont (tickFont);
    g.setColour (style::axisMinorLabel);
    float lastRight = -1.0e9f;
    for (int i = 0; i < n; ++i)
    {
        if (ticks[i].isEmpty()) continue;
        const float w = textWidth (tickFont, ticks[i]) + 2.0f;
        const float x = juce::jlimit (0.0f, std::max (0.0f, (float) getWidth() - w), xOfStep (i) - w * 0.5f);
        if (x < lastRight + 3.0f) continue;
        g.drawText (ticks[i], juce::Rectangle<float> (x, (float) getHeight() - kTickLabelH, w, kTickLabelH),
                    juce::Justification::centred, false);
        lastRight = x + w;
    }

    // the thumb, ringed while the drag is fine
    const int current = juce::jlimit (0, n - 1, getStep());
    const float tx = xOfStep (current);
    const juce::Rectangle<float> thumb (tx - kThumbR, y - kThumbR, 2.0f * kThumbR, 2.0f * kThumbR);
    g.setColour (style::accent);
    g.fillEllipse (thumb);
    g.setColour (drag.fine() ? style::selectionEdge : style::text.withAlpha (0.9f));
    g.drawEllipse (thumb, drag.fine() ? 2.5f : 1.5f);

    // the current step's name over the thumb
    const auto nameFont = style::font (14.0f, true);
    const float w = textWidth (nameFont, names[current]) + 4.0f;
    g.setFont (nameFont);
    g.setColour (style::text);
    g.drawText (names[current],
                juce::Rectangle<float> (juce::jlimit (0.0f, std::max (0.0f, (float) getWidth() - w), tx - w * 0.5f), 0.0f, w, kNameH),
                juce::Justification::centred, false);

    if (! enabled) g.endTransparencyLayer();
}

void StepSlider::updateReadout()
{
    if (! drag.fingerDown() || ! isShowing() || names.isEmpty())
    {
        readout.reset();
        return;
    }
    auto& screen = safearea::screenOf (*this);
    if (readout == nullptr || readout->getParentComponent() != &screen)
    {
        readout = std::make_unique<Readout>();
        screen.addAndMakeVisible (*readout);
    }
    const int s = juce::jlimit (0, names.size() - 1, getStep());
    readout->previous = s > 0 ? names[s - 1] : juce::String();
    readout->name = names[s];
    readout->next = s + 1 < names.size() ? names[s + 1] : juce::String();
    readout->fine = drag.fine();

    // above the finger (the finger hides the thumb), inside the screen's safe area
    const auto size = readout->wantedSize();
    const auto safe = safearea::safeBoundsOf (*this);
    const auto at = screen.getLocalPoint (this, finger).toInt();
    const int w = std::min (size.x, safe.getWidth());
    const int x = juce::jlimit (safe.getX(), std::max (safe.getX(), safe.getRight() - w), at.x - w / 2);
    const int top = std::max (safe.getY(), at.y - (int) precise::kLensGap - size.y);
    readout->setBounds (x, top, w, size.y);
    readout->toFront (false);
    readout->repaint();
}

// ---------------------------------------------------------------- the finger

void StepSlider::mouseDown (const juce::MouseEvent& e)
{
    if (touch >= 0 && ! juce::Desktop::getInstance().getMouseSource (touch))
        endTouch();   // a test touch (no such finger)
    if (touch < 0) press (e.position, e.source.getIndex());   // one finger drives it
}

void StepSlider::touchForTest (int i)
{
    syncTrack();
    press ({ xOfStep (i), trackY() }, 1000);
}

void StepSlider::press (juce::Point<float> at, int touchIndex)
{
    if (! isEnabled() || names.isEmpty()) return;
    touch = touchIndex;
    syncTrack();
    finger = at;
    setStep (drag.press (finger.x, juce::Time::getMillisecondCounter()), juce::sendNotificationSync);
    startTimerHz (30);
    updateReadout();
    repaint();
}

void StepSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (e.source.getIndex() != touch || ! drag.fingerDown()) return;
    finger = e.position;
    setStep (drag.move (finger.x, juce::Time::getMillisecondCounter()), juce::sendNotificationSync);
    updateReadout();
}

void StepSlider::mouseUp (const juce::MouseEvent& e)
{
    if (e.source.getIndex() == touch) endTouch();
}

void StepSlider::timerCallback()
{
    if (! drag.fingerDown())
    {
        stopTimer();
        return;
    }
    if (drag.hold (finger.x, juce::Time::getMillisecondCounter()))
    {
        stopTimer();   // fine until the finger is lifted
        updateReadout();
        repaint();
    }
}

void StepSlider::endTouch()
{
    touch = -1;
    drag.release();
    stopTimer();
    readout.reset();
    repaint();
}

void StepSlider::valueChanged()
{
    if (! drag.fingerDown()) drag.setStep (getStep());
    repaint();
    if (readout != nullptr) updateReadout();
}

void StepSlider::visibilityChanged()
{
    juce::Slider::visibilityChanged();
    if (! isShowing() && touch >= 0) endTouch();
}
} // namespace scm
