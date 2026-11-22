#include "ith/editor_core/Automation.h"

#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/serialization/Json.h"
#include "ith/serialization/TextArchive.h"

#include <optional>
#include <utility>

namespace ith {

namespace {

using serialization::Value;

Expected<Value> readScript(std::string_view text, std::string_view source) {
    Expected<Value, serialization::Error> data =
        serialization::readDocument(text, kAutomationScriptFormat, {.source = source, .maxSize = kMaxScriptFileSize});
    if (!data) {
        return makeError(data.error().code, data.error().toString());
    }
    const Value* steps = data->kind() == Value::Kind::Object ? data->find("steps") : nullptr;
    if (steps == nullptr || steps->kind() != Value::Kind::Array) {
        return makeError("automation.invalid_script", std::string(source) + R"(: "data" must hold an array "steps")");
    }
    return *steps;
}

std::string describe(const Value& value) {
    Expected<std::string, serialization::Error> text = serialization::writeJson(value);
    return text ? *text : std::string("<unprintable>");
}

} // namespace

bool matchesExpectation(const Value& expected, const Value& actual, std::string& where) {
    const std::string here = where.empty() ? std::string("$") : where;
    switch (expected.kind()) {
        case Value::Kind::Object: {
            if (actual.kind() != Value::Kind::Object) {
                where = here;
                return false;
            }
            for (const Value::Member& member : expected.members()) {
                const Value* found = actual.find(member.key);
                std::string inner = here + "." + member.key;
                if (found == nullptr) {
                    where = inner;
                    return false;
                }
                if (!matchesExpectation(member.value, *found, inner)) {
                    where = inner;
                    return false;
                }
            }
            return true;
        }
        case Value::Kind::Array: {
            if (actual.kind() != Value::Kind::Array || actual.elements().size() != expected.elements().size()) {
                where = here;
                return false;
            }
            for (std::size_t index = 0; index < expected.elements().size(); ++index) {
                std::string inner = here + "[" + std::to_string(index) + "]";
                if (!matchesExpectation(expected.elements()[index], actual.elements()[index], inner)) {
                    where = inner;
                    return false;
                }
            }
            return true;
        }
        case Value::Kind::Number: {
            const std::optional<double> wanted = expected.toNumber<double>();
            const std::optional<double> got =
                actual.kind() == Value::Kind::Number ? actual.toNumber<double>() : std::nullopt;
            if (!wanted || !got || *wanted != *got) {
                where = here;
                return false;
            }
            return true;
        }
        default:
            if (!(expected == actual)) {
                where = here;
                return false;
            }
            return true;
    }
}

std::vector<std::string> scriptCommands(std::string_view text) {
    std::vector<std::string> commands;
    Expected<Value> steps = readScript(text, {});
    if (!steps) {
        return commands;
    }
    for (const Value& step : steps->elements()) {
        const Value* command = step.kind() == Value::Kind::Object ? step.find("command") : nullptr;
        if (command != nullptr && command->kind() == Value::Kind::String) {
            commands.push_back(command->asString());
        }
    }
    return commands;
}

Expected<Value> Automation::run(std::string_view id, const Value& args) {
    return m_session->execute(id, args);
}

void Automation::waitIdle() {
    m_session->waitIdle();
}

Expected<ScriptReport> Automation::runScript(std::string_view text, std::string_view source) {
    Expected<Value> steps = readScript(text, source);
    if (!steps) {
        return makeUnexpected(std::move(steps).error());
    }
    const std::string name = source.empty() ? std::string("script") : std::string(source);
    ScriptReport report;
    for (const Value& step : steps->elements()) {
        const std::string at = name + " step " + std::to_string(report.steps);
        const Value* wait = step.kind() == Value::Kind::Object ? step.find("wait") : nullptr;
        if (wait != nullptr) {
            if (wait->kind() != Value::Kind::String || wait->asString() != "idle") {
                return makeError("automation.invalid_script", at + R"(: "wait" must be "idle")");
            }
            waitIdle();
            ++report.steps;
            continue;
        }
        const Value* command = step.kind() == Value::Kind::Object ? step.find("command") : nullptr;
        if (command == nullptr || command->kind() != Value::Kind::String) {
            return makeError("automation.invalid_script", at + R"(: a step has a "command" or a "wait")");
        }
        const Value* args = step.find("args");
        const Value* expect = step.find("expect");
        const Value* expectError = step.find("expectError");
        const std::string label = at + " (" + command->asString() + ")";
        Expected<Value> result = run(command->asString(), args != nullptr ? *args : Value());
        if (expectError != nullptr) {
            if (expectError->kind() != Value::Kind::String) {
                return makeError("automation.invalid_script", label + R"(: "expectError" must be an error code)");
            }
            if (result) {
                return makeError("automation.step_failed", label + ": succeeded, expected " + expectError->asString());
            }
            if (result.error().code != expectError->asString()) {
                return makeError("automation.step_failed", label + ": failed with " + result.error().code + " (" +
                                                               result.error().message + "), expected " +
                                                               expectError->asString());
            }
            ++report.steps;
            continue;
        }
        if (!result) {
            return makeError("automation.step_failed",
                             label + ": " + result.error().code + ": " + result.error().message);
        }
        if (expect != nullptr) {
            std::string where;
            if (!matchesExpectation(*expect, *result, where)) {
                std::string message = label;
                message += ": the result differs at ";
                message += where;
                message += ": ";
                message += describe(*result);
                return makeError("automation.step_failed", std::move(message));
            }
        }
        ++report.steps;
    }
    return report;
}

Expected<ScriptReport> Automation::runScriptFile(std::string_view path) {
    Expected<std::string> text = fs::readFile(path, kMaxScriptFileSize);
    if (!text) {
        return makeUnexpected(std::move(text).error());
    }
    return runScript(*text, path);
}

} // namespace ith
