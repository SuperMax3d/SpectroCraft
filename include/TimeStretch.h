#pragma once
// SpectroCraft - pitch-preserving time stretch (SoundTouch WSOLA engine).
//
// Thin wrapper around the vendored SoundTouch library (extern/soundtouch,
// LGPL-2.1) that Audacity uses for its Change Tempo effect. The tempo change
// is expressed as a percentage exactly like Audacity's "Change Tempo" dialog:
//   +50 %  -> plays 1.5x faster, output is 2/3 of the input length
//   -50 %  -> plays half speed,    output is twice the input length
// Pitch is preserved (WSOLA time-domain stretching).

#include <vector>

namespace sc { namespace stretch {

/** Runs a pitch-preserving tempo change on one mono channel.
    @param in            Input samples (mono).
    @param len           Number of input samples.
    @param rate          Sample rate in Hz.
    @param tempoPercent  Tempo change in percent (+ faster / - slower),
                         Audacity semantics: new length ~= len * 100/(100+pct).
    @param out           Receives the stretched samples (resized to the exact
                         output length produced by SoundTouch).
    @return The number of output samples written. */
int stretchTempo (const float* in, int len, double rate, float tempoPercent,
                  std::vector<float>& out);

}} // namespace sc::stretch
