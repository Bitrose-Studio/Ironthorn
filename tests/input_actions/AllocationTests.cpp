// Steady-state evaluation allocates no memory; only a mapping table rebuild may
// (docs/specs/input-actions.md, requirement 48). This file replaces the global allocation functions
// of the test executable with counting ones; they count only while a test asks them to.

#include "ith/input_actions/InputActions.h"

#include "ActionTestSupport.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void* allocate(std::size_t size) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    void* memory = std::malloc(size == 0 ? 1 : size);
    if (memory == nullptr) {
        std::abort(); // the engine is built without exceptions
    }
    return memory;
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) {
    if (g_counting.load(std::memory_order_relaxed)) {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    void* memory = std::aligned_alloc(align, ((size + align - 1) / align) * align);
#endif
    if (memory == nullptr) {
        std::abort();
    }
    return memory;
}

void freeAligned(void* memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

// Counts the allocations made while it lives.
class AllocationCounter {
public:
    AllocationCounter() {
        g_allocations.store(0);
        g_counting.store(true);
    }

    ~AllocationCounter() { g_counting.store(false); }

    AllocationCounter(const AllocationCounter&) = delete;
    AllocationCounter& operator=(const AllocationCounter&) = delete;
    AllocationCounter(AllocationCounter&&) = delete;
    AllocationCounter& operator=(AllocationCounter&&) = delete;

    [[nodiscard]] std::size_t count() const { return g_allocations.load(); }
};

} // namespace

// The replaceable global allocation functions ([new.delete]), every form, so no allocation of this
// executable reaches the default ones and every pair matches.
void* operator new(std::size_t size) {
    return allocate(size);
}

void* operator new[](std::size_t size) {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocateAligned(size, alignment);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t /*size*/) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete(void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::size_t /*size*/, std::align_val_t /*alignment*/) noexcept {
    freeAligned(memory);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& /*tag*/) noexcept {
    return allocate(size);
}

void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t& /*tag*/) noexcept {
    return allocateAligned(size, alignment);
}

void operator delete(void* memory, const std::nothrow_t& /*tag*/) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, const std::nothrow_t& /*tag*/) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(memory);
}

void operator delete[](void* memory, std::align_val_t /*alignment*/, const std::nothrow_t& /*tag*/) noexcept {
    freeAligned(memory);
}

using namespace ith;
using namespace ith::test;

TEST_SUITE("input_actions.performance") {
    TEST_CASE("steady-state evaluation allocates nothing; a table rebuild may (requirement 48)") {
        Rig rig;
        rig.library.setSettings({InputSettingDecl{.name = "look", .defaultFloat = 2.0f}});
        const AssetId move = rig.addAction(ActionValueKind::Axis2);
        const AssetId look = rig.addAction(
            InputActionAsset{.valueKind = ActionValueKind::Axis2,
                             .combine = ActionCombine::Sum,
                             .modifiers = {InputModifier{.type = InputModifierType::UserScale,
                                                         .userScale = UserScaleParams{.setting = "look"}}}});
        const AssetId ctrl = rig.addAction();
        const AssetId save = rig.addAction();
        const AssetId jump = rig.addAction();
        const AssetId combo = rig.addAction();
        const InputModifier swizzle = modifier(InputModifierType::Swizzle);
        const AssetId onFoot = rig.addContext({
            mapping(move, "Keyboard.W", {}, {swizzle}),
            mapping(move, "Keyboard.S", {}, {swizzle, modifier(InputModifierType::Negate)}),
            mapping(move, "Gamepad.LeftStick", {}, {modifier(InputModifierType::DeadZone)}),
            mapping(look, "Mouse.Delta"),
            mapping(ctrl, "Keyboard.LeftControl"),
            mapping(
                save, "Keyboard.S",
                {InputTrigger{.type = InputTriggerType::Chord, .chord = ChordParams{.action = AssetRef::to(ctrl)}}}),
            mapping(jump, "Keyboard.Space",
                    {trigger(InputTriggerType::Tap), trigger(InputTriggerType::Hold), trigger(InputTriggerType::Pulse),
                     trigger(InputTriggerType::MultiTap)}),
            mapping(combo, "Keyboard.E",
                    {InputTrigger{.type = InputTriggerType::Combo,
                                  .combo = ComboParams{.steps = {{.action = AssetRef::to(jump)},
                                                                 {.action = AssetRef::to(save)}}}}}),
        });
        const AssetId overlay = rig.addContext({mapping(jump, "Keyboard.W", {trigger(InputTriggerType::Pressed)})});
        const Entity player = rig.addPlayer({{onFoot, 0}, {overlay, 1}});

        const auto inputFor = [](int frame, double at) {
            std::vector<Event> events{mouseMove(1.0f, -2.0f, at - 3),
                                      padAxis(kPad, GamepadAxis::LeftStickX, (frame % 10) * 0.1f, at - 2)};
            const Key keys[] = {Key::W, Key::S, Key::Space, Key::LeftControl, Key::E};
            const Key key = keys[frame % 5];
            events.push_back((frame / 5) % 2 == 0 ? keyDown(key, at - 1) : keyUp(key, at - 1));
            if (frame % 7 == 0) {
                // A tap within the frame, which waits in the edge queue.
                events.push_back(keyDown(Key::Q, at - 1));
                events.push_back(keyUp(Key::Q, at - 0.5));
            }
            return events;
        };
        rig.frame(5, {padConnected(kPad, 1)});
        for (int frame = 0; frame < 200; ++frame) { // warm-up: buffers reach their sizes
            rig.frame(10 + (frame * 16.0), inputFor(frame, 10 + (frame * 16.0)));
        }
        std::size_t allocations = 0;
        std::size_t events = 0;
        for (int frame = 200; frame < 400; ++frame) {
            const double at = 10 + (frame * 16.0);
            const std::vector<Event> input = inputFor(frame, at);
            rig.input.processEvents(input, ms(at));
            const AllocationCounter counter;
            rig.actions.update(rig.world);
            allocations += counter.count();
            events += rig.actions.events().size();
        }
        CHECK(events > 0);
        CHECK(allocations == 0);
        CHECK(rig.states(player).actions.size() == 6);

        // A change of the stack rebuilds the table, which may allocate.
        rig.removeContext(player, overlay);
        rig.input.processEvents({}, ms(10'000));
        const AllocationCounter counter;
        rig.actions.update(rig.world);
        CHECK(counter.count() > 0);
    }

    TEST_CASE("a key that overflows its edge queue every frame allocates nothing after its first warning") {
        Rig rig;
        const AssetId jump = rig.addAction();
        rig.addPlayer({{rig.addContext({mapping(jump, "Keyboard.Space")}), 0}});
        const auto flood = [](double at) {
            std::vector<Event> events;
            for (int i = 0; i < 6; ++i) {
                events.push_back(keyDown(Key::Space, at + i));
                events.push_back(keyUp(Key::Space, at + i + 0.5));
            }
            return events;
        };
        for (int frame = 0; frame < 20; ++frame) { // warm-up, including the one warning
            rig.frame(100 + (frame * 16.0) + 10, flood(100 + (frame * 16.0)));
        }
        std::size_t allocations = 0;
        for (int frame = 20; frame < 60; ++frame) {
            const double at = 100 + (frame * 16.0);
            const std::vector<Event> input = flood(at);
            rig.input.processEvents(input, ms(at + 10));
            const AllocationCounter counter;
            rig.actions.update(rig.world);
            allocations += counter.count();
        }
        CHECK(allocations == 0);
    }
}
