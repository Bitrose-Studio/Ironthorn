#include "ith/editor_ui/OutputLog.h"

#include "ith/editor_core/PanelRegistry.h"
#include "ith/foundation/StringUtils.h"

#include <imgui.h>

#include <array>
#include <string_view>

namespace ith {

namespace {

ImVec4 colorOf(ThemeColor color) {
    constexpr float kScale = 1.0f / 255.0f;
    return {static_cast<float>((color.rgb >> 16) & 0xFFu) * kScale,
            static_cast<float>((color.rgb >> 8) & 0xFFu) * kScale, static_cast<float>(color.rgb & 0xFFu) * kScale,
            static_cast<float>(color.alpha) * kScale};
}

constexpr std::array<const char*, 3> kFilterNames = {"All", "Warnings and errors", "Errors"};

bool passes(OutputLogFilter filter, LogLevel level) {
    switch (filter) {
        case OutputLogFilter::WarningsAndErrors:
            return level >= LogLevel::Warning;
        case OutputLogFilter::Errors:
            return level >= LogLevel::Error;
        case OutputLogFilter::All:
            break;
    }
    return true;
}

} // namespace

OutputLog::OutputLog(const LogRingBuffer& buffer, const ThemeTokens& theme)
    : m_buffer(&buffer), m_warning(theme.warning), m_error(theme.error) {}

void OutputLog::update() {
    std::vector<LogEntry> taken;
    const std::uint64_t next = m_buffer->read(m_next, taken);
    if (!taken.empty() && taken.front().sequence > m_next) {
        m_missed += taken.front().sequence - m_next;
    }
    for (LogEntry& entry : taken) {
        m_entries.push_back(std::move(entry));
    }
    while (m_entries.size() > m_buffer->capacity()) {
        m_entries.pop_front();
    }
    m_next = next;
}

std::vector<const LogEntry*> OutputLog::visible() const {
    const std::string search = toLowerAscii(m_search);
    std::vector<const LogEntry*> shown;
    for (const LogEntry& entry : m_entries) {
        if (!passes(m_filter, entry.level)) {
            continue;
        }
        if (!search.empty() && toLowerAscii(entry.message).find(search) == std::string::npos &&
            toLowerAscii(entry.category->name()).find(search) == std::string::npos) {
            continue;
        }
        shown.push_back(&entry);
    }
    return shown;
}

void OutputLog::draw() {
    update();
    int filter = static_cast<int>(m_filter);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 11.0f);
    if (ImGui::Combo("##level", &filter, kFilterNames.data(), static_cast<int>(kFilterNames.size()))) {
        m_filter = static_cast<OutputLogFilter>(filter);
    }
    ImGui::SameLine();
    std::array<char, 256> search{};
    m_search.copy(search.data(), search.size() - 1);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::InputTextWithHint("##search", "Search", search.data(), search.size())) {
        m_search = search.data();
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        clear();
    }
    ImGui::Separator();

    const std::vector<const LogEntry*> shown = visible();
    if (ImGui::BeginChild("##records", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        // Follow the newest record while the view is at the bottom.
        const bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(shown.size()));
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const LogEntry& entry = *shown[static_cast<std::size_t>(row)];
                const bool colored = entry.level >= LogLevel::Warning;
                if (colored) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colorOf(entry.level >= LogLevel::Error ? m_error : m_warning));
                }
                const std::string_view level = logLevelName(entry.level);
                ImGui::Text("%s: %.*s: %s", entry.category->name(), static_cast<int>(level.size()), level.data(),
                            entry.message.c_str());
                if (colored) {
                    ImGui::PopStyleColor();
                }
            }
        }
        if (atBottom) {
            ImGui::SetScrollHereY(1.0f);
        }
    }
    ImGui::EndChild();
}

void registerOutputLog(PanelRegistry& panels, OutputLog& log) {
    panels.add({.id = "ith.output_log",
                .displayName = "Output Log",
                .defaultDock = DockPlace::Bottom,
                .draw = [&log](std::string_view /*instance*/) { log.draw(); }});
}

} // namespace ith
