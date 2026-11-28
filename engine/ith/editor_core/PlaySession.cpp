#include "ith/editor_core/PlaySession.h"

#include "ith/ecs/Query.h"
#include "ith/ecs/StableId.h"
#include "ith/ecs/WorldCopy.h"
#include "ith/editor_core/CommandArgs.h"
#include "ith/editor_core/Viewports.h"
#include "ith/foundation/Assert.h"
#include "ith/foundation/Log.h"
#include "ith/gameplay/GameWorld.h"
#include "ith/gameplay/Time.h"

#include <utility>

namespace ith {

namespace {

ITH_LOG_CATEGORY(PlayInEditor);

} // namespace

PlaySession::PlaySession(PlayMode mode, bool paused, std::uint64_t identity)
    : m_mode(mode),
      m_possessed(mode == PlayMode::Play),
      m_paused(paused),
      m_identity(identity),
      m_mapping(detail::stableIdMapping(m_world)) {
    // The game systems change the world outside the transactions.
    m_transactions.setForeignWriters(true);
}

Expected<std::unique_ptr<PlaySession>> PlaySession::start(const World& editorWorld, const Selection& editorSelection,
                                                          std::span<const SystemDescriptor> gameSystems, PlayMode mode,
                                                          bool paused, std::uint64_t identity, std::size_t viewport,
                                                          const EditorCamera& camera) {
    ITH_ASSERT_MSG(identity != 0, "editor: a play session's identity is never the editor world's");
    ITH_ASSERT_MSG(viewport < kMaxViewports, "editor: a session plays in one of the four viewports");
    // The constructor is private, so make_unique can't reach it; the pointer is owned at once.
    std::unique_ptr<PlaySession> session(new PlaySession(mode, paused, identity));
    if (Expected<void> built = buildGameSchedule(session->m_schedule, gameSystems); !built) {
        ITH_LOG_ERROR(PlayInEditor, "can't start: the game-world schedule doesn't build: {}", built.error().message);
        return makeUnexpected(std::move(built).error());
    }
    if (!copyWorld(editorWorld, session->m_world, {.excludeEditorOnly = true})) {
        return makeError("editor.play.copy_failed", "the editor world can't be copied into a game world now");
    }
    // WorldStarted goes here, once the engine has events (ADR-0053 decision 11).
    session->m_world.insertResource(Time{});
    session->m_selection.replace(editorSelection.entities());
    session->m_viewport = viewport < kMaxViewports ? viewport : 0;
    session->m_savedCamera = camera;
    session->m_seenStructure = session->m_world.structureGeneration();
    // A new world for every panel: its counters start moved, so nothing reads it as unchanged.
    session->m_changes.bumpStructure();
    session->m_changes.bumpSelection();
    session->m_changes.bumpDocument();
    session->m_changes.bumpAllValues();
    return session;
}

void PlaySession::setPossessed(bool possessed, EditorCamera& camera) noexcept {
    if (possessed == m_possessed) {
        return;
    }
    if (possessed) {
        // The spectator resumes where it was ejected; with no Eject yet, at the camera.
        if (m_spectatorPose.has_value()) {
            camera = *m_spectatorPose;
        }
    } else {
        m_spectatorPose = camera; // the camera stays where the spectator was
    }
    m_possessed = possessed;
}

void PlaySession::setPaused(bool paused) noexcept {
    if (m_paused && !paused) {
        m_lastTickNs.reset();
    }
    m_paused = paused;
}

void PlaySession::tick(std::uint64_t nowNs) {
    if (m_paused) {
        return;
    }
    const std::optional<std::uint64_t> last = std::exchange(m_lastTickNs, nowNs);
    if (!last.has_value()) {
        return; // the baseline after a start or a resume
    }
    // A clock that went back counts as no time; tickGameWorld clamps a long gap.
    const double seconds = nowNs > *last ? static_cast<double>(nowNs - *last) * 1e-9 : 0.0;
    runTick(seconds);
}

void PlaySession::step(std::uint32_t frames, double seconds) {
    const bool valid = frames >= 1 && frames <= kMaxStepFrames && seconds > 0.0 && seconds <= kMaxTickDelta;
    ITH_ASSERT_MSG(valid, "editor: a step runs 1 to kMaxStepFrames ticks of (0, kMaxTickDelta] seconds");
    if (!valid) {
        return;
    }
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        runTick(seconds);
    }
}

void PlaySession::runTick(double seconds) {
    tickGameWorld(m_world, m_schedule, seconds);
    if (const std::uint64_t structure = m_world.structureGeneration(); structure != m_seenStructure) {
        m_seenStructure = structure;
        m_changes.bumpStructure();
    }
    m_changes.bumpAllValues();
}

const Time* PlaySession::time() const {
    return m_world.resource<const Time>();
}

std::size_t PlaySession::spawnedCount() {
    std::size_t spawned = 0;
    m_world.query<Without<StableId>, IncludeDisabled>().forEach([&spawned](Entity) { ++spawned; });
    return spawned;
}

} // namespace ith
