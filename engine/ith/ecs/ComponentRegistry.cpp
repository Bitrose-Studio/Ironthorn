#include "ith/ecs/ComponentRegistry.h"

#include "ith/ecs/Hooks.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Mutex.h"

#include <array>
#include <atomic>
#include <bitset>
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
    // Hooks by index. A node-based map, so a value keeps its address: archetypes keep pointers to
    // it, and it never changes once set.
    std::unordered_map<ComponentIndex, ComponentHooks> hooks ITH_GUARDED_BY(mutex);
    // The indices a World has stored; their hooks can't be set any more.
    std::bitset<kMaxComponentTypes> used ITH_GUARDED_BY(mutex);
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
        if (known.name != type.name || known.size != type.size || known.alignment != type.alignment ||
            known.flags != type.flags) {
            ITH_FATAL("ecs: two component types with the same id but a different name, layout or flags");
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

bool setComponentHooks(ComponentIndex index, const ComponentHooks& hooks) {
    Registry& table = registry();
    const MutexLock lock(table.mutex);
    ITH_ASSERT(index < table.count);
    const bool set = table.hooks.contains(index);
    const bool used = table.used.test(index);
    ITH_ASSERT_MSG(!set, "ecs: component hooks set twice for one type");
    ITH_ASSERT_MSG(set || !used, "ecs: component hooks set after a World stored the type; set them at startup");
    if (used || set) {
        return false;
    }
    table.hooks.emplace(index, hooks);
    return true;
}

const ComponentHooks* useComponentType(ComponentIndex index) {
    Registry& table = registry();
    const MutexLock lock(table.mutex);
    table.used.set(index);
    const auto found = table.hooks.find(index);
    return found != table.hooks.end() ? &found->second : nullptr;
}

} // namespace ith::detail

namespace ith {

bool setComponentHooks(const TypeInfo& type, const ComponentHooks& hooks) {
    const detail::ComponentIndex index = detail::registerComponentType(type);
    return index != detail::kNoComponent && detail::setComponentHooks(index, hooks);
}

} // namespace ith
