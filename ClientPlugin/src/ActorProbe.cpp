#include "pch.h"
#include "ActorProbe.h"

#include <cmath>
#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        constexpr float kExteriorRadius = 12000.0f;
        constexpr float kExteriorRadiusSq = kExteriorRadius * kExteriorRadius;

        constexpr float kMoveThreshold = 64.0f;
        constexpr float kMoveThresholdSq = kMoveThreshold * kMoveThreshold;

        constexpr float kRotationThreshold = 0.05f;
        constexpr float kHealthThreshold = 0.1f;

        struct ActorSnapshot
        {
            std::uint32_t formId{};
            std::uint32_t baseFormId{};
            std::uint32_t cellFormId{};
            std::uint32_t worldspaceFormId{};

            float x{};
            float y{};
            float z{};

            float rotX{};
            float rotY{};
            float rotZ{};

            float health{};

            bool dead{};
        };

        std::unordered_map<std::uint32_t, ActorSnapshot> g_previousActors;

        float DistanceSquared(
            float ax,
            float ay,
            float az,
            float bx,
            float by,
            float bz)
        {
            const float dx = ax - bx;
            const float dy = ay - by;
            const float dz = az - bz;

            return dx * dx + dy * dy + dz * dz;
        }

        bool RotationChanged(
            const ActorSnapshot& current,
            const ActorSnapshot& previous)
        {
            return
                std::fabs(current.rotX - previous.rotX) > kRotationThreshold ||
                std::fabs(current.rotY - previous.rotY) > kRotationThreshold ||
                std::fabs(current.rotZ - previous.rotZ) > kRotationThreshold;
        }

        bool PositionChanged(
            const ActorSnapshot& current,
            const ActorSnapshot& previous)
        {
            return DistanceSquared(
                current.x,
                current.y,
                current.z,
                previous.x,
                previous.y,
                previous.z) > kMoveThresholdSq;
        }
    }

    void ActorProbe::Sample()
    {
        static auto readyAt = std::chrono::steady_clock::time_point{};
        static auto lastSample = std::chrono::steady_clock::time_point{};

        const auto now = std::chrono::steady_clock::now();

        if (readyAt.time_since_epoch().count() == 0) {
            readyAt = now + 5s;
            logs::info("[RE-0.4] actor probe warmup started");
            return;
        }

        if (now < readyAt) {
            return;
        }

        if (lastSample.time_since_epoch().count() != 0 &&
            now - lastSample < 500ms) {
            return;
        }

        lastSample = now;

        auto* player = RE::PlayerCharacter::GetSingleton();
        auto* processLists = RE::ProcessLists::GetSingleton();

        if (!player || !processLists) {
            return;
        }

        auto* playerCell = player->GetParentCell();

        if (!playerCell) {
            return;
        }

        const auto playerCellId = playerCell->GetFormID();

        RE::TESWorldSpace* playerWorldspace =
            playerCell->GetRuntimeData().worldSpace;

        const auto playerWorldspaceId =
            playerWorldspace ? playerWorldspace->GetFormID() : 0;

        const auto playerPosition = player->GetPosition();

        const bool playerIsExterior = playerWorldspace != nullptr;

        std::unordered_map<std::uint32_t, ActorSnapshot> currentActors;

        processLists->ForAllActors(
            [&](RE::Actor* actor)
            {
                if (!actor || actor == player) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }

                auto* cell = actor->GetParentCell();

                if (!cell) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }

                auto* actorWorldspace =
                    cell->GetRuntimeData().worldSpace;

                const auto actorCellId = cell->GetFormID();

                const auto actorWorldspaceId =
                    actorWorldspace ? actorWorldspace->GetFormID() : 0;

                const auto position = actor->GetPosition();

                bool relevant = false;

                if (playerIsExterior) {
                    if (actorWorldspaceId == playerWorldspaceId) {
                        const float distanceSq = DistanceSquared(
                            position.x,
                            position.y,
                            position.z,
                            playerPosition.x,
                            playerPosition.y,
                            playerPosition.z);

                        relevant = distanceSq <= kExteriorRadiusSq;
                    }
                } else {
                    relevant = actorCellId == playerCellId;
                }

                if (!relevant) {
                    return RE::BSContainer::ForEachResult::kContinue;
                }

                ActorSnapshot snapshot{};

                snapshot.formId = actor->GetFormID();

                if (auto* base = actor->GetActorBase()) {
                    snapshot.baseFormId = base->GetFormID();
                }

                snapshot.cellFormId = actorCellId;
                snapshot.worldspaceFormId = actorWorldspaceId;

                snapshot.x = position.x;
                snapshot.y = position.y;
                snapshot.z = position.z;

                const auto angle = actor->GetAngle();

                snapshot.rotX = angle.x;
                snapshot.rotY = angle.y;
                snapshot.rotZ = angle.z;

                snapshot.health =
                    actor->GetActorValue(RE::ActorValue::kHealth);

                snapshot.dead = actor->IsDead();

                currentActors.emplace(snapshot.formId, snapshot);

                const auto previousIt =
                    g_previousActors.find(snapshot.formId);

                if (previousIt == g_previousActors.end()) {
                    logs::info(
                        "[ACTOR ADD] form={:08X} base={:08X} "
                        "cell={:08X} world={:08X} "
                        "pos=({:.2f},{:.2f},{:.2f}) "
                        "rot=({:.3f},{:.3f},{:.3f}) "
                        "health={:.2f} dead={}",
                        snapshot.formId,
                        snapshot.baseFormId,
                        snapshot.cellFormId,
                        snapshot.worldspaceFormId,
                        snapshot.x,
                        snapshot.y,
                        snapshot.z,
                        snapshot.rotX,
                        snapshot.rotY,
                        snapshot.rotZ,
                        snapshot.health,
                        snapshot.dead);

                    return RE::BSContainer::ForEachResult::kContinue;
                }

                const auto& previous = previousIt->second;

                if (PositionChanged(snapshot, previous) ||
                    RotationChanged(snapshot, previous) ||
                    snapshot.cellFormId != previous.cellFormId) {

                    logs::info(
                        "[ACTOR MOVE] form={:08X} "
                        "cell={:08X} "
                        "pos=({:.2f},{:.2f},{:.2f}) "
                        "rot=({:.3f},{:.3f},{:.3f})",
                        snapshot.formId,
                        snapshot.cellFormId,
                        snapshot.x,
                        snapshot.y,
                        snapshot.z,
                        snapshot.rotX,
                        snapshot.rotY,
                        snapshot.rotZ);
                }

                if (std::fabs(snapshot.health - previous.health) >
                        kHealthThreshold ||
                    snapshot.dead != previous.dead) {

                    logs::info(
                        "[ACTOR STATE] form={:08X} "
                        "health={:.2f} dead={}",
                        snapshot.formId,
                        snapshot.health,
                        snapshot.dead);
                }

                return RE::BSContainer::ForEachResult::kContinue;
            });

        for (const auto& [formId, previous] : g_previousActors) {
            if (!currentActors.contains(formId)) {
                logs::info(
                    "[ACTOR REMOVE] form={:08X} base={:08X}",
                    previous.formId,
                    previous.baseFormId);
            }
        }

        static std::size_t previousCount =
            static_cast<std::size_t>(-1);

        if (currentActors.size() != previousCount) {
            logs::info(
                "[ACTOR SET] relevant={} mode={} cell={:08X} world={:08X}",
                currentActors.size(),
                playerIsExterior ? "exterior" : "interior",
                playerCellId,
                playerWorldspaceId);

            previousCount = currentActors.size();
        }

        g_previousActors = std::move(currentActors);
    }
}
