// SpectroCraft - pitch-preserving time stretch (SoundTouch WSOLA engine).
#include "TimeStretch.h"

#include <algorithm>

// The soundtouch target exposes its headers directly (extern/soundtouch/include).
#include <SoundTouch.h>

namespace sc { namespace stretch {

int stretchTempo (const float* in, int len, double rate, float tempoPercent,
                  std::vector<float>& out)
{
    out.clear ();
    if (len <= 0 || in == nullptr || !(rate > 0.0))
        return 0;

    soundtouch::SoundTouch st;
    st.setChannels (1);
    st.setSampleRate ((unsigned int) (rate + 0.5));
    // Audacity semantics: positive percent = faster (shorter output).
    st.setTempoChange (tempoPercent);

    auto drain = [&] ()
    {
        const unsigned int ready = st.numSamples ();
        if (ready == 0) return;
        const size_t pos = out.size ();
        out.resize (pos + ready);
        st.receiveSamples (out.data () + pos, ready);
    };

    out.reserve ((size_t) len);
    const int kBlock = 8192;   // same feed size as Audacity's EffectSoundTouch
    for (int s = 0; s < len; s += kBlock)
    {
        const int block = std::min (kBlock, len - s);
        st.putSamples (in + s, (unsigned int) block);
        drain ();
    }

    // Flush the pipeline so the tail samples are emitted (Audacity does this
    // for tempo changes; it may add a few blank samples at the very end).
    st.flush ();
    drain ();

    return (int) out.size ();
}

}} // namespace sc::stretch
