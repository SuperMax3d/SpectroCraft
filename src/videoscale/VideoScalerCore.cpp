/**
 * SpectroCraft - Video picture scaler core (see include/VideoScalerCore.h)
 *
 * Plain C++ (no JUCE): src/videoscale/CMakeLists.txt compiles it optimised in
 * every configuration, because it runs for every video frame shown.
 */
#include "VideoScalerCore.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

#if defined(__aarch64__) || defined(_M_ARM64)
  #include <arm_neon.h>
  #define SC_VS_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #include <emmintrin.h>
  #define SC_VS_SSE2 1
#endif

namespace sc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kBicubicA = -0.6;   // swscale SWS_BICUBIC default (B = 0, C = 0.6)
constexpr double kLanczosA = 3.0;    // swscale SWS_LANCZOS default parameter

double sinc (double x)
{
    if (std::abs (x) < 1e-12) return 1.0;
    const double px = kPi * x;
    return std::sin (px) / px;
}

double radiusOf (VideoUpscaling m)
{
    return m == VideoUpscaling::lanczos ? kLanczosA : 2.0;
}

} // namespace

double videoScalerKernel (VideoUpscaling m, double x)
{
    x = std::abs (x);
    switch (m)
    {
        case VideoUpscaling::lanczos:
            return x < kLanczosA ? sinc (x) * sinc (x / kLanczosA) : 0.0;
        case VideoUpscaling::bicubic:
        {
            // Keys cubic convolution; with A = -C it equals the
            // Mitchell-Netravali form swscale uses with B = 0.
            const double a = kBicubicA;
            if (x < 1.0) return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
            if (x < 2.0) return ((a * x - 5.0 * a) * x + 8.0 * a) * x - 4.0 * a;
            return 0.0;
        }
        case VideoUpscaling::nearest:
            return x < 0.5 ? 1.0 : 0.0;
    }
    return 0.0;
}

namespace vscale {

namespace {

//==============================================================================
// One pixel as four floats: its four bytes in memory order (B, G, R, A for
// JUCE's ARGB on every platform this app ships for). The filters treat
// the four lanes alike; only vsToBytes() knows lane 3 is alpha.
//
// The per-tap operations are macros straight over the intrinsics, so the
// loops cost the same however the compiler inlines (this library is
// compiled optimised in every configuration: src/videoscale/CMakeLists.txt).
#if SC_VS_NEON
  using F4 = float32x4_t;
  #define VS_ZERO()            vdupq_n_f32 (0.0f)
  #define VS_LOAD(p)           vld1q_f32 (p)
  #define VS_STORE(p, v)       vst1q_f32 ((p), (v))
  #define VS_MADD(acc, p, w)   vmlaq_n_f32 ((acc), vld1q_f32 (p), (w))   // acc + p[0..3] * w

  inline F4 vsFromBytes (const uint8_t* p) noexcept
  {
      uint32_t px;
      std::memcpy (&px, p, 4);
      const uint16x8_t h = vmovl_u8 (vreinterpret_u8_u32 (vdup_n_u32 (px)));
      return vcvtq_f32_u32 (vmovl_u16 (vget_low_u16 (h)));
  }
  // Rounded, clamped to 0..255, colours never above alpha (JUCE's ARGB is
  // premultiplied; a ringing filter could otherwise overshoot it).
  inline uint32_t vsToBytes (F4 v) noexcept
  {
      const float32x4_t c = vminq_f32 (v, vdupq_laneq_f32 (v, 3));
      const uint16x4_t h = vqmovun_s32 (vcvtnq_s32_f32 (c));
      const uint8x8_t b = vqmovn_u16 (vcombine_u16 (h, h));
      return vget_lane_u32 (vreinterpret_u32_u8 (b), 0);
  }
#elif SC_VS_SSE2
  using F4 = __m128;
  #define VS_ZERO()            _mm_setzero_ps()
  #define VS_LOAD(p)           _mm_loadu_ps (p)
  #define VS_STORE(p, v)       _mm_storeu_ps ((p), (v))
  #define VS_MADD(acc, p, w)   _mm_add_ps ((acc), _mm_mul_ps (_mm_loadu_ps (p), _mm_set1_ps (w)))

