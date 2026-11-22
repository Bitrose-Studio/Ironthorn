// The only place yyjson is used (ADR-0010 rule 1): its types never leave this file.

#include "ith/serialization/Json.h"

#include "ith/serialization/Document.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace ith::serialization {
namespace {

// yyjson allocates through this table rather than its built-in default, so the engine controls
// every allocation it makes. It forwards to the C heap until the engine has a general-purpose
// allocator to plug in here.
void* allocate(void* /*context*/, std::size_t size) {
    return std::malloc(size); // NOLINT(cppcoreguidelines-no-malloc): yyjson frees with free()
}

void* reallocate(void* /*context*/, void* memory, std::size_t /*oldSize*/, std::size_t size) {
    return std::realloc(memory, size); // NOLINT(cppcoreguidelines-no-malloc)
}

void release(void* /*context*/, void* memory) {
    std::free(memory); // NOLINT(cppcoreguidelines-no-malloc)
}

const yyjson_alc kAllocator = {&allocate, &reallocate, &release, nullptr};

struct DocumentDeleter {
    void operator()(yyjson_doc* document) const noexcept { yyjson_doc_free(document); }
};

struct MutableDocumentDeleter {
    void operator()(yyjson_mut_doc* document) const noexcept { yyjson_mut_doc_free(document); }
};

struct TextDeleter {
    void operator()(char* text) const noexcept { kAllocator.free(kAllocator.ctx, text); }
};

Error makeError(std::string_view source, std::string code, std::string message) {
    return Error{.code = std::move(code), .message = std::move(message), .source = std::string(source)};
}

std::string_view textOf(const yyjson_val* value) {
    const char* text = yyjson_is_raw(value) ? yyjson_get_raw(value) : yyjson_get_str(value);
    return {text, yyjson_get_len(value)};
}

// Copies a parsed yyjson tree into a Value. `depth` counts the containers above `value`.
bool toValue(const yyjson_val* value, std::size_t depth, Value& out) {
    if ((yyjson_is_arr(value) || yyjson_is_obj(value)) && depth + 1 > kMaxDepth) {
        return false;
    }
    if (yyjson_is_null(value)) {
        out = Value();
    } else if (yyjson_is_bool(value)) {
        out = Value::boolean(yyjson_get_bool(value));
    } else if (yyjson_is_raw(value)) {
        out = Value::number(textOf(value));
    } else if (yyjson_is_str(value)) {
        out = Value::string(std::string(textOf(value)));
    } else if (yyjson_is_arr(value)) {
        out = Value::array();
        std::vector<Value>& elements = out.elements();
        elements.reserve(yyjson_arr_size(value));
        yyjson_arr_iter iterator;
        yyjson_arr_iter_init(value, &iterator);
        while (const yyjson_val* element = yyjson_arr_iter_next(&iterator)) {
            if (!toValue(element, depth + 1, elements.emplace_back())) {
                return false;
            }
        }
    } else {
        out = Value::object();
        std::vector<Value::Member>& members = out.members();
        members.reserve(yyjson_obj_size(value));
        yyjson_obj_iter iterator;
        yyjson_obj_iter_init(value, &iterator);
        while (yyjson_val* key = yyjson_obj_iter_next(&iterator)) {
            Value::Member& member = members.emplace_back();
            member.key = std::string(textOf(key));
            if (!toValue(yyjson_obj_iter_get_val(key), depth + 1, member.value)) {
                return false;
            }
        }
    }
    return true;
}

// Builds the yyjson tree of `value`. Strings and numbers point into `value`, which outlives the
// document. Returns null when memory runs out or the nesting is too deep (`tooDeep` is set).
yyjson_mut_val* toYyjson(yyjson_mut_doc* document, const Value& value, std::size_t depth, bool& tooDeep) {
    switch (value.kind()) {
        case Value::Kind::Null:
            return yyjson_mut_null(document);
        case Value::Kind::Bool:
            return yyjson_mut_bool(document, value.asBool());
        case Value::Kind::Number: {
            const std::string_view text = value.numberText();
            return yyjson_mut_rawn(document, text.data(), text.size());
        }
        case Value::Kind::String:
            return yyjson_mut_strn(document, value.asString().data(), value.asString().size());
        case Value::Kind::Array:
        case Value::Kind::Object:
            break;
    }
    if (depth + 1 > kMaxDepth) {
        tooDeep = true;
        return nullptr;
    }
    if (value.kind() == Value::Kind::Array) {
        yyjson_mut_val* array = yyjson_mut_arr(document);
        if (array == nullptr) {
            return nullptr;
        }
        for (const Value& element : value.elements()) {
            yyjson_mut_val* converted = toYyjson(document, element, depth + 1, tooDeep);
            if (converted == nullptr || !yyjson_mut_arr_append(array, converted)) {
                return nullptr;
            }
        }
        return array;
    }
    yyjson_mut_val* object = yyjson_mut_obj(document);
    if (object == nullptr) {
        return nullptr;
    }
    for (const Value::Member& member : value.members()) {
        yyjson_mut_val* key = yyjson_mut_strn(document, member.key.data(), member.key.size());
        yyjson_mut_val* converted = toYyjson(document, member.value, depth + 1, tooDeep);
        if (key == nullptr || converted == nullptr || !yyjson_mut_obj_add(object, key, converted)) {
            return nullptr;
        }
    }
    return object;
}

} // namespace

