// iOS: the safe area of the component's own UIWindow (see SafeArea.h).
// No "using namespace juce": see AudioRoute_ios.mm.
#define DONT_SET_USING_JUCE_NAMESPACE 1
#include "SafeArea.h"
#import <UIKit/UIKit.h>

namespace scm::safearea
{
juce::BorderSize<int> insetsFor (const juce::Component& c)
{
    if (auto* peer = c.getPeer())
        if (auto* view = (__bridge UIView*) peer->getNativeHandle())
            if (UIWindow* window = view.window)
            {
                const UIEdgeInsets in = window.safeAreaInsets;
                const double scale = (double) juce::Desktop::getInstance().getGlobalScaleFactor();
                return { juce::roundToInt (in.top / scale), juce::roundToInt (in.left / scale),
                         juce::roundToInt (in.bottom / scale), juce::roundToInt (in.right / scale) };
            }

    // Not on screen yet: JUCE's value, corrected once the window exists.
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return d->safeAreaInsets;
    return {};
}
} // namespace scm::safearea