  inline F4 vsFromBytes (const uint8_t* p) noexcept
  {
      int px;
      std::memcpy (&px, p, 4);
      const __m128i z = _mm_setzero_si128();
      return _mm_cvtepi32_ps (_mm_unpacklo_epi16 (_mm_unpacklo_epi8 (_mm_cvtsi32_si128 (px), z), z));
  }
  inline uint32_t vsToBytes (F4 v) noexcept
  {
      const __m128 c = _mm_min_ps (v, _mm_shuffle_ps (v, v, _MM_SHUFFLE (3, 3, 3, 3)));
      const __m128i i = _mm_cvtps_epi32 (c);              // round to nearest
      const __m128i h = _mm_packs_epi32 (i, i);           // saturate to int16
      return (uint32_t) _mm_cvtsi128_si32 (_mm_packus_epi16 (h, h));   // ... and to 0..255
  }
#else
  struct F4 { float v[4]; };
  inline F4 vsZero() noexcept { return { { 0.0f, 0.0f, 0.0f, 0.0f } }; }
  inline F4 vsLoad (const float* p) noexcept { F4 r; std::memcpy (r.v, p, sizeof (r.v)); return r; }
  inline void vsStore (float* p, F4 v) noexcept { std::memcpy (p, v.v, sizeof (v.v)); }
  inline F4 vsMadd (F4 acc, const float* p, float w) noexcept
  {
      for (int i = 0; i < 4; ++i) acc.v[i] += p[i] * w;
      return acc;
  }
  #define VS_ZERO()            vsZero()
  #define VS_LOAD(p)           vsLoad (p)
  #define VS_STORE(p, v)       vsStore ((p), (v))
  #define VS_MADD(acc, p, w)   vsMadd ((acc), (p), (w))

  inline F4 vsFromBytes (const uint8_t* p) noexcept
  {
      F4 r;
      for (int i = 0; i < 4; ++i) r.v[i] = (float) p[i];
      return r;
  }
  inline uint32_t vsToBytes (F4 v) noexcept
  {
      uint8_t b[4];
      for (int i = 0; i < 4; ++i)
      {
          const float x = i < 3 ? std::min (v.v[i], v.v[3]) : v.v[i];
          b[i] = (uint8_t) std::clamp ((long) std::lrint (x), 0L, 255L);
      }
      uint32_t r;
      std::memcpy (&r, b, 4);
      return r;
  }
#endif

/** The source as displayed (after the rotation): pixel (u, v) of the turned
    picture sits at origin + u * su + v * sv bytes. */
struct SrcView
{
    const uint8_t* origin = nullptr;
    ptrdiff_t su = 4, sv = 0;
    int wd = 0, hd = 0;   // displayed width / height in source pixels
};

} // namespace

//==============================================================================
/** Filter weights of one axis: for output pixel i, `taps` weights applied to
    the source pixels first[i] .. first[i] + taps - 1 (indexes past the
    picture edge repeat the edge pixel). */
struct Resampler::Axis
{
    int n = 0, srcN = 0, taps = 0;
    double start = 0.0, length = 0.0;
    VideoUpscaling method = VideoUpscaling::lanczos;
    std::vector<int> first;
    std::vector<float> weights;   // n * taps; each pixel's weights sum to 1
    int lo = 0, hi = -1;          // source indexes used by any tap

    bool same (int nOut, double s0, double len, int srcCount, VideoUpscaling m) const noexcept
    {
        return taps > 0 && n == nOut && start == s0 && length == len && srcN == srcCount && method == m;
    }

