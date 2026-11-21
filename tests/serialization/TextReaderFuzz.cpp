// libFuzzer target for the text archive: parses arbitrary bytes as JSON and reads them, as strict
// and as config syntax, into reflected test types, through a format whose older versions run
// the migration runner (ADR-0010 rule 16). Built only in Clang sanitizer builds; CTest runs a
// short smoke run seeded with tests/serialization/fuzz_seeds/text and the dictionary
// fuzz_seeds/json.dict, and longer runs are started by hand:
//
//   ith_serialization_text_fuzz -max_total_time=600 -dict=tests/serialization/fuzz_seeds/json.dict
//       <corpus directory> tests/serialization/fuzz_seeds/text

#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include "ArchiveTypes.h"
#include "Loadout.h"
#include "ReflectedTypes.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace serialization = ith::serialization;

// Version 1 wraps the data in an extra object {"value": ...}; version 2 is the current layout.
ith::Expected<void, serialization::Error> unwrap(serialization::Value& data) {
    if (serialization::Value* inner = data.find("value")) {
        serialization::Value moved = std::move(*inner);
        data = std::move(moved);
    }
    return {};
}

constexpr serialization::Migration kMigrations[] = {&unwrap};
constexpr serialization::DocumentFormat kFormat{
    .name = "test.text", .version = 2, .minimumVersion = 1, .migrations = kMigrations};

template <typename T>
void readAs(std::string_view text) {
    for (const serialization::JsonSyntax syntax :
         {serialization::JsonSyntax::Strict, serialization::JsonSyntax::Config}) {
        T value{};
        static_cast<void>(serialization::readText(text, kFormat, value, {.syntax = syntax}));
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    const auto parsed = serialization::parseJson(text);
    if (parsed.hasValue()) {
        static_cast<void>(serialization::writeJson(*parsed));
    }
    readAs<ith::test::gear::Loadout>(text);
    readAs<ith::test::Containers>(text);
    readAs<ith::test::AllLeaves>(text);
    readAs<ith::test::archive::Node>(text);
    readAs<ith::test::archive::Defaults>(text);
    readAs<std::vector<ith::test::archive::Bulky>>(text);
    return 0;
}
