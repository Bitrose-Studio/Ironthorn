// The Window commands and the layout queries (ADR-0040 rule 14). They are Actions: no transaction,
// no undo entry, allowed while playing. Their logic is LayoutSession's.

#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/EditorSession.h"
#include "ith/editor_core/LayoutSession.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ith {

namespace {

using detail::invalidArgs;
using detail::requiredString;
using serialization::Value;

Value warningsValue(const std::vector<Error>& warnings) {
    Value list = Value::array();
    for (const Error& warning : warnings) {
        Value entry = Value::object();
        entry.set("code", Value::string(warning.code));
        entry.set("message", Value::string(warning.message));
        list.elements().push_back(std::move(entry));
    }
    return list;
}

Value loadValue(const LayoutLoad& load) {
    Value result = Value::object();
    result.set("name", Value::string(load.name));
    result.set("warnings", warningsValue(load.warnings));
    return result;
}

Value nameValue(std::string name) {
    Value result = Value::object();
    result.set("name", Value::string(std::move(name)));
    return result;
}

// The required "name" argument of `command`.
Expected<std::string_view> nameArg(std::string_view command, const Value& args) {
    return requiredString(command, args, "name");
}

Expected<Value> saveLayoutAs(EditorSession& session, const Value& args) {
    constexpr std::string_view kCommand = "ith.window.save_layout_as";
    Expected<std::string_view> name = nameArg(kCommand, args);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    bool overwrite = false;
    if (const Value* flag = args.kind() == Value::Kind::Object ? args.find("overwrite") : nullptr;
        flag != nullptr && flag->kind() != Value::Kind::Null) {
        if (flag->kind() != Value::Kind::Bool) {
            return invalidArgs(kCommand, R"("overwrite" must be a boolean)");
        }
        overwrite = flag->asBool();
    }
    Expected<std::string> saved = session.layouts().saveAs(*name, overwrite);
    if (!saved) {
        return makeUnexpected(std::move(saved).error());
    }
    return nameValue(std::move(*saved));
}

Expected<Value> loadLayout(EditorSession& session, const Value& args) {
    Expected<std::string_view> name = nameArg("ith.window.load_layout", args);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    Expected<LayoutLoad> loaded = session.layouts().load(*name);
    if (!loaded) {
        return makeUnexpected(std::move(loaded).error());
    }
    return loadValue(*loaded);
}

Expected<Value> setDefaultLayout(EditorSession& session, const Value& args) {
    Expected<std::string_view> name = nameArg("ith.window.set_default_layout", args);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    Expected<std::string> chosen = session.layouts().setDefault(*name);
    if (!chosen) {
        return makeUnexpected(std::move(chosen).error());
    }
    return nameValue(std::move(*chosen));
}

Expected<Value> deleteLayout(EditorSession& session, const Value& args) {
    Expected<std::string_view> name = nameArg("ith.window.delete_layout", args);
    if (!name) {
        return makeUnexpected(std::move(name).error());
    }
    Expected<std::string> removed = session.layouts().remove(*name);
    if (!removed) {
        return makeUnexpected(std::move(removed).error());
    }
    return nameValue(std::move(*removed));
}

Expected<Value> resetLayout(EditorSession& session, const Value& /*args*/) {
    return loadValue(session.layouts().reset());
}

Expected<Value> openPanelCommand(EditorSession& session, const Value& args) {
    Expected<std::string_view> panel = requiredString("ith.window.open_panel", args, "panel");
    if (!panel) {
        return makeUnexpected(std::move(panel).error());
    }
    Expected<PanelOpened> opened = session.layouts().open(*panel);
    if (!opened) {
        return makeUnexpected(std::move(opened).error());
    }
    Value result = Value::object();
    result.set("panel", Value::string(std::move(opened->instance)));
    result.set("opened", Value::boolean(opened->opened));
    return result;
}

Expected<Value> closePanelCommand(EditorSession& session, const Value& args) {
    Expected<std::string_view> panel = requiredString("ith.window.close_panel", args, "panel");
    if (!panel) {
        return makeUnexpected(std::move(panel).error());
    }
    Expected<bool> closed = session.layouts().close(*panel);
    if (!closed) {
        return makeUnexpected(std::move(closed).error());
    }
    Value result = Value::object();
    result.set("panel", Value::string(std::string(*panel)));
    result.set("closed", Value::boolean(*closed));
    return result;
}

Expected<Value> layoutQuery(EditorSession& session, const Value& /*args*/) {
    const LayoutSession& layouts = session.layouts();
    Value open = Value::array();
    for (std::string& id : openPanels(layouts.current())) {
        open.elements().push_back(Value::string(std::move(id)));
    }
    Value result = Value::object();
    result.set("name", Value::string(layouts.name()));
    result.set("layout", layoutToValue(layouts.current()));
    result.set("open", std::move(open));
    return result;
}

Expected<Value> layoutsQuery(EditorSession& session, const Value& /*args*/) {
    Expected<std::vector<LayoutListing>> listings = session.layouts().list();
    if (!listings) {
        return makeUnexpected(std::move(listings).error());
    }
    Value list = Value::array();
    for (LayoutListing& listing : *listings) {
        Value entry = Value::object();
        entry.set("name", Value::string(std::move(listing.name)));
        entry.set("layer", Value::string(std::move(listing.layer)));
        list.elements().push_back(std::move(entry));
    }
    Value result = Value::object();
    result.set("layouts", std::move(list));
    result.set("default", Value::string(session.layouts().defaultName()));
    return result;
}

void addWindowCommand(CommandRegistry& registry, std::string id, std::string displayName, std::string tooltip,
                      Expected<Value> (*execute)(EditorSession&, const Value&)) {
    registry.add({.id = std::move(id),
                  .displayName = std::move(displayName),
                  .tooltip = std::move(tooltip),
                  .category = "Window",
                  .kind = CommandKind::Action,
                  .execute = execute});
}

} // namespace

void registerLayoutCommands(CommandRegistry& registry) {
    addWindowCommand(registry, "ith.window.open_panel", "Open Panel",
                     "Open a panel at its usual place, or focus it when it is open", &openPanelCommand);
    addWindowCommand(registry, "ith.window.close_panel", "Close Panel", "Close a panel", &closePanelCommand);
    addWindowCommand(registry, "ith.window.load_layout", "Load Layout", "Replace the layout with a saved one",
                     &loadLayout);
    addWindowCommand(registry, "ith.window.save_layout_as", "Save Layout As",
                     "Save the layout under a name of your own", &saveLayoutAs);
    addWindowCommand(registry, "ith.window.set_default_layout", "Set as Default",
                     "Make a layout the one Reset Layout loads", &setDefaultLayout);
    addWindowCommand(registry, "ith.window.delete_layout", "Delete Layout", "Delete a layout you saved", &deleteLayout);
    addWindowCommand(registry, "ith.window.reset_layout", "Reset Layout", "Load the default layout", &resetLayout);
    registry.add({.id = "ith.query.layout",
                  .displayName = "Layout",
                  .tooltip = "The current layout, its name and the open panels",
                  .category = "Query",
                  .kind = CommandKind::Query,
                  .execute = &layoutQuery});
    registry.add({.id = "ith.query.layouts",
                  .displayName = "Layouts",
                  .tooltip = "The saved layouts and the default one",
                  .category = "Query",
                  .kind = CommandKind::Query,
                  .execute = &layoutsQuery});
}

} // namespace ith