    void build (int nOut, double s0, double len, int srcCount, VideoUpscaling m)
    {
        n = nOut; start = s0; length = len; srcN = srcCount; method = m;
        first.assign ((size_t) n, 0);
        const double scale = len / (double) n;   // source pixels per output pixel

        if (m == VideoUpscaling::nearest)
        {
            taps = 1;
            weights.assign ((size_t) n, 1.0f);
            for (int i = 0; i < n; ++i)
                first[(size_t) i] = (int) std::floor (s0 + ((double) i + 0.5) * scale);
        }
        else
        {
            // Shrinking stretches the kernel by the shrink factor so it
            // averages the pixels it skips (no aliasing); enlarging keeps it.
            const double stretch = std::max (1.0, scale);
            const double support = radiusOf (m) * stretch;
            taps = std::max (1, (int) std::ceil (2.0 * support));
            weights.assign ((size_t) n * (size_t) taps, 0.0f);
            std::vector<double> w ((size_t) taps);
            for (int i = 0; i < n; ++i)
            {
                // Output pixel centre in source pixels; source pixel j has its
                // centre at j + 0.5. The first tap is the first pixel strictly
                // inside the support.
                const double c = s0 + ((double) i + 0.5) * scale;
                const int j0 = (int) std::floor (c - 0.5 - support) + 1;
                first[(size_t) i] = j0;
                double sum = 0.0;
                for (int k = 0; k < taps; ++k)
                {
                    w[(size_t) k] = videoScalerKernel (m, ((double) (j0 + k) + 0.5 - c) / stretch);
                    sum += w[(size_t) k];
                }
                float* out = weights.data() + (size_t) i * (size_t) taps;
                for (int k = 0; k < taps; ++k)
                    out[k] = std::abs (sum) > 1e-12 ? (float) (w[(size_t) k] / sum) : (k == 0 ? 1.0f : 0.0f);
            }
        }
        lo = first.front();
        hi = first.back() + taps - 1;   // first[] never decreases
    }
};

//==============================================================================
/** A few worker threads that split one job (a list of independent tasks)
    with the calling thread, which waits for all of them to finish. */
class Resampler::Pool
{
public:
    explicit Pool (int n)
    {
        // A thread that cannot be created just means fewer workers (the
        // caller always takes part); never terminate over it.
        try
        {
            for (int i = 0; i < n; ++i)
                threads_.emplace_back ([this, id = i + 1] { loop (id); });
        }
        catch (...) {}
    }

    ~Pool()
    {
        {
            std::lock_guard<std::mutex> lk (m_);
            quit_ = true;
        }
        wake_.notify_all();
        for (auto& t : threads_) t.join();
    }

    int size() const noexcept { return (int) threads_.size(); }

    /** Runs fn (task, threadIndex) for every task in [0, nTasks): the workers
        (indexes 1..size()) and the caller (index 0) take tasks until none is
        left; returns once every task has finished. */
    void run (int nTasks, std::function<void (int, int)> fn)
    {
        // Each run has its own counters: a worker that wakes up late can
        // only find this run exhausted, never take a task of the next one.
        auto r = std::make_shared<Run>();
        r->fn = std::move (fn);
        r->total = nTasks;
        r->pending.store (nTasks);
        {
            std::lock_guard<std::mutex> lk (m_);
            current_ = r;
            ++generation_;
        }
        wake_.notify_all();
        work (*r, 0);
        std::unique_lock<std::mutex> lk (m_);
        done_.wait (lk, [&r] { return r->pending.load() == 0; });
        current_.reset();
    }

private:
    struct Run
    {
        std::function<void (int, int)> fn;
        std::atomic<int> next { 0 };
        int total = 0;
        std::atomic<int> pending { 0 };
    };

