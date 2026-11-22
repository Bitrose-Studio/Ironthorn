#pragma once

// ith::EditorSession: one editor's document and commands, without any UI (ADR-0037 rules 1 and 18).
//
//   ith::TypeRegistry types;
//   ith::generated::registerReflection_ecs(types);
//   ith::generated::registerReflection_editor_core(types);
//   ith::EditorSession session(types);
//   session.commands().execute(session, "ith.entity.create", args);
//
// The session registers the built-in commands of editor_core (EditCommands.cpp); editor_ui and
// game editor modules add theirs to the same registry. `types` names the component types that
// commands and level files may use and must outlive the session. Headless: tests and automation
// use it directly. Main thread only.

#include "ith/editor_core/CommandRegistry.h"
#include "ith/editor_core/EditorDocument.h"
#include "ith/serialization/Document.h"

namespace ith {

class TypeRegistry;

class EditorSession {
public:
    explicit EditorSession(const TypeRegistry& types);

    EditorSession(const EditorSession&) = delete;
    EditorSession& operator=(const EditorSession&) = delete;
    EditorSession(EditorSession&&) = delete;
    EditorSession& operator=(EditorSession&&) = delete;
    ~EditorSession() = default;

    [[nodiscard]] EditorDocument& document() noexcept { return m_document; }

    [[nodiscard]] const EditorDocument& document() const noexcept { return m_document; }

    [[nodiscard]] CommandRegistry& commands() noexcept { return m_commands; }

    [[nodiscard]] const TypeRegistry& types() const noexcept { return *m_types; }

    // Runs a command: commands().execute(*this, id, args).
    Expected<serialization::Value> execute(std::string_view id, const serialization::Value& args = {});

    // The entity mapping between the editor world and JSON: an entity is written as its stable id
    // (the nil id when it has none), and an id reads as the live entity that has it (the null
    // handle when none does).
    [[nodiscard]] const serialization::EntityMapping& entityMapping() const noexcept { return m_mapping; }

private:
    const TypeRegistry* m_types;
    EditorDocument m_document;
    CommandRegistry m_commands;
    serialization::EntityMapping m_mapping;
};

// Registers the Edit, Entity and Selection commands (EditCommands.cpp).
void registerEditCommands(CommandRegistry& registry);

} // namespace ith
