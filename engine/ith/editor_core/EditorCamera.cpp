#include "ith/editor_core/EditorCamera.h"

#include "ith/foundation/Assert.h"
#include "ith/math/Geometry.h"
#include "ith/math/Math.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace ith {

namespace {

constexpr std::array<float, 8> kFlySpeeds{0.5f, 1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f, 64.0f};
constexpr float kLookSensitivity = 0.005f;
constexpr float kPitchLimit = kHalfPi - 0.001f;
constexpr float kDollyScale = 0.9f;
constexpr float kDollyDragPixels = 100.0f;
constexpr float kPanScale = 0.001f;
constexpr float kFocusMargin = 1.1f;
constexpr float kMinimumFocusDistance = 1.0f;

struct CameraBasis {
    Vec3 forward;
    Vec3 right;
    Vec3 up;
};

CameraBasis basisOf(float yaw, float pitch) noexcept {
    const Quat rotation = glm::angleAxis(yaw, kWorldUp) * glm::angleAxis(pitch, kWorldRight);
    return {.forward = rotation * kWorldForward, .right = rotation * kWorldRight, .up = rotation * kWorldUp};
}

float clampPitch(float pitch) noexcept {
    return std::clamp(pitch, -kPitchLimit, kPitchLimit);
}

float wrapYaw(float yaw) noexcept {
    if (yaw >= -kPi && yaw < kPi) {
        return yaw;
    }
    float wrapped = std::fmod(yaw + kPi, kTwoPi);
    if (wrapped < 0.0f) {
        wrapped += kTwoPi;
    }
    return wrapped - kPi;
}

float flySpeed(int speedStep) noexcept {
    const std::size_t index = static_cast<std::size_t>(std::clamp(speedStep, 1, 8) - 1);
    return kFlySpeeds[index];
}

void dolly(EditorCamera& camera, float steps) noexcept {
    const Vec3 offset = camera.position - camera.pivot;
    if (offset == Vec3(0.0f)) {
        return;
    }
    camera.position = camera.pivot + offset * std::pow(kDollyScale, steps);
}

bool isEmpty(const Aabb& bounds) noexcept {
    return bounds.min.x > bounds.max.x || bounds.min.y > bounds.max.y || bounds.min.z > bounds.max.z;
}

} // namespace

void updateCamera(EditorCamera& camera, const CameraInput& input, float deltaSeconds) noexcept {
    const bool validDelta = std::isfinite(deltaSeconds) && deltaSeconds >= 0.0f;
    ITH_ASSERT_MSG(validDelta, "the camera frame time must be finite and non-negative");
    if (!validDelta) {
        return;
    }

    camera.pitch = clampPitch(camera.pitch);
    camera.speedStep = std::clamp(camera.speedStep, 1, 8);
    if (input.rmb && input.wheel != 0.0f) {
        const int notches = static_cast<int>(std::clamp(input.wheel, -8.0f, 8.0f));
        camera.speedStep = std::clamp(camera.speedStep + notches, 1, 8);
    }

    if (input.alt && input.lmb) {
        const float distance = glm::length(camera.position - camera.pivot);
        camera.yaw -= input.mouseDelta.x * kLookSensitivity;
        camera.pitch = clampPitch(camera.pitch - input.mouseDelta.y * kLookSensitivity);
        camera.position = camera.pivot - basisOf(camera.yaw, camera.pitch).forward * distance;
    } else if (input.alt && input.rmb) {
        dolly(camera, -input.mouseDelta.y / kDollyDragPixels);
    } else if (input.mmb) {
        const CameraBasis basis = basisOf(camera.yaw, camera.pitch);
        const float scale = std::max(glm::length(camera.position - camera.pivot), 1.0f) * kPanScale;
        const Vec3 delta = (basis.up * input.mouseDelta.y - basis.right * input.mouseDelta.x) * scale;
        camera.position += delta;
        camera.pivot += delta;
    } else if (input.rmb) {
        const float pivotDistance = std::max(glm::length(camera.position - camera.pivot), 1.0f);
        camera.yaw -= input.mouseDelta.x * kLookSensitivity;
        camera.pitch = clampPitch(camera.pitch - input.mouseDelta.y * kLookSensitivity);
        const CameraBasis basis = basisOf(camera.yaw, camera.pitch);
        const float forward = static_cast<float>(input.forward) - static_cast<float>(input.back);
        const float right = static_cast<float>(input.right) - static_cast<float>(input.left);
        const float up = static_cast<float>(input.up) - static_cast<float>(input.down);
        const Vec3 direction = basis.forward * forward + basis.right * right + kWorldUp * up;
        if (direction != Vec3(0.0f)) {
            camera.position += glm::normalize(direction) * (flySpeed(camera.speedStep) * deltaSeconds);
        }
        camera.pivot = camera.position + basis.forward * pivotDistance;
    }

    if (!input.rmb && input.wheel != 0.0f) {
        dolly(camera, input.wheel);
    }
    camera.yaw = wrapYaw(camera.yaw);
}

Mat4 viewMatrix(const EditorCamera& camera) noexcept {
    const Vec3 forward = basisOf(camera.yaw, camera.pitch).forward;
    return glm::lookAt(camera.position, camera.position + forward, kWorldUp);
}

float verticalFov(float fovX, float aspect) noexcept {
    const bool validFov = std::isfinite(fovX) && fovX > 0.0f && fovX < kPi;
    ITH_ASSERT_MSG(validFov, "the horizontal field of view must be finite and in (0, pi)");
    const bool validAspect = std::isfinite(aspect) && aspect > 0.0f;
    ITH_ASSERT_MSG(validAspect, "the camera aspect ratio must be finite and positive");
    if (!validFov || !validAspect) {
        return fovX;
    }
    return 2.0f * std::atan(std::tan(0.5f * fovX) / aspect);
}

Mat4 projectionMatrix(const EditorCamera& camera, float aspect, float nearZ) noexcept {
    return perspectiveReversedInfinite(verticalFov(camera.fovX, aspect), aspect, nearZ);
}

void focus(EditorCamera& camera, const Aabb& bounds, float aspect) noexcept {
    const bool nonEmpty = !isEmpty(bounds);
    ITH_ASSERT_MSG(nonEmpty, "camera focus needs non-empty world bounds");
    const bool validAspect = std::isfinite(aspect) && aspect > 0.0f;
    ITH_ASSERT_MSG(validAspect, "the camera aspect ratio must be finite and positive");
    const bool validFov = std::isfinite(camera.fovX) && camera.fovX > 0.0f && camera.fovX < kPi;
    ITH_ASSERT_MSG(validFov, "the horizontal field of view must be finite and in (0, pi)");
    if (!nonEmpty || !validAspect || !validFov) {
        return;
    }

    const float limitingHalfFov = 0.5f * std::min(camera.fovX, verticalFov(camera.fovX, aspect));
    const float radius = glm::length(bounds.halfExtents());
    const float sphereDistance = kFocusMargin * radius / std::sin(limitingHalfFov);
    const float distance = std::max(kMinimumFocusDistance, sphereDistance);
    camera.pivot = bounds.center();
    camera.position = camera.pivot - basisOf(camera.yaw, camera.pitch).forward * distance;
}

} // namespace ith
