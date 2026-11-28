#pragma once

// ith::PlaySession: one Play In Editor or Simulate run, from start to Stop (ADR-0053 decisions 3 and
// 7 to 11, docs/specs/play-in-editor.md 3.1 to 3.5).
//
//   if (auto started = session.startPlay(ith::PlayMode::Simulate, true); !started) { report(started.error()); }
//   ith::PlaySession& play = *session.play();
//   play.step(3, 1.0 / 60.0);          // headless: three ticks of exactly 1/60 s
//   play.tick(input.frameTimeNs());    // in the editor: once per frame, after input
//   session.stopPlay();                // the editor is as before Play
//
// EditorSession owns the session while it exists and starts and stops it (startPlay, stopPlay). It
// holds the game world, a copy of the editor world without editor-only data under a new world id,
// with a default ith::Time resource; the game-world schedule (ith::buildGameSchedule with
// EditorSessionOptions::gameSystems); a Transactions over the game world with a private history
// that is never undone, so edits of the game world record nothing anywhere a user sees; the play
// selection; the game world's EditorChanges and entity mapping (stable ids, as the editor world's);
// and the state flags. Discarding it discards all of them: nothing is restored by hand.
//
// Ticking: tick() runs one ith::tickGameWorld with the time since its previous call, except the
// first call after a start or a resume, which only records the time, so time spent paused never
// reaches the game. step() runs ticks of a given length at once. After every tick the game world's
// EditorChanges move: `structure` when World::structureGeneration() moved, and `values` as
// "everything may have changed" (EditorChanges::bumpAllValues), so the Outliner rebuilds only on
// structure and Details re-reads only what it shows. Main thread only, like the editor session.
//
// Views (ADR-0053 decisions 5, 7, 10, 17 and 18): the session plays in one viewport, the selected
// one at its start. That viewport's own camera is the play camera: the session keeps its pose from
// the start (savedCamera), which EditorSession puts back at Stop. Ejected, the camera has every
// editor control; possessed, the spectator (SpectatorConsumer, PlayInput.h) flies the same camera.
// Possess and Eject keep one spectator pose (setPossessed).

#include "ith/ecs/Schedule.h"
#include "ith/ecs/World.h"
#include "ith/editor_core/EditorCamera.h"
#include "ith/editor_core/EditorChanges.h"
#include "ith/editor_core/Selection.h"
#include "ith/editor_core/Transactions.h"
#include "ith/editor_core/UndoHistory.h"
#include "ith/foundation/Expected.h"
#include "ith/serialization/Document.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace ith {

struct Time;

// How a session started: Play gives the game the view (possessed), Simulate runs it without a
// player, watched through the editor camera (ejected).
enum class PlayMode : std::uint8_t {
    Play,
    Simulate,
};

// The longest step ith.play.step takes in one call, in ticks.
inline constexpr std::uint32_t kMaxStepFrames = 1000;

class PlaySession {
public:
    // A session over a copy of `editorWorld` (ADR-0053 decision 7): the game-world schedule built
    // from `gameSystems`, a new World filled by copyWorld with excludeEditorOnly, a default Time,
    // and the play selection set to `editorSelection`. Possessed for Play, not for Simulate; paused
    // when `paused`. `identity` is what the view models key on (EditorSession::active). It plays in
    // viewport `viewport` (Viewports::at), whose camera at the start is `camera`: the session keeps
    // that pose to be restored at Stop. Fails with the schedule's "ecs.schedule.*" error and with
    // "editor.play.copy_failed" when copyWorld refuses (the editor world is being iterated; a bug);
    // nothing is left behind then. Reads nothing from disk and writes nothing. Precondition
    // (asserted): viewport < kMaxViewports.
    [[nodiscard]] static Expected<std::unique_ptr<PlaySession>>
    start(const World& editorWorld, const Selection& editorSelection, std::span<const SystemDescriptor> gameSystems,
          PlayMode mode, bool paused, std::uint64_t identity, std::size_t viewport, const EditorCamera& camera);

    PlaySession(const PlaySession&) = delete;
    PlaySession& operator=(const PlaySession&) = delete;
    PlaySession(PlaySession&&) = delete;
    PlaySession& operator=(PlaySession&&) = delete;
    ~PlaySession() = default;

    // The game world, read-only: the editor changes it through transactions(); systems change it
    // while it ticks.
    [[nodiscard]] const World& world() const noexcept { return m_world; }

    // The game world for code that drives it besides the schedule: transform propagation before a
    // viewport draws it, and tests.
    [[nodiscard]] World& mutableWorld() noexcept { return m_world; }

