#pragma once

// A pool of fixed-size slots for objects of one type.
//
// Every slot has the same size, so creating and destroying objects is O(1) and never fragments
// memory. The pool grows by whole chunks and returns memory to the system only when it is
// destroyed. Use it for many objects of one type that are created and destroyed often.
//
//   ith::ObjectPool<Particle> particles;
//   ith::ObjectPool<Particle>::Ptr particle = particles.create(position, velocity);
//   ...
//   particle.reset(); // destroys the particle and returns its slot to the pool
//
// Ownership: create() returns a Ptr, a std::unique_ptr whose deleter destroys the object and
// returns its slot. The pool owns the memory and must outlive every Ptr it handed out;
// destroying it while objects are alive is a fatal error. The pool can't be copied or moved,
// because each Ptr refers to it. In AddressSanitizer builds, the bytes of a free slot are
// poisoned, so use after destruction is reported.
//
// Not thread-safe: create objects and destroy them (reset or destroy a Ptr) on one thread at a
// time.

#include "ith/foundation/Assert.h"
#include "ith/foundation/Compiler.h"

#include <cstddef>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace ith {

template <typename T>
class ObjectPool {
public:
    // Destroys an object and returns its slot to the pool it came from.
    class Deleter {
    public:
        Deleter() = default;

        // Precondition (asserted): a non-null `object` comes with the pool that created it,
        // i.e. the deleter was not default-constructed. Such an object is left alone.
        void operator()(T* object) const noexcept {
            ITH_ASSERT_MSG(object == nullptr || m_pool != nullptr, "ObjectPool: deleting an object without its pool");
            if (object != nullptr && m_pool != nullptr) {
                m_pool->destroy(object);
            }
        }

    private:
        friend class ObjectPool;

        explicit Deleter(ObjectPool* pool) : m_pool(pool) {}

        ObjectPool* m_pool = nullptr;
    };

    // An owning pointer to an object in the pool.
    using Ptr = std::unique_ptr<T, Deleter>;

    // `objectsPerChunk` is how many slots are obtained from the system at a time. Precondition
    // (asserted): it is at least 1; 1 is used otherwise. No memory is allocated until the first
    // create().
    explicit ObjectPool(std::size_t objectsPerChunk = 64) : m_objectsPerChunk(objectsPerChunk) {
        ITH_ASSERT_MSG(objectsPerChunk > 0, "ObjectPool: objectsPerChunk must be at least 1");
        if (m_objectsPerChunk == 0) {
            m_objectsPerChunk = 1;
        }
    }

    // Destroying a pool while any of its objects is alive is fatal (ITH_FATAL): their Ptrs
    // would dangle.
    ~ObjectPool() {
        if (m_liveCount != 0) {
            ITH_FATAL("ObjectPool destroyed while objects are still alive");
        }
        // Hand the memory back to the system allocator in the state it gave it out.
        for (std::vector<Slot>& chunk : m_chunks) {
            ITH_ASAN_UNPOISON(chunk.data(), chunk.size() * sizeof(Slot));
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;
    ObjectPool(ObjectPool&&) = delete;
    ObjectPool& operator=(ObjectPool&&) = delete;

    // Constructs a T from `args` in a free slot and returns it; the Ptr is never empty. When no
    // slot is free the pool grows by one chunk. Running out of system memory ends the process,
    // as for any allocation in a build without exceptions.
    template <typename... Args>
    [[nodiscard]] Ptr create(Args&&... args) {
        if (m_freeList == nullptr) {
            grow();
        }
        Slot* slot = m_freeList;
        m_freeList = slot->next;
        ITH_ASAN_UNPOISON(slot, sizeof(Slot));
        T* object = ::new (static_cast<void*>(std::addressof(slot->object))) T(std::forward<Args>(args)...);
        ++m_liveCount;
        return Ptr(object, Deleter(this));
    }

    // The number of objects alive.
    [[nodiscard]] std::size_t liveCount() const { return m_liveCount; }

    // The number of objects the pool can hold without growing.
    [[nodiscard]] std::size_t capacity() const { return m_chunks.size() * m_objectsPerChunk; }

private:
#ifdef _MSC_VER
    // MSVC warns (C4324) that a slot is padded whenever T is over-aligned; honouring that
    // alignment is the point of the slot.
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
    // A slot holds either a live T or, while free, the link to the next free slot. Its
    // constructor and destructor leave `object` alone: the pool constructs and destroys it
    // explicitly.
    union Slot {
        Slot() : next(nullptr) {}

        ~Slot() {}

        Slot(const Slot&) = delete;
        Slot& operator=(const Slot&) = delete;
        Slot(Slot&&) = delete;
        Slot& operator=(Slot&&) = delete;

        Slot* next;
        T object;
    };
#ifdef _MSC_VER
#pragma warning(pop)
#endif

    // Links a free slot into the free list, then poisons everything but the link.
    void release(Slot* slot) {
        slot->next = m_freeList;
        m_freeList = slot;
        ITH_ASAN_POISON(reinterpret_cast<std::byte*>(slot) + sizeof(Slot*), sizeof(Slot) - sizeof(Slot*));
    }

    void destroy(T* object) {
        // Every member of a union starts at its address, so the object's address is the slot's.
        auto* slot = reinterpret_cast<Slot*>(object);
        object->~T();
        release(slot);
        --m_liveCount;
    }

    void grow() {
        std::vector<Slot> chunk(m_objectsPerChunk);
        // Release the slots in reverse, so they are handed out in address order.
        for (std::size_t index = m_objectsPerChunk; index-- > 0;) {
            release(&chunk[index]);
        }
        // Moving the vector keeps its buffer, so the free-list links stay valid.
        m_chunks.push_back(std::move(chunk));
    }

    std::size_t m_objectsPerChunk;
    std::size_t m_liveCount = 0;
    Slot* m_freeList = nullptr;
    std::vector<std::vector<Slot>> m_chunks;
};

} // namespace ith
