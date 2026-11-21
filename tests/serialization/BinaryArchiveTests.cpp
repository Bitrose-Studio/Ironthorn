// The binary archive: header layout, bit-exact values, cooked-data rules and a reader that never
// trusts its input (ADR-0010 rules 7, 8 and 14).

#include "ith/serialization/BinaryArchive.h"

#include "ith/serialization/TextArchive.h"

#include "ArchiveTypes.h"
#include "EditorArchiveTypes.h"
#include "ReflectedTypes.h"

#include <doctest/doctest.h>

// doctest prints std::string operands of failed checks through an ostream.
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace {

namespace serialization = ith::serialization;
using Bytes = std::vector<std::byte>;

constexpr serialization::DocumentFormat kFormat{.name = "test.binary", .version = 2, .minimumVersion = 2};

template <typename T>
T loadAt(const Bytes& bytes, std::size_t offset) {
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(T));
    return value;
}

std::size_t payloadOffset(const Bytes& blob) {
    const std::size_t end = 26 + loadAt<std::uint16_t>(blob, 24);
    return (end + 15) / 16 * 16;
}

// `blob`'s header, with the payload size set, followed by `payload`.
Bytes withPayload(const Bytes& blob, const Bytes& payload) {
    Bytes result(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(payloadOffset(blob)));
    const std::uint64_t size = payload.size();
    std::memcpy(result.data() + 16, &size, sizeof(size));
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}

template <typename T>
Bytes blobOf(const T& value) {
    const auto blob = serialization::writeBinary(kFormat, value);
    REQUIRE_MESSAGE(blob.hasValue(), blob.error().toString());
    return *blob;
}

template <typename T>
serialization::Error readError(const Bytes& blob) {
    T value{};
    const auto result = serialization::readBinary(blob, kFormat, value, {.source = "test.bin"});
    return result.hasValue() ? serialization::Error{} : result.error();
}

void append32(Bytes& bytes, std::uint32_t value) {
    const auto* data = reinterpret_cast<const std::byte*>(&value);
    bytes.insert(bytes.end(), data, data + sizeof(value));
}

} // namespace

