// libFuzzer target for the sidecar reader, the native asset header reader and replaceHeaderId (ADR-0015 rule 8:
// tools that read files from anywhere get fuzz targets). Built only in Clang sanitizer builds,
// which build the asset database; CTest runs a short smoke run seeded with
// tests/asset_database/fuzz_seeds/sidecar, and longer runs are started by hand:
//
//   ith_asset_sidecar_fuzz -max_total_time=600 <corpus directory> tests/asset_database/fuzz_seeds/sidecar

#include "ith/asset_database/AssetHeader.h"
#include "ith/asset_database/Sidecar.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    std::vector<ith::serialization::Error> warnings;
    const auto sidecar = ith::readSidecar(text, "fuzz.meta", &warnings);
    if (sidecar.hasValue()) {
        // Whatever reads must write, and read back as the same value.
        const auto written = ith::writeSidecar(*sidecar);
        if (!written.hasValue()) {
            __builtin_trap();
        }
        const auto again = ith::readSidecar(*written);
        if (!again.hasValue() || !(*again == *sidecar)) {
            __builtin_trap();
        }
    }
    const auto header = ith::readAssetHeader(text);
    const ith::AssetId newId(ith::Uuid(1, 2));
    const auto replaced = ith::replaceHeaderId(text, newId);
    if (header.hasValue()) {
        // A readable header stays readable with exactly the new id and nothing else changed.
        if (!replaced.hasValue()) {
            __builtin_trap();
        }
        const auto after = ith::readAssetHeader(*replaced);
        if (!after.hasValue() || after->id != newId || after->format != header->format ||
            after->version != header->version) {
            __builtin_trap();
        }
    }
    return 0;
}
