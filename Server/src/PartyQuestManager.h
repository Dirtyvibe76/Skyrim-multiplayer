#pragma once

#include "ServerQuestProgram.h"

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    using PartyId = std::uint64_t;
    using CharacterId = std::uint64_t;

    struct PartyQuestInstance
    {
        std::string questKey;
        std::uint16_t currentStage{};
        std::uint64_t revision{ 1 };
        bool complete{};
        std::set<CharacterId> eligibleMembers;
        std::set<CharacterId> rewardedMembers;
    };

    struct PartyState
    {
        PartyId id{};
        CharacterId leader{};
        std::set<CharacterId> members;
        std::map<std::string, PartyQuestInstance> quests;
    };

    struct FrozenPersonalQuest
    {
        CharacterId character{};
        std::string questKey;
        std::uint16_t stage{};
        std::uint64_t sourceRevision{};
        bool complete{};
    };

    class PartyQuestManager
    {
    public:
        explicit PartyQuestManager(const ServerQuestProgramDatabase& a_programs);

        PartyId CreateParty(CharacterId a_leader);
        bool JoinParty(PartyId a_party, CharacterId a_character);
        bool LeaveParty(PartyId a_party, CharacterId a_character);
        bool StartSharedQuest(PartyId a_party, const std::string& a_questKey);
        bool AdvanceSharedQuest(PartyId a_party, const std::string& a_questKey, CharacterId a_actor, std::uint16_t a_requestedStage);
        bool CompleteSharedQuest(PartyId a_party, const std::string& a_questKey, CharacterId a_actor);
        bool RecordReward(PartyId a_party, const std::string& a_questKey, CharacterId a_character);

        const PartyState* FindParty(PartyId a_party) const;
        const FrozenPersonalQuest* FindFrozenQuest(CharacterId a_character, const std::string& a_questKey) const;

    private:
        const ServerQuestProgramDatabase* programs_{};
        std::map<PartyId, PartyState> parties_;
        std::map<std::pair<CharacterId, std::string>, FrozenPersonalQuest> frozen_;
        PartyId nextPartyId_{ 1 };
    };

    void RunPartyQuestManagerSelfTest();
}
