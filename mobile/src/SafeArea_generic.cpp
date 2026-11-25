// Android: JUCE's display insets (refreshed from the window's insets
// listener). Desktop development build: none (see SafeArea.h).
#include "SafeArea.h"

namespace scm::safearea
{
juce::BorderSize<int> insetsFor (const juce::Component&)
{
   #if JUCE_ANDROID
    if (auto* d = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        return d->safeAreaInsets;
   #endif
    return {};
}
} // namespace scm::safearea
