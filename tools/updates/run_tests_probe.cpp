// Standalone runner for the update tests (tests/regression/updates.inc) so the
// manifest / staging / plan / result logic can actually be executed here even
// though the full app cannot be linked (this machine has CMake 3.19 < 3.22).
// The same test bodies, the same helpers, minus the ones that need the app.
#include <juce_core/juce_core.h>

static int total = 0, failures = 0;
static juce::StringArray skippedTests;

#define CHECK(cond, name) \
    do { ++total; if (!(cond)) { ++failures; std::cout << "FAIL: " << name << "\n"; } } while (0)

static void scSkip (const juce::String& why) { skippedTests.add (why); std::cout << "SKIP: " << why << "\n"; }
static void scMissingFixture (const juce::String& name) { scSkip ("test fixture " + name + " not found"); }

static juce::File scTestTempDir()
{
    static const juce::File dir = [] {
        auto d = juce::File::getSpecialLocation (juce::File::tempDirectory)
                     .getChildFile ("SpectroCraft-upd-tests");
        d.deleteRecursively();
        d.createDirectory();
        return d;
    }();
    return dir;
}

static juce::File makeTempFile (const juce::String& name)
{
    static int counter = 0;
    return scTestTempDir().getChildFile ("sc_test_" + juce::String (++counter) + "_" + name);
}

// The app's real Settings struct (header-only for these fields): updTest_setting
// checks the default and samePersistent behaviour of the new key.
#include "Settings.h"
#include "tests/regression/updates.inc"

int main()
{
    scupdtest::updTest_ed25519();
    scupdtest::updTest_hashes();
    scupdtest::updTest_versions();
    scupdtest::updTest_assets();
    scupdtest::updTest_releaseJson();
    scupdtest::updTest_manifest();
    scupdtest::updTest_staging();
    scupdtest::updTest_planAndResult();
    scupdtest::updTest_folders();
    scupdtest::updTest_setting();

    // Optional live check of the lookup path (not part of the suite): SC_UPD_LIVE=1
    if (juce::SystemStats::getEnvironmentVariable ("SC_UPD_LIVE", "").isNotEmpty())
    {
        scupd::ReleaseInfo rel;
        juce::String err;
        if (scupd::fetchLatestRelease (rel, err))
        {
            std::cout << "LIVE: tag=" << rel.tag << " version=" << rel.version
                      << " title=" << rel.title << " assets=" << rel.assets.size() << "\n";
            for (auto& a : rel.assets)
                std::cout << "  asset " << a.name << " " << a.size << "\n";
            const scupd::AssetInfo* pick = scupd::pickAsset (rel);
            std::cout << "  pick=" << (pick ? pick->name : juce::String ("<none>"))
                      << " manifest=" << (scupd::findAsset (rel, scupd::kManifestName) ? "yes" : "no")
                      << "\n";
        }
        else std::cout << "LIVE: lookup failed: " << err << "\n";
    }

    std::cout << (total - failures) << "/" << total << " checks passed\n";
    for (auto& s : skippedTests) std::cout << "skipped: " << s << "\n";
    scTestTempDir().deleteRecursively();
    return failures == 0 ? 0 : 1;
}
