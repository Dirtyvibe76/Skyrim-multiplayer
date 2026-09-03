#include "pch.h"

#include "RuntimeProbe.h"

#include <cmath>

namespace SkyrimMP
{
    PlayerState RuntimeProbe::ReadLocalPlayer()
    {
        PlayerState state{};

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return state;
        }

        state.formId = player->GetFormID();

        const auto position = player->GetPosition();
        state.position = {
            position.x,
            position.y,
            position.z
        };

        const auto angle = player->GetAngle();
        state.rotation = {
            angle.x,
            angle.y,
            angle.z
        };

        if (auto* cell = player->GetParentCell()) {
            state.cellFormId = cell->GetFormID();

            if (auto* worldspace = cell->GetRuntimeData().worldSpace) {
                state.worldspaceFormId = worldspace->GetFormID();
            }
        }

        return state;
    }

    void RuntimeProbe::LogLocalPlayer()
    {
        static PlayerState previous{};
        static bool firstSample = true;
        static bool firstLookSample = true;
        static float previousAimPitch = 0.0f;
        static float previousAimHeading = 0.0f;

        const auto state = ReadLocalPlayer();

        auto* player = RE::PlayerCharacter::GetSingleton();
        float aimPitchCurrent = 0.0f;
        float aimHeadingCurrent = 0.0f;
        bool haveAimPitch = false;
        bool haveAimHeading = false;

        if (player) {
            if (auto* fixedStrings = RE::FixedStrings::GetSingleton()) {
                haveAimPitch = player->GetGraphVariableFloat(
                    fixedStrings->aimPitchCurrent,
                    aimPitchCurrent);
                haveAimHeading = player->GetGraphVariableFloat(
                    fixedStrings->aimHeadingCurrent,
                    aimHeadingCurrent);
            }
        }

        const bool changed =
            firstSample ||
            state.formId != previous.formId ||
            state.cellFormId != previous.cellFormId ||
            state.worldspaceFormId != previous.worldspaceFormId ||
            state.position.x != previous.position.x ||
            state.position.y != previous.position.y ||
            state.position.z != previous.position.z ||
            state.rotation.x != previous.rotation.x ||
            state.rotation.y != previous.rotation.y ||
            state.rotation.z != previous.rotation.z;

        if (changed) {
            logs::info(
                "[PLAYER] form={:08X} cell={:08X} world={:08X} "
                "pos=({:.2f},{:.2f},{:.2f}) "
                "rot=({:.3f},{:.3f},{:.3f})",
                state.formId,
                state.cellFormId,
                state.worldspaceFormId,
                state.position.x,
                state.position.y,
                state.position.z,
                state.rotation.x,
                state.rotation.y,
                state.rotation.z);

            if (!firstSample) {
                if (state.cellFormId != previous.cellFormId) {
                    logs::info(
                        "[CELL CHANGE] {:08X} -> {:08X}",
                        previous.cellFormId,
                        state.cellFormId);
                }

                if (state.worldspaceFormId != previous.worldspaceFormId) {
                    logs::info(
                        "[WORLDSPACE CHANGE] {:08X} -> {:08X}",
                        previous.worldspaceFormId,
                        state.worldspaceFormId);
                }
            }

            previous = state;
            firstSample = false;
        }

        constexpr float kLookDeltaThreshold = 0.01f;
        const bool lookChanged =
            firstLookSample ||
            (haveAimPitch && std::fabs(aimPitchCurrent - previousAimPitch) >= kLookDeltaThreshold) ||
            (haveAimHeading && std::fabs(aimHeadingCurrent - previousAimHeading) >= kLookDeltaThreshold);

        if (lookChanged) {
            logs::info(
                "[RE-0.5c LOOK] pitch={} value={:.4f} heading={} value={:.4f} bodyRot=({:.3f},{:.3f},{:.3f})",
                haveAimPitch ? "ok" : "missing",
                aimPitchCurrent,
                haveAimHeading ? "ok" : "missing",
                aimHeadingCurrent,
                state.rotation.x,
                state.rotation.y,
                state.rotation.z);

            if (haveAimPitch) {
                previousAimPitch = aimPitchCurrent;
            }
            if (haveAimHeading) {
                previousAimHeading = aimHeadingCurrent;
            }
            firstLookSample = false;
        }
    }
}
