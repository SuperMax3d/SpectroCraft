#pragma once
// ============================================================================
// SpectroCraft Recorder Plugin API - version 1
//
// A recorder plugin is a native DLL (Windows) / shared library that captures
// audio from some source (WASAPI, ASIO, network, ...) and exposes it to the
// host through this small C ABI. The host pulls samples: after sc_rec_start()
// it repeatedly calls sc_rec_available_frames()/sc_rec_read() until recording
// stops.
//
// Conventions
//   * Every function returns 0 on success, non-zero on error. Call
//     sc_rec_last_error() for a human-readable message (may be "").
//   * All strings are UTF-8, NUL-terminated. Pointers returned by the plugin
//     stay valid until the next call into the same plugin or until it is
//     unloaded - copy them if you need to keep them.
//   * Sample data handed to the host is always interleaved 32-bit float in
//     [-1, 1], channel order = device channel order.
//   * The plugin must be thread-safe for the pull calls: sc_rec_read() and
//     sc_rec_available_frames() may be called from any thread while a capture
//     thread is producing data.
// ============================================================================

#ifdef _WIN32
    #define SC_REC_EXPORT __declspec(dllexport)
#else
    #define SC_REC_EXPORT __attribute__((visibility("default")))
#endif

#define SC_RECORDER_API_VERSION 1

typedef struct sc_rec_plugin_info
{
    const char* id;        // short unique identifier, e.g. "wasapi"
    const char* name;      // display name, e.g. "WASAPI (Windows)"
    const char* version;   // plugin version string, e.g. "1.0"
} sc_rec_plugin_info;

typedef struct sc_rec_device_info
{
    const char* id;            // unique within the plugin, e.g. "cap:..." / "loopback:..."
    const char* name;          // display name (loopback devices should say so)
    int minChannels;           // >= 1
    int maxChannels;           // >= minChannels
    double defaultRate;        // preferred sample rate, Hz
    int numRates;              // number of entries in `rates` (0 = any rate in [minRate,maxRate])
    const double* rates;       // supported sample rates, Hz
    double minRate;            // continuous range fallback, Hz
    double maxRate;
    int numDepths;             // number of entries in `depths`
    const int* depths;         // supported bit depths: 16 / 24 / 32 (32 = 32-bit float)
} sc_rec_device_info;

/** Filled by the plugin after sc_rec_configure(): what it will actually use. */
typedef struct sc_rec_negotiated
{
    int channels;      // actual channel count
    double rate;       // actual sample rate, Hz
    int bitDepth;      // actual bit depth (16 / 24 / 32)
} sc_rec_negotiated;

#ifdef __cplusplus
extern "C" {
#endif

/** @return SC_RECORDER_API_VERSION. */
SC_REC_EXPORT int sc_rec_get_api_version(void);

/** Fills `out`. */
SC_REC_EXPORT int sc_rec_get_plugin_info(sc_rec_plugin_info* out);

/** Number of capture devices currently available (0 = none). */
SC_REC_EXPORT int sc_rec_count_devices(void);

/** Fills `out` for device index 0..count-1. */
SC_REC_EXPORT int sc_rec_get_device(int index, sc_rec_device_info* out);

/** Selects the device and desired format. The plugin clamps to what the
    hardware supports and reports the real values in *actual (may be NULL).
    Must be called before sc_rec_start(). */
SC_REC_EXPORT int sc_rec_configure(const char* deviceId,
                                   int wantChannels, double wantRate,
                                   int wantBitDepth, sc_rec_negotiated* actual);

/** Starts capturing. Returns an error if no device was configured or the
    device cannot be opened. */
SC_REC_EXPORT int sc_rec_start(void);

/** Stops capturing and joins the capture thread. Frames already delivered to
    the internal buffer remain readable via sc_rec_read() until drained. */
SC_REC_EXPORT int sc_rec_stop(void);

/** 1 while a capture is in progress, else 0. */
SC_REC_EXPORT int sc_rec_is_running(void);

/** Number of frames (one frame = `channels` interleaved samples) currently
    available to read. */
SC_REC_EXPORT long long sc_rec_available_frames(void);

/** Reads up to maxFrames frames into dest (which must hold
    channels * maxFrames floats). @return number of frames actually read. */
SC_REC_EXPORT int sc_rec_read(float* dest, int channels, long long maxFrames);

/** Human-readable description of the last error ("", if none). */
SC_REC_EXPORT const char* sc_rec_last_error(void);

#ifdef __cplusplus
}
#endif
