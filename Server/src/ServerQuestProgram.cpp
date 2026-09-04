#include "ServerQuestProgram.h"

#include <algorithm>
#include <stdexcept>

namespace SkyrimMP::Server
{
    ServerQuestProgramDatabase CompileServerQuestPrograms(const ImportedQuestDatabase& imported)
    {
        ServerQuestProgramDatabase result;
        for (const auto& [stableKey, quest] : imported.byStableKey) {
            if (quest.compatibility == QuestCompatibility::AdapterRequired) {
                ++result.skippedAdapterRequired;
                continue;
            }
            if (quest.compatibility == QuestCompatibility::Unsupported) {
                ++result.skippedUnsupported;
                continue;
            }
            if (quest.stages.empty()) {
                ++result.skippedNoStages;
                continue;
            }

            ServerQuestProgram program;
            program.quest = quest.key;
            program.stableKey = stableKey;
            program.editorId = quest.editorId;
            for (const auto& stage : quest.stages) {
                const auto it = std::find_if(program.stages.begin(), program.stages.end(), [&](const auto& value) {
                    return value.index == stage.index;
                });
                if (it == program.stages.end()) {
                    program.stages.push_back({ stage.index, std::nullopt, stage.logEntries, stage.conditions });
                } else {
                    it->logEntries += stage.logEntries;
                    it->conditionCount += stage.conditions;
                    ++result.duplicateStageEntriesMerged;
                }
            }
            std::sort(program.stages.begin(), program.stages.end(), [](const auto& left, const auto& right) {
                return left.index < right.index;
            });
            for (std::size_t i = 0; i + 1 < program.stages.size(); ++i) {
                program.stages[i].nextStage = program.stages[i + 1].index;
            }
            for (const auto& objective : quest.objectives) program.objectives.push_back(objective.index);
            std::sort(program.objectives.begin(), program.objectives.end());
            program.objectives.erase(std::unique(program.objectives.begin(), program.objectives.end()), program.objectives.end());

            if (!result.programs.emplace(stableKey, std::move(program)).second) {
                throw std::runtime_error("duplicate server quest program key");
            }
        }
        const auto classified = result.programs.size() + result.skippedAdapterRequired +
            result.skippedUnsupported + result.skippedNoStages;
        if (classified != imported.byStableKey.size()) {
            throw std::runtime_error("server quest program compilation did not classify every imported quest");
        }
        return result;
    }
}
