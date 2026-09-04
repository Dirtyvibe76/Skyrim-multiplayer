#include "PartyQuestManager.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    PartyQuestManager::PartyQuestManager(const ServerQuestProgramDatabase& programs) : programs_(&programs) {}

    PartyId PartyQuestManager::CreateParty(CharacterId leader)
    {
        if (!leader) throw std::runtime_error("party leader identity cannot be zero");
        for (const auto& [id, party] : parties_) {
            if (party.members.contains(leader)) return id;
        }
        const auto id = nextPartyId_++;
        parties_.emplace(id, PartyState{ id, leader, { leader }, {} });
        return id;
    }

    bool PartyQuestManager::JoinParty(PartyId partyId, CharacterId character)
    {
        if (!character) return false;
        for (const auto& [id, party] : parties_) {
            if (id != partyId && party.members.contains(character)) return false;
        }
        const auto it = parties_.find(partyId);
        if (it == parties_.end()) return false;
        return it->second.members.insert(character).second;
    }

    bool PartyQuestManager::LeaveParty(PartyId partyId, CharacterId character)
    {
        const auto it = parties_.find(partyId);
        if (it == parties_.end() || !it->second.members.contains(character)) return false;
        for (const auto& [questKey, quest] : it->second.quests) {
            frozen_[{ character, questKey }] = FrozenPersonalQuest{
                character, questKey, quest.currentStage, quest.revision, quest.complete
            };
        }
        it->second.members.erase(character);
        for (auto& [questKey, quest] : it->second.quests) quest.eligibleMembers.erase(character);
        if (it->second.members.empty()) {
            parties_.erase(it);
        } else if (it->second.leader == character) {
            it->second.leader = *it->second.members.begin();
        }
        return true;
    }

    bool PartyQuestManager::StartSharedQuest(PartyId partyId, const std::string& questKey)
    {
        const auto party = parties_.find(partyId);
        const auto program = programs_->programs.find(questKey);
        if (party == parties_.end() || program == programs_->programs.end() || program->second.stages.empty()) return false;
        const auto initialStage = program->second.stages.front().index;
        return party->second.quests.emplace(questKey, PartyQuestInstance{
            questKey, initialStage, 1, false, party->second.members, {}
        }).second;
    }

    bool PartyQuestManager::AdvanceSharedQuest(
        PartyId partyId, const std::string& questKey, CharacterId actor, std::uint16_t requestedStage)
    {
        const auto party = parties_.find(partyId);
        if (party == parties_.end() || !party->second.members.contains(actor)) return false;
        const auto quest = party->second.quests.find(questKey);
        const auto program = programs_->programs.find(questKey);
        if (quest == party->second.quests.end() || program == programs_->programs.end() || quest->second.complete) return false;
        const auto current = std::find_if(program->second.stages.begin(), program->second.stages.end(), [&](const auto& stage) {
            return stage.index == quest->second.currentStage;
        });
        if (current == program->second.stages.end() || !current->nextStage || *current->nextStage != requestedStage) return false;
        quest->second.currentStage = requestedStage;
        ++quest->second.revision;
        return true;
    }

    bool PartyQuestManager::CompleteSharedQuest(PartyId partyId, const std::string& questKey, CharacterId actor)
    {
        const auto party = parties_.find(partyId);
        if (party == parties_.end() || !party->second.members.contains(actor)) return false;
        const auto quest = party->second.quests.find(questKey);
        const auto program = programs_->programs.find(questKey);
        if (quest == party->second.quests.end() || program == programs_->programs.end() ||
            quest->second.complete || program->second.stages.empty() ||
            quest->second.currentStage != program->second.stages.back().index) return false;
        quest->second.complete = true;
        quest->second.eligibleMembers = party->second.members;
        ++quest->second.revision;
        return true;
    }

    bool PartyQuestManager::RecordReward(PartyId partyId, const std::string& questKey, CharacterId character)
    {
        const auto party = parties_.find(partyId);
        if (party == parties_.end()) return false;
        const auto quest = party->second.quests.find(questKey);
        if (quest == party->second.quests.end() || !quest->second.complete ||
            !quest->second.eligibleMembers.contains(character)) return false;
        return quest->second.rewardedMembers.insert(character).second;
    }

    const PartyState* PartyQuestManager::FindParty(PartyId partyId) const
    {
        const auto it = parties_.find(partyId);
        return it == parties_.end() ? nullptr : &it->second;
    }

    const FrozenPersonalQuest* PartyQuestManager::FindFrozenQuest(CharacterId character, const std::string& questKey) const
    {
        const auto it = frozen_.find({ character, questKey });
        return it == frozen_.end() ? nullptr : &it->second;
    }

    void RunPartyQuestManagerSelfTest()
    {
        ServerQuestProgramDatabase programs;
        ServerQuestProgram program;
        program.stableKey = "F:0:1";
        program.stages = { { 10, 20, 1, 0 }, { 20, 30, 1, 0 }, { 30, std::nullopt, 1, 0 } };
        programs.programs.emplace(program.stableKey, program);
        PartyQuestManager manager(programs);
        const auto party = manager.CreateParty(100);
        if (!manager.JoinParty(party, 200) || !manager.StartSharedQuest(party, program.stableKey) ||
            manager.AdvanceSharedQuest(party, program.stableKey, 999, 20) ||
            manager.AdvanceSharedQuest(party, program.stableKey, 100, 30) ||
            !manager.AdvanceSharedQuest(party, program.stableKey, 100, 20) || !manager.LeaveParty(party, 200)) {
            throw std::runtime_error("party quest membership or transition self-test failed");
        }
        const auto* frozen = manager.FindFrozenQuest(200, program.stableKey);
        const auto* shared = manager.FindParty(party);
        if (!frozen || frozen->stage != 20 || !manager.JoinParty(party, 300) || !shared ||
            shared->quests.at(program.stableKey).currentStage != 20 ||
            !manager.AdvanceSharedQuest(party, program.stableKey, 100, 30) ||
            !manager.CompleteSharedQuest(party, program.stableKey, 100) || !manager.RecordReward(party, program.stableKey, 100) ||
            manager.RecordReward(party, program.stableKey, 100) || manager.RecordReward(party, program.stableKey, 200)) {
            throw std::runtime_error("party quest freeze, completion, or reward self-test failed");
        }
        std::cout << "[PARTY-QUEST-SELFTEST] membership=true sharedInstance=true validatedTransitions=true lateJoinProjection=true leaveFreeze=true completion=true rewardIdempotency=true\n";
    }
}
