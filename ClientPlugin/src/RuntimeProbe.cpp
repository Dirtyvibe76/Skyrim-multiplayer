#include "pch.h"

#include "ClientNetwork.h"
#include "GameplayEventProbe.h"
#include "RuntimeProbe.h"

#include <algorithm>

namespace SkyrimMP
{
    namespace
    {
        bool MeaningfullyDifferent(float left, float right, float epsilon)
        {
            return std::abs(left - right) > epsilon;
        }

        std::uint64_t CharacterId(const RE::PlayerCharacter& player)
        {
            if (const auto* saves = RE::BGSSaveLoadManager::GetSingleton(); saves && saves->currentCharacterID != 0) {
                return saves->currentCharacterID;
            }

            // Older/edge runtimes can briefly expose no save-manager ID. Keep
            // a stable fallback based on immutable character-creation traits.
            constexpr std::uint64_t offset = 14695981039346656037ull;
            constexpr std::uint64_t prime = 1099511628211ull;
            auto hash = offset;
            const auto* name = player.GetName();
            for (const auto* p = name; p && *p; ++p) {
                auto c = static_cast<unsigned char>(*p);
                if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + ('a' - 'A'));
                hash = (hash ^ c) * prime;
            }
            if (const auto* race = player.GetRace()) hash = (hash ^ race->GetFormID()) * prime;
            if (const auto* base = player.GetActorBase()) {
                hash = (hash ^ static_cast<std::uint64_t>(base->GetSex())) * prime;
            }
            return hash == 0 ? 1 : hash;
        }

        std::uint64_t AppearanceRevision(const PlayerAppearance& appearance)
        {
            constexpr std::uint64_t offset = 14695981039346656037ull;
            constexpr std::uint64_t prime = 1099511628211ull;
            auto hash = offset;
            const auto mix = [&](std::uint64_t value) mutable {
                for (std::size_t i = 0; i < sizeof(value); ++i) {
                    hash = (hash ^ static_cast<std::uint8_t>(value >> (i * 8))) * prime;
                }
            };
            for (const auto c : appearance.displayName) mix(static_cast<std::uint8_t>(c));
            mix(appearance.raceFormId);
            mix(appearance.sex);
            mix(std::bit_cast<std::uint32_t>(appearance.weight));
            mix(appearance.hairColorFormId);
            mix(appearance.faceDetailsFormId);
            mix(appearance.bodyTintColor);
            for (const auto formId : appearance.headPartFormIds) mix(formId);
            for (const auto value : appearance.faceMorphs) mix(std::bit_cast<std::uint32_t>(value));
            for (const auto value : appearance.faceParts) mix(static_cast<std::uint32_t>(value));
            return hash == 0 ? 1 : hash;
        }

        PlayerAppearance CaptureAppearance(RE::PlayerCharacter& player)
        {
            PlayerAppearance appearance;
            auto* base = player.GetActorBase();
            auto* race = player.GetRace();
            if (!base || !race) return appearance;

            appearance.valid = true;
            appearance.displayName = player.GetName() ? player.GetName() : "Player";
            if (appearance.displayName.size() > 63) appearance.displayName.resize(63);
            appearance.raceFormId = race->GetFormID();
            appearance.sex = static_cast<std::uint8_t>(base->GetSex());
            appearance.weight = base->weight;
            appearance.bodyTintColor =
                static_cast<std::uint32_t>(base->bodyTintColor.red) |
                (static_cast<std::uint32_t>(base->bodyTintColor.green) << 8) |
                (static_cast<std::uint32_t>(base->bodyTintColor.blue) << 16);

            if (base->headRelatedData) {
                if (base->headRelatedData->hairColor) appearance.hairColorFormId = base->headRelatedData->hairColor->GetFormID();
                if (base->headRelatedData->faceDetails) appearance.faceDetailsFormId = base->headRelatedData->faceDetails->GetFormID();
            }

            const auto headPartCount = std::clamp<int>(base->numHeadParts, 0, 32);
            appearance.headPartFormIds.reserve(static_cast<std::size_t>(headPartCount));
            for (int i = 0; i < headPartCount; ++i) {
                if (base->headParts && base->headParts[i]) appearance.headPartFormIds.push_back(base->headParts[i]->GetFormID());
            }

            if (base->faceData) {
                for (std::size_t i = 0; i < appearance.faceMorphs.size(); ++i) appearance.faceMorphs[i] = base->faceData->morphs[i];
                for (std::size_t i = 0; i < appearance.faceParts.size(); ++i) appearance.faceParts[i] = base->faceData->parts[i];
            }

            appearance.revision = AppearanceRevision(appearance);
            return appearance;
        }
    }

    PlayerState RuntimeProbe::ReadLocalPlayer()
    {
        PlayerState state{};

        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return state;
        }

        state.characterId = CharacterId(*player);
        state.formId = player->GetFormID();
        state.appearance = CaptureAppearance(*player);

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

        const auto gameplay = GameplayEventProbe::Snapshot();
        state.dead = gameplay.dead;
        state.inCombat = gameplay.inCombat;
        state.hasStatusState = gameplay.valid;
        state.hasEquipmentState = gameplay.equipmentValid;
        if (player->IsWeaponDrawn()) state.actionFlags |= kWeaponDrawn;
        if (player->IsMoving()) state.actionFlags |= kMoving;
        if (player->IsRunning()) state.actionFlags |= kRunning;
        if (player->IsSprinting()) state.actionFlags |= kSprinting;
        if (player->IsSneaking()) state.actionFlags |= kSneaking;
        if (player->IsInJumpState()) state.actionFlags |= kJumping;
        if (player->IsAttacking()) state.actionFlags |= kAttacking;
        if (player->IsBlocking()) state.actionFlags |= kBlocking;
        bool casting = false;
        if (player->GetGraphVariableBool("IsCasting", casting) && casting) state.actionFlags |= kCasting;
        state.equippedFormIds = gameplay.equippedFormIds;

        return state;
    }

    void RuntimeProbe::LogLocalPlayer()
    {
        static PlayerState previous{};
        static bool firstSample = true;

        const auto state = ReadLocalPlayer();
        ClientNetwork::SubmitLocalPlayer(state);

        if (state.appearance.valid && (firstSample || state.appearance.revision != previous.appearance.revision)) {
            logs::info(
                "[APPEARANCE-CAPTURE] revision={:016X} name={} race={:08X} sex={} weight={:.3f} headParts={}",
                state.appearance.revision,
                state.appearance.displayName,
                state.appearance.raceFormId,
                state.appearance.sex,
                state.appearance.weight,
                state.appearance.headPartFormIds.size());
        }

        const bool changed =
            firstSample ||
            state.formId != previous.formId ||
            state.cellFormId != previous.cellFormId ||
            state.worldspaceFormId != previous.worldspaceFormId ||
            MeaningfullyDifferent(state.position.x, previous.position.x, 0.05f) ||
            MeaningfullyDifferent(state.position.y, previous.position.y, 0.05f) ||
            MeaningfullyDifferent(state.position.z, previous.position.z, 0.05f) ||
            MeaningfullyDifferent(state.rotation.x, previous.rotation.x, 0.002f) ||
            MeaningfullyDifferent(state.rotation.y, previous.rotation.y, 0.002f) ||
            MeaningfullyDifferent(state.rotation.z, previous.rotation.z, 0.002f) ||
            state.actionFlags != previous.actionFlags ||
            state.equippedFormIds != previous.equippedFormIds ||
            state.appearance.revision != previous.appearance.revision;

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
