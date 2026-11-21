// libFuzzer target for the reflection generator: tokenizes, parses and generates arbitrary bytes
// as a header (ADR-0001 rule 8). Built only in Clang sanitizer builds; CTest runs a short smoke
// run seeded with the fixture headers, and longer runs are started by hand:
//
//   ith_reflect_fuzz -max_total_time=600 <corpus directory> tests/reflect_tool/fixtures

#include "ith_reflect/Generator.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::vector<ith::reflect::SourceHeader> headers = {ith::reflect::SourceHeader{
        .path = "Fuzz.h", .includePath = "Fuzz.h", .text = std::string(reinterpret_cast<const char*>(data), size)}};
    for (const ith::reflect::ModuleKind kind : {ith::reflect::ModuleKind::Runtime, ith::reflect::ModuleKind::Editor}) {
        static_cast<void>(
            ith::reflect::generate(ith::reflect::GenerateOptions{.module = "fuzz", .kind = kind}, headers));
    }
    return 0;
}