    void loop (int id)
    {
        uint64_t seen = 0;
        for (;;)
        {
            std::shared_ptr<Run> r;
            {
                std::unique_lock<std::mutex> lk (m_);
                wake_.wait (lk, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
                r = current_;
            }
            if (r != nullptr) work (*r, id);
        }
    }

    void work (Run& r, int id)
    {
        for (;;)
        {
            const int t = r.next.fetch_add (1);
            if (t >= r.total) return;
            r.fn (t, id);
            if (r.pending.fetch_sub (1) == 1)
            {
                std::lock_guard<std::mutex> lk (m_);
                done_.notify_all();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::mutex m_;
    std::condition_variable wake_, done_;
    std::shared_ptr<Run> current_;
    uint64_t generation_ = 0;
    bool quit_ = false;
};

//==============================================================================
namespace {

using Axis = Resampler::Axis;

// Displayed row v (clamped) from column lo to hi (each clamped) as floats.
void gatherRow (const SrcView& s, int v, int lo, int hi, float* out)
{
    const uint8_t* row = s.origin + (ptrdiff_t) std::clamp (v, 0, s.hd - 1) * s.sv;
    const int maxU = s.wd - 1;
    for (int u = lo; u <= hi; ++u, out += 4)
        VS_STORE (out, vsFromBytes (row + (ptrdiff_t) std::clamp (u, 0, maxU) * s.su));
}

template <int T>
void hpassFixed (const float* in, const Axis& ax, float* out)
{
    const float* w = ax.weights.data();
    const int* first = ax.first.data();
    for (int i = 0; i < ax.n; ++i, w += T, out += 4)
    {
        const float* p = in + (size_t) (first[i] - ax.lo) * 4;
        F4 acc = VS_ZERO();
        for (int k = 0; k < T; ++k)
            acc = VS_MADD (acc, p + 4 * k, w[k]);
        VS_STORE (out, acc);
    }
}

void hpassAny (const float* in, const Axis& ax, float* out)
{
    const int taps = ax.taps;
    const float* w = ax.weights.data();
    const int* first = ax.first.data();
    for (int i = 0; i < ax.n; ++i, w += taps, out += 4)
    {
        const float* p = in + (size_t) (first[i] - ax.lo) * 4;
        F4 acc = VS_ZERO();
        for (int k = 0; k < taps; ++k, p += 4)
            acc = VS_MADD (acc, p, w[k]);
        VS_STORE (out, acc);
    }
}

// Horizontal pass of one gathered row into ax.n filtered pixels.
void hpass (const float* in, const Axis& ax, float* out)
{
    switch (ax.taps)
    {
        case 4:  hpassFixed<4> (in, ax, out); break;   // Bicubic, enlarging
        case 6:  hpassFixed<6> (in, ax, out); break;   // Lanczos, enlarging
        default: hpassAny (in, ax, out); break;
    }
}

template <int T>
void vpassFixed (const float* rows, size_t stride, const float* w, int n, uint32_t* out)
{
    const float* r[T];
    for (int k = 0; k < T; ++k) r[k] = rows + (size_t) k * stride;
    for (int x = 0; x < n; ++x)
    {
        F4 acc = VS_ZERO();
        for (int k = 0; k < T; ++k)
            acc = VS_MADD (acc, r[k] + 4 * x, w[k]);
        out[x] = vsToBytes (acc);
    }
}

void vpassAny (const float* rows, size_t stride, int taps, const float* w, int n, uint32_t* out)
{
    for (int x = 0; x < n; ++x)
    {
        F4 acc = VS_ZERO();
        const float* p = rows + 4 * (size_t) x;
        for (int k = 0; k < taps; ++k, p += stride)
            acc = VS_MADD (acc, p, w[k]);
        out[x] = vsToBytes (acc);
    }
}

// Vertical pass: `taps` consecutive filtered rows into one output row.
void vpass (const float* rows, size_t stride, int taps, const float* w, int n, uint32_t* out)
{
    switch (taps)
    {
        case 4:  vpassFixed<4> (rows, stride, w, n, out); break;
        case 6:  vpassFixed<6> (rows, stride, w, n, out); break;
        default: vpassAny (rows, stride, taps, w, n, out); break;
    }
}

// Output rows [y0, y1): Nearest copies pixels; the filters run a horizontal
// pass over every source row the band needs, then the vertical pass.
void renderBand (const SrcView& s, const Axis& ax, const Axis& ay, int y0, int y1,
                 const Pixels& dd, std::vector<float>& scratch)
{
    if (ax.method == VideoUpscaling::nearest)
    {
        const int maxU = s.wd - 1, maxV = s.hd - 1;
        for (int y = y0; y < y1; ++y)
        {
            const uint8_t* row = s.origin + (ptrdiff_t) std::clamp (ay.first[(size_t) y], 0, maxV) * s.sv;
            auto* out = reinterpret_cast<uint32_t*> (dd.data + (ptrdiff_t) y * dd.lineStride);
            for (int x = 0; x < ax.n; ++x)
                std::memcpy (out + x, row + (ptrdiff_t) std::clamp (ax.first[(size_t) x], 0, maxU) * s.su, 4);
        }
        return;
    }

    const int nIn = ax.hi - ax.lo + 1;
    const int rowLo = ay.first[(size_t) y0];
    const int rowHi = ay.first[(size_t) (y1 - 1)] + ay.taps - 1;
    const size_t rowFloats = (size_t) ax.n * 4;
    scratch.resize ((size_t) nIn * 4 + (size_t) (rowHi - rowLo + 1) * rowFloats);
    float* gathered = scratch.data();
    float* filtered = gathered + (size_t) nIn * 4;

    for (int r = rowLo; r <= rowHi; ++r)
    {
        gatherRow (s, r, ax.lo, ax.hi, gathered);
        hpass (gathered, ax, filtered + (size_t) (r - rowLo) * rowFloats);
    }
    for (int y = y0; y < y1; ++y)
        vpass (filtered + (size_t) (ay.first[(size_t) y] - rowLo) * rowFloats, rowFloats, ay.taps,
               ay.weights.data() + (size_t) y * (size_t) ay.taps, ax.n,
               reinterpret_cast<uint32_t*> (dd.data + (ptrdiff_t) y * dd.lineStride));
}

} // namespace

//==============================================================================
Resampler::Resampler() = default;
Resampler::~Resampler() = default;

int Resampler::weightTableBuilds() const noexcept { return tableBuilds_; }

void Resampler::setMaxWorkerThreads (int n)
{
    n = std::max (0, n);
    if (n == maxWorkers_) return;
    maxWorkers_ = n;
    pool_.reset();   // recreated with the new size on the next large render
}

bool Resampler::render (const ConstPixels& src, int rotation,
                        double areaX, double areaY, double areaW, double areaH,
                        const Pixels& dst, VideoUpscaling method)
{
    const int dstW = dst.width, dstH = dst.height;
    if (src.data == nullptr || src.width <= 0 || src.height <= 0
        || dst.data == nullptr || dstW <= 0 || dstH <= 0
        || (int64_t) dstW * (int64_t) dstH > (int64_t) 16384 * 16384
        || !std::isfinite (areaX) || !std::isfinite (areaY)
        || !(areaW > 0.0) || !(areaH > 0.0) || !std::isfinite (areaW) || !std::isfinite (areaH))
        return false;

    const int rot = (((rotation / 90) * 90) % 360 + 360) % 360;
    const int w = src.width, h = src.height;
    const ptrdiff_t rs = src.lineStride;
    SrcView s;
    s.wd = (rot == 90 || rot == 270) ? h : w;
    s.hd = (rot == 90 || rot == 270) ? w : h;
    // Turning clockwise: displayed (u, v) shows source (x, y) =
    //   0: (u, v)   90: (v, h-1-u)   180: (w-1-u, h-1-v)   270: (w-1-v, u).
    switch (rot)
    {
        case 90:  s.origin = src.data + (ptrdiff_t) (h - 1) * rs;                         s.su = -rs; s.sv = 4;   break;
        case 180: s.origin = src.data + (ptrdiff_t) (w - 1) * 4 + (ptrdiff_t) (h - 1) * rs; s.su = -4;  s.sv = -rs; break;
        case 270: s.origin = src.data + (ptrdiff_t) (w - 1) * 4;                          s.su = rs;  s.sv = -4;  break;
        default:  s.origin = src.data;                                                    s.su = 4;   s.sv = rs;  break;
    }

    if (ax_ == nullptr) ax_ = std::make_unique<Axis>();
    if (ay_ == nullptr) ay_ = std::make_unique<Axis>();
    if (!ax_->same (dstW, areaX, areaW, s.wd, method))
    {   ax_->build (dstW, areaX, areaW, s.wd, method); ++tableBuilds_; }
    if (!ay_->same (dstH, areaY, areaH, s.hd, method))
    {   ay_->build (dstH, areaY, areaH, s.hd, method); ++tableBuilds_; }

    // Small pictures stay on this thread; larger ones are cut into bands of
    // rows (several per thread, so uneven bands still balance out).
    if (maxWorkers_ < 0)
    {
        const int cores = (int) std::thread::hardware_concurrency();
        maxWorkers_ = std::clamp (cores - 1, 0, 3);
    }
    int workers = 0;
    if ((int64_t) dstW * dstH >= 96 * 1024 && maxWorkers_ > 0)
    {
        if (pool_ == nullptr) pool_ = std::make_unique<Pool> (maxWorkers_);
        workers = pool_->size();
    }
    const int band = workers > 0 ? std::clamp ((dstH + 4 * (workers + 1) - 1) / (4 * (workers + 1)), 8, 64)
                                 : dstH;
    const int nBands = (dstH + band - 1) / band;
    if ((int) scratch_.size() < workers + 1) scratch_.resize ((size_t) workers + 1);

    const Axis& ax = *ax_;
    const Axis& ay = *ay_;
    auto job = [&] (int b, int thread)
    {
        renderBand (s, ax, ay, b * band, std::min (dstH, (b + 1) * band), dst, scratch_[(size_t) thread]);
    };
    if (workers > 0 && nBands > 1)
        pool_->run (nBands, job);
    else
        for (int b = 0; b < nBands; ++b) job (b, 0);
    return true;
}

} // namespace vscale
} // namespace sc
