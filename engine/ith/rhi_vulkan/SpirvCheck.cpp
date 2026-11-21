#include "ith/rhi_vulkan/SpirvCheck.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ith::rhi::vulkan {
namespace {

// The SPIR-V opcodes, decorations and storage classes the check reads (SPIR-V specification,
// section 3).
constexpr std::uint32_t kMagic = 0x0723'0203;
constexpr std::size_t kHeaderWords = 5;
constexpr std::uint32_t kOpTypeImage = 25;
constexpr std::uint32_t kOpTypeSampler = 26;
constexpr std::uint32_t kOpTypeArray = 28;
constexpr std::uint32_t kOpTypeRuntimeArray = 29;
constexpr std::uint32_t kOpTypePointer = 32;
constexpr std::uint32_t kOpConstant = 43;
constexpr std::uint32_t kOpVariable = 59;
constexpr std::uint32_t kOpDecorate = 71;
constexpr std::uint32_t kDecorationBinding = 33;
constexpr std::uint32_t kDecorationDescriptorSet = 34;
constexpr std::uint32_t kStorageClassUniformConstant = 0;
constexpr std::uint32_t kStorageClassUniform = 2;
constexpr std::uint32_t kStorageClassStorageBuffer = 12;
constexpr std::uint32_t kNone = 0xFFFF'FFFFu;

// The bindings of the set (VulkanBackend.h).
constexpr std::uint32_t kBufferBinding = 0;
constexpr std::uint32_t kSampledImageBinding = 1;
constexpr std::uint32_t kStorageImageBinding = 2;
constexpr std::uint32_t kSamplerBinding = 3;

std::uint32_t word(std::span<const std::byte> code, std::size_t index) {
    std::uint32_t value = 0;
    std::memcpy(&value, code.data() + index * 4, sizeof(value));
    return value;
}

// What the check knows about one id: the instruction that defined it (op), two of its operands
// (meaning by op: variable = pointer type, storage class; pointer = storage class, pointee;
// array = element, length id; runtime array = element; image = Sampled; constant = value), and
// its descriptor decorations.
struct Id {
    std::uint32_t op = 0;
    std::uint32_t a = 0;
    std::uint32_t b = 0;
    std::uint32_t set = kNone;
    std::uint32_t binding = kNone;
};

std::string describe(const Id& variable) {
    const auto number = [](std::uint32_t value) {
        return value == kNone ? std::string("none") : std::to_string(value);
    };
    return "set " + number(variable.set) + ", binding " + number(variable.binding);
}

// One instruction of the module.
struct Instruction {
    std::span<const std::byte> code;
    std::size_t offset = 0;
    std::uint32_t length = 0;

