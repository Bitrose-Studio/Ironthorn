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
inline constexpr Icon kPlus{"plus", 0xE13D};
inline constexpr Icon kReset{"rotate-ccw", 0xE148};
inline constexpr Icon kSearch{"search", 0xE151};
inline constexpr Icon kClose{"x", 0xE1B2};
inline constexpr Icon kFolder{"folder", 0xE0D7};
inline constexpr Icon kFile{"file", 0xE0C0};
inline constexpr Icon kFileQuestion{"file-question", 0xE322};
inline constexpr Icon kFileText{"file-text", 0xE0CC};
inline constexpr Icon kMap{"map", 0xE110};
inline constexpr Icon kBox{"box", 0xE061};
inline constexpr Icon kImage{"image", 0xE0F6};
inline constexpr Icon kMusic{"music", 0xE122};
inline constexpr Icon kWarning{"triangle-alert", 0xE193};
inline constexpr Icon kRefresh{"refresh-cw", 0xE145};
inline constexpr Icon kGrid{"layout-grid", 0xE0FF};
inline constexpr Icon kList{"list", 0xE106};
inline constexpr Icon kFilter{"filter", 0xE0DC};
inline constexpr Icon kSort{"arrow-down-up", 0xE046};
inline constexpr Icon kChevronRight{"chevron-right", 0xE06F};
inline constexpr Icon kImport{"import", 0xE22F};
inline constexpr Icon kFolderPlus{"folder-plus", 0xE0D9};
inline constexpr Icon kFolders{"folders", 0xE33F};
inline constexpr Icon kPlay{"play", 0xE13C};
inline constexpr Icon kSimulate{"monitor-play", 0xE485};
inline constexpr Icon kPause{"pause", 0xE12E};
inline constexpr Icon kStop{"square", 0xE167};
inline constexpr Icon kStep{"step-forward", 0xE3EA};
inline constexpr Icon kPossess{"gamepad-2", 0xE0DF};
inline constexpr Icon kEject{"eject", 0xE733};
inline constexpr Icon kKeepChanges{"clipboard-check", 0xE219};

// Every icon above, for the codepoint check.
inline constexpr std::array kAll{kFilePlus, kFolderOpen,   kSave,      kSaveAll,      kUndo,     kRedo,       kTrash,
                                 kCopy,     kPencil,       kSelectAll, kCircleX,      kPlus,     kReset,      kSearch,
                                 kClose,    kFolder,       kFile,      kFileQuestion, kFileText, kMap,        kBox,
                                 kImage,    kMusic,        kWarning,   kRefresh,      kGrid,     kList,       kFilter,
                                 kSort,     kChevronRight, kImport,    kFolderPlus,   kFolders,  kPlay,       kSimulate,
                                 kPause,    kStop,         kStep,      kPossess,      kEject,    kKeepChanges};

} // namespace icons

// The icon as UTF-8 text.
[[nodiscard]] std::string iconText(const Icon& icon);

// The icon of the command `id` in menus and toolbars, or null when it has none.
[[nodiscard]] const Icon* commandIcon(std::string_view id) noexcept;

} // namespace ith
