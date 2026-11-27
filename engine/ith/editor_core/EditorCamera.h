#pragma once

// ith::EditorCamera: the camera of one editor viewport, without any UI (ADR-0042 rules 2, 5 to 7,
// 12 and 27, with ADR-0046's horizontal field of view).
//
//   ith::EditorCamera camera;
//   ith::CameraInput input;
//   input.rmb = true;
//   input.forward = true;
//   ith::updateCamera(camera, input, deltaSeconds);
//   const ith::Mat4 view = ith::viewMatrix(camera);
//   const ith::Mat4 projection = ith::projectionMatrix(camera, aspect, 0.1f);
//   ith::focus(camera, worldBounds, aspect);
//
// The camera is view state, not an entity: changing it never touches the level, its dirty state or
// the undo history. Mouse motion is in window coordinates (see CameraInput::mouseDelta) and
// wheel input is in notches. Angles are radians.
// There is no hidden state or allocation; distinct cameras may be used concurrently. The camera is
// reflected, so the Viewports store saves it per level (Viewports.h).

#include "ith/math/Types.h"
#include "ith/reflection/Annotations.h"

namespace ith {

struct Aabb;

// A default-constructed camera is the default camera of a level without a saved one (ADR-0042
// rule 5).
struct EditorCamera {
    ITH_TYPE(Struct)
    // Six metres from the origin, to its back-right and above, looking at the origin.
    ITH_FIELD()
    Vec3 position{3.6742346f, 3.0f, 3.6742346f};
    // Around world +Y; 0 looks along -Z.
    ITH_FIELD()
    float yaw = 0.25f * kPi;
    // Kept in (-pi/2, pi/2) by updateCamera, so looking up or down never flips the camera.
    ITH_FIELD()
    float pitch = -kPi / 6.0f;
    // The full horizontal field of view, in (0, pi); 90 degrees by default (ADR-0046). The
    // vertical angle follows from the image's aspect (verticalFov) and is never stored.
    ITH_FIELD()
    float fovX = kHalfPi;
    // An index in the speed table 0.5, 1, 2, 4, 8, 16, 32, 64 m/s, from 1 through 8.
    ITH_FIELD()
    int speedStep = 4;
    // The world-space point orbit and dolly gestures move around or toward.
    ITH_FIELD()
    Vec3 pivot{0.0f};

    friend bool operator==(const EditorCamera&, const EditorCamera&) = default;
};

// One frame of held viewport input. The viewport's input routing decides when these are set.
struct CameraInput {
    // The frame's mouse motion, right and down positive, in window coordinates (pixels divided by
    // the display's pixel density), so orbit, pan and dolly feel the same at every display scale;
    // the per-pixel rates below are per window coordinate. In relative mode (fly) it is the mouse's
    // raw motion, which no density scales.
    Vec2 mouseDelta{0.0f};
    // Accumulated notches; the caller retains fractional wheel input for speed stepping.
    float wheel = 0.0f;
    bool rmb = false;
    bool lmb = false;
    bool mmb = false;
    bool alt = false;
    bool forward = false;
    bool back = false;
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
};

// Applies one frame of navigation. Clamps pitch and speedStep, and wraps yaw into [-pi, pi). RMB
// looks and flies with W/A/S/D along the camera's axes and Q/E down/up in world space at the selected
// speed; opposing keys cancel and diagonals are normalized. The wheel while RMB is held changes one
// speed step per whole notch, clamped to 1--8; the caller accumulates fractional wheel input. RMB
// look/fly leaves the pivot at position + forward * max(previous pivot distance, 1 m). Alt+LMB
// orbits with the same drag signs as look, so the scene follows the drag; Alt+RMB dollies, MMB pans,
// and the wheel without RMB dollies by 0.9^wheel on the pivot distance. Orbit, dolly, pan, then fly
// take precedence in that order if a caller supplies more than one gesture. Mouse look and orbit use
// 0.005 radians per pixel; Alt+RMB uses one dolly step per 100 vertical pixels, upward positive. Pan
// moves position and pivot together by 0.001 times their distance per pixel (at least one metre),
// opposite the horizontal drag and along the vertical drag. `deltaSeconds` must be finite and
// non-negative (asserted); an invalid value leaves the camera unchanged.
void updateCamera(EditorCamera& camera, const CameraInput& input, float deltaSeconds) noexcept;

// The right-handed world-to-view matrix: identity yaw and pitch look along -Z with +Y up.
// `camera.pitch` must be in the clamp range maintained by updateCamera.
[[nodiscard]] Mat4 viewMatrix(const EditorCamera& camera) noexcept;

// The full vertical field of view of an image `aspect` (width over height) wide that shows the
// full horizontal angle `fovX`: 2 * atan(tan(fovX / 2) / aspect), in (0, pi). Preconditions
// (asserted): fovX finite and in (0, pi), aspect finite and > 0; invalid input returns fovX.
[[nodiscard]] float verticalFov(float fovX, float aspect) noexcept;

// A reversed-depth, infinite-far-plane perspective matrix with the camera's fovX across the image
// and verticalFov(fovX, aspect) up it. Preconditions (asserted): fovX in (0, pi), aspect > 0 and
// nearZ > 0. At the near plane depth is 1, approaching 0 in the distance.
[[nodiscard]] Mat4 projectionMatrix(const EditorCamera& camera, float aspect, float nearZ) noexcept;

// Frames non-empty world-space bounds at the viewport's width/height aspect. Keeps yaw and pitch,
// moves the pivot to the bounds' centre and backs away along the view direction until their
// enclosing sphere fits with a 10% margin in both directions: the smaller of fovX and
// verticalFov(fovX, aspect) limits the distance. This conservative fit also covers every bounds
// corner; a point is viewed from one metre. Preconditions (asserted): non-empty bounds, finite
// aspect > 0 and fovX in (0, pi). Invalid input leaves the camera unchanged.
void focus(EditorCamera& camera, const Aabb& bounds, float aspect) noexcept;

} // namespace ith
