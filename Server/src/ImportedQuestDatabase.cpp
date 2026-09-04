#include "ImportedQuestDatabase.h"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000u;

        std::uint16_t ReadU16(const char* data, std::size_t size)
        {
            if (size < 2) throw std::runtime_error("truncated quest uint16 field");
            std::uint16_t value{};
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        std::uint32_t ReadU32(const char* data, std::size_t size)
        {
            if (size < 4) throw std::runtime_error("truncated quest uint32 field");
            std::uint32_t value{};
            std::memcpy(&value, data, sizeof(value));
            return value;
        }

        std::uint32_t ReadQuestIndex(const char* data, std::size_t size)
        {
            return size >= 4 ? ReadU32(data, size) : ReadU16(data, size);
        }

        std::string TrimString(const char* data, std::size_t size)
        {
            std::string value(data, size);
            if (const auto zero = value.find('\0'); zero != std::string::npos) value.resize(zero);
            return value;
        }

        std::string StableKey(const CanonicalRecordKey& key)
        {
            std::ostringstream out;
            out << (key.kind == FormNamespaceKind::Light ? 'L' : 'F') << ':'
                << key.namespaceIndex << ':' << key.localId;
            return out.str();
        }

        std::vector<char> ReadPayload(const WinningRecord& record, const PluginStackEntry& plugin, std::ifstream& input)
        {
            std::vector<char> stored(record.dataSize);
            input.clear();
            input.seekg(static_cast<std::streamoff>(record.dataOffset), std::ios::beg);
            if (!stored.empty()) input.read(stored.data(), static_cast<std::streamsize>(stored.size()));
            if (!input) throw std::runtime_error("truncated QUST payload in " + plugin.path.string());
            if ((record.recordFlags & kCompressedRecordFlag) == 0) return stored;
            if (stored.size() < 4) throw std::runtime_error("compressed QUST payload missing size prefix");
            const auto expected = ReadU32(stored.data(), stored.size());
            if (expected > static_cast<std::uint32_t>(std::numeric_limits<uLongf>::max())) {
                throw std::runtime_error("compressed QUST payload exceeds zlib limit");
            }
            std::vector<char> result(expected);
            uLongf actual = expected;
            const auto status = ::uncompress(
                reinterpret_cast<Bytef*>(result.data()), &actual,
                reinterpret_cast<const Bytef*>(stored.data() + 4),
                static_cast<uLong>(stored.size() - 4));
            if (status != Z_OK || actual != expected) throw std::runtime_error("failed to decompress QUST payload");
            return result;
        }

        bool KnownQuestSubrecord(const std::string& type)
        {
            static const std::set<std::string> known{
                "EDID", "VMAD", "FULL", "DNAM", "ENAM", "QTGL", "FLTR", "INDX", "QSDT", "CTDA",
                "CIS1", "CIS2", "CNAM", "SCHR", "SCDA", "SCTX", "SCRO", "SCRV", "QNAM",
                "NEXT", "NAM0", "QOBJ", "FNAM", "NNAM", "QSTA", "ANAM", "ALST", "ALLS", "ALCS",
                "ALID", "ALFI", "ALFR", "ALUA", "ALCO", "ALCA", "ALCL", "ALNA", "ALNT",
                "ALFE", "ALFD", "ALFA", "ALRT", "ALDN", "ALSP", "ALFC", "VTCK", "ALED",
                "KSIZ", "KWDA", "COCT", "CNTO"
            };
            return known.contains(type);
        }

        ImportedQuestDefinition ParseQuest(const WinningRecord& record, const std::vector<char>& payload)
        {
            ImportedQuestDefinition quest;
            quest.key = record.key;
            quest.plugin = record.sourcePlugin;
            ImportedQuestStage* stage = nullptr;
            ImportedQuestObjective* objective = nullptr;
            ImportedQuestAlias* alias = nullptr;
            std::size_t offset{};
            std::uint32_t extended{};
            while (offset < payload.size()) {
                if (offset + 6 > payload.size()) throw std::runtime_error("QUST ends with partial subrecord header");
                const std::string type(payload.data() + offset, 4);
                offset += 4;
                const auto shortSize = ReadU16(payload.data() + offset, payload.size() - offset);
                offset += 2;
                if (type == "XXXX") {
                    if (shortSize != 4 || offset + 4 > payload.size()) throw std::runtime_error("invalid QUST XXXX subrecord");
                    extended = ReadU32(payload.data() + offset, payload.size() - offset);
                    offset += 4;
                    continue;
                }
                const std::uint32_t size = extended ? extended : shortSize;
                extended = 0;
                if (offset + size > payload.size()) throw std::runtime_error("QUST subrecord exceeds payload");
                const char* data = payload.data() + offset;
                if (type == "EDID" && quest.editorId.empty()) quest.editorId = TrimString(data, size);
                else if (type == "VMAD") quest.scriptBytes += size;
                else if (type == "INDX") {
                    quest.stages.push_back({ ReadU16(data, size) });
                    stage = &quest.stages.back(); objective = nullptr; alias = nullptr;
                } else if (type == "CNAM" && stage) ++stage->logEntries;
                else if (type == "SCHR" || type == "SCDA" || type == "SCTX") {
                    if (stage) stage->hasScriptFragment = true;
                    quest.scriptBytes += size;
                } else if (type == "QOBJ") {
                    quest.objectives.push_back({ ReadQuestIndex(data, size) });
                    objective = &quest.objectives.back(); stage = nullptr; alias = nullptr;
                } else if (type == "QSTA" && objective) ++objective->targetCount;
                else if (type == "ALST" || type == "ALLS" || type == "ALCS") {
                    const auto kind = type == "ALST" ? ImportedQuestAliasKind::Reference :
                        (type == "ALLS" ? ImportedQuestAliasKind::Location : ImportedQuestAliasKind::Collection);
                    quest.aliases.push_back({ ReadQuestIndex(data, size), kind });
                    alias = &quest.aliases.back(); stage = nullptr; objective = nullptr;
                } else if (type == "CTDA") {
                    if (objective) ++objective->conditions;
                    else if (alias) ++alias->conditions;
                    else if (stage) ++stage->conditions;
                    else ++quest.questConditions;
                } else if (type == "ALSP" && alias) alias->hasScript = true;
                if (!KnownQuestSubrecord(type)) ++quest.unknownSubrecords;
                offset += size;
            }
            if (extended) throw std::runtime_error("QUST has dangling XXXX subrecord");

            const bool fragments = std::any_of(quest.stages.begin(), quest.stages.end(), [](const auto& value) {
                return value.hasScriptFragment;
            });
            const bool aliasScripts = std::any_of(quest.aliases.begin(), quest.aliases.end(), [](const auto& value) {
                return value.hasScript;
            });
            if (quest.scriptBytes || fragments || aliasScripts) {
                quest.compatibility = QuestCompatibility::AdapterRequired;
                quest.compatibilityReasons.push_back("Papyrus or compiled quest fragments require an explicit server adapter");
            }
            if (quest.unknownSubrecords) {
                if (quest.compatibility == QuestCompatibility::RecordDriven) quest.compatibility = QuestCompatibility::AdapterRequired;
                quest.compatibilityReasons.push_back("contains unmodeled quest subrecords");
            }
            if (quest.stages.empty() && quest.objectives.empty() && quest.aliases.empty()) {
                quest.compatibility = QuestCompatibility::Unsupported;
                quest.compatibilityReasons.push_back("contains no importable stages, objectives, or aliases");
            }
            return quest;
        }
    }

    ImportedQuestDatabase BuildImportedQuestDatabase(
        const CanonicalRecordDatabase& database,
        const std::vector<PluginStackEntry>& stack)
    {
        ImportedQuestDatabase result;
        for (std::size_t stackIndex = 0; stackIndex < stack.size(); ++stackIndex) {
            std::ifstream input(stack[stackIndex].path, std::ios::binary);
            if (!input) throw std::runtime_error("failed to open plugin for QUST import: " + stack[stackIndex].path.string());
            for (const auto& [key, record] : database.winners) {
                if (record.sourceStackIndex != stackIndex || record.type != "QUST") continue;
                auto quest = ParseQuest(record, ReadPayload(record, stack[stackIndex], input));
                result.stages += quest.stages.size();
                result.objectives += quest.objectives.size();
                result.aliases += quest.aliases.size();
                result.conditions += quest.questConditions;
                for (const auto& value : quest.stages) result.conditions += value.conditions;
                for (const auto& value : quest.objectives) result.conditions += value.conditions;
                for (const auto& value : quest.aliases) result.conditions += value.conditions;
                if (quest.compatibility == QuestCompatibility::RecordDriven) ++result.recordDriven;
                else if (quest.compatibility == QuestCompatibility::AdapterRequired) ++result.adapterRequired;
                else ++result.unsupported;
                if (!result.byStableKey.emplace(StableKey(key), std::move(quest)).second) {
                    throw std::runtime_error("duplicate canonical QUST key during import");
                }
            }
        }
        if (result.recordDriven + result.adapterRequired + result.unsupported != result.byStableKey.size()) {
            throw std::runtime_error("QUST compatibility classification is incomplete");
        }
        return result;
    }

    const char* QuestCompatibilityName(QuestCompatibility compatibility)
    {
        switch (compatibility) {
        case QuestCompatibility::RecordDriven: return "record-driven";
        case QuestCompatibility::AdapterRequired: return "adapter-required";
        case QuestCompatibility::Unsupported: return "unsupported";
        }
        return "unknown";
    }
}
