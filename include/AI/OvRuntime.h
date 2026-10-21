// SpectroCraft — OpenVINO runtime wrapper (Phase 0 of the AI plan).
//
// Owns a single ov::Core for the whole process. The OpenVINO DLLs are vendored
// in extern/OpenVINO and copied next to the executable by CMake; linking is
// done against the generated import library extern/OpenVINO/lib/openvino.lib,
// but with /DELAYLOAD:openvino.dll so the executable starts even when the
// runtime DLLs are missing. init() probes openvino.dll via LoadLibrary and
// reports a clear error in that case — only the AI features degrade.
//
// When built with -DSPECTRO_ENABLE_AI=OFF this class degrades to a stub that
// reports "AI disabled" — the rest of the app keeps working.
//
// Known limitation: the OpenVINO 2024.6 binaries fail to initialise under the
// MSVC debug CRT (std::bad_alloc inside plugin discovery). AI features
// therefore require a Release build on Windows; Debug builds report "AI
// unavailable" through the normal error path.

#pragma once

#include <memory>
#include <string>
#include <vector>

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    #include <openvino/openvino.hpp>
#endif

// Resolve a user device preference against the devices actually available in
// the OpenVINO runtime (see OvRuntime::devices()). Used by both noise
// suppression and Whisper transcription so they share one auto-detect rule:
//   pref:      "auto" (case-insensitive) or one of "CPU", "GPU", "NPU"
//              (empty/unknown is treated as "auto").
//   available: e.g. {"CPU"}, {"CPU","GPU"}, {"CPU","GPU","NPU"} - empty when
//              the runtime failed to initialise.
// Returns the device name to compile on. "auto" prefers GPU over NPU over CPU.
// A specifically requested device that is not available falls back to "CPU",
// and (when noteOut != nullptr) a short human-readable explanation of the
// fallback is written there. Pure string logic - no OpenVINO calls, so it can
// be unit-tested without the runtime DLLs.
namespace sc {

std::string resolveAiDevice(const std::string& pref,
                            const std::vector<std::string>& available,
                            std::string* noteOut = nullptr);

namespace ai {

class OvRuntime {
public:
    static OvRuntime& instance();

    // Create the ov::Core and point its compiled-model cache at
    // %APPDATA%/SpectroCraft/openvino-cache. Safe to call repeatedly.
    bool init(std::string* errOut = nullptr);

    bool ready() const { return _ready; }

    // True when this build was configured with -DSPECTRO_ENABLE_AI=ON.
#ifdef SC_ENABLE_AI
    static constexpr bool aiEnabled() { return (SC_ENABLE_AI != 0); }
#else
    static constexpr bool aiEnabled() { return false; }
#endif

#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    std::shared_ptr<ov::Core> core() const { return _core; }

    // Devices the runtime can see (always contains "CPU" when ready).
    std::vector<std::string> devices() const;

    // Read + compile an OpenVINO IR model (.xml path) for `device`.
    // Returns a default-constructed CompiledModel (empty) and fills errOut
    // on failure.
    ov::CompiledModel compile(const std::string& xmlPath,
                              const std::string& device = "CPU",
                              std::string* errOut = nullptr);
#endif

private:
    OvRuntime() = default;
    bool _ready = false;
#if defined(SC_ENABLE_AI) && SC_ENABLE_AI
    std::shared_ptr<ov::Core> _core;
#endif
};

}} // namespace sc::ai
