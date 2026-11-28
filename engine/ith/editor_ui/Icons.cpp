#include "ith/editor_ui/Icons.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace ith {
namespace {

// The icons of the built-in commands in the File and Edit menus and the main toolbar.
constexpr std::array<std::pair<std::string_view, const Icon*>, 17> kCommandIcons{{
    {"ith.play.keep_simulation_changes", &icons::kKeepChanges},
    {"ith.play.pause", &icons::kPause},
    {"ith.play.play", &icons::kPlay},
    {"ith.play.simulate", &icons::kSimulate},
    {"ith.play.step", &icons::kStep},
    {"ith.play.stop", &icons::kStop},
    {"ith.edit.delete", &icons::kTrash},
    {"ith.edit.duplicate", &icons::kCopy},
    {"ith.edit.redo", &icons::kRedo},
    {"ith.edit.rename", &icons::kPencil},
    {"ith.edit.select_all", &icons::kSelectAll},
    {"ith.edit.select_none", &icons::kCircleX},
    {"ith.edit.undo", &icons::kUndo},
    {"ith.file.new_level", &icons::kFilePlus},
    {"ith.file.open_level", &icons::kFolderOpen},
    {"ith.file.save", &icons::kSave},
    {"ith.file.save_as", &icons::kSaveAll},
}};

} // namespace

std::string iconText(const Icon& icon) {
    const char32_t code = icon.codepoint;
    std::string text;
    if (code < 0x80) {
        text += static_cast<char>(code);
    } else if (code < 0x800) {
        text += static_cast<char>(0xC0 | (code >> 6));
        text += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        text += static_cast<char>(0xE0 | (code >> 12));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        text += static_cast<char>(0xF0 | (code >> 18));
        text += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code & 0x3F));
    }
    return text;
}

const Icon* commandIcon(std::string_view id) noexcept {
    for (const auto& [command, icon] : kCommandIcons) {
        if (command == id) {
            return icon;
        }
    }
    return nullptr;
}

} // namespace ith
