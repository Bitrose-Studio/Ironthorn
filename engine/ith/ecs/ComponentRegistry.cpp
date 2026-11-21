#include "ith/ecs/ComponentRegistry.h"

#include "ith/foundation/Assert.h"
#include "ith/foundation/Mutex.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <unordered_map>

namespace ith::detail {
namespace {

struct Registry {
    Mutex mutex;
    std::unordered_map<std::uint64_t, ComponentIndex> indexById ITH_GUARDED_BY(mutex);
    ComponentIndex count ITH_GUARDED_BY(mutex) = 0;
    // Written once per index under the mutex, before the index is handed out; read without it.
    std::array<std::atomic<const TypeInfo*>, kMaxComponentTypes> types{};
};

Registry& registry() {
    static Registry instance;
    return instance;
}

} // namespace

ComponentIndex registerComponentType(const TypeInfo& type) {
    const bool isComponent = type.kind == TypeKind::Struct && type.role == TypeRole::Component;
    ITH_ASSERT_MSG(isComponent, "ecs: a component type must be a reflected struct with ITH_TYPE(Component)");
    if (!isComponent) {
        return kNoComponent;
    }
    Registry& table = registry();
    const MutexLock lock(table.mutex);
    if (const auto found = table.indexById.find(type.id); found != table.indexById.end()) {
        const TypeInfo& known = *table.types[found->second].load(std::memory_order_relaxed);
        if (known.name != type.name || known.size != type.size || known.alignment != type.alignment) {
            ITH_FATAL("ecs: two component types with the same id but a different name or layout");
        }
        return found->second;
    }
    if (table.count == kMaxComponentTypes) {
        ITH_FATAL("ecs: too many component types; raise ith::detail::kMaxComponentTypes");
    }
    const ComponentIndex index = table.count++;
    table.types[index].store(&type, std::memory_order_release);
    table.indexById.emplace(type.id, index);
    return index;
}

const TypeInfo& componentType(ComponentIndex index) noexcept {
    const TypeInfo* type =
        index < kMaxComponentTypes ? registry().types[index].load(std::memory_order_acquire) : nullptr;
    if (type == nullptr) {
        ITH_FATAL("ecs: unknown component index");
    }
    return *type;
}

} // namespace ith::detail
