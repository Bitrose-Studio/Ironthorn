#include "ith/asset_database/AssetHeader.h"

#include "ith/foundation/StringUtils.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/Value.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace ith {
namespace {

using serialization::Value;

serialization::Error headerError(std::string code, std::string message, std::string_view source) {
    return serialization::Error{.code = std::move(code), .message = std::move(message), .source = std::string(source)};
}

// Checks the envelope of a parsed document: an object with a string "format", a version number,
// an "id" and a "data".
std::optional<serialization::Error> checkEnvelope(const Value& document, std::string_view source) {
    if (document.kind() == Value::Kind::Object) {
        for (const std::string_view key : {"format", "version", "id", "data"}) {
            const auto count = std::ranges::count_if(document.members(),
                                                     [key](const Value::Member& member) { return member.key == key; });
            if (count > 1) {
                return headerError("serialization.duplicate_field",
                                   "the document key '" + std::string(key) + "' is given twice", source);
            }
        }
    }
    const bool valid = document.kind() == Value::Kind::Object && document.find("format") != nullptr &&
                       document.find("format")->kind() == Value::Kind::String && document.find("version") != nullptr &&
                       document.find("version")->toNumber<std::uint32_t>().has_value() &&
                       document.find("id") != nullptr && document.find("data") != nullptr;
    if (!valid) {
        return headerError("serialization.not_a_document",
                           R"(an asset is an object with "format", "version", "id" and "data")", source);
    }
    return std::nullopt;
}

} // namespace

bool isNativeAssetPath(std::string_view path) {
    return endsWithIgnoreCaseAscii(path, kNativeAssetExtension) || endsWithIgnoreCaseAscii(path, kLevelExtension);
}

bool formatMatchesExtension(std::string_view path, std::string_view format) {
    return endsWithIgnoreCaseAscii(path, kLevelExtension) == (format == kLevelFormat);
}

Expected<AssetHeader, serialization::Error> readAssetHeader(std::string_view text, std::string_view source) {
    Expected<Value, serialization::Error> document = serialization::parseJson(text, {.source = source});
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    if (std::optional<serialization::Error> error = checkEnvelope(*document, source)) {
        return makeUnexpected(std::move(*error));
    }
    const Value& id = *document->find("id");
    Expected<AssetId> parsed = id.kind() == Value::Kind::String ? AssetId::parse(id.asString())
                                                                : Expected<AssetId>(makeError("uuid.malformed", ""));
    if (!parsed || parsed->isNil()) {
        return makeUnexpected(headerError("asset.invalid_id", R"("id" is not a UUID other than nil)", source));
    }
    return AssetHeader{.format = document->find("format")->asString(),
                       .version = document->find("version")->toNumber<std::uint32_t>().value_or(0),
                       .id = *parsed};
}

Expected<std::string, serialization::Error> replaceHeaderId(std::string_view text, AssetId id,
                                                            std::string_view source) {
    Expected<Value, serialization::Error> document = serialization::parseJson(text, {.source = source});
    if (!document) {
        return makeUnexpected(std::move(document).error());
    }
    if (std::optional<serialization::Error> error = checkEnvelope(*document, source)) {
        return makeUnexpected(std::move(*error));
    }
    document->set("id", Value::string(id.toString()));
    return serialization::writeJson(*document);
}

} // namespace ith
