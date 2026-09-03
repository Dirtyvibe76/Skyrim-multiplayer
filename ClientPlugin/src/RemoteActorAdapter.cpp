#include "pch.h"

#include "RemoteActorAdapter.h"

#include <algorithm>
#include <deque>
#include <mutex>

namespace SkyrimMP
{
    namespace
    {
        constexpr std::size_t kMaxQueuedTransforms = 256;

        std::mutex g_queueMutex;
        std::deque<RemoteTransform> g_pendingTransforms;

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

        if (g_pendingTransforms.size() >= kMaxQueuedTransforms) {
            g_pendingTransforms.pop_front();
            logs::warn("[REMOTE ADAPTER] inbound queue full; dropped oldest transform");
        }

        g_pendingTransforms.push_back(a_transform);
    }

    std::size_t RemoteActorAdapter::ApplyPending(std::size_t a_budget)
    {
        if (a_budget == 0) {
            return 0;
        }

        std::deque<RemoteTransform> batch;
        {
            std::scoped_lock lock(g_queueMutex);
            const auto count = (std::min)(a_budget, g_pendingTransforms.size());
            for (std::size_t i = 0; i < count; ++i) {
                batch.push_back(g_pendingTransforms.front());
                g_pendingTransforms.pop_front();
            }
        }

        std::size_t applied = 0;

        for (const auto& transform : batch) {
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
        g_pendingTransforms.clear();
        logs::info("[REMOTE ADAPTER] inbound queue reset");
    }
}
