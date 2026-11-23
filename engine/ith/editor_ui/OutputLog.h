#pragma once

// ith::OutputLog: the Output Log panel (ADR-0008, ADR-0029, ADR-0040 rule 3), the editor's view of
// the log through a LogRingBuffer sink (ADR-0007).
//
//   ith::LogRingBuffer history(4096);
//   ith::addLogSink(history.sink()).value();
//   ith::OutputLog log(history, ui.theme());
//   ith::registerOutputLog(session.panels(), log);   // the "ith.output_log" panel
//
// Each frame it draws, it takes the records written since the last frame and shows them oldest
// first, coloured by level, with a level filter (all, warnings and errors, errors only), a search
// box (case-insensitive, over category and message) and Clear. It keeps at most as many records as
// the buffer does, and scrolls to the newest record while the view is at the bottom.

#include "ith/editor_ui/Theme.h"
#include "ith/foundation/Log.h"
#include "ith/foundation/LogRingBuffer.h"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace ith {

class PanelRegistry;

// Which records the Output Log shows.
enum class OutputLogFilter : std::uint8_t {
    All,
    WarningsAndErrors,
    Errors,
};

class OutputLog {
public:
    // `buffer` must outlive the OutputLog; the theme's warning and error colours are copied.
    OutputLog(const LogRingBuffer& buffer, const ThemeTokens& theme);

    // Takes the records written since the last call. draw() calls it.
    void update();

    // Draws the panel's content inside its window. Main thread, inside a UI frame.
    void draw();

    // Forgets the records taken so far.
    void clear() { m_entries.clear(); }

    void setFilter(OutputLogFilter filter) noexcept { m_filter = filter; }

    void setSearch(std::string text) { m_search = std::move(text); }

    // The records the filter and the search let through, oldest first.
    [[nodiscard]] std::vector<const LogEntry*> visible() const;

    // Records skipped because the buffer overwrote them before they were taken.
    [[nodiscard]] std::uint64_t missed() const noexcept { return m_missed; }

private:
    const LogRingBuffer* m_buffer;
    ThemeColor m_warning;
    ThemeColor m_error;
    std::deque<LogEntry> m_entries;
    std::uint64_t m_next = 0;
    std::uint64_t m_missed = 0;
    OutputLogFilter m_filter = OutputLogFilter::All;
    std::string m_search;
};

// Registers the "ith.output_log" panel (Output Log, docked at the bottom, one instance) drawing
// `log`, which must outlive the registry's use.
void registerOutputLog(PanelRegistry& panels, OutputLog& log);

} // namespace ith
