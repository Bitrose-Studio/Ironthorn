#pragma once

// The editor's icons, by name (ADR-0029 rule 14). They are glyphs of the Lucide icon font, merged
// into the editor's UI fonts, so an icon is text: draw iconText(icons::kSave), or put it before a
// label. Panel code names icons only through this header and never writes a codepoint, because
// Lucide moves codepoints between releases; the editor_ui test checks each entry against the
// pinned font's codepoints.json. Add an icon here (name and codepoint from codepoints.json) when a
// panel needs it.

#include <array>
#include <string>
#include <string_view>

namespace ith {

// A Lucide icon: its name in Lucide's catalogue and its codepoint in the pinned font.
struct Icon {
    std::string_view name;
    char32_t codepoint = 0;
};

namespace icons {

inline constexpr Icon kFilePlus{"file-plus", 0xE0C9};
inline constexpr Icon kFolderOpen{"folder-open", 0xE247};
inline constexpr Icon kSave{"save", 0xE14D};
inline constexpr Icon kSaveAll{"save-all", 0xE40F};
inline constexpr Icon kUndo{"undo-2", 0xE2A1};
inline constexpr Icon kRedo{"redo-2", 0xE2A0};
inline constexpr Icon kTrash{"trash-2", 0xE18E};
inline constexpr Icon kCopy{"copy", 0xE09E};
inline constexpr Icon kPencil{"pencil", 0xE1F9};
inline constexpr Icon kSelectAll{"square-dashed-mouse-pointer", 0xE509};
inline constexpr Icon kCircleX{"circle-x", 0xE084};

// Every icon above, for the codepoint check.
inline constexpr std::array kAll{kFilePlus, kFolderOpen, kSave,   kSaveAll,   kUndo,   kRedo,
                                 kTrash,    kCopy,       kPencil, kSelectAll, kCircleX};

} // namespace icons

// The icon as UTF-8 text.
[[nodiscard]] std::string iconText(const Icon& icon);

// The icon of the command `id` in menus and toolbars, or null when it has none.
[[nodiscard]] const Icon* commandIcon(std::string_view id) noexcept;

} // namespace ith
