// libFuzzer target for the untrusted readers of the input action layer: the serialization envelope
// through readDocument and the id path of readText, the input action and context assets with their
// validation, and the bindings document applied to a binding configuration. Built only in Clang
// sanitizer builds; CTest runs a short smoke run seeded with tests/input_actions/fuzz_seeds and the
// JSON dictionary, and longer runs are started by hand:
//
//   ith_input_actions_fuzz -max_total_time=600 -dict=tests/serialization/fuzz_seeds/json.dict
//       <corpus directory> tests/input_actions/fuzz_seeds

#include "ith/input_actions/InputActionAssets.h"
#include "ith/input_actions/InputAssetLibrary.h"
#include "ith/input_actions/InputBindings.h"
#include "ith/serialization/TextArchive.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace serialization = ith::serialization;

// Reads `text` as an asset document of `format`, without the id it must have and with it; a
// successful read is written back and returned with its id.
template <typename T>
bool readAsset(std::string_view text, const serialization::DocumentFormat& format, T& asset, ith::Uuid& id) {
    static_cast<void>(serialization::readText(text, format, asset));
    if (!serialization::readText(text, format, asset, {.id = &id}).hasValue()) {
        return false;
    }
    static_cast<void>(serialization::writeText(format, asset, &id));
    return true;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    ith::Uuid id;
    for (ith::Uuid* expected : {static_cast<ith::Uuid*>(nullptr), &id}) {
        const auto document = serialization::readDocument(text, ith::kInputBindingsFormat, {.id = expected});
        if (document.hasValue()) {
            static_cast<void>(serialization::writeDocument(ith::kInputBindingsFormat, *document, expected));
        }
    }

    // A library with one slot and one setting, plus the assets read from the input.
    ith::InputAssetLibrary library;
    const ith::AssetId jump(ith::Uuid(1, 2));
    library.setAction(jump, {});
    library.setContext(ith::AssetId(ith::Uuid(1, 3)),
                       ith::InputContextAsset{.mappings = {ith::InputMapping{.action = ith::AssetRef::to(jump),
                                                                             .key = "Keyboard.Space",
                                                                             .slot = "jump.keyboard"}}});
    library.setSettings({ith::InputSettingDecl{.name = "look_sensitivity"}});
    ith::InputActionAsset action;
    if (readAsset(text, ith::kInputActionFormat, action, id)) {
        library.setAction(ith::AssetId(id), std::move(action));
    }
    ith::InputContextAsset context;
    if (readAsset(text, ith::kInputContextFormat, context, id)) {
        library.setContext(ith::AssetId(id), std::move(context));
    }
    static_cast<void>(library.validate());
    static_cast<void>(library.slots());

    std::vector<serialization::Error> warnings;
    const auto bindings = ith::readInputBindings(text, {.source = "fuzz", .warnings = &warnings});
    if (bindings.hasValue()) {
        static_cast<void>(ith::writeInputBindings(*bindings));
        ith::InputBindingConfig config(library);
        config.setLayer(ith::InputBindingLayer::User, *bindings, "fuzz", &warnings);
        static_cast<void>(config.key("default", "jump.keyboard"));
        static_cast<void>(config.setting("default", "look_sensitivity"));
    }
    return 0;
}
