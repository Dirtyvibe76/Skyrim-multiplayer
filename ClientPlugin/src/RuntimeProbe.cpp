#include "pch.h"

#include "ClientNetwork.h"
#include "RuntimeProbe.h"

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

        const auto state = ReadLocalPlayer();
        ClientNetwork::SubmitLocalPlayer(state);

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

        if (!changed) {
            return;
        }

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
}
