#include "ith/reflection/OwnedValue.h"

#include "ith/foundation/Assert.h"

#include <new>

namespace ith {
namespace {

void* allocate(const TypeInfo& type) {
    return ::operator new(type.size, std::align_val_t(type.alignment));
}

} // namespace

void OwnedValue::Deleter::operator()(void* memory) const noexcept {
    type->destroy(memory);
    ::operator delete(memory, std::align_val_t(type->alignment));
}

OwnedValue::OwnedValue(const TypeInfo& type) {
    void* memory = allocate(type);
    type.construct(memory);
    m_memory = std::unique_ptr<void, Deleter>(memory, Deleter{&type});
}

OwnedValue OwnedValue::copyOf(const TypeInfo& type, const void* source) {
    OwnedValue result;
    void* memory = allocate(type);
    type.copy(memory, source);
    result.m_memory = std::unique_ptr<void, Deleter>(memory, Deleter{&type});
    return result;
}

OwnedValue::OwnedValue(const OwnedValue& other) {
    if (other.hasValue()) {
        *this = copyOf(*other.type(), other.get());
    }
}

OwnedValue& OwnedValue::operator=(const OwnedValue& other) {
    if (this != &other) {
        *this = other.hasValue() ? copyOf(*other.type(), other.get()) : OwnedValue();
    }
    return *this;
}

void OwnedValue::moveInto(void* target) {
    const bool held = hasValue();
    ITH_ASSERT_MSG(held, "OwnedValue::moveInto: no value is held");
    if (held) {
        type()->destroy(target);
        type()->move(target, get());
    }
}

} // namespace ith
