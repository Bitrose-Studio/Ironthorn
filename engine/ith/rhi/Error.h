#pragma once

// rhi::Error: why an RHI call failed (ADR-0009 rule 19).
//
//   ith::Expected<ith::rhi::BufferHandle, ith::rhi::Error> buffer = device.createBuffer(desc);
//   if (!buffer) {
//       log(buffer.error().toString()); // "Out of GPU memory creating 'Mesh vertices' [rhi.out_of_memory]"
//   }
//
// The code is an enum rather than a string, because callers switch on it: device loss ends the
// program, out-of-memory makes the streamer drop data, and the rest are bugs to report.
// codeName() gives the stable dotted string for logs and tools. Once the device is lost, every
// later call that can fail reports ErrorCode::DeviceLost.

#include <cstdint>
#include <string>
#include <string_view>

namespace ith::rhi {

enum class ErrorCode : std::uint8_t {
    // The GPU was removed, reset or hung. The message carries the backend's diagnostics. The
    // device can't be used again; the engine logs the error and shuts down (ADR-0009 rule 19).
    DeviceLost,
    // GPU or driver memory ran out.
    OutOfMemory,
    // The requested backend is not built into this program, or can't start.
    BackendUnavailable,
    // No GPU, or the chosen one, meets the minimum feature set of ADR-0005. The message names
    // every missing feature.
    UnsupportedDevice,
    // A programmer error: a description broke a documented rule, or a call came at the wrong
    // time (beginFrame() while a frame is open, a command list opened outside a frame). It is
    // also asserted; the error is what the call returns when asserts continue or are compiled
    // out.
    InvalidUsage,
    // A shader was rejected: wrong bytecode format, or constants over 128 bytes
    // (ADR-0009 rule 16).
    InvalidShader,
    // Every handle slot of the object type is in use (Limits in Capabilities.h).
    OutOfHandles,
    // Every bindless descriptor of the heap is in use (Limits in Capabilities.h).
    OutOfDescriptors,
    // A swapchain could not be created for the window.
    SurfaceFailed,
    // Any other failure the graphics API reported; the message says which call.
    BackendFailure,
};

// The stable dotted identifier of an error code, e.g. "rhi.device_lost". Never localized.
[[nodiscard]] std::string_view codeName(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::BackendFailure;
    // What went wrong, for people; names the object by its debug name where there is one.
    std::string message;

    // One line for logs: "<message> [<code name>]".
    [[nodiscard]] std::string toString() const;

    friend bool operator==(const Error&, const Error&) = default;
};

} // namespace ith::rhi
