// The Play In Editor commands and query (ADR-0053 decision 6, docs/specs/play-in-editor.md 3.14).
// All are Actions: they record no undo entry, and the registry refuses them while a level opens. They
// run while an interaction is open (CommandDesc::runsDuringInteraction): Stop drops a game-world
// drag, Pause and Step keep the game going during one, and Play and Simulate refuse in the spec's
// order through EditorSession::canStartPlay ("editor.play.running", "editor.document.busy",
// "editor.command.interaction_open"). The other play commands refuse with
// "editor.play.not_running" without a session, and Step with "editor.play.not_paused" on a running
// one. Play, Simulate and Stop refuse to run from inside another command ("editor.command.nested"),
// whose transaction scope could point into the game world they create or destroy. The registry
// keeps these codes.
//
// Headless sessions have no frame loop: automation starts them paused ("paused": true) and advances
// them with ith.play.step, which runs exact ticks. A session plays in the selected viewport; Possess
// and Eject (EditorSession::setPossessed) move its camera between the spectator and the editor
// controls and turn capture on click on and off, and Free the Mouse releases a captured mouse. Without
// a bound Input (headless) nothing is ever captured.

#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/PlaySession.h"
#include "ith/editor_core/Viewports.h"
#include "ith/gameplay/Time.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ith {

namespace {

using detail::invalidArgs;
using serialization::Value;

// The default length of one ith.play.step tick, in seconds.
constexpr double kDefaultStepSeconds = 1.0 / 60.0;

// The boolean member `key` of `args`: nullopt when missing or null.
Expected<std::optional<bool>> optionalBool(std::string_view command, const Value& args, std::string_view key) {
    const Value* value = args.kind() == Value::Kind::Object ? args.find(key) : nullptr;
    if (value == nullptr || value->kind() == Value::Kind::Null) {
        return std::optional<bool>();
    }
    if (value->kind() != Value::Kind::Bool) {
        return invalidArgs(command, "\"" + std::string(key) + "\" must be true or false");
    }
    return std::optional<bool>(value->asBool());
}

// Play, Simulate and Stop create or destroy the game world and its transactions, which an outer
// command's scope may point at: they run only as commands of their own, never from inside another.
Expected<void> notNested(EditorSession& session) {
    if (session.commands().depth() > 0) {
        return makeError("editor.command.nested", "Play, Simulate and Stop can't run from inside another command");
    }
    return {};
}

Expected<void> canStart(EditorSession& session) {
    if (Expected<void> alone = notNested(session); !alone) {
        return alone;
    }
    return session.canStartPlay();
}

Expected<void> running(EditorSession& session) {
    if (session.play() == nullptr) {
        return makeError("editor.play.not_running", "no play session is running");
    }
    return {};
}

Expected<void> canStop(EditorSession& session) {
    if (Expected<void> alone = notNested(session); !alone) {
        return alone;
    }
    return running(session);
}

Expected<void> pausedSession(EditorSession& session) {
    if (Expected<void> exists = running(session); !exists) {
        return exists;
    }
    if (!session.play()->paused()) {
        return makeError("editor.play.not_paused", "step runs only while the session is paused");
    }
    return {};
}

Value stateOf(const PlaySession& play) {
    Value result = Value::object();
    result.set("state", Value::string("running"));
    result.set("possessed", Value::boolean(play.possessed()));
    result.set("paused", Value::boolean(play.paused()));
    result.set("viewport", Value::string(Viewports::instanceId(play.viewport())));
    return result;
}

Expected<Value> start(EditorSession& session, const Value& args, PlayMode mode, std::string_view command) {
    Expected<std::optional<bool>> paused = optionalBool(command, args, "paused");
    if (!paused) {
        return makeUnexpected(std::move(paused).error());
    }
    if (Expected<void> started = session.startPlay(mode, paused->value_or(false)); !started) {
        return makeUnexpected(std::move(started).error());
    }
    return stateOf(*session.play());
}

Expected<Value> stop(EditorSession& session, const Value& /*args*/) {
    session.stopPlay();
    Value result = Value::object();
    result.set("state", Value::string("stopped"));
    return result;
}

Expected<Value> pause(EditorSession& session, const Value& args) {
    Expected<std::optional<bool>> paused = optionalBool("ith.play.pause", args, "paused");
    if (!paused) {
        return makeUnexpected(std::move(paused).error());
    }
    PlaySession& play = *session.play();
    play.setPaused(paused->value_or(!play.paused()));
    Value result = Value::object();
    result.set("paused", Value::boolean(play.paused()));
    return result;
}

Expected<Value> step(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.play.step";
    std::uint32_t frames = 1;
    double seconds = kDefaultStepSeconds;
    if (const Value* given = args.kind() == Value::Kind::Object ? args.find("frames") : nullptr;
        given != nullptr && given->kind() != Value::Kind::Null) {
        const std::optional<std::uint32_t> number = given->toNumber<std::uint32_t>();
        if (!number || *number < 1 || *number > kMaxStepFrames) {
            return invalidArgs(kCommand, "\"frames\" must be an integer from 1 to " + std::to_string(kMaxStepFrames));
        }
        frames = *number;
    }
    if (const Value* given = args.kind() == Value::Kind::Object ? args.find("deltaSeconds") : nullptr;
        given != nullptr && given->kind() != Value::Kind::Null) {
        const std::optional<double> number = given->toNumber<double>();
        if (!number || !(*number > 0.0) || *number > kMaxTickDelta) {
            return invalidArgs(kCommand, "\"deltaSeconds\" must be greater than 0 and at most 0.1");
        }
        seconds = *number;
    }
    PlaySession& play = *session.play();
    play.step(frames, seconds);
    const Time* time = play.time();
    Value result = Value::object();
    result.set("frame", Value::unsignedInteger(time != nullptr ? time->frame : 0));
    result.set("elapsed", Value::real(time != nullptr ? time->elapsed : 0.0));
    return result;
}

Expected<Value> possessOrEject(EditorSession& session, const Value& args) {
    Expected<std::optional<bool>> possess = optionalBool("ith.play.possess_or_eject", args, "possess");
    if (!possess) {
        return makeUnexpected(std::move(possess).error());
    }
    session.setPossessed(possess->value_or(!session.play()->possessed()));
    Value result = Value::object();
    result.set("possessed", Value::boolean(session.play()->possessed()));
    return result;
}

Expected<Value> freeMouse(EditorSession& session, const Value& /*args*/) {
    session.freeMouse();
    Value result = Value::object();
    result.set("captured", Value::boolean(false));
    return result;
}

Expected<Value> playQuery(EditorSession& session, const Value& /*args*/) {
    PlaySession* play = session.play();
    Value result = play != nullptr ? stateOf(*play) : Value::object();
    if (play == nullptr) {
        result.set("state", Value::string("stopped"));
        result.set("possessed", Value::boolean(false));
        result.set("paused", Value::boolean(false));
        result.set("viewport", Value());
    }
    const Time* time = play != nullptr ? play->time() : nullptr;
    result.set("captured", Value::boolean(session.mouseCaptured()));
    result.set("frame", Value::unsignedInteger(time != nullptr ? time->frame : 0));
    result.set("elapsed", Value::real(time != nullptr ? time->elapsed : 0.0));
    result.set("entities", Value::unsignedInteger(play != nullptr ? play->world().entityCount() : 0));
    result.set("spawned", Value::unsignedInteger(play != nullptr ? play->spawnedCount() : 0));
    return result;
}

void addPlay(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
             Expected<void> (*canExecute)(EditorSession&), Expected<Value> (*execute)(EditorSession&, const Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Play",
                  .kind = CommandKind::Action,
                  .runsDuringInteraction = true,
                  .canExecute = canExecute,
                  .execute = execute});
}

} // namespace