    // Edits of the game world (the playEditable commands, Details drags). Their history is private
    // and keeps no entry (UndoLimits::maxEntries 0), so nothing is ever undone or recorded. They
    // treat the systems as foreign writers (Transactions::setForeignWriters): cancelling a drag on
    // an entity a system changed or despawned since is logged, not asserted. Game-world handles,
    // here and in panels, are never kept across Stop: world ids are recycled.
    [[nodiscard]] Transactions& transactions() noexcept { return m_transactions; }

    [[nodiscard]] const Transactions& transactions() const noexcept { return m_transactions; }

    // The play selection, by stable id; separate from the editor selection.
    [[nodiscard]] Selection& selection() noexcept { return m_selection; }

    [[nodiscard]] const Selection& selection() const noexcept { return m_selection; }

    // The game world's change counters.
    [[nodiscard]] EditorChanges& changes() noexcept { return m_changes; }

    [[nodiscard]] const EditorChanges& changes() const noexcept { return m_changes; }

    // The game world's entities as stable ids in JSON, as EditorSession::entityMapping is for the
    // editor world.
    [[nodiscard]] const serialization::EntityMapping& entityMapping() const noexcept { return m_mapping; }

    // Never the same for two sessions of one EditorSession, and never 0 (the editor world's).
    [[nodiscard]] std::uint64_t identity() const noexcept { return m_identity; }

    [[nodiscard]] PlayMode mode() const noexcept { return m_mode; }

    // The game has the view and, when captured, the input (Play); false while ejected.
    [[nodiscard]] bool possessed() const noexcept { return m_possessed; }

    // Possesses or ejects (ADR-0053 decision 18); `camera` is the play viewport's camera, which the
    // spectator flies while possessed. Eject leaves the camera where the spectator was and keeps that
    // pose as the spectator pose. Possess moves the camera to the spectator pose, or, before any
    // Eject (a session started with Simulate), leaves it where it is. Setting the current state
    // changes nothing.
    void setPossessed(bool possessed, EditorCamera& camera) noexcept;

    // The play viewport's index (Viewports::at): the selected viewport at the start.
    [[nodiscard]] std::size_t viewport() const noexcept { return m_viewport; }

    // The play viewport's camera as it was at the start, which Stop restores bit for bit.
    [[nodiscard]] const EditorCamera& savedCamera() const noexcept { return m_savedCamera; }

    // The pose the spectator resumes at on Possess: the camera at the last Eject; nullopt before any.
    [[nodiscard]] const std::optional<EditorCamera>& spectatorPose() const noexcept { return m_spectatorPose; }

    // The editor's Pause: no tick runs. Not Time::paused, the game's own pause.
    [[nodiscard]] bool paused() const noexcept { return m_paused; }

    // Pauses or resumes. A resume makes the next tick() call only record its time.
    void setPaused(bool paused) noexcept;

    // One editor frame at `nowNs` (Input::frameTimeNs, nanoseconds on any steady clock). Running and
    // not paused, it ticks the game world with the time since the previous call, clamped by
    // tickGameWorld to [0, kMaxTickDelta]; the first call after a start or a resume only records
    // `nowNs`. Paused, it does nothing.
    void tick(std::uint64_t nowNs);

    // Runs `frames` ticks of exactly `seconds` each, at once, paused or not. Preconditions
    // (asserted; nothing runs otherwise), which ith.play.step checks as argument errors: 1 to
    // kMaxStepFrames frames, seconds in (0, kMaxTickDelta]. tick()'s recorded time doesn't change.
    void step(std::uint32_t frames, double seconds);

    // The game world's Time, or null when a system removed it (the next tick inserts a new one).
    [[nodiscard]] const Time* time() const;

    // The game world's entities without a StableId: those spawned during play. Linear in the number
    // of archetypes plus the number of such entities. Not const: World::query needs a mutable world.
    [[nodiscard]] std::size_t spawnedCount();

private:
    PlaySession(PlayMode mode, bool paused, std::uint64_t identity);

    void runTick(double seconds);

    PlayMode m_mode;
    bool m_possessed;
    bool m_paused;
    std::uint64_t m_identity;
    std::size_t m_viewport = 0;
    EditorCamera m_savedCamera;
    std::optional<EditorCamera> m_spectatorPose;
    World m_world;
    Schedule m_schedule;
    UndoHistory m_history{UndoLimits{.maxEntries = 0, .maxBytes = 0}};
    Selection m_selection;
    EditorChanges m_changes;
    Transactions m_transactions{m_world, m_history, m_selection, m_changes};
    serialization::EntityMapping m_mapping;
    // The time of the previous tick() call; empty after a start or a resume.
    std::optional<std::uint64_t> m_lastTickNs;
    // The game world's structure generation the counters last followed.
    std::uint64_t m_seenStructure = 0;
};

} // namespace ith
