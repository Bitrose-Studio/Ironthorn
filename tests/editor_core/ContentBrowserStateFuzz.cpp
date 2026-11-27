// libFuzzer target for the Content Browser's session state reader (parseContentBrowserState): the
// file in Saved/Editor comes from the disk, so a broken or hostile one must only fail to read. Built
// only in Clang sanitizer builds; CTest runs a short smoke run seeded with
// tests/editor_core/fuzz_seeds/content_browser_state, and longer runs are started by hand:
//
//   ith_content_state_fuzz -max_total_time=600 <corpus directory> tests/editor_core/fuzz_seeds/content_browser_state

#include "ith/editor_core/ContentBrowserModel.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto state = ith::parseContentBrowserState(text, "fuzz.json");
    // What reads holds sizes the browser can use.
    if (state.hasValue() &&
        (!(state->treeWidth >= 0.05f && state->treeWidth <= 0.95f) || !(state->drawerHeight >= 0.0f))) {
        __builtin_trap();
    }
    return 0;
}
