#include "pch.h"

#include "RemoteActorAdapter.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        constexpr std::size_t kMaxPendingEntities = 256;

        std::mutex g_queueMutex;
        std::deque<std::uint32_t> g_pendingOrder;
        std::unordered_map<std::uint32_t, RemoteTransform> g_pendingByEntity;
        std::unordered_map<std::uint32_t, std::uint32_t> g_lastAppliedSequence;

        bool IsNewerSequence(std::uint32_t a_candidate, std::uint32_t a_reference)
        {
            // RFC-style serial arithmetic for a wrapping 32-bit sequence space.
            // Equal values are duplicates; a positive signed delta is newer.
            return static_cast<std::int32_t>(a_candidate - a_reference) > 0;
        }

        float ClampPitch(float a_pitch)
        {
            constexpr float kPi = 3.14159265358979323846f;
            constexpr float kLimit = kPi * 0.5f;
            return std::clamp(a_pitch, -kLimit, kLimit);
        }

        float NormalizeYaw(float a_yaw)
        {
            constexpr float kPi = 3.14159265358979323846f;
            constexpr float kTwoPi = 2.0f * kPi;

            float normalized = std::fmod(a_yaw, kTwoPi);
            if (normalized < 0.0f) {
                normalized += kTwoPi;
            }
            return normalized;
        }
    }

    void RemoteActorAdapter::Enqueue(const RemoteTransform& a_transform)
    {
        if (a_transform.runtimeFormId == 0) {
            return;
        }

        std::scoped_lock lock(g_queueMutex);

        if (const auto appliedIt = g_lastAppliedSequence.find(a_transform.runtimeFormId);
            appliedIt != g_lastAppliedSequence.end() &&
            !IsNewerSequence(a_transform.sequence, appliedIt->second)) {

            logs::info(
                "[REMOTE ADAPTER STALE] form={:08X} seq={} lastApplied={} reason=not newer than applied state",
                a_transform.runtimeFormId,
                a_transform.sequence,
                appliedIt->second);
            return;
        }

        if (const auto pendingIt = g_pendingByEntity.find(a_transform.runtimeFormId);
            pendingIt != g_pendingByEntity.end()) {

            if (!IsNewerSequence(a_transform.sequence, pendingIt->second.sequence)) {
                logs::info(
                    "[REMOTE ADAPTER STALE] form={:08X} seq={} pending={} reason=not newer than pending state",
                    a_transform.runtimeFormId,
                    a_transform.sequence,
                    pendingIt->second.sequence);
                return;
            }

            logs::info(
                "[REMOTE ADAPTER COALESCE] form={:08X} seq={}->{}",
                a_transform.runtimeFormId,
                pendingIt->second.sequence,
                a_transform.sequence);
            pendingIt->second = a_transform;
            return;
        }

        if (g_pendingByEntity.size() >= kMaxPendingEntities) {
            const auto droppedFormId = g_pendingOrder.front();
            g_pendingOrder.pop_front();

            const auto droppedIt = g_pendingByEntity.find(droppedFormId);
            if (droppedIt != g_pendingByEntity.end()) {
                logs::warn(
                    "[REMOTE ADAPTER DROP] form={:08X} seq={} reason=pending entity capacity",
                    droppedIt->second.runtimeFormId,
                    droppedIt->second.sequence);
                g_pendingByEntity.erase(droppedIt);
            }
        }

        g_pendingOrder.push_back(a_transform.runtimeFormId);
        g_pendingByEntity.insert_or_assign(a_transform.runtimeFormId, a_transform);
    }

    std::size_t RemoteActorAdapter::ApplyPending(std::size_t a_budget)
    {
        if (a_budget == 0) {
            return 0;
        }

        std::deque<RemoteTransform> batch;
        {
            std::scoped_lock lock(g_queueMutex);

            while (!g_pendingOrder.empty() && batch.size() < a_budget) {
                const auto runtimeFormId = g_pendingOrder.front();
                g_pendingOrder.pop_front();

                const auto pendingIt = g_pendingByEntity.find(runtimeFormId);
                if (pendingIt == g_pendingByEntity.end()) {
                    continue;
                }

                batch.push_back(pendingIt->second);
                g_pendingByEntity.erase(pendingIt);
            }
        }

        std::size_t applied = 0;

        for (const auto& transform : batch) {
            {
                std::scoped_lock lock(g_queueMutex);
                if (const auto appliedIt = g_lastAppliedSequence.find(transform.runtimeFormId);
                    appliedIt != g_lastAppliedSequence.end() &&
                    !IsNewerSequence(transform.sequence, appliedIt->second)) {

                    logs::info(
                        "[REMOTE ADAPTER STALE] form={:08X} seq={} lastApplied={} reason=became stale before apply",
                        transform.runtimeFormId,
                        transform.sequence,
                        appliedIt->second);
                    continue;
                }
            }

            auto* form = RE::TESForm::LookupByID(transform.runtimeFormId);
            auto* actor = form ? form->As<RE::Actor>() : nullptr;
            if (!actor || !actor->Get3D()) {
                logs::warn(
                    "[REMOTE ADAPTER DROP] form={:08X} seq={} reason=actor unavailable",
                    transform.runtimeFormId,
                    transform.sequence);
                continue;
            }

            const RE::NiPoint3 requestedPosition{
                transform.position.x,
                transform.position.y,
                transform.position.z
            };

            const float requestedPitch = ClampPitch(transform.rotation.x);
            const float requestedYaw = NormalizeYaw(transform.rotation.z);

            actor->SetPosition(requestedPosition, true);
            actor->data.angle.x = requestedPitch;
            actor->data.angle.y = transform.rotation.y;
            actor->data.angle.z = requestedYaw;
            actor->Update3DPosition(true);

            const auto observedPosition = actor->GetPosition();
            const auto observedAngle = actor->GetAngle();

            {
                std::scoped_lock lock(g_queueMutex);
                const auto appliedIt = g_lastAppliedSequence.find(transform.runtimeFormId);
                if (appliedIt == g_lastAppliedSequence.end() ||
                    IsNewerSequence(transform.sequence, appliedIt->second)) {
                    g_lastAppliedSequence.insert_or_assign(transform.runtimeFormId, transform.sequence);
                }
            }

            logs::info(
                "[REMOTE ADAPTER APPLIED] form={:08X} seq={} reqPos=({:.2f},{:.2f},{:.2f}) obsPos=({:.2f},{:.2f},{:.2f}) reqRot=({:.3f},{:.3f},{:.3f}) obsRot=({:.3f},{:.3f},{:.3f})",
                transform.runtimeFormId,
                transform.sequence,
                requestedPosition.x,
                requestedPosition.y,
                requestedPosition.z,
                observedPosition.x,
                observedPosition.y,
                observedPosition.z,
                requestedPitch,
                transform.rotation.y,
                requestedYaw,
                observedAngle.x,
                observedAngle.y,
                observedAngle.z);

            ++applied;
        }

        return applied;
    }

    void RemoteActorAdapter::Reset()
    {
        std::scoped_lock lock(g_queueMutex);
        g_pendingOrder.clear();
        g_pendingByEntity.clear();
        g_lastAppliedSequence.clear();
        logs::info("[REMOTE ADAPTER] inbound latest-state buffer reset");
    }
}
