// Automation (ADR-0037 rules 17 to 20): the expectation matcher, script errors, every script in
// tests/editor_core/scripts run headless in a fresh project with a job system, and the coverage
// rule: a registered command that no script runs fails the build's tests.

#include "ith/editor_core/Automation.h"

#include "ith/editor_core/EditorSession.h"
#include "ith/file_system/FileSystem.h"
#include "ith/file_system/Path.h"
#include "ith/jobs/JobSystem.h"

#include "EditorCoreComponents.h"
#include "EditorTestSupport.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <ostream> // doctest prints std::string operands through an ostream
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

using ith::Automation;
using ith::EditorSession;
using ith::test::editor::json;
using ith::test::editor::TempProject;
using ith::test::editor::types;

// Run with the source directory as the working directory (ith_add_test_suite).
constexpr std::string_view kScripts = "tests/editor_core/scripts";

std::vector<std::string> scriptPaths() {
    std::vector<std::string> paths;
    const auto entries = ith::fs::listDirectory(kScripts);
    REQUIRE_MESSAGE(entries.hasValue(), (entries.hasValue() ? std::string() : entries.error().message));
    for (const ith::fs::DirectoryEntry& entry : *entries) {
        if (entry.kind == ith::fs::FileKind::File && entry.name.ends_with(".json")) {
            paths.push_back(ith::fs::joinPath(kScripts, entry.name));
        }
    }
    std::ranges::sort(paths);
    return paths;
}

std::string script(std::string_view steps) {
    return R"({"format": "ith.automation_script", "version": 1, "data": {"steps": )" + std::string(steps) + "}}";
}

} // namespace

TEST_SUITE("editor_core.automation") {
    TEST_CASE("expectations match subsets of objects, whole arrays and numbers by value") {
        std::string where;
        CHECK(ith::matchesExpectation(json(R"({"a": 1})"), json(R"({"a": 1.0, "b": 2})"), where));
        CHECK(ith::matchesExpectation(json(R"({"list": [1, {"x": true}]})"),
                                      json(R"({"list": [1, {"x": true, "y": 0}]})"), where));
        CHECK_FALSE(ith::matchesExpectation(json(R"({"list": [1]})"), json(R"({"list": [1, 2]})"), where));
        CHECK(where == "$.list");
        where.clear();
        CHECK_FALSE(
            ith::matchesExpectation(json(R"({"a": {"b": [0, "x"]}})"), json(R"({"a": {"b": [0, "y"]}})"), where));
        CHECK(where == "$.a.b[1]");
        where.clear();
        CHECK_FALSE(ith::matchesExpectation(json(R"({"missing": null})"), json("{}"), where));
        CHECK(where == "$.missing");
        CHECK(ith::matchesExpectation(json("null"), json("null"), where));
    }

    TEST_CASE("a script stops at the first failing step and names it") {
        EditorSession session(types());
        Automation automation(session);
        const auto bad = automation.runScript(
            script(R"([{"command": "ith.query.document", "expect": {"dirty": true}}])"), "check.json");
        REQUIRE_FALSE(bad.hasValue());
        CHECK(bad.error().code == "automation.step_failed");
        CHECK(bad.error().message.find("check.json step 0 (ith.query.document)") != std::string::npos);
        CHECK(bad.error().message.find("$.dirty") != std::string::npos);

        const auto unexpected = automation.runScript(
            script(R"([{"command": "ith.edit.select_all", "expectError": "editor.command.disabled"}])"));
        REQUIRE_FALSE(unexpected.hasValue());
        CHECK(unexpected.error().message.find("succeeded") != std::string::npos);

        const auto failed =
            automation.runScript(script(R"([{"command": "ith.edit.undo"}, {"command": "ith.query.document"}])"));
        REQUIRE_FALSE(failed.hasValue());
        CHECK(failed.error().message.find("editor.command.disabled") != std::string::npos);

        CHECK(automation.runScript(script(R"([{"wait": "later"}])")).error().code == "automation.invalid_script");
        CHECK(automation.runScript(script(R"([{"args": {}}])")).error().code == "automation.invalid_script");
        CHECK(automation.runScript(R"({"format": "other", "version": 1, "data": {}})").error().code ==
              "serialization.wrong_format");
        const auto passed = automation.runScript(script(R"([{"command": "ith.query.selection"}, {"wait": "idle"}])"));
        REQUIRE(passed.hasValue());
        CHECK(passed->steps == 2);
    }

    TEST_CASE("every script in tests/editor_core/scripts passes, headless, in a fresh project") {
        const std::vector<std::string> paths = scriptPaths();
        REQUIRE_FALSE(paths.empty());
        for (const std::string& path : paths) {
            CAPTURE(path);
            TempProject project;
            ith::JobSystem jobs({.foregroundWorkers = 1, .backgroundWorkers = 1});
            EditorSession session(types(), {.projectDirectory = project.root(), .jobs = &jobs});
            Automation automation(session);
            const auto report = automation.runScriptFile(path);
            CHECK_MESSAGE(report.hasValue(), (report.hasValue() ? std::string() : report.error().message));
        }
    }

    TEST_CASE("every registered command is run by at least one script") {
        std::set<std::string, std::less<>> scripted;
        for (const std::string& path : scriptPaths()) {
            const auto text = ith::fs::readFile(path, ith::kMaxScriptFileSize);
            REQUIRE(text.hasValue());
            for (std::string& id : ith::scriptCommands(*text)) {
                scripted.insert(std::move(id));
            }
        }
        EditorSession session(types());
        for (const ith::CommandDesc* command : session.commands().commands()) {
            CHECK_MESSAGE(scripted.contains(command->id),
                          (command->id + " is not run by any script in tests/editor_core/scripts"));
        }
    }
}
