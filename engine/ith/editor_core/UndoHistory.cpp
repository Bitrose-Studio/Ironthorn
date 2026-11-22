#include "ith/editor_core/UndoHistory.h"

#include "ith/reflection/OwnedValue.h"
#include "ith/reflection/Patch.h"
#include "ith/reflection/TypeInfo.h"

#include <string>
#include <utility>

namespace ith {

namespace {

// The heap memory a value of `type` at `value` owns, beyond its own size.
std::size_t ownedBytes(const TypeInfo& type, const void* value) {
    switch (type.kind) {
        case TypeKind::String:
            return static_cast<const std::string*>(value)->capacity();
        case TypeKind::Struct: {
            std::size_t total = 0;
            for (const FieldInfo& field : type.fields) {
                total += ownedBytes(field.type(), static_cast<const std::byte*>(value) + field.offset);
            }
            return total;
        }
        case TypeKind::FixedArray:
        case TypeKind::DynamicArray:
        case TypeKind::Optional: {
            const TypeInfo& element = type.element();
            const std::size_t count = type.container->size(value);
            std::size_t total = type.kind == TypeKind::DynamicArray ? count * element.size : 0;
            for (std::size_t index = 0; index < count; ++index) {
                total += ownedBytes(element, type.container->constAt(value, index));
            }
            return total;
        }
        default:
            return 0;
    }
}

std::size_t valueBytes(const OwnedValue& value) {
    return value.hasValue() ? value.type()->size + ownedBytes(*value.type(), value.get()) : 0;
}

} // namespace

std::size_t estimateBytes(const UndoEntry& entry) {
    std::size_t total = sizeof(UndoEntry) + entry.name.capacity() + entry.ids.size() * 2 * sizeof(Uuid) +
                        (entry.selectionBefore.size() + entry.selectionAfter.size()) * sizeof(Uuid);
    for (const EntityPatch& patch : entry.patches) {
        total += sizeof(EntityPatch);
        for (const ComponentChange& change : patch.changes) {
            total += sizeof(ComponentChange) + valueBytes(change.value);
            for (const PatchEntry& patchEntry : change.patch.entries()) {
                total += sizeof(PatchEntry) + valueBytes(patchEntry.value);
            }
        }
    }
    return total;
}

void UndoHistory::setLimits(const UndoLimits& limits) {
    m_limits = limits;
    enforceLimits();
}

void UndoHistory::push(UndoEntry entry) {
    while (m_entries.size() > m_cursor) {
        m_bytes -= m_entries.back().bytes;
        m_entries.pop_back();
    }
    entry.bytes = estimateBytes(entry);
    entry.serial = m_nextSerial++;
    m_bytes += entry.bytes;
    m_entries.push_back(std::move(entry));
    m_cursor = m_entries.size();
    enforceLimits();
}

void UndoHistory::stepBack() {
    if (m_cursor > 0) {
        --m_cursor;
        refresh(m_entries[m_cursor]);
    }
}

void UndoHistory::stepForward() {
    if (m_cursor < m_entries.size()) {
        refresh(m_entries[m_cursor]);
        ++m_cursor;
    }
}

void UndoHistory::refresh(UndoEntry& entry) {
    m_bytes -= entry.bytes;
    entry.bytes = estimateBytes(entry);
    m_bytes += entry.bytes;
    enforceLimits();
}

void UndoHistory::clear() {
    if (m_entries.empty()) {
        return;
    }
    // The current position stays the current position; the entries can't bring others back.
    m_base = position();
    m_entries.clear();
    m_cursor = 0;
    m_bytes = 0;
}

void UndoHistory::reset() {
    m_entries.clear();
    m_cursor = 0;
    m_bytes = 0;
    // A position no entry has had, so no later state matches it by accident.
    m_base = m_nextSerial++;
    m_saved = m_base;
}

std::uint64_t UndoHistory::position() const noexcept {
    return m_cursor > 0 ? m_entries[m_cursor - 1].serial : m_base;
}

std::uint64_t UndoHistory::markSaved() noexcept {
    return std::exchange(m_saved, position());
}

void UndoHistory::enforceLimits() {
    if (m_limits.maxEntries == 0) {
        clear();
        return;
    }
    while (m_entries.size() > 1 && (m_entries.size() > m_limits.maxEntries || m_bytes > m_limits.maxBytes)) {
        if (m_cursor > 1) {
            // The oldest undoable entry, which isn't the last undoable one: the state before it
            // can't be reached again.
            m_bytes -= m_entries.front().bytes;
            m_base = m_entries.front().serial;
            m_entries.pop_front();
            --m_cursor;
        } else {
            // The furthest redoable entry (with at most one undoable entry, there are redoable ones).
            m_bytes -= m_entries.back().bytes;
            m_entries.pop_back();
        }
    }
}

} // namespace ith