Expected<Value, Error> parseJson(std::string_view text, const JsonReadOptions& options) {
    const std::size_t maxSize = std::min(options.maxSize, kMaxTextSize);
    if (text.size() > maxSize) {
        return makeUnexpected(makeError(options.source, "serialization.too_large",
                                        "the document has " + std::to_string(text.size()) + " bytes; the limit is " +
                                            std::to_string(maxSize)));
    }
    if (text.empty()) {
        // Handled here: an empty view may have a null data pointer, which yyjson must not get.
        Error error = makeError(options.source, "serialization.syntax", "the document is empty");
        error.offset = 0;
        error.line = 1;
        error.column = 1;
        return makeUnexpected(std::move(error));
    }
    yyjson_read_flag flags = YYJSON_READ_NUMBER_AS_RAW;
    if (options.syntax == JsonSyntax::Config) {
        flags |= YYJSON_READ_ALLOW_COMMENTS;
    }
    yyjson_read_err readError{};
    // yyjson doesn't write to the input without YYJSON_READ_INSITU.
    const std::unique_ptr<yyjson_doc, DocumentDeleter> document(
        yyjson_read_opts(const_cast<char*>(text.data()), text.size(), flags, &kAllocator, &readError));
    if (document == nullptr) {
        Error error = makeError(options.source, "serialization.syntax",
                                readError.msg != nullptr ? readError.msg : "invalid JSON");
        if (readError.code == YYJSON_READ_ERROR_DEPTH) {
            error.code = "serialization.too_deep";
            error.message = "arrays and objects nest deeper than " + std::to_string(kMaxDepth);
        }
        error.offset = readError.pos;
        std::size_t character = 0;
        if (!yyjson_locate_pos(text.data(), text.size(), readError.pos, &error.line, &error.column, &character)) {
            error.line = 0;
            error.column = 0;
        }
        return makeUnexpected(std::move(error));
    }
    // yyjson counts keys and values; each takes at most a Member (a key and a Value) in the tree.
    // Refuse trees larger than the budget before building them.
    const std::size_t values = yyjson_doc_get_val_count(document.get());
    const std::size_t budget = allocationBudget(text.size(), options.allocationBudget);
    if (values > budget / sizeof(Value::Member)) {
        return makeUnexpected(makeError(options.source, "serialization.too_large",
                                        "the document has " + std::to_string(values) +
                                            " values, more than the allocation budget of " + std::to_string(budget) +
                                            " bytes allows"));
    }
    Value result;
    if (!toValue(yyjson_doc_get_root(document.get()), 0, result)) {
        return makeUnexpected(makeError(options.source, "serialization.too_deep",
                                        "arrays and objects nest deeper than " + std::to_string(kMaxDepth)));
    }
    return result;
}

Expected<std::string, Error> writeJson(const Value& value) {
    const std::unique_ptr<yyjson_mut_doc, MutableDocumentDeleter> document(yyjson_mut_doc_new(&kAllocator));
    if (document == nullptr) {
        return makeUnexpected(makeError({}, "serialization.write_failed", "out of memory"));
    }
    bool tooDeep = false;
    yyjson_mut_val* root = toYyjson(document.get(), value, 0, tooDeep);
    if (root == nullptr) {
        return makeUnexpected(tooDeep ? makeError({}, "serialization.too_deep",
                                                  "arrays and objects nest deeper than " + std::to_string(kMaxDepth))
                                      : makeError({}, "serialization.write_failed", "out of memory"));
    }
    yyjson_mut_doc_set_root(document.get(), root);

    yyjson_write_err writeError{};
    std::size_t length = 0;
    const std::unique_ptr<char, TextDeleter> text(
        yyjson_mut_write_opts(document.get(), YYJSON_WRITE_PRETTY_TWO_SPACES | YYJSON_WRITE_NEWLINE_AT_END, &kAllocator,
                              &length, &writeError));
    if (text == nullptr) {
        return makeUnexpected(makeError({}, "serialization.write_failed",
                                        writeError.msg != nullptr ? writeError.msg : "the JSON writer failed"));
    }
    if (length > kMaxTextSize) {
        return makeUnexpected(makeError({}, "serialization.too_large",
                                        "the document would have " + std::to_string(length) + " bytes; the limit is " +
                                            std::to_string(kMaxTextSize)));
    }
    return std::string(text.get(), length);
}

} // namespace ith::serialization
