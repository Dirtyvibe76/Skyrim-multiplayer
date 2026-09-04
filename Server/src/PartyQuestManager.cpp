#include "PartyQuestManager.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
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

    void PartyQuestManager::Save(const std::filesystem::path& path) const
    {
        if (path.empty()) throw std::runtime_error("party quest persistence path is empty");
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp";
        {
            std::ofstream output(temporary, std::ios::trunc);
            if (!output) throw std::runtime_error("failed to open party quest temporary persistence file");
            output << "SKYRIMMP_PARTY_QUESTS 1\n" << "NEXT " << nextPartyId_ << '\n';
            for (const auto& [id, party] : parties_) {
                output << "PARTY " << id << ' ' << party.leader << ' ' << party.members.size();
                for (const auto member : party.members) output << ' ' << member;
                output << ' ' << party.quests.size() << '\n';
                for (const auto& [questKey, quest] : party.quests) {
                    output << "QUEST " << std::quoted(questKey) << ' ' << quest.currentStage << ' '
                           << quest.revision << ' ' << quest.complete << ' ' << quest.eligibleMembers.size();
                    for (const auto member : quest.eligibleMembers) output << ' ' << member;
                    output << ' ' << quest.rewardedMembers.size();
                    for (const auto member : quest.rewardedMembers) output << ' ' << member;
                    output << '\n';
                }
            }
            for (const auto& [key, frozen] : frozen_) {
                output << "FROZEN " << frozen.character << ' ' << std::quoted(frozen.questKey) << ' '
                       << frozen.stage << ' ' << frozen.sourceRevision << ' ' << frozen.complete << '\n';
            }
            output << "END\n";
            if (!output) throw std::runtime_error("failed to write party quest persistence file");
        }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::filesystem::remove(path, error);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
        if (error) throw std::runtime_error("failed to replace party quest persistence file: " + error.message());
    }

    void PartyQuestManager::Load(const std::filesystem::path& path)
    {
        if (!std::filesystem::exists(path)) return;
        std::ifstream input(path);
        std::string magic;
        unsigned version{};
        if (!(input >> magic >> version) || magic != "SKYRIMMP_PARTY_QUESTS" || version != 1) {
            throw std::runtime_error("unsupported party quest persistence format");
        }
        std::map<PartyId, PartyState> parties;
        std::map<std::pair<CharacterId, std::string>, FrozenPersonalQuest> frozen;
        PartyId nextParty{};
        std::string token;
        if (!(input >> token >> nextParty) || token != "NEXT" || !nextParty) {
            throw std::runtime_error("malformed party quest NEXT record");
        }
        PartyState* currentParty = nullptr;
        std::size_t remainingQuests{};
        while (input >> token) {
            if (token == "END") break;
            if (token == "PARTY") {
                if (remainingQuests != 0) throw std::runtime_error("persisted party omitted quest records");
                PartyState party;
                std::size_t memberCount{};
                if (!(input >> party.id >> party.leader >> memberCount) || !party.id || !party.leader || !memberCount || memberCount > 64) {
                    throw std::runtime_error("malformed persisted party");
                }
                for (std::size_t i = 0; i < memberCount; ++i) {
                    CharacterId member{};
                    if (!(input >> member) || !member || !party.members.insert(member).second) throw std::runtime_error("invalid persisted party member");
                }
                if (!party.members.contains(party.leader) || !(input >> remainingQuests) || remainingQuests > 4096) {
                    throw std::runtime_error("invalid persisted party leader or quest count");
                }
                const auto [it, inserted] = parties.emplace(party.id, std::move(party));
                if (!inserted) throw std::runtime_error("duplicate persisted party id");
                currentParty = &it->second;
            } else if (token == "QUEST") {
                if (!currentParty || remainingQuests == 0) throw std::runtime_error("orphan persisted party quest");
                PartyQuestInstance quest;
                std::size_t eligibleCount{}, rewardedCount{};
                if (!(input >> std::quoted(quest.questKey) >> quest.currentStage >> quest.revision >> quest.complete >> eligibleCount) ||
                    !programs_->programs.contains(quest.questKey) || !quest.revision || eligibleCount > 64) {
                    throw std::runtime_error("invalid persisted party quest");
                }
                for (std::size_t i = 0; i < eligibleCount; ++i) {
                    CharacterId member{};
                    if (!(input >> member) || !member || !quest.eligibleMembers.insert(member).second) throw std::runtime_error("invalid persisted eligible member");
                }
                if (!(input >> rewardedCount) || rewardedCount > eligibleCount) throw std::runtime_error("invalid persisted reward count");
                for (std::size_t i = 0; i < rewardedCount; ++i) {
                    CharacterId member{};
                    if (!(input >> member) || !quest.eligibleMembers.contains(member) || !quest.rewardedMembers.insert(member).second) {
                        throw std::runtime_error("invalid persisted rewarded member");
                    }
                }
                const auto& program = programs_->programs.at(quest.questKey);
                if (std::none_of(program.stages.begin(), program.stages.end(), [&](const auto& stage) { return stage.index == quest.currentStage; }) ||
                    !currentParty->quests.emplace(quest.questKey, std::move(quest)).second) {
                    throw std::runtime_error("invalid persisted quest stage or duplicate quest");
                }
                --remainingQuests;
            } else if (token == "FROZEN") {
                if (remainingQuests != 0) throw std::runtime_error("persisted party omitted quest records");
                FrozenPersonalQuest quest;
                if (!(input >> quest.character >> std::quoted(quest.questKey) >> quest.stage >> quest.sourceRevision >> quest.complete) ||
                    !quest.character || !quest.sourceRevision || !programs_->programs.contains(quest.questKey) ||
                    !frozen.emplace(std::pair{ quest.character, quest.questKey }, quest).second) {
                    throw std::runtime_error("invalid persisted frozen quest");
                }
                const auto& program = programs_->programs.at(quest.questKey);
                if (std::none_of(program.stages.begin(), program.stages.end(), [&](const auto& stage) { return stage.index == quest.stage; })) {
                    throw std::runtime_error("invalid persisted frozen quest stage");
                }
            } else {
                throw std::runtime_error("unknown party quest persistence record");
            }
        }
        if (token != "END" || remainingQuests != 0) throw std::runtime_error("incomplete party quest persistence file");
        PartyId maximum{};
        for (const auto& [id, party] : parties) maximum = std::max(maximum, id);
        if (nextParty <= maximum) throw std::runtime_error("persisted next party id would collide");
        parties_ = std::move(parties);
        frozen_ = std::move(frozen);
        nextPartyId_ = nextParty;
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
        const auto persistencePath = std::filesystem::temp_directory_path() /
            ("skyrimmp-party-quest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".state");
        manager.Save(persistencePath);
        PartyQuestManager restored(programs);
        restored.Load(persistencePath);
        std::error_code cleanupError;
        std::filesystem::remove(persistencePath, cleanupError);
        const auto* restoredParty = restored.FindParty(party);
        const auto* restoredFrozen = restored.FindFrozenQuest(200, program.stableKey);
        if (!restoredParty || !restoredFrozen || !restoredParty->quests.at(program.stableKey).complete ||
            !restoredParty->quests.at(program.stableKey).rewardedMembers.contains(100) ||
            restored.RecordReward(party, program.stableKey, 100)) {
            throw std::runtime_error("party quest restart persistence self-test failed");
        }
        std::cout << "[PARTY-QUEST-SELFTEST] membership=true sharedInstance=true validatedTransitions=true lateJoinProjection=true leaveFreeze=true completion=true rewardIdempotency=true restartPersistence=true\n";
    }
}
