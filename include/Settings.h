#pragma once
// SpectroCraft - shared settings and colour maps
#include <JuceHeader.h>
#include <array>
#include <vector>

// Shared layout constants so the waveform and spectral views line up:
// both use the same left-axis width (their plots start at the same x).
inline constexpr int kLeftAxisWidth = 60;

// Right-click context menu action IDs, shared by both views and MainWindow.
enum ContextMenuIDs : int
{
    CtxZoomToSelection = 1,
    CtxZoomOutFull     = 2,
    CtxCut             = 3,
    CtxCopy            = 4,
    CtxPaste           = 5,
    CtxDelete          = 6,
    CtxSelectAll       = 7,
    CtxMoveSelection   = 8,   // spectral view only: interactive move gesture
    CtxScaleSelection  = 9,   // spectral view only: interactive scale gesture
    CtxCopyToNewFile   = 10,
    CtxPasteMix        = 11,
    CtxSetHarmonicRefs = 12,   // Frequency Analysis only
    CtxCopyHarmonics   = 13,   // Frequency Analysis only
    CtxResetHarmonic   = 14,   // Frequency Analysis only
    // Scale menu (right-click on either view): time axis + vertical axis.
    CtxScaleTimeTime      = 15,
    CtxScaleTimeOffset    = 16,   // prompts for the time at the playhead
    CtxScaleTimeSample    = 17,
    CtxScaleWaveWavUnits  = 18,   // waveform vertical axis only
    CtxScaleWaveDb        = 19,
    CtxScaleWaveNorm      = 20,
    CtxScaleSpecHz        = 21,   // spectrogram frequency axis only
    CtxScaleSpecCustom    = 22,   // arms the reference-click flow
    // Alternative Scale menu (spectrogram right-click only).
    CtxAltScaleTime       = 23,   // "New" time reference: arms the ref-click flow
    CtxAltScaleFreq       = 24,   // "New" frequency reference: arms the ref-click flow
    CtxAltRefTimeBase     = 100,  // + index: edit an existing time reference
    CtxAltRefFreqBase     = 200   // + index: edit an existing frequency reference
};

// Time-axis scale for the shared time window (both views, footer, markers).
enum class TimeAxisMode : int { Time = 0, TimeOffset = 1, Sample = 2 };

struct Settings
{
    int fftSize = 2048;
    int hopSize = 512;
    int windowType = 1;   // 0=rect 1=hann 2=hamming
    bool logFrequency = true;
    float minFrequency = 20.0f;
    float maxFrequency = 20000.0f;
    // Spectrogram level window in dB relative to full scale (0 = FS): the
    // colour map spans [dbMinDb, dbMaxDb] and magnitudes outside it clamp.
    float dbMinDb = -120.0f;    // bottom of the window (left end of the palette)
    float dbMaxDb = 0.0f;       // top of the window (right end; 0 = full scale)

    /** Window width in dB (the "dB range"). */
    float dbRange() const { return dbMaxDb - dbMinDb; }
    float gain = 1.0f;
    float bias = 0.0f;
    int smoothing = 1;          // 0=off, 1=light box blur on magnitudes
    bool snapToZeroCrossings = false;   // OFF by default; persisted between sessions

    // Frequency Analysis tab state (independent of the spectrogram settings).
    // Persisted between sessions: axis mode and its own FFT size.
    int fftAxisMode = 1;        // 0=FFT Linear, 1=FFT Log, 2=1/3 Octave,
                                // 3=Freq vs Time, 4=Amp vs Time, 5=Amp vs Freq
    int fftLocalN = 2048;       // Frequency Analysis FFT size (power of two)
    // Butterworth bandpass order for the 1/3-octave dB-SPL pipeline (one of
    // 1, 2, 3, 4, 5, 10, 20, 30 or 40; the filter itself is 2*order).
    int fftThirdOctOrder = 40;
    // Curve-mode options (modes 3..5), persisted between sessions:
    int fftRefTime = 0;         // time-based modes: 0=Relative, 1=Absolute
    int fftSmooth = 0;          // curve smoothing window in frames: 0=None, or 3/5/7/9/11
    int fftTrack = 0;           // 0=Max (strongest component), 1=Total, 2=Weighted

    // Drawing panel defaults - the last used ink settings for NEW drawings.
    // Persisted between sessions (saved by the Drawing panel on change); not
    // part of operator==/samePersistent so changing them never invalidates
    // rendered spectral images.
    float drawWidth = 2.0f;             // line width in px (Arrow/Rect/Freehand)
    int drawColourARGB = 0xFF00FF00;     // ink colour, AARRGGBB (bright green)
    float drawTextSize = 14.0f;          // font size for Text drawings

    bool operator== (const Settings& o) const
    {
        return fftSize == o.fftSize && hopSize == o.hopSize && windowType == o.windowType
            && logFrequency == o.logFrequency && minFrequency == o.minFrequency
            && maxFrequency == o.maxFrequency
            && dbMinDb == o.dbMinDb && dbMaxDb == o.dbMaxDb
            && gain == o.gain && bias == o.bias && smoothing == o.smoothing;
    }

