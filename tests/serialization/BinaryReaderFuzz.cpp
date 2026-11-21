// libFuzzer target for the binary archive reader (ADR-0010 rule 16). Half of the inputs are read
// as whole blobs; the other half are read as payloads behind a valid header, so the fuzzer
// reaches the value decoding without having to find the schema hash. Built only in Clang
// sanitizer builds; CTest runs a short smoke run seeded with tests/serialization/fuzz_seeds/binary
// (blobs of the test types, each after a 0 byte), and longer runs are started by hand:
//
//   ith_serialization_binary_fuzz -max_total_time=600 <corpus directory> tests/serialization/fuzz_seeds/binary

#include "ith/serialization/BinaryArchive.h"

#include "ArchiveTypes.h"
#include "Loadout.h"
#include "ReflectedTypes.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace {

namespace serialization = ith::serialization;

constexpr serialization::DocumentFormat kFormat{.name = "test.binary", .version = 1};

template <typename T>
void readAs(std::span<const std::byte> input, bool asPayload) {
    T value{};
    if (!asPayload) {
        static_cast<void>(serialization::readBinary(input, kFormat, value));
        return;
    }
    // The header of a valid blob, with the payload size set to the input's.
    std::vector<std::byte> blob = *serialization::writeBinary(kFormat, value);
    const std::size_t headerSize = blob.size() - [&blob] {
        std::uint64_t payloadSize = 0;
        std::memcpy(&payloadSize, blob.data() + 16, sizeof(payloadSize));
        return static_cast<std::size_t>(payloadSize);
    }();
    blob.resize(headerSize);
    const std::uint64_t payloadSize = input.size();
    std::memcpy(blob.data() + 16, &payloadSize, sizeof(payloadSize));
    blob.insert(blob.end(), input.begin(), input.end());
    static_cast<void>(serialization::readBinary(blob, kFormat, value));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const bool asPayload = (data[0] & 1) != 0;
    const std::span<const std::byte> input(reinterpret_cast<const std::byte*>(data) + 1, size - 1);
    readAs<ith::test::gear::Loadout>(input, asPayload);
    readAs<ith::test::Containers>(input, asPayload);
    readAs<ith::test::AllLeaves>(input, asPayload);
    readAs<ith::test::archive::Node>(input, asPayload);
    readAs<ith::test::archive::Defaults>(input, asPayload);
    readAs<std::vector<ith::test::archive::Bulky>>(input, asPayload);
    return 0;
}
