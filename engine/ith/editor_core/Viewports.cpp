#include "ith/editor_core/Viewports.h"

#include "ith/file_system/FileSystem.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Expected.h"
#include "ith/foundation/Log.h"
#include "ith/reflection/TypeOf.h"
#include "ith/serialization/TextArchive.h"
#include "ith/serialization/Value.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(EditorViewports);

using serialization::Value;
using CameraSet = std::array<EditorCamera, kMaxViewports>;
using CameraMap = std::map<Uuid, CameraSet>;

// The index of the viewport instance id `instance`, or nullopt.
std::optional<std::size_t> indexOf(std::string_view instance) {
    const std::string_view prefix = "ith.viewport.";
    if (instance.size() != prefix.size() + 1 || !instance.starts_with(prefix)) {
        return std::nullopt;
    }
    const char digit = instance.back();
    if (digit < '1' || digit > static_cast<char>('0' + kMaxViewports)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(digit - '1');
}

bool isFinite(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

// A camera the controller can use: what updateCamera and focus keep.
bool isValid(const EditorCamera& camera) {
    return isFinite(camera.position) && isFinite(camera.pivot) && std::isfinite(camera.yaw) &&
           std::isfinite(camera.pitch) && std::abs(camera.pitch) < kHalfPi && std::isfinite(camera.fovY) &&
           camera.fovY > 0.0f && camera.fovY < kPi && camera.speedStep >= 1 && camera.speedStep <= 8;
}

Unexpected<Error> invalidFile(std::string_view source, const std::string& message) {
    return makeError("editor.viewports.invalid", std::string(source) + ": " + message);
}

// The cameras of a viewports file. Viewport instance ids it doesn't know are dropped.
Expected<CameraMap> parseCameras(std::string_view text, std::string_view source) {
    Expected<Value, serialization::Error> data =
        serialization::readDocument(text, kEditorViewportsFormat, {.source = source, .maxSize = kMaxViewportsFileSize});
    if (!data) {
        return makeError(data.error().code, data.error().toString());
    }
    const Value* levels = data->kind() == Value::Kind::Object ? data->find("levels") : nullptr;
    if (levels == nullptr || levels->kind() != Value::Kind::Object) {
        return invalidFile(source, "\"levels\" must be an object");
    }
    CameraMap result;
    for (const Value::Member& level : levels->members()) {
        const Expected<Uuid> id = Uuid::parse(level.key);
        if (!id || id->isNil()) {
            return invalidFile(source, "\"" + level.key + "\" is not a level id");
        }
        if (level.value.kind() != Value::Kind::Object) {
            return invalidFile(source, "the cameras of " + level.key + " must be an object");
        }
        CameraSet cameras{};
        for (const Value::Member& viewport : level.value.members()) {
            const std::optional<std::size_t> index = indexOf(viewport.key);
            if (!index.has_value()) {
                continue;
            }
            EditorCamera camera;
            if (Expected<void, serialization::Error> read =
                    serialization::fromValue(viewport.value, typeOf<EditorCamera>(), &camera);
                !read) {
                return invalidFile(source, level.key + "." + viewport.key + ": " + read.error().toString());
            }
            if (!isValid(camera)) {
                return invalidFile(source, level.key + "." + viewport.key + " is not a usable camera");
            }
            cameras[*index] = camera;
        }
        result.insert_or_assign(*id, cameras);
    }
    return result;
}

} // namespace

Viewports::Viewports(std::string file) : m_file(std::move(file)) {}

ViewportState* Viewports::find(std::string_view instance) noexcept {
    const std::optional<std::size_t> index = indexOf(instance);
    return index.has_value() ? &m_states[*index] : nullptr;
}

const ViewportState* Viewports::find(std::string_view instance) const noexcept {
    const std::optional<std::size_t> index = indexOf(instance);
    return index.has_value() ? &m_states[*index] : nullptr;
}

ViewportState& Viewports::at(std::size_t index) noexcept {
    ITH_ASSERT_MSG(index < kMaxViewports, "there are only four viewports");
    return m_states[index < kMaxViewports ? index : kMaxViewports - 1];
}

const ViewportState& Viewports::at(std::size_t index) const noexcept {
    ITH_ASSERT_MSG(index < kMaxViewports, "there are only four viewports");
    return m_states[index < kMaxViewports ? index : kMaxViewports - 1];
}

std::string Viewports::instanceId(std::size_t index) {
    ITH_ASSERT_MSG(index < kMaxViewports, "there are only four viewports");
    const std::size_t number = (index < kMaxViewports ? index : kMaxViewports - 1) + 1;
    return std::string(kViewportPanel) + "." + std::to_string(number);
}

void Viewports::setSnap(bool enabled, float step) noexcept {
    const bool valid = std::isfinite(step) && step > 0.0f;
    ITH_ASSERT_MSG(valid, "the snapping step must be finite and positive");
    if (!valid) {
        return;
    }
    m_snapEnabled = enabled;
    m_snapStep = step;
}

void Viewports::saveLevel(const Uuid& level) {
    load();
    Cameras cameras;
    for (std::size_t i = 0; i < kMaxViewports; ++i) {
        cameras[i] = m_states[i].camera;
    }
    if (!m_levels.contains(level) && cameras == Cameras{}) {
        return; // nothing worth keeping: the defaults come back anyway
    }
    m_levels.insert_or_assign(level, cameras);
    write();
}

void Viewports::restoreLevel(const Uuid& level) {
    load();
    const auto found = m_levels.find(level);
    for (std::size_t i = 0; i < kMaxViewports; ++i) {
        m_states[i].camera = found != m_levels.end() ? found->second[i] : EditorCamera{};
    }
}

void Viewports::load() {
    if (m_loaded || m_file.empty()) {
        return;
    }
    m_loaded = true;
    const Expected<std::optional<fs::FileInfo>> info = fs::fileInfo(m_file);
    if (info && !info->has_value()) {
        // A fresh project has none yet: default cameras, as the session layout does without its file.
        m_written = LevelCameras{};
        return;
    }
    Expected<std::string> text =
        info ? fs::readFile(m_file, kMaxViewportsFileSize) : Expected<std::string>(makeUnexpected(info.error()));
    Expected<LevelCameras> cameras =
        text ? parseCameras(*text, m_file) : Expected<LevelCameras>(makeUnexpected(text.error()));
    if (!cameras) {
        ITH_LOG_WARNING(EditorViewports, "the viewport cameras can't be read; the viewports use default cameras: {}",
                        cameras.error().message);
        return;
    }
    m_written = *cameras;
    m_levels = std::move(*cameras);
}

void Viewports::write() {
    if (m_file.empty() || (m_written.has_value() && *m_written == m_levels)) {
        return;
    }
    Value levels = Value::object();
    Expected<void> written;
    for (const auto& [id, cameras] : m_levels) {
        Value level = Value::object();
        for (std::size_t i = 0; i < kMaxViewports && written; ++i) {
            Expected<Value, serialization::Error> camera = serialization::toValue(typeOf<EditorCamera>(), &cameras[i]);
            if (camera) {
                level.set(instanceId(i), std::move(*camera));
            } else {
                written = makeError(camera.error().code, camera.error().toString());
            }
        }
        levels.set(id.toString(), std::move(level));
    }
    if (written) {
        Value data = Value::object();
        data.set("levels", std::move(levels));
        Expected<std::string, serialization::Error> text = serialization::writeDocument(kEditorViewportsFormat, data);
        written = text ? fs::writeFileAtomic(m_file, *text, {.durability = fs::Durability::Atomic})
                       : Expected<void>(makeError(text.error().code, text.error().toString()));
    }
    if (!written) {
        if (!m_writeFailed) {
            ITH_LOG_WARNING(EditorViewports, "the viewport cameras can't be written: {}", written.error().message);
        }
        m_writeFailed = true;
        return;
    }
    m_writeFailed = false;
    m_written = m_levels;
}

} // namespace ith
