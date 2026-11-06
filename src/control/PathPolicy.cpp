// SpectroCraft - AI control: path roots (see include/control/PathPolicy.h).
#include "control/PathPolicy.h"
#include "control/ControlServer.h"
#include "Application.h"
#include "PythonScripts.h"

#if !JUCE_WINDOWS
 #include <climits>
 #include <cstdlib>
#endif

namespace sc::control {

juce::File canonicalFile (const juce::File& f)
{
#if JUCE_WINDOWS
    // Windows: resolve a link on the final component only (junctions in the
    // middle of a path are rare in user folders; see lot-1.md).
    return f.isSymbolicLink() ? f.getLinkedTarget() : f;
#else
    // realpath() on the deepest existing ancestor, then re-append the rest.
    juce::File existing = f;
    juce::StringArray tail;
    while (!existing.exists() && existing.getParentDirectory() != existing)
    {
        tail.insert (0, existing.getFileName());
        existing = existing.getParentDirectory();
    }
    char buf[PATH_MAX];
    juce::File base = existing;
    if (::realpath (existing.getFullPathName().toRawUTF8(), buf) != nullptr)
        base = juce::File (juce::String::fromUTF8 (buf));
    for (const auto& t : tail)
        base = base.getChildFile (t);
    return base;
#endif
}

juce::Array<juce::File> allowedRoots (Application& app)
{
    juce::Array<juce::File> roots;
    const juce::String custom = app.recentStore != nullptr ? app.recentStore->getValue ("control_roots") : juce::String();
    if (custom.trim().isNotEmpty())
    {
        for (const auto& r : juce::StringArray::fromTokens (custom, ";", "\""))
            if (juce::File::isAbsolutePath (r.trim()))
                roots.add (canonicalFile (juce::File (r.trim())));
    }
    else
    {
        roots.add (canonicalFile (juce::File::getSpecialLocation (juce::File::userHomeDirectory)));
        roots.add (canonicalFile (sc::py::scriptFolder()));
        roots.add (canonicalFile (juce::File::getSpecialLocation (juce::File::tempDirectory)));
#if !JUCE_WINDOWS
        // JUCE's tempDirectory is $TMPDIR/<app name> on macOS: the user's
        // temporary folder itself is $TMPDIR (or /tmp).
        const juce::String envTmp = juce::SystemStats::getEnvironmentVariable ("TMPDIR", {});
        if (juce::File::isAbsolutePath (envTmp))
            roots.add (canonicalFile (juce::File (envTmp)));
        roots.add (canonicalFile (juce::File ("/tmp")));
#endif
    }
    return roots;
}

static bool under (const juce::File& f, const juce::File& root)
{
    return f == root || f.isAChildOf (root);
}

Outcome checkPath (Application& app, const juce::File& f, bool forWrite, bool confirmed,
                   const juce::String& what)
{
    const juce::File c = canonicalFile (f);

    // Never: the control folder (tokens) and the settings store.
    const juce::File ctl = canonicalFile (ControlServer::discoveryFolder());
    const juce::File store = app.recentStore != nullptr ? canonicalFile (app.recentStore->getFile()) : juce::File();
    if (under (c, ctl) || (store != juce::File() && c == store))
        return Outcome::failure (err::failed, what + " is a protected application file",
                                 obj ({ { "path", f.getFullPathName() }, { "reason", "protected" } }));

    bool inside = false;
    for (const auto& r : allowedRoots (app))
        if (under (c, r)) { inside = true; break; }
    if (!inside && !confirmed)
        return Outcome::failure (err::confirmRequired,
                                 what + " is outside the allowed folders; resend with params.confirm=true",
                                 obj ({ { "path", f.getFullPathName() }, { "resolved", c.getFullPathName() },
                                        { "reason", "outside_roots" },
                                        { "hint", "allowed: home, scripts and temp folders (setting control_roots)" } }));
    juce::ignoreUnused (forWrite);
    return Outcome::success (obj ({ { "path", c.getFullPathName() } }));
}

} // namespace sc::control
