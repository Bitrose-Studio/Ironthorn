#include "ith/ecs/Commands.h"

#include "ith/ecs/World.h"
#include "ith/foundation/Assert.h"
#include "ith/reflection/TypeInfo.h"

#include <memory>

namespace ith {
namespace {

// Blocks are at least this large, so small values share one allocation.
constexpr std::size_t kBlockSize = std::size_t{16} * 1024;

} // namespace

Commands::~Commands() {
    discard();
}

Commands::Commands(Commands&& other) noexcept
    : m_world(other.m_world),
      m_commands(std::move(other.m_commands)),
      m_values(std::move(other.m_values)),
      m_blocks(std::move(other.m_blocks)),
      m_block(other.m_block) {
    other.m_commands.clear();
    other.m_values.clear();
    other.m_blocks.clear();
    other.m_block = 0;
}

void Commands::despawn(Entity entity) {
    m_commands.push_back({.kind = Kind::Despawn, .entity = entity});
}

Entity Commands::reserveEntity() {
    return m_world->reserveEntity();
}

void* Commands::allocate(std::size_t size, std::size_t alignment) {
    for (; m_block < m_blocks.size(); ++m_block) {
        Block& block = m_blocks[m_block];
        void* memory = block.memory.get() + block.used;
        std::size_t space = block.size - block.used;
        if (std::align(alignment, size, memory, space) != nullptr) {
            block.used = block.size - space + size;
            return memory;
        }
    }
    // A new block, large enough for the value at any alignment.
    const std::size_t blockSize = std::max(kBlockSize, size + alignment);
    Block& block = m_blocks.emplace_back(Block{
        .memory = std::make_unique_for_overwrite<std::byte[]>(blockSize), // NOLINT(modernize-avoid-c-arrays)
        .size = blockSize,
    });
    void* memory = block.memory.get();
    std::size_t space = block.size;
    std::align(alignment, size, memory, space);
    block.used = block.size - space + size;
    return memory;
}

void Commands::append(Commands& other) {
    ITH_ASSERT_MSG(other.m_world == m_world, "ecs: commands appended to a buffer of another world");
    ITH_ASSERT_MSG(&other != this, "ecs: a command buffer appended to itself");
    if (other.m_world != m_world || &other == this) {
        return;
    }
    const auto offset = static_cast<std::uint32_t>(m_values.size());
    for (const Value& value : other.m_values) {
        const TypeInfo& type = detail::componentType(value.type);
        void* memory = allocate(type.size, type.alignment);
        detail::relocateValue(type, memory, value.memory);
        m_values.push_back({value.type, memory});
    }
    for (Command command : other.m_commands) {
        command.firstValue += offset;
        m_commands.push_back(command);
    }
    other.reset();
}

void Commands::reset() noexcept {
    m_commands.clear();
    m_values.clear();
    for (Block& block : m_blocks) {
        block.used = 0;
    }
    m_block = 0;
}

void Commands::discard() noexcept {
    std::vector<Entity> reserved;
    for (const Command& command : m_commands) {
        if (command.kind == Kind::Spawn) {
            reserved.push_back(command.entity);
        }
    }
    if (!reserved.empty()) {
        m_world->abandonReserved(reserved);
    }
    for (const Value& value : m_values) {
        detail::componentType(value.type).destroy(value.memory);
    }
    reset();
}

} // namespace ith
