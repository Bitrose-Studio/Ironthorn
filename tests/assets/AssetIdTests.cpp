#include "ith/assets/AssetId.h"

#include "ith/assets/AssetRef.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/BinaryArchive.h"
#include "ith/serialization/TextArchive.h"

#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

namespace {

namespace serialization = ith::serialization;

constexpr serialization::DocumentFormat kFormat{.name = "test.refs", .version = 1};

} // namespace

TEST_SUITE("assets.id") {
    TEST_CASE("asset and sub-asset ids are distinct types over a Uuid") {
        static_assert(!std::is_convertible_v<ith::AssetId, ith::SubAssetId>);
        static_assert(!std::is_convertible_v<ith::Uuid, ith::AssetId>);
        constexpr ith::AssetId nil;
        static_assert(nil.isNil());

        const ith::AssetId id = ith::AssetId::generate();
        CHECK_FALSE(id.isNil());
        CHECK(id != ith::AssetId::generate());
        const ith::Expected<ith::AssetId> parsed = ith::AssetId::parse(id.toString());
        REQUIRE(parsed.hasValue());
        if (parsed) {
            CHECK(*parsed == id);
        }
        CHECK(ith::AssetId(id.uuid()) == id);
    }

    TEST_CASE("parsing a malformed id fails") {
        const ith::Expected<ith::SubAssetId> parsed = ith::SubAssetId::parse("not-a-uuid");
        REQUIRE_FALSE(parsed.hasValue());
        if (!parsed) {
            CHECK(parsed.error().code == "uuid.malformed");
        }
    }

    TEST_CASE("ids are hashable and ordered like their Uuids") {
        std::unordered_set<ith::AssetId> set;
        const ith::AssetId a(ith::Uuid(1, 2));
        const ith::AssetId b(ith::Uuid(1, 3));
        set.insert(a);
        set.insert(a);
        set.insert(b);
        CHECK(set.size() == 2);
        CHECK(a < b);
    }
}

TEST_SUITE("assets.ref") {
    TEST_CASE("a reference holds the asset and sub-asset ids") {
        const ith::AssetId asset = ith::AssetId::generate();
        const ith::SubAssetId sub = ith::SubAssetId::generate();
        const ith::AssetRef ref = ith::AssetRef::to(asset, sub);
        CHECK(ref.isSet());
        CHECK(ref.assetId() == asset);
        CHECK(ref.subAssetId() == sub);
        CHECK_FALSE(ith::AssetRef{}.isSet());
        CHECK(ith::SoftAssetRef::to(asset).subAssetId().isNil());
    }

    TEST_CASE("the reference kind is given by the reflected type") {
        CHECK(ith::assetReferenceKind(ith::typeOf<ith::AssetRef>()) == ith::AssetReferenceKind::Hard);
        CHECK(ith::assetReferenceKind(ith::typeOf<ith::SoftAssetRef>()) == ith::AssetReferenceKind::Soft);
        CHECK(ith::assetReferenceKind(ith::typeOf<ith::Uuid>()) == ith::AssetReferenceKind::None);
        CHECK(ith::assetReferenceKind(ith::typeOf<std::vector<ith::AssetRef>>()) == ith::AssetReferenceKind::None);
        CHECK(ith::typeOf<ith::AssetRef>().id != ith::typeOf<ith::SoftAssetRef>().id);
    }

    TEST_CASE("references are saved as ids only and round-trip through both archives") {
        const ith::AssetRef ref = ith::AssetRef::to(
            ith::AssetId(ith::Uuid(0x0123456789abcdefull, 0x1122334455667788ull)), ith::SubAssetId(ith::Uuid(1, 2)));
        const ith::Expected<std::string, serialization::Error> text = serialization::writeText(kFormat, ref);
        REQUIRE(text.hasValue());
        if (!text) {
            return;
        }
        CHECK(*text == "{\n"
                       "  \"format\": \"test.refs\",\n"
                       "  \"version\": 1,\n"
                       "  \"data\": {\n"
                       "    \"asset\": \"01234567-89ab-cdef-1122-334455667788\",\n"
                       "    \"subAsset\": \"00000000-0000-0001-0000-000000000002\"\n"
                       "  }\n"
                       "}\n");
        ith::AssetRef read;
        CHECK(serialization::readText(*text, kFormat, read).hasValue());
        CHECK(read == ref);

        const std::vector<ith::SoftAssetRef> soft = {ith::SoftAssetRef::to(ith::AssetId::generate()), {}};
        const auto blob = serialization::writeBinary(kFormat, soft);
        REQUIRE(blob.hasValue());
        if (!blob) {
            return;
        }
        std::vector<ith::SoftAssetRef> readSoft;
        CHECK(serialization::readBinary(*blob, kFormat, readSoft).hasValue());
        CHECK(readSoft == soft);
    }
}
