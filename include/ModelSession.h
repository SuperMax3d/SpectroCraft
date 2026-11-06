#pragma once
// SpectroCraft - Model session: parametric sound-source models built from
// measurements of one or more files.
//
// A session is a saved group of named components (New / Open / Save / Save As,
// versioned JSON). Each component has a type (Harmonic or Noise), time-varying
// inputs (X, Y, SPEED, AMPLITUDE 1..N) that are expressions over the Tables
// panel's parameters, and a table of cells laid out on X/Y breakpoints. The
// list order of components is their computation order: only components ABOVE
// a cell may be subtracted from it.
//
// Time references (noise-cell ranges, harmonic pick times) point into source
// files and follow edits of those files through FileDoc::onTimelineEdit,
// inside the same undo step as the audio edit (see ModelSession::applyTimelineEdit
// and the serialise/applyTimeRefsFor pair used by UndoPatch).

#include <JuceHeader.h>
#include "Tables.h"          // sc::tables::TableData / ParamData (parameter resolution)
#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace sc::model
{
    // ================================================================== curves

    /** One point of a remap curve: x = input value, y = output value. */
    struct CurvePoint { double x = 0.0, y = 0.0; };

    /** Remap curve applied to an input value before it reaches the component.
        The editor shows axis ranges [axisXLo,axisXHi] x [axisYLo,axisYHi];
        apply() clamps the input to the first/last point's x and linearly
        interpolates between points - values outside the curve are held at the
        nearest endpoint (no extrapolation). An empty or single-point curve is
        the identity. Points must be kept sorted by x (the editor enforces it). */
    class RemapCurve
    {
    public:
        std::vector<CurvePoint> points;   // sorted by x
        double axisXLo = 0.0, axisXHi = 1.0, axisYLo = 0.0, axisYHi = 1.0;

        bool empty() const { return points.size() < 2; }

        /** Apply the curve to v (identity when fewer than two points). */
        double apply (double v) const;
    };

    // ============================================================ expressions

    /** Expression over numbers and table parameters: + - * / parentheses,
        unary minus. Parameter names are identifiers ([A-Za-z_][A-Za-z0-9_]*)
        optionally qualified as "TableName.ParamName". Parsing is syntax-only;
        parameter resolution happens at evaluation time through a resolver
        callback so one expression can be evaluated against any file's tables. */
    class Expression
    {
    public:
        /** resolve(name, &found) -> value of the named parameter at the current
            time. *found is set false when the name matches no table/parameter. */
        using ParamResolver = std::function<double (const juce::String& name, bool* found)>;

        struct Result { bool ok = true; double value = 0.0; juce::String error; };

        /** Parses text into out. Returns false (and fills *errOut) on a syntax
            error. An empty/whitespace-only text parses to an empty expression. */
        static bool parse (const juce::String& text, Expression& out,
                           juce::String* errOut = nullptr);

        /** True when the expression is empty (the input then uses its default). */
        bool empty() const { return root_ == nullptr; }

        Result evaluate (const ParamResolver& resolve) const;

        /** All parameter names referenced, in order of first appearance. */
        std::vector<juce::String> params() const;

    private:
        struct Node
        {
            enum class Kind { Number, Param, Neg, Add, Sub, Mul, Div };
            Kind kind = Kind::Number;
            double num = 0.0;
            juce::String param;
            std::shared_ptr<Node> a, b;
        };
        std::shared_ptr<Node> root_;

        static void collectParams (const Node* n, std::vector<juce::String>& out);
        friend struct ExpressionParser;   // recursive-descent parser in ModelSession.cpp
    };

    // ============================================================ grid mapping

    /** Continuous axis index in [0, n-1] for value v on an axis with sorted
        breakpoints (n = breaks.size() + 1 cells). Interpolation nodes sit at
        the midpoints between surrounding breakpoints; edge nodes are
        extrapolated by half the adjacent interior cell width (unit width when
        there is exactly one breakpoint); v is clamped to the outermost nodes,
        so values outside the breakpoints never extrapolate. */
    double axisIndex (const std::vector<double>& breaks, double v);

    /** Bilinear weights of the four cells surrounding (x, y).
        w[] order: [ (row0,col0), (row0,col0+1), (row0+1,col0), (row0+1,col0+1) ].
        Cells for which isEmpty(row,col) is true are ignored and their weight
        renormalised over the remaining cells; when all four are empty every
        weight is zero. */
    struct BilinearResult
    {
        int col = 0, row = 0;   // top-left cell of the surrounding rectangle
        double w[4] = { 1.0, 0.0, 0.0, 0.0 };
    };
    BilinearResult bilinearWeights (const std::vector<double>& xBreaks,
                                    const std::vector<double>& yBreaks,
                                    int numCols, int numRows,
                                    double x, double y,
                                    const std::function<bool(int row, int col)>& isEmpty);

    // ============================================================ parameters

    /** Resolves a (possibly qualified) parameter name against a document's
        tables at time tSecs (document seconds). Unqualified names use the
        FIRST match in table order; when several tables contain the same
        unqualified name, *ambiguous is set true (the caller may warn). The
        value is interpolated in the parameter's own table time under its
        effective time map (docTime = offset + scale*tTable); outside the
        parameter's domain the nearest endpoint value is held. */
    double resolveParam (const std::vector<sc::tables::TableData>& tables,
                         const juce::String& name, double tSecs,
                         bool* found = nullptr, bool* ambiguous = nullptr);

    // ============================================================ data model

    /** One measured harmonic peak of a harmonic cell. */
    struct HarmonicPick
    {
        double freqHz = 0.0;       // measured peak frequency
        double widthHz = 50.0;     // editable peak half-width (Hz)
        juce::String sourceFile;   // full path of the analysed file at pick time
        int channel = 0;           // analysed channel
        double timeSecs = 0.0;     // analysis window centre - a TIME REFERENCE
        int fftSize = 4096;        // FFT size used for the local spectrum
        int windowType = 1;        // 0=Rectangular 1=Hann 2=Hamming
        double binHz = 0.0;        // Hz per bin of the cached local spectrum
        int peakBin = -1;          // index of the peak within the local cache
        std::vector<float> localPower;   // linear power around the peak:
                                         // +/- max(3*width, 8) bins each side
        double rawPeakAmp = 0.0;   // cached measured amplitude (linear RMS)
        bool timeRemoved = false;  // analysis window was deleted from its file
    };

    /** Per-candidate subtraction state stored on a cell: the BYPASS flag and
        optional input overrides ("X", "Y", "SPEED", "AMPLITUDE1", ...) that
        replace the candidate's inputs for this cell only. */
    struct SubOverride
    {
        juce::String candidateId;   // stable id of a component above in the list
        bool bypass = false;
        std::map<std::string, std::string> overrides;  // input name -> expression text
    };

    /** One element of a component's table. All cells of a component share its
        type (Harmonic or Noise). */
    struct Cell
    {
        juce::String sourceFile;    // full path of the analysed file (both types)

        // --- Harmonic cells ---
        std::vector<HarmonicPick> picks;
        double refSpeed = 0.0;      // Reference Speed in Hz (normalisation base)
        bool refSpeedFromFirstPick = false;   // ref speed follows the first pick's freq

        // --- Noise cells ---
        double t0 = 0.0, t1 = 0.0;  // source time range - TIME REFERENCES
        int channel = 0;            // analysed channel
        int fftSize = 4096;         // FFT size of the raw analysis
        int windowType = 1;         // 0=Rectangular 1=Hann 2=Hamming
        bool detached = false;      // range deleted: keep cached layers, no live source

        // --- Cached layers (stored in the session file) ---
        std::vector<float> rawPower;          // noise: linear power on own bin grid
        double rawBinHz = 0.0;                // Hz per bin of rawPower (noise cells)
        std::vector<HarmonicPick> residualPeaks;   // harmonic: peaks with residual amplitudes
                                                 // (freqHz/widthHz kept, rawPeakAmp replaced)
        std::vector<float> residualPower;     // noise: residual linear power on own grid

        // --- Subtraction candidates (components above this one in the list) ---
        std::vector<SubOverride> subtractions;

        /** A cell is empty when it carries no usable content for its type. */
        bool isEmptyForType (int componentType) const;
    };

    /** One time-varying input of a component: an expression over table
        parameters, optionally remapped through a curve. An empty expression
        evaluates to the input's default value (X=0 Y=0 SPEED=1 AMPLITUDE=1). */
    struct ModelInput
    {
        juce::String expression;   // e.g. "PARAM1/100" or "" for the default
        RemapCurve curve;          // applied after the expression, before the component
        double defaultValue = 0.0; // set by the owner (X/Y=0, SPEED/AMPLITUDE=1)

        /** Evaluate at document time tSecs using resolve() to look up parameter
            values. On failure (syntax error or missing parameter) ok=false and
            the caller falls back to defaultValue. */
        Expression::Result evaluateAt (double tSecs, const Expression::ParamResolver& resolve) const;
    };

    /** One named component of a session. List order is computation order: only
        components ABOVE a cell can be subtracted from it. */
    struct Component
    {
        juce::String id;             // stable unique id (survives renames/reorders)
        juce::String name;           // display name (editable, may duplicate)
        juce::String comment;        // free-text note
        int type = 0;                // 0=Harmonic 1=Noise

        ModelInput inputX, inputY, inputSpeed;   // defaults X=0 Y=0 SPEED=1
        std::vector<ModelInput> amplitudes;      // one or more, default 1 each

        std::vector<double> xBreaks;             // column breakpoints (sorted)
        std::vector<double> yBreaks;             // row breakpoints (sorted)
        std::vector<Cell> cells;                 // rows x cols, row-major

        int numCols() const { return (int) xBreaks.size() + 1; }
        int numRows() const { return (int) yBreaks.size() + 1; }
    };

    // ============================================================ session

    /** A saved group of components: New / Open / Save / Save As on a versioned
        JSON file. Source-file paths are stored relative to the session file's
        directory when possible (absolute fallback on load). */
    class ModelSession
    {
    public:
        static constexpr int kFormatVersion = 1;

        juce::String filePath;       // the session's own .json path ("" until saved)
        bool dirty = false;          // unsaved changes

        // Session-wide settings (editable in the panel).
        double matchTolerancePct = 3.0;      // harmonic matching tolerance (% of mean f_norm)
        double inclusionThresholdDb = -60.0; // candidate included when it exceeds this dB below the measured peak
        double overSubtractToleranceDb = 1.0;// red flag when a candidate subtracts more than raw + this many dB

        std::vector<Component> components;   // list order = computation order

        // ---- component management ----
        Component& addComponent (const juce::String& name, int type);
        void removeComponent (size_t index);
        void moveComponent (size_t from, size_t to);
        const Component* findComponent (const juce::String& id) const;

        // ---- time references (follow edits of the referenced files) ----
        enum class TimelineEdit { Deletion, Insertion, Stretch };

        /** Warp every time reference that points into `file`: noise-cell ranges
            and harmonic pick times. Called by FileDoc::onTimelineEdit inside
            the same step as the audio edit (and restored through the patch's
            serialised refs on undo/redo). Parameters per kind:
              Deletion:  [a,b) removed - values in [a,b) collapse to a; >= b shift left.
                         A pick fully inside is kept with its values but flagged
                         timeRemoved; a range fully inside becomes detached (cached
                         layers are kept).
              Insertion: length b inserted at position a - values >= a shift right by b.
              Stretch:   [a,b] -> [a,c] - values in [a,b] scale linearly, values
                         beyond b follow the new end. */
        void applyTimelineEdit (const juce::File& file, TimelineEdit kind,
                                double a, double b, double c = 0.0);

        /** Serialised time-reference state for one source file: "" when this
            session holds no references to it. Used by UndoPatch so undo/redo
            restores the refs together with the audio. */
        juce::String serialiseTimeRefsFor (const juce::File& file) const;
        void applyTimeRefsFor (const juce::File& file, const juce::String& json);

        // ---- JSON persistence ----
        static bool saveToFile (const ModelSession& s, const juce::File& target);
        static bool loadFromFile (ModelSession& s, const juce::File& source,
                                  juce::String* errOut = nullptr);

    private:
        friend struct ModelJsonTestAccess;   // unit tests poke at the var tree
        static juce::var toVar (const ModelSession& s, const juce::File& sessionDir);
        static bool fromVar (ModelSession& s, const juce::var& root,
                             const juce::File& sessionDir, juce::String* errOut);
    };

    // ============================================================ analysis DSP

    /** Frame reader over in-memory audio (one vector per channel). The live
        AudioEngine adapter and the test buffers both funnel into these. */
    class FrameReader
    {
    public:
        virtual ~FrameReader() = default;
        virtual double sampleRate() const = 0;
        virtual int numChannels() const = 0;
        virtual int64_t numSamples() const = 0;   // per channel
        /** Reads count samples of channel ch starting at start (zero-filled
            beyond the end). Returns false when ch is out of range. */
        virtual bool readChannel (int ch, int64_t start, int count, float* out) const = 0;
    };

    /** Window function for the given type: 0=Rectangular 1=Hann 2=Hamming. */
    static void makeWindow (int n, int windowType, std::vector<float>& win);

    /** Averaged linear power spectrum of channel ch over [t0,t1] seconds with
        the given FFT settings: frames hop by fftSize/4 (single centred frame
        when the range is shorter than one window), Hann-coherent-gain normalised
        so a full-scale sine reads 1/sqrt(2) in its peak bin. outPower has
        fftSize/2 + 1 entries; outBinHz = rate / fftSize. */
    bool analyseNoiseRange (const FrameReader& src, int ch, double t0, double t1,
                            int fftSize, int windowType,
                            std::vector<float>& outPower, double& outBinHz);

    /** Local linear power spectrum around a peak: the frame centred on
        timeSecs is FFT'd and the bins within +/- max(3*widthHz, 8 bins) of
        freqHz are cached (outPeakBin indexes into it). Same normalisation as
        analyseNoiseRange. */
    bool analyseLocalPeak (const FrameReader& src, int ch, double timeSecs,
                           double freqHz, double widthHz, int fftSize, int windowType,
                           std::vector<float>& outPower, double& outBinHz, int& outPeakBin);

}   // namespace sc::model
