#pragma once

// ith::Automation: scripts and code that drive the editor through its commands (ADR-0037 rules 17
// to 20, ADR-0029 rule 18).
//
//   ith::Automation automation(session);
//   automation.run("ith.entity.create", args);    // C++: one command or query
//   automation.waitIdle();                        // saves and opens have finished
//   ith::Expected<ith::ScriptReport> report = automation.runScript(text, "build_and_save.json");
//
// A script is an "ith.automation_script" document, version 1, whose "data" holds "steps":
//
//   {"format": "ith.automation_script", "version": 1, "data": {"steps": [
//       {"command": "ith.entity.create", "args": {"id": "...", "label": "Lamp"}},
//       {"command": "ith.edit.undo"},
//       {"command": "ith.query.selection", "expect": {"entities": []}},
//       {"command": "ith.edit.redo", "expectError": "editor.command.disabled"},
//       {"wait": "idle"}
//   ]}}
//
// - "command" runs a command with "args" (an object; missing means none).
// - "expect" matches the result: an object matches when each of its keys is in the result and
//   matches; arrays match element by element with the same length; numbers by value; other values
//   exactly. Keys the expectation leaves out are not checked.
// - "expectError" passes when the command fails with that error code, and fails when it succeeds.
// - {"wait": "idle"} waits until no file operation runs.
//
// There are no variables: commands that create things take their ids as arguments, so scripts are
// deterministic. The runner stops at the first failing step. The same runner serves the headless
// editor_core tests and the editor's --automation run; a later MCP adapter calls run(). Main
// thread only.

#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"
#include "ith/serialization/Value.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ith {

class EditorSession;

inline constexpr serialization::DocumentFormat kAutomationScriptFormat{
    .name = "ith.automation_script", .version = 1, .minimumVersion = 1};

// The largest script file runScriptFile reads.
inline constexpr std::size_t kMaxScriptFileSize = std::size_t{16} * 1024 * 1024;

// What a script run did.
struct ScriptReport {
    // The number of steps run, all passed.
    std::size_t steps = 0;
};

class Automation {
public:
    explicit Automation(EditorSession& session) noexcept : m_session(&session) {}

    // Runs one command or query (EditorSession::execute).
    Expected<serialization::Value> run(std::string_view id, const serialization::Value& args = {});

    // Returns once no file operation runs (EditorSession::waitIdle).
    void waitIdle();

    // Runs the script `text`; `source` names it in errors. Fails with the document errors of the
    // envelope, "automation.invalid_script" (a step that is not a command or a wait), and
    // "automation.step_failed" (a command failed, or failed with another code than expectError, or
    // succeeded despite expectError, or its result didn't match expect), whose message names the
    // source, the step and, for a mismatch, the JSON path.
    Expected<ScriptReport> runScript(std::string_view text, std::string_view source = {});

    // Reads the file at `path` (UTF-8) and runs it.
    Expected<ScriptReport> runScriptFile(std::string_view path);

private:
    EditorSession* m_session;
};

// True when `actual` matches `expected` as "expect" does; otherwise `where` receives the JSON path of
// the first difference ("$.entities[0]").
[[nodiscard]] bool matchesExpectation(const serialization::Value& expected, const serialization::Value& actual,
                                      std::string& where);

// The command ids of a script's steps, in order; empty when the text is not a script. For checks
// over a set of scripts, such as the test that every command is scripted.
[[nodiscard]] std::vector<std::string> scriptCommands(std::string_view text);

} // namespace ith
