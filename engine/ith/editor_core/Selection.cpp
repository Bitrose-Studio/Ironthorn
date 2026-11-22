#include "ith/editor_core/Selection.h"

#include "ith/ecs/World.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace ith {

namespace {

auto sameAs(const Uuid& wanted) {
    return [&wanted](const Uuid& id) { return id == wanted; };
}

} // namespace

std::optional<Uuid> Selection::primary() const {
    if (m_entities.empty()) {
        return std::nullopt;
    }
    return m_entities.back();
}

bool Selection::contains(const Uuid& id) const noexcept {
    return std::ranges::any_of(m_entities, sameAs(id));
}

bool Selection::replace(std::span<const Uuid> ids) {
    std::vector<Uuid> unique;
    unique.reserve(ids.size());
    for (const Uuid& id : ids) {
        if (std::ranges::none_of(unique, sameAs(id))) {
            unique.push_back(id);
        }
    }
    if (unique == m_entities) {
        return false;
    }
    m_entities = std::move(unique);
    return true;
}

bool Selection::add(const Uuid& id) {
    if (!m_entities.empty() && m_entities.back() == id) {
        return false;
    }
    std::erase_if(m_entities, sameAs(id));
    m_entities.push_back(id);
    return true;
}

bool Selection::remove(const Uuid& id) {
    return std::erase_if(m_entities, sameAs(id)) != 0;
}

bool Selection::toggle(const Uuid& id) {
    if (!remove(id)) {
        m_entities.push_back(id);
    }
    return true;
}

bool Selection::clear() {
    if (m_entities.empty()) {
        return false;
    }
    m_entities.clear();
    return true;
}

bool Selection::prune(const World& world) {
    return std::erase_if(m_entities, [&world](const Uuid& id) { return world.findByStableId(id).isNull(); }) != 0;
}

} // namespace ith
