#pragma once

// Helpers of the editor_ui_gpu golden-image tests (ADR-0029 rule 18): binary PPM images, the
// software device of a backend, shader compilation, and the comparison with a golden image in
// tests/editor_ui_gpu/golden (stored with Git LFS).
//
// A pixel differs when a channel differs by more than kChannelTolerance of 255; an image fails when
// more than kDifferingPixelsAllowed of its pixels differ. ITH_UPDATE_GOLDEN=1 writes the images
// instead of comparing them. On a failure the rendered image is written next to the test executable
// as <name>.actual.ppm.

#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Expected.h"
#include "ith/rhi/Device.h"
#include "ith/rhi_select/CreateDevice.h"
#include "ith/shader_compiler/ShaderCompiler.h"

#include <doctest/doctest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <ostream> // doctest prints std::string operands through an ostream
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith::test::golden {

inline constexpr int kChannelTolerance = 8;
inline constexpr double kDifferingPixelsAllowed = 0.005;

// Whether the environment variable `name` is set to `value`.
inline bool environmentIs(const char* name, std::string_view value) {
#ifdef _WIN32
    // getenv_s: the C runtime deprecates getenv on Windows.
    std::array<char, 32> buffer{};
    std::size_t length = 0;
    if (getenv_s(&length, buffer.data(), buffer.size(), name) != 0 || length == 0) {
        return false;
    }
    return value == buffer.data();
#else
    const char* set = std::getenv(name); // NOLINT(concurrency-mt-unsafe): read before any thread starts
    return set != nullptr && value == set;
#endif
}

// An RGB image in binary PPM (P6), 8 bits per channel.
struct Image {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgb;
};

inline std::string toPpm(const Image& image) {
    std::string text = "P6\n" + std::to_string(image.width) + " " + std::to_string(image.height) + "\n255\n";
    text.append(reinterpret_cast<const char*>(image.rgb.data()), image.rgb.size());
    return text;
}

// The image in `bytes`, or nothing when they are not a binary PPM of 8-bit channels (an LFS
// pointer file of a checkout without LFS, for one).
inline std::optional<Image> fromPpm(std::string_view bytes) {
    std::size_t at = 0;
    const auto token = [&]() -> std::string_view {
        while (at < bytes.size() && (bytes[at] == ' ' || bytes[at] == '\n' || bytes[at] == '\r' || bytes[at] == '\t')) {
            ++at;
        }
        const std::size_t start = at;
        while (at < bytes.size() && bytes[at] != ' ' && bytes[at] != '\n' && bytes[at] != '\r' && bytes[at] != '\t') {
            ++at;
        }
        return bytes.substr(start, at - start);
    };
    if (token() != "P6") {
        return std::nullopt;
    }
    Image image;
    image.width = static_cast<std::uint32_t>(std::strtoul(std::string(token()).c_str(), nullptr, 10));
    image.height = static_cast<std::uint32_t>(std::strtoul(std::string(token()).c_str(), nullptr, 10));
    if (token() != "255" || at >= bytes.size()) {
        return std::nullopt;
    }
    ++at; // the one whitespace character after the maximum
    const std::size_t size = std::size_t{image.width} * image.height * 3;
    if (bytes.size() - at != size) {
        return std::nullopt;
    }
    image.rgb.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.end());
    return image;
}

// The image of `width` x `height` RGBA8 texels read back into `texels` (tightly packed rows), alpha
// dropped.
inline Image imageFromTexels(std::span<const std::byte> texels, std::uint32_t width, std::uint32_t height) {
    REQUIRE(texels.size() >= std::size_t{width} * height * 4);
    Image image{.width = width, .height = height, .rgb = {}};
    image.rgb.resize(std::size_t{width} * height * 3);
    for (std::size_t i = 0; i < std::size_t{width} * height; ++i) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            image.rgb[i * 3 + channel] = std::to_integer<std::uint8_t>(texels[i * 4 + channel]);
        }
    }
    return image;
}

// The backend's software device (WARP, lavapipe), or nothing, reported as skipped, when there is none.
inline std::optional<rhi::Device> softwareDevice(rhi::Backend backend) {
    rhi::DeviceDesc desc{.adapter = rhi::AdapterPreference::Software, .applicationName = "editor_ui_gpu"};
    if (environmentIs("ITH_RHI_TEST_VALIDATION", "off")) {
        desc.validation = false;
    }
    Expected<rhi::Device, rhi::Error> device = rhi::createDevice(desc, backend);
    if (!device) {
        MESSAGE("skipped: no software device: " << device.error().toString());
        return std::nullopt;
    }
    return std::move(*device);
}

// Whether `device` is lavapipe; reports the test skipped when it isn't.
inline bool isLavapipe(const rhi::Device& device) {
    if (device.capabilities().adapter.name.find("llvmpipe") == std::string::npos) {
        MESSAGE("skipped: the software Vulkan device is not lavapipe: " << device.capabilities().adapter.name);
        return false;
    }
    return true;
}

// One stage of `source` compiled for the device's bytecode format.
inline std::vector<std::byte> compileStage(ShaderCompiler& compiler, rhi::ShaderFormat format, std::string_view name,
                                           std::string_view source, std::string_view entry, ShaderStage stage) {
    Expected<std::vector<std::byte>> code =
        compiler.compile({.name = name,
                          .source = source,
                          .entryPoint = entry,
                          .stage = stage,
                          .target = format == rhi::ShaderFormat::Dxil ? ShaderTarget::Dxil : ShaderTarget::Spirv});
    REQUIRE_MESSAGE(code.hasValue(), (code ? std::string() : code.error().message));
    return std::move(*code);
}

// Compares `actual` with the golden image `name`, or writes it with ITH_UPDATE_GOLDEN=1.
inline void checkGolden(const Image& actual, std::string_view name) {
    const std::string golden = fs::joinPath(ITH_GOLDEN_DIR, std::string(name) + ".ppm");
    if (environmentIs("ITH_UPDATE_GOLDEN", "1")) {
        REQUIRE(fs::writeFileAtomic(golden, toPpm(actual)).hasValue());
        MESSAGE("wrote " << golden);
        return;
    }
    const Expected<std::string> bytes = fs::readFile(golden, std::size_t{64} << 20);
    REQUIRE_MESSAGE(bytes.hasValue(), ("no golden image " + golden + "; run with ITH_UPDATE_GOLDEN=1"));
    const std::optional<Image> expected = fromPpm(*bytes);
    REQUIRE_MESSAGE(expected.has_value(), (golden + " is not a binary PPM (is Git LFS installed?)"));
    REQUIRE(expected->width == actual.width);
    REQUIRE(expected->height == actual.height);
    std::size_t differing = 0;
    for (std::size_t pixel = 0; pixel < std::size_t{actual.width} * actual.height; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const int difference =
                std::abs(int{actual.rgb[pixel * 3 + channel]} - int{expected->rgb[pixel * 3 + channel]});
            if (difference > kChannelTolerance) {
                ++differing;
                break;
            }
        }
    }
    const double share = static_cast<double>(differing) / (static_cast<double>(actual.width) * actual.height);
    MESSAGE(name << ": " << differing << " pixels differ (" << share * 100.0 << "%)");
    if (share > kDifferingPixelsAllowed) {
        const Expected<std::string> directory = fs::executableDirectory();
        if (directory) {
            static_cast<void>(
                fs::writeFileAtomic(fs::joinPath(*directory, std::string(name) + ".actual.ppm"), toPpm(actual)));
        }
    }
    CHECK(share <= kDifferingPixelsAllowed);
}

} // namespace ith::test::golden
