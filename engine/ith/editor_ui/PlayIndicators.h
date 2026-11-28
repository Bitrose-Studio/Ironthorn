#pragma once

// Play In Editor's state as the editor shows it (ADR-0053 decision 4, docs/specs/play-in-editor.md
// 3.13): the word and colour of the play viewport's border and label and of the status bar's chip,
// and the game time the chip shows. Pure functions over the session; no ImGui.

#include "ith/editor_ui/Theme.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace ith {

class EditorSession;

// A play session's state as the indicators show it.
enum class PlayIndicator : std::uint8_t {
    // No session.
    None,
    // Running, possessed.
    Playing,
    // Running, ejected (Simulate, or Play after Eject).
    Simulating,
    // The editor's Pause (PlaySession::paused), possessed or not: no tick runs. Not the game's own
    // pause (Time::paused), which the indicators don't show.
    Paused,
};

// The indicator of `session`'s play session.
[[nodiscard]] PlayIndicator playIndicator(const EditorSession& session);

// "Playing", "Simulating" or "Paused"; empty for None.
[[nodiscard]] std::string_view playIndicatorLabel(PlayIndicator indicator);

// The theme's playActive, playSimulate or playPaused; text for None.
[[nodiscard]] ThemeColor playIndicatorColor(PlayIndicator indicator, const ThemeTokens& theme);

// The longest game time formatPlayTime shows, in seconds (about 31.7 million years); longer times
// show as this one.
inline constexpr double kMaxPlayTimeSeconds = 1.0e15;

// Game time as the status bar shows it, "m:ss.t" ("1:05.3"), rounded down to a tenth of a second
// (a time a hair under a tenth because of floating-point error, such as 2.3 s, counts as that
// tenth); a negative or non-finite time shows as "0:00.0", and one above kMaxPlayTimeSeconds as
// kMaxPlayTimeSeconds.
[[nodiscard]] std::string formatPlayTime(double seconds);

} // namespace ith
