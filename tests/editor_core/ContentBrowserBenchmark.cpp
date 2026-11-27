// The Content Browser benchmark (docs/specs/content-browser.md, requirement 59): 10 000 assets in 500
// folders, one of which holds 1 000 of them. The targets are 5 ms to build the view model of that
// folder (navigating into it: the shown items gathered and sorted) and 5 ms per keystroke for a
// search over all 10 000 items, on the Low tier reference machine (ADR-0005). Run on a Release
// build, by hand or as the CTest `content_browser_benchmark` (label "benchmark", excluded from normal
// test runs). It writes the project into the temporary folder, prints numbers, removes the project
// and always exits with 0.
//
//   cmake --build build/windows --config Release --target ith_content_browser_benchmark
//   build/windows/bin/Release/ith_content_browser_benchmark
//
// The times are medians of repeated runs. Building the items from the database after a Rescan is
// printed too; it is not a per-frame cost.

#include "ith/asset_database/AssetDatabase.h"
#include "ith/asset_database/Importer.h"
#include "ith/editor_core/ContentBrowserModel.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/foundation/Stopwatch.h"
#include "ith/foundation/Uuid.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using namespace ith;

constexpr int kAssets = 10'000;
constexpr int kFolders = 500;
constexpr int kBigFolder = 1'000;
constexpr int kRuns = 51;
constexpr double kTargetMs = 5.0;

double median(std::vector<double> samples) {
    std::ranges::sort(samples);
    return samples[samples.size() / 2];
}

void report(const char* what, double milliseconds, bool targeted) {
    std::printf("  %-44s %8.3f ms%s\n", what, milliseconds,
                targeted ? (milliseconds <= kTargetMs ? "  (target 5 ms: met)" : "  (target 5 ms: MISSED)") : "");
}

// Removes the folder `path` and everything in it when it goes out of scope.
struct RemoveOnExit {
    explicit RemoveOnExit(std::string folder) : path(std::move(folder)) {}

    RemoveOnExit(const RemoveOnExit&) = delete;
    RemoveOnExit& operator=(const RemoveOnExit&) = delete;
    RemoveOnExit(RemoveOnExit&&) = delete;
    RemoveOnExit& operator=(RemoveOnExit&&) = delete;

    ~RemoveOnExit() {
        std::error_code ignored;
        std::filesystem::remove_all(std::filesystem::path(std::u8string(path.begin(), path.end())), ignored);
    }

    std::string path;
};

} // namespace

int main() {
    std::error_code error;
    const std::filesystem::path temp = std::filesystem::temp_directory_path(error);
    if (error) {
        std::printf("Content Browser benchmark: no temporary folder (%s); skipped\n", error.message().c_str());
        return 0;
    }
    const std::u8string base = temp.generic_u8string();
    const std::string root =
        fs::joinPath(std::string(base.begin(), base.end()), "ith-content-benchmark-" + Uuid::generate().toString());
    // The project goes on every way out, an early one included.
    const RemoveOnExit cleanup{root};
    std::printf("Content Browser benchmark: %d assets in %d folders, one of %d items\n", kAssets, kFolders, kBigFolder);

    // The big folder first, then the rest spread over the other folders.
    for (int i = 0; i < kAssets; ++i) {
        const int folder = i < kBigFolder ? 0 : 1 + (i - kBigFolder) % (kFolders - 1);
        const std::string path = "Content/Folder" + std::to_string(folder) + "/Asset" + std::to_string(i) + ".ithasset";
        const std::string text =
            R"({"format": "ith.material", "version": 1, "id": ")" + Uuid::generate().toString() + R"(", "data": {}})";
        if (!fs::writeFileAtomic(fs::joinPath(root, path), text, {.durability = fs::Durability::Atomic})) {
            std::printf("  can't write the project; skipped\n");
            return 0;
        }
    }

    {
        const ImporterRegistry importers;
        AssetDatabase assets(root, importers);
        ContentBrowserModel model(&assets, {});
        Stopwatch stopwatch;
        if (!model.rescan()) {
            std::printf("  the scan failed; skipped\n");
            return 0;
        }
        report("scan and build every item", stopwatch.elapsedMilliseconds(), false);
        std::vector<double> refresh;
        for (int run = 0; run < 11; ++run) {
            stopwatch.restart();
            model.refresh();
            refresh.push_back(stopwatch.elapsedMilliseconds());
        }
        report("build every item from the database", median(refresh), false);

        std::vector<double> navigate;
        for (int run = 0; run < kRuns; ++run) {
            static_cast<void>(model.navigate(""));
            stopwatch.restart();
            static_cast<void>(model.navigate("Folder0"));
            navigate.push_back(stopwatch.elapsedMilliseconds());
        }
        std::printf("  (the folder shows %zu items)\n", model.shown().size());
        report("view model of the 1 000-item folder", median(navigate), true);

        static_cast<void>(model.navigate(""));
        const std::string typed = "asset99";
        std::vector<double> keystrokes;
        for (int run = 0; run < kRuns / 5; ++run) {
            model.setSearch({});
            for (std::size_t length = 1; length <= typed.size(); ++length) {
                stopwatch.restart();
                model.setSearch(typed.substr(0, length));
                keystrokes.push_back(stopwatch.elapsedMilliseconds());
            }
        }
        model.setSearch("a");
        std::printf("  (a search for \"a\" shows %zu items)\n", model.shown().size());
        report("search over every item, per keystroke", median(keystrokes), true);
    }
    return 0;
}
