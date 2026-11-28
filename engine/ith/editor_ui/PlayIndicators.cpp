#include "ith/editor_ui/PlayIndicators.h"

#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PlaySession.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ith {

PlayIndicator playIndicator(const EditorSession& session) {
    const PlaySession* play = session.play();
    if (play == nullptr) {
        return PlayIndicator::None;
    }
    if (play->paused()) {
        return PlayIndicator::Paused;
    }
    return play->possessed() ? PlayIndicator::Playing : PlayIndicator::Simulating;
}

std::string_view playIndicatorLabel(PlayIndicator indicator) {
    switch (indicator) {
        case PlayIndicator::Playing:
            return "Playing";
        case PlayIndicator::Simulating:
            return "Simulating";
        case PlayIndicator::Paused:
            return "Paused";
        case PlayIndicator::None:
            break;
    }
    return {};
}

ThemeColor playIndicatorColor(PlayIndicator indicator, const ThemeTokens& theme) {
    switch (indicator) {
        case PlayIndicator::Playing:
            return theme.playActive;
        case PlayIndicator::Simulating:
            return theme.playSimulate;
        case PlayIndicator::Paused:
            return theme.playPaused;
        case PlayIndicator::None:
            break;
    }
    return theme.text;
}

std::string formatPlayTime(double seconds) {
    // Clamped before the cast, so the tenths always fit 64 bits.
    const double clamped = std::isfinite(seconds) && seconds > 0.0 ? std::min(seconds, kMaxPlayTimeSeconds) : 0.0;
    // A little over a millionth of a tenth: 2.3 * 10 is 22.999999999999996 in doubles.
    constexpr double kTenthEpsilon = 1.0e-6;
    const auto tenths = static_cast<unsigned long long>(std::floor(clamped * 10.0 + kTenthEpsilon));
    std::array<char, 40> text{};
    std::snprintf(text.data(), text.size(), "%llu:%02llu.%llu", tenths / 600, (tenths / 10) % 60, tenths % 10);
    return text.data();
}

} // namespace ith
