#include "ith/editor_core/CommandRegistry.h"

#include "ith/editor_core/EditorSession.h"
#include "ith/foundation/Assert.h"

#include <utility>

namespace ith {

void CommandRegistry::add(CommandDesc command) {
    const bool valid = !command.id.empty() && command.execute && !m_commands.contains(command.id);
    ITH_ASSERT_MSG(valid, "editor: a command needs a new, non-empty id and an execute function");
    if (!valid) {
        return;
    }
    std::string id = command.id;
    m_commands.emplace(std::move(id), std::move(command));
}

const CommandDesc* CommandRegistry::find(std::string_view id) const {
    const auto found = m_commands.find(id);
    return found != m_commands.end() ? &found->second : nullptr;
}

std::vector<const CommandDesc*> CommandRegistry::commands() const {
    std::vector<const CommandDesc*> result;
    result.reserve(m_commands.size());
    for (const auto& [id, command] : m_commands) {
        result.push_back(&command);
    }
    return result;
}

Expected<void> CommandRegistry::check(EditorSession& session, const CommandDesc& command) const {
    EditorDocument& document = session.document();
    if (command.kind != CommandKind::Query && m_depth == 0 && document.transactions().isOpen()) {
        return makeError("editor.command.interaction_open",
                         command.id + ": an interaction is in progress; finish or cancel it first");
    }
    if (command.kind != CommandKind::Query && document.opening()) {
        return makeError("editor.document.busy", command.id + ": a level is being opened");
    }
    if (command.kind == CommandKind::Edit && document.playing()) {
        return makeError("editor.command.playing", command.id + ": the level can't change while playing");
    }
    if (command.canExecute) {
        if (Expected<void> allowed = command.canExecute(session); !allowed) {
            return makeError("editor.command.disabled", command.id + ": " + allowed.error().message);
        }
    }
    return {};
}

Expected<void> CommandRegistry::canExecute(EditorSession& session, std::string_view id) const {
    const CommandDesc* command = find(id);
    if (command == nullptr) {
        return makeError("editor.command.unknown", "no command " + std::string(id));
    }
    return check(session, *command);
}

Expected<serialization::Value> CommandRegistry::execute(EditorSession& session, std::string_view id,
                                                        const serialization::Value& args) {
    const CommandDesc* command = find(id);
    if (command == nullptr) {
        return makeError("editor.command.unknown", "no command " + std::string(id));
    }
    if (Expected<void> allowed = check(session, *command); !allowed) {
        return makeUnexpected(std::move(allowed).error());
    }
    if (args.kind() != serialization::Value::Kind::Object && args.kind() != serialization::Value::Kind::Null) {
        return makeError("editor.command.invalid_args", command->id + ": the arguments must be an object");
    }
    ++m_depth;
    if (command->kind != CommandKind::Edit) {
        Expected<serialization::Value> result = command->execute(session, args);
        --m_depth;
        return result;
    }
    TransactionScope scope = session.document().transactions().begin(command->displayName);
    Expected<serialization::Value> result = command->execute(session, args);
    if (result) {
        scope.commit();
    } else {
        scope.cancel();
    }
    --m_depth;
    return result;
}

void CommandRegistry::setPrompt(std::string_view id, std::function<bool(EditorSession&)> prompt) {
    const bool known = find(id) != nullptr;
    ITH_ASSERT_MSG(known, "editor: a prompt for a command that isn't registered");
    if (!known) {
        return;
    }
    if (prompt) {
        m_prompts.insert_or_assign(std::string(id), std::move(prompt));
    } else if (const auto found = m_prompts.find(id); found != m_prompts.end()) {
        m_prompts.erase(found);
    }
}

Expected<serialization::Value> CommandRegistry::invoke(EditorSession& session, std::string_view id) {
    if (const auto prompt = m_prompts.find(id); prompt != m_prompts.end()) {
        if (Expected<void> allowed = canExecute(session, id); !allowed) {
            return makeUnexpected(std::move(allowed).error());
        }
        // A copy: the prompt may change the prompts.
        const std::function<bool(EditorSession&)> start = prompt->second;
        if (start(session)) {
            return serialization::Value();
        }
    }
    return execute(session, id);
}

} // namespace ith