TEST_SUITE("serialization.binary") {
    TEST_CASE("a blob is a 16-byte aligned header and a packed little-endian payload") {
        ith::test::Health health;
        health.current = 12.5f;
        const Bytes blob = blobOf(health);
        REQUIRE(blob.size() == 48 + 8);
        CHECK(std::memcmp(blob.data(), "ITHB", 4) == 0);
        CHECK(loadAt<std::uint32_t>(blob, 4) == 2);
        CHECK(loadAt<std::uint64_t>(blob, 8) == serialization::schemaHash(ith::typeOf<ith::test::Health>()));
        CHECK(loadAt<std::uint64_t>(blob, 16) == 8);
        CHECK(loadAt<std::uint16_t>(blob, 24) == 11);
        CHECK(std::memcmp(blob.data() + 26, "test.binary", 11) == 0);
        for (std::size_t offset = 37; offset < 48; ++offset) {
            CHECK(blob[offset] == std::byte{0});
        }
        CHECK(loadAt<float>(blob, 48) == 12.5f);
        CHECK(loadAt<float>(blob, 52) == 100.0f);
    }

    TEST_CASE("every float bit pattern reads back, NaN and infinities included") {
        for (const std::uint64_t bits : {0x7ff8000000000001ull, 0xfff0000000000000ull, 0x8000000000000000ull,
                                         0x0000000000000001ull, 0x7fefffffffffffffull}) {
            const auto value = std::bit_cast<double>(bits);
            double read = 0.0;
            REQUIRE(serialization::readBinary(blobOf(value), kFormat, read).hasValue());
            CHECK(std::bit_cast<std::uint64_t>(read) == bits);
        }
        const auto nan = std::bit_cast<float>(0x7fc00123u);
        float read = 0.0f;
        REQUIRE(serialization::readBinary(blobOf(nan), kFormat, read).hasValue());
        CHECK(std::bit_cast<std::uint32_t>(read) == 0x7fc00123u);
    }

    TEST_CASE("the header must match the format, version and schema") {
        const Bytes blob = blobOf(ith::test::Health{});
        Bytes badMagic = blob;
        badMagic[0] = std::byte{'X'};
        CHECK(readError<ith::test::Health>(badMagic).code == "serialization.bad_magic");

        const serialization::DocumentFormat other{.name = "test.other", .version = 2, .minimumVersion = 2};
        ith::test::Health health;
        CHECK(serialization::readBinary(blob, other, health).error().code == "serialization.wrong_format");

        const serialization::DocumentFormat older{.name = "test.binary", .version = 1};
        const serialization::DocumentFormat newer{.name = "test.binary", .version = 3, .minimumVersion = 3};
        CHECK(serialization::readBinary(blob, older, health).error().code == "serialization.newer_version");
        CHECK(serialization::readBinary(blob, newer, health).error().code == "serialization.version_too_old");

        const serialization::Error schema = readError<ith::test::Vector3>(blob);
        CHECK(schema.code == "serialization.schema_mismatch");
        CHECK(schema.source == "test.bin");

        Bytes padding = blob;
        padding[40] = std::byte{1};
        CHECK(readError<ith::test::Health>(padding).code == "serialization.invalid_value");

        Bytes trailing = blob;
        trailing.push_back(std::byte{0});
        CHECK(readError<ith::test::Health>(trailing).code == "serialization.trailing_data");
        const Bytes longPayload = withPayload(blob, Bytes(9));
        const serialization::Error unused = readError<ith::test::Health>(longPayload);
        CHECK(unused.code == "serialization.trailing_data");
        CHECK(unused.offset == longPayload.size() - 1);
    }

    TEST_CASE("the schema hash follows the saved layout") {
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::Health>()) ==
              serialization::schemaHash(ith::typeOf<ith::test::Health>()));
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::Health>()) !=
              serialization::schemaHash(ith::typeOf<ith::test::Vector3>()));
        CHECK(serialization::schemaHash(ith::typeOf<std::array<std::int32_t, 2>>()) !=
              serialization::schemaHash(ith::typeOf<std::array<std::int32_t, 3>>()));
        CHECK(serialization::schemaHash(ith::typeOf<std::int32_t>()) !=
              serialization::schemaHash(ith::typeOf<std::uint32_t>()));
        // A type that contains itself through a vector has a finite hash.
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::archive::Node>()) != 0);
    }

    TEST_CASE("every prefix of a blob fails cleanly, and changed bytes never crash") {
        ith::test::Containers containers;
        containers.names = {"alpha", "beta"};
        containers.points.resize(2);
        containers.maybe = 1.5;
        containers.maybePoint = ith::test::Vector3{.x = 1.0f};
        containers.nested = {{1, 2, 3}, {}};
        containers.kinds = {ith::test::DamageKind::Poison};
        const Bytes blob = blobOf(containers);
        for (std::size_t length = 0; length < blob.size(); ++length) {
            const Bytes prefix(blob.begin(), blob.begin() + static_cast<std::ptrdiff_t>(length));
            CHECK_FALSE(readError<ith::test::Containers>(prefix).code.empty());
        }
        for (std::size_t offset = 0; offset < blob.size(); ++offset) {
            for (const std::byte flip : {std::byte{0x01}, std::byte{0x80}, std::byte{0xff}}) {
                Bytes changed = blob;
                changed[offset] ^= flip;
                ith::test::Containers read;
                static_cast<void>(serialization::readBinary(changed, kFormat, read));
            }
        }
    }

    TEST_CASE("counts and lengths are checked against the bytes left before allocating") {
        const Bytes blob = blobOf(ith::test::Containers{});
        Bytes payload(12); // the std::array<int32, 3>
        append32(payload, 0xffffffffu);
        const serialization::Error count = readError<ith::test::Containers>(withPayload(blob, payload));
        CHECK(count.code == "serialization.truncated");
        CHECK(count.path == "$.names");
        CHECK(count.offset == payloadOffset(blob) + 12);

        payload.resize(12);
        append32(payload, 1);
        append32(payload, 0xffffffffu); // the length of the first name
        const serialization::Error length = readError<ith::test::Containers>(withPayload(blob, payload));
        CHECK(length.code == "serialization.truncated");
        CHECK(length.path == "$.names[0]");

        // 1000 elements of a 12-byte struct need more than the 4000 bytes left.
        payload.resize(12);
        append32(payload, 0);
        append32(payload, 1000);
        payload.resize(payload.size() + 4000);
        CHECK(readError<ith::test::Containers>(withPayload(blob, payload)).path == "$.points");
    }

    TEST_CASE("booleans, presence bytes and enums are validated") {
        const Bytes leaves = blobOf(ith::test::AllLeaves{});
        Bytes payload(leaves.begin() + static_cast<std::ptrdiff_t>(payloadOffset(leaves)), leaves.end());
        payload[0] = std::byte{2};
        const serialization::Error flag = readError<ith::test::AllLeaves>(withPayload(leaves, payload));
        CHECK(flag.code == "serialization.invalid_value");
        CHECK(flag.path == "$.flag");

        const Bytes hit = blobOf(ith::test::Hit{});
        payload.assign(hit.begin() + static_cast<std::ptrdiff_t>(payloadOffset(hit)), hit.end());
        payload[0] = std::byte{5}; // DamageKind has 0, 1 and 7
        CHECK(readError<ith::test::Hit>(withPayload(hit, payload)).path == "$.kind");
        payload[0] = std::byte{7};
        payload[1] = std::byte{8}; // Abilities has the bits 1, 2 and 4
        CHECK(readError<ith::test::Hit>(withPayload(hit, payload)).path == "$.abilities");

        const Bytes optional = blobOf(std::optional<std::int32_t>{});
        CHECK(readError<std::optional<std::int32_t>>(withPayload(optional, Bytes{std::byte{2}})).code ==
              "serialization.invalid_value");

        ith::test::Hit invalid;
        invalid.altitude = static_cast<ith::test::Altitude>(1);
        const auto written = serialization::writeBinary(kFormat, invalid);
        REQUIRE_FALSE(written.hasValue());
        CHECK(written.error().code == "serialization.invalid_enum");
        CHECK(written.error().path == "$.altitude");
    }

    TEST_CASE("nesting deeper than kMaxDepth is refused in both directions") {
        ith::test::archive::Node root;
        ith::test::archive::Node* leaf = &root;
        for (int level = 0; level < 31; ++level) {
            leaf = &leaf->children.emplace_back();
        }
        const Bytes blob = blobOf(root); // a struct and a vector per node: 64 levels
        ith::test::archive::Node read;
        REQUIRE(serialization::readBinary(blob, kFormat, read).hasValue());
        leaf->children.emplace_back();
        const auto deep = serialization::writeBinary(kFormat, root);
        REQUIRE_FALSE(deep.hasValue());
        CHECK(deep.error().code == "serialization.too_deep");

        Bytes payload;
        for (int level = 0; level < 100000; ++level) {
            append32(payload, 0); // value
            append32(payload, 1); // one child
        }
        const serialization::Error error = readError<ith::test::archive::Node>(withPayload(blob, payload));
        CHECK(error.code == "serialization.too_deep");
    }

    TEST_CASE("cooked data leaves out Transient and EditorOnly data") {
        ith::test::archive::Annotated annotated;
        annotated.value = 4;
        annotated.label = "door";
        annotated.note.text = "check the hinge";
        const Bytes blob = blobOf(annotated);
        CHECK(blob.size() == payloadOffset(blob) + 4);
        ith::test::archive::Annotated read;
        read.label = "old";
        REQUIRE(serialization::readBinary(blob, kFormat, read).hasValue());
        CHECK(read.value == 4);
        CHECK(read.label.empty());
        CHECK(read.note.text.empty());

        const auto note = serialization::writeBinary(kFormat, ith::test::archive::EditorNote{});
        REQUIRE_FALSE(note.hasValue());
        CHECK(note.error().code == "serialization.not_saved");

        ith::test::Inventory inventory;
        CHECK(blobOf(inventory).size() == payloadOffset(blobOf(inventory)) + 4);
    }

    TEST_CASE("a vector of zero-byte elements is refused") {
        const std::vector<ith::test::Marker> markers(3);
        const auto blob = serialization::writeBinary(kFormat, markers);
        REQUIRE_FALSE(blob.hasValue());
        CHECK(blob.error().code == "serialization.unsupported_type");
        CHECK(serialization::writeText(kFormat, markers).hasValue());
    }

    TEST_CASE("a failed read leaves the target unchanged") {
        ith::test::Containers source;
        source.names = {"a", "b", "c"};
        source.maybe = 2.0;
        Bytes blob = blobOf(source);
        blob.back() = std::byte{9}; // the high byte of the count of `kinds`, the last field
        ith::test::Containers target;
        target.names = {"keep"};
        const ith::test::Containers before = target;
        CHECK_FALSE(serialization::readBinary(blob, kFormat, target).hasValue());
        CHECK(ith::typeOf<ith::test::Containers>().equals(&target, &before));
    }

    TEST_CASE("schema hashes are the same on every compiler, platform and configuration") {
        // Golden values: a change here means every cooked blob of these types must be recooked.
        CHECK(serialization::schemaHash(ith::typeOf<std::string>()) == 0x30330257f988382cull);
        CHECK(serialization::schemaHash(ith::typeOf<ith::Uuid>()) == 0x06b00cc6c6f37811ull);
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::AllLeaves>()) == 0x88dd0a34f32d1281ull);
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::Containers>()) == 0x7db6c13b3f22caf8ull);
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::Hit>()) == 0xec90811f47e36c48ull);
        CHECK(serialization::schemaHash(ith::typeOf<ith::test::archive::Node>()) == 0x6bd4db79249a32e8ull);
    }

    TEST_CASE("reads stay within the allocation budget") {
        // 100000 elements of 4 bytes each in the blob, but over 4 KiB each in memory.
        const Bytes blob = blobOf(std::vector<ith::test::archive::Bulky>{});
        Bytes payload;
        append32(payload, 100000);
        payload.resize(payload.size() + 4 * 100000);
        const serialization::Error bulky =
            readError<std::vector<ith::test::archive::Bulky>>(withPayload(blob, payload));
        CHECK(bulky.code == "serialization.too_large");
        CHECK(bulky.path == "$");

        ith::test::AllLeaves leaves;
        leaves.text = std::string(1000, 'x');
        ith::test::AllLeaves read;
        const auto small = serialization::readBinary(blobOf(leaves), kFormat, read, {.allocationBudget = 100});
        REQUIRE_FALSE(small.hasValue());
        CHECK(small.error().code == "serialization.too_large");
        CHECK(small.error().path == "$.text");
        REQUIRE(serialization::readBinary(blobOf(leaves), kFormat, read, {.allocationBudget = 1000}).hasValue());
        CHECK(read.text == leaves.text);
    }
}
