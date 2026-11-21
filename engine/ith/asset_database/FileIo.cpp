#include "ith/asset_database/FileIo.h"

#include "ith/foundation/StringUtils.h"
#include "ith/foundation/Uuid.h"

#include <cstdint>
#include <fstream>
#include <ios>
#include <system_error>

namespace ith::detail::asset_files {
namespace {

// True when UTF-16 text has no unpaired surrogate, so it converts to UTF-8.
template <typename Text>
bool isValidUtf16(const Text& text) noexcept {
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto unit = static_cast<std::uint16_t>(text[index]);
        if (unit >= 0xdc00 && unit <= 0xdfff) {
            return false;
        }
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (index + 1 == text.size()) {
                return false;
            }
            const auto low = static_cast<std::uint16_t>(text[index + 1]);
            if (low < 0xdc00 || low > 0xdfff) {
                return false;
            }
            ++index;
        }
    }
    return true;
}

} // namespace

std::string toUtf8(const std::filesystem::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::optional<std::string> toUtf8Checked(const std::filesystem::path& path) {
    using Character = std::filesystem::path::value_type;
    // Windows paths are UTF-16 and their conversion fails on an unpaired surrogate; other
    // platforms store bytes, which must be UTF-8.
    if constexpr (sizeof(Character) == 2) {
        if (!isValidUtf16(path.native())) {
            return std::nullopt;
        }
        return toUtf8(path);
    } else {
        std::string text = toUtf8(path);
        if (!isValidUtf8(text)) {
            return std::nullopt;
        }
        return text;
    }
}

std::filesystem::path fromUtf8(std::string_view text) {
    return {std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
}

Unexpected<Error> ioError(std::string_view what, const std::filesystem::path& path, const std::error_code& error) {
    std::string message =
        std::string(what) + " '" + toUtf8Checked(path).value_or("<a name that is not valid Unicode>") + "'";
    if (error) {
        message += ": " + error.message();
    }
    return makeError("asset.io", std::move(message));
}

Expected<std::string> readFile(const std::filesystem::path& path, std::size_t maxSize) {
    std::error_code error;
    // file_size follows symbolic links, so a link to a huge file is refused here too.
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    if (error) {
        return ioError("can't get the size of", path, error);
    }
    if (size > maxSize) {
        return makeError("asset.too_large", "'" + toUtf8Checked(path).value_or("?") + "' has " + std::to_string(size) +
                                                " bytes; the limit is " + std::to_string(maxSize));
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return ioError("can't open", path, {});
    }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    if (size != 0 && !stream.read(bytes.data(), static_cast<std::streamsize>(size))) {
        return ioError("can't read", path, {});
    }
    return bytes;
}

Expected<void> writeFileAtomically(const std::filesystem::path& path, std::string_view bytes) {
    std::error_code error;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) {
            return ioError("can't create the directory", path.parent_path(), error);
        }
    }
    std::filesystem::path temporary = path;
    temporary += fromUtf8(std::string(kTemporaryMarker) + Uuid::generate().toString());
    {
        std::ofstream stream(temporary, std::ios::binary);
        if (!stream || !stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size())) || !stream.flush()) {
            stream.close();
            std::filesystem::remove(temporary, error);
            return ioError("can't write", temporary, {});
        }
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return ioError("can't replace", path, error);
    }
    return {};
}

} // namespace ith::detail::asset_files