void registerPlayCommands(CommandRegistry& registry) {
    addPlay(registry, "ith.play.play", "Play", "Play the level in the viewport, as a player", &canStart,
            [](EditorSession& session, const Value& args) {
                return start(session, args, PlayMode::Play, "ith.play.play");
            });
    addPlay(registry, "ith.play.simulate", "Simulate", "Run the level without a player, watched by the editor camera",
            &canStart, [](EditorSession& session, const Value& args) {
                return start(session, args, PlayMode::Simulate, "ith.play.simulate");
            });
    addPlay(registry, "ith.play.stop", "Stop", "Stop playing; the level is as it was before Play", &canStop, &stop);
    addPlay(registry, "ith.play.pause", "Pause", "Pause or resume the game", &running, &pause);
    addPlay(registry, "ith.play.step", "Step", "Advance the paused game by frames of a given length", &pausedSession,
            &step);
    addPlay(registry, "ith.play.possess_or_eject", "Possess or Eject",
            "Give the game the view and input, or take them back for the editor", &running, &possessOrEject);
    addPlay(registry, "ith.play.free_mouse", "Free the Mouse", "Give the mouse back to the editor", &running,
            &freeMouse);
    registry.add({.id = "ith.query.play",
                  .displayName = "Play State",
                  .tooltip = "The play session's state, time and entities",
                  .category = "Query",
                  .kind = CommandKind::Query,
                  .execute = &playQuery});
}

} // namespace ith