    // Operand `index` (1 is the word after the opcode), or kNone past the instruction's end.
    [[nodiscard]] std::uint32_t operand(std::uint32_t index) const {
        return index < length ? word(code, offset + index) : kNone;
    }
};

// Records what the check needs from one instruction in the table of ids.
void read(std::vector<Id>& ids, std::uint32_t op, const Instruction& instruction) {
    const auto at = [&](std::uint32_t id) -> Id* { return id < ids.size() ? &ids[id] : nullptr; };
    const std::uint32_t length = instruction.length;
    // Which operand holds the result id, and which two operands the table keeps (Id).
    std::uint32_t result = 1;
    std::uint32_t first = 2;
    std::uint32_t second = 3;
    std::uint32_t minimumLength = 4;
    switch (op) {
        case kOpDecorate:
            if (Id* target = at(instruction.operand(1)); target != nullptr && length >= 4) {
                if (instruction.operand(2) == kDecorationDescriptorSet) {
                    target->set = instruction.operand(3);
                } else if (instruction.operand(2) == kDecorationBinding) {
                    target->binding = instruction.operand(3);
                }
            }
            return;
        case kOpVariable:
            result = 2;
            first = 1;
            break;
        case kOpTypePointer:
        case kOpTypeArray:
            break;
        case kOpTypeRuntimeArray:
            minimumLength = 3;
            break;
        case kOpTypeImage:
            first = 7; // Sampled
            minimumLength = 9;
            break;
        case kOpTypeSampler:
            minimumLength = 2;
            break;
        case kOpConstant:
            result = 2;
            first = 3;
            break;
        default:
            return;
    }
    if (Id* id = at(instruction.operand(result)); id != nullptr && length >= minimumLength) {
        id->op = op;
        id->a = instruction.operand(first);
        id->b = instruction.operand(second);
    }
}

// Why a resource variable is outside the bindless set, or std::nullopt.
std::optional<std::string> variableProblem(const std::vector<Id>& ids, const Id& variable, BindlessCounts counts) {
    const auto at = [&](std::uint32_t id) -> const Id* { return id < ids.size() ? &ids[id] : nullptr; };
    const std::uint32_t storageClass = variable.b;
    const std::string where = describe(variable);
    if (variable.set != 0 || variable.binding == kNone) {
        return "it binds a resource at " + where +
               ", which is not in the bindless set (set 0; constants must be push constants)";
    }
    // The variable's type: a pointer to the resource, or to an array of them.
    const Id* pointer = at(variable.a);
    if (pointer == nullptr || pointer->op != kOpTypePointer) {
        return "it is not well-formed SPIR-V (the resource at " + where + " has no pointer type)";
    }
    const Id* type = at(pointer->b);
    if (type != nullptr && type->op == kOpTypeArray) {
        const Id* length = at(type->b);
        const std::uint32_t capacity = variable.binding == kSamplerBinding ? counts.samplers : counts.resources;
        if (length == nullptr || length->op != kOpConstant || length->a > capacity) {
            return "its array at " + where + " is longer than the bindless set's binding";
        }
    }
    if (type != nullptr && (type->op == kOpTypeArray || type->op == kOpTypeRuntimeArray)) {
        type = at(type->a);
    }
    const bool opaque = storageClass == kStorageClassUniformConstant && type != nullptr;
    bool matches = false;
    switch (variable.binding) {
        case kBufferBinding:
            matches = storageClass == kStorageClassStorageBuffer;
            break;
        case kSampledImageBinding:
            matches = opaque && type->op == kOpTypeImage && type->a == 1;
            break;
        case kStorageImageBinding:
            matches = opaque && type->op == kOpTypeImage && type->a == 2;
            break;
        case kSamplerBinding:
            matches = opaque && type->op == kOpTypeSampler;
            break;
        default:
            break;
    }
    if (!matches) {
        return "it binds a resource at " + where +
               " whose type the bindless set doesn't have there (binding 0 storage buffers, 1 sampled images, 2 "
               "storage images, 3 samplers; constants must be push constants)";
    }
    return std::nullopt;
}

} // namespace

bool isSpirv(std::span<const std::byte> code) {
    return code.size() >= kHeaderWords * 4 && code.size() % 4 == 0 && word(code, 0) == kMagic;
}

std::optional<std::string> bindingProblem(std::span<const std::byte> code, BindlessCounts counts) {
    const std::size_t words = code.size() / 4;
    const std::uint32_t bound = word(code, 3);
    // Every id is defined by an instruction of at least two words, so a larger bound is malformed;
    // the limit also keeps the table below within a few times the input's size.
    if (bound > words) {
        return "it is not well-formed SPIR-V (its id bound is larger than the module)";
    }
    std::vector<Id> ids(bound);
    for (std::size_t offset = kHeaderWords; offset < words;) {
        const std::uint32_t instruction = word(code, offset);
        const std::uint32_t length = instruction >> 16;
        if (length == 0 || length > words - offset) {
            return "it is not well-formed SPIR-V (an instruction runs past the end)";
        }
        read(ids, instruction & 0xFFFFu, Instruction{.code = code, .offset = offset, .length = length});
        offset += length;
    }
    for (const Id& variable : ids) {
        const bool resource = variable.op == kOpVariable &&
                              (variable.b == kStorageClassUniformConstant || variable.b == kStorageClassUniform ||
                               variable.b == kStorageClassStorageBuffer);
        if (resource) {
            if (std::optional<std::string> problem = variableProblem(ids, variable, counts)) {
                return problem;
            }
        }
    }
    return std::nullopt;
}

} // namespace ith::rhi::vulkan