    // Compares exactly the fields that are persisted between sessions. Used by
    // MainWindow to trigger a settings save when any of them changes.
    bool samePersistent (const Settings& o) const
    {
        return fftSize == o.fftSize && logFrequency == o.logFrequency
            && snapToZeroCrossings == o.snapToZeroCrossings
            && fftAxisMode == o.fftAxisMode && fftLocalN == o.fftLocalN
            && fftThirdOctOrder == o.fftThirdOctOrder
            && fftRefTime == o.fftRefTime && fftSmooth == o.fftSmooth
            && fftTrack == o.fftTrack;
    }
};

enum class ColourMap : int
{
    Classic = 0, Viridis, Plasma, Inferno, Magma, Hot, Cool, Grayscale
};

// Control points for the colour maps: { position, r, g, b }
struct ColourStop { float pos; uint8_t r, g, b; };

inline const std::array<std::array<ColourStop, 8>, 8>& colourMapStops()
{
    static const std::array<std::array<ColourStop, 8>, 8> maps = { {
        // 0: Classic default spectral map (the traditional DAW spectrogram ramp):
        //    black -> dark indigo -> purple -> magenta/pink -> red-orange -> orange -> yellow
        { { { 0.00f,   0,   0,   0 }, { 0.15f,  26,   0,  78 }, { 0.30f,  92,   0, 140 }, { 0.45f, 190,  20, 150 },
            { 0.60f, 255,  60,  80 }, { 0.75f, 255, 130,  20 }, { 0.90f, 255, 200,  40 }, { 1.00f, 255, 245,  80 } } },
        // 1: Viridis
        { { { 0.00f,  68,  10, 130 }, { 0.14f,  72,  40, 160 }, { 0.28f,  62,  74, 180 }, { 0.42f,  49, 104, 142 },
            { 0.57f,  53, 150, 122 }, { 0.71f, 109, 172,  83 }, { 0.86f, 180, 181,  41 }, { 1.00f, 253, 231,  37 } } },
        // 2: Plasma
        { { { 0.00f,  13,   8,  62 }, { 0.14f,  75,   3,  95 }, { 0.28f, 126,  35, 104 }, { 0.42f, 163,  58,  98 },
            { 0.57f, 199,  84,  80 }, { 0.71f, 229, 116,  59 }, { 0.86f, 248, 166,  45 }, { 1.00f, 252, 231,  37 } } },
        // 3: Inferno
        { { { 0.00f,   0,   0,   4 }, { 0.14f,  20,  12,  60 }, { 0.28f,  70,  20, 100 }, { 0.42f, 125,  45,  92 },
            { 0.57f, 180,  60,  70 }, { 0.71f, 230,  90,  40 }, { 0.86f, 250, 170,  40 }, { 1.00f, 252, 255, 140 } } },
        // 4: Magma
        { { { 0.00f,   0,   0,   4 }, { 0.14f,  22,  12,  60 }, { 0.28f,  65,  30, 100 }, { 0.42f, 120,  50, 110 },
            { 0.57f, 175,  75, 105 }, { 0.71f, 220, 105,  95 }, { 0.86f, 250, 160,  90 }, { 1.00f, 252, 240, 190 } } },
        // 5: Hot
        { { { 0.00f,   0,   0,   0 }, { 0.20f, 128,   0,   0 }, { 0.40f, 255,   0,   0 }, { 0.60f, 255, 160,   0 },
            { 0.80f, 255, 255,   0 }, { 1.00f, 255, 255, 255 }, { 1.00f, 255, 255, 255 }, { 1.00f, 255, 255, 255 } } },
        // 6: Cool
        { { { 0.00f,   0,   0,   0 }, { 0.20f,   0,   0, 160 }, { 0.40f,   0, 128, 255 }, { 0.60f, 128, 255, 255 },
            { 0.80f, 255, 255, 128 }, { 1.00f, 255, 255, 255 }, { 1.00f, 255, 255, 255 }, { 1.00f, 255, 255, 255 } } },
        // 7: Grayscale
        { { { 0.00f,   0,   0,   0 }, { 0.14f,  36,  36,  36 }, { 0.28f,  72,  72,  72 }, { 0.42f, 108, 108, 108 },
            { 0.57f, 144, 144, 144 }, { 0.71f, 180, 180, 180 }, { 0.86f, 216, 216, 216 }, { 1.00f, 255, 255, 255 } } },
    } };
    return maps;
}

// Build a 256-entry RGBA LUT for a colour map
inline std::array<uint8_t, 256 * 4> buildColourLUT (ColourMap map)
{
    const auto& stops = colourMapStops()[(int) map];
    std::array<uint8_t, 256 * 4> lut{};
    for (int i = 0; i < 256; ++i)
    {
        float t = (float) i / 255.0f;
        const ColourStop* a = &stops[0], * b = &stops[stops.size () - 1];
        for (size_t s = 0; s + 1 < stops.size (); ++s)
        {
            if (t >= stops[s].pos && t <= stops[s + 1].pos)
            {
                a = &stops[s]; b = &stops[s + 1];
                break;
            }
        }
        float span = b->pos - a->pos;
        float f = span > 0.0001f ? (t - a->pos) / span : 0.0f;
        lut[i * 4 + 0] = (uint8_t) (a->r + f * (b->r - a->r));
        lut[i * 4 + 1] = (uint8_t) (a->g + f * (b->g - a->g));
        lut[i * 4 + 2] = (uint8_t) (a->b + f * (b->b - a->b));
        lut[i * 4 + 3] = 255;
    }
    return lut;
}
