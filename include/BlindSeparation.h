#pragma once
// SpectroCraft - blind source separation of multi-channel recordings (issue
// #68): the sources (motor, airflow, speech...) are told apart by how each
// one reaches the microphones, with no model trained beforehand.
//
// Four methods work on the short-time spectrum of every channel:
//   AuxIVA     independent vector analysis (one demixing matrix per
//              frequency, the sources' frequencies kept together);
//   ILRMA      AuxIVA whose sources are also low-rank spectra (NMF);
//   cACGMM     clustering of the spatial directions of each time-frequency
//              point (complex angular central Gaussian mixture), then a mask
//              or a beamformer per source;
//   FastMNMF2  a full-rank spatial model with NMF sources and one jointly
//              diagonalised covariance per frequency.
// Each output is one source as the reference microphone hears it. Long
// inputs are processed in blocks (bounded memory): IVA / ILRMA start each
// block from the previous one's demixing, and the outputs of every block are
// put in the order of the previous one by their correlation over the overlap.
// The loops over frequencies run on every core (sc::par::forRange).
#include <JuceHeader.h>
#include <functional>
#include <string>
#include <vector>

namespace sc::bss
{
enum class Method { auxIva = 0, ilrma, cacgmm, fastMnmf2 };

struct MethodInfo
{
    Method method;
    const char* name;          // the combo's text
    const char* description;   // one sentence
    const char* pros;
    const char* cons;
    bool usesNmf;              // the NMF bases setting applies
    bool determined;           // sources <= channels, one demixing matrix (IVA family)
};
const std::vector<MethodInfo>& methods();
const MethodInfo& infoOf (Method m);

struct Params
{
    Method method = Method::auxIva;
    int sources = 0;              // 0 = as many as channels; at most the channel count
    int fftSize = 4096;           // STFT window (samples, a power of two); hop = fftSize / 4
    int iterations = 50;
    int nmfBases = 8;             // ILRMA, FastMNMF2: spectral bases per source
    int refChannel = 0;           // the microphone the outputs are heard at (0-based)
    double blockSeconds = 60.0;   // longer inputs are processed in blocks of this length
    double overlapSeconds = 2.0;  // between blocks (cross-faded, used to keep the order)
    bool noiseClass = true;       // cACGMM: one more class for diffuse noise (not output)
    bool beamformer = true;       // cACGMM: MVDR beamformer per source (else a mask on the reference)
    unsigned int seed = 1;        // random initialisation (reproducible)
};

/** The sources counted from the params and the channels: `sources` clamped to
    [1, channels], 0 meaning `channels`. */
int sourcesFor (const Params& p, int channels);

/** Progress callback: fraction 0..1; returns false to cancel. Called from the
    calling thread only. */
using Progress = std::function<bool (double fraction)>;

/** Separates `in` (one vector per channel, same length, at least 2
    channels) into sourcesFor() outputs of the same length. Returns false
    with errOut set on invalid input, or when cancelled ("cancelled"). */
bool separate (const std::vector<std::vector<float>>& in, double sampleRate, const Params& p,
               std::vector<std::vector<float>>& out, std::string* errOut = nullptr,
               const Progress& progress = {});

// ---- internals exposed for the tests ------------------------------------
/** STFT then inverse STFT of one signal (Hann windows, hop fftSize / 4):
    returns the signal itself (round-trip check). */
std::vector<float> stftRoundTrip (const std::vector<float>& x, int fftSize);
/** For each output, the index of the previous block's output it continues
    (largest |correlation| over the overlap), as a permutation. */
std::vector<int> matchOrder (const std::vector<std::vector<float>>& previousTail,
                             const std::vector<std::vector<float>>& currentHead);
} // namespace sc::bss
