#include "TypedGameplayDatabase.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000u;

        template <class T>
        T ReadValue(const char* data, std::size_t size, std::size_t offset)
        {
            if (offset + sizeof(T) > size) {
                throw std::runtime_error("typed gameplay field exceeds subrecord bounds");
            }
            T value{};
            std::memcpy(&value, data + offset, sizeof(T));
            return value;
        }

        std::string TrimZeroTerminated(std::string value)
        {
            const auto zero = value.find('\0');
            if (zero != std::string::npos) value.resize(zero);
            return value;
        }

        std::vector<char> ReadPayload(std::ifstream& input, const WinningRecord& record, bool& compressed)
        {
            std::vector<char> raw(record.dataSize);
            if (!raw.empty()) {
                input.seekg(static_cast<std::streamoff>(record.dataOffset), std::ios::beg);
                input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
                if (!input) throw std::runtime_error("truncated typed gameplay record payload");
            }

            compressed = (record.recordFlags & kCompressedRecordFlag) != 0;
            if (!compressed) return raw;
            if (raw.size() < 4) throw std::runtime_error("compressed typed gameplay record missing size prefix");

            std::uint32_t expectedSize{};
            std::memcpy(&expectedSize, raw.data(), sizeof(expectedSize));
            std::vector<char> output(expectedSize);
            uLongf outputSize = static_cast<uLongf>(output.size());
            const auto result = uncompress(
                reinterpret_cast<Bytef*>(output.data()), &outputSize,
                reinterpret_cast<const Bytef*>(raw.data() + 4),
                static_cast<uLong>(raw.size() - 4));
            if (result != Z_OK || outputSize != expectedSize) {
                throw std::runtime_error("typed gameplay zlib decompression failed or size mismatched");
            }
            return output;
        }

        struct RawItem
        {
            std::uint32_t rawFormId{};
            std::int32_t count{};
        };

        struct RawLevelEntry
        {
            std::int16_t level{};
            std::uint32_t rawFormId{};
            std::int16_t count{};
        };

        struct TypedAccumulator
        {
            std::string editorId;
            std::uint32_t actorFlags{};
            std::uint32_t value{};
            float weight{};
            std::uint16_t weaponDamage{};
            std::uint32_t armorRating{};
            std::uint32_t projectile{};
            std::uint32_t ammoFlags{};
            float ammoDamage{};
            std::uint32_t ammoValue{};
            std::uint8_t chanceNone{};
            std::uint8_t leveledFlags{};
            std::vector<RawItem> items;
            std::vector<RawLevelEntry> leveled;
        };

        void ParseTypedSubrecords(const std::string& recordType, const std::vector<char>& data, TypedAccumulator& out)
        {
            std::size_t offset = 0;
            std::uint32_t extendedSize = 0;
            while (offset < data.size()) {
                if (offset + 6 > data.size()) throw std::runtime_error("typed gameplay payload ends with partial subrecord header");
                const std::string type(data.data() + offset, 4);
                offset += 4;
                std::uint16_t shortSize{};
                std::memcpy(&shortSize, data.data() + offset, sizeof(shortSize));
                offset += 2;

                if (type == "XXXX") {
                    if (shortSize != 4 || offset + 4 > data.size()) throw std::runtime_error("invalid XXXX in typed gameplay payload");
                    std::memcpy(&extendedSize, data.data() + offset, sizeof(extendedSize));
                    offset += 4;
                    continue;
                }

                const std::uint32_t size = extendedSize ? extendedSize : shortSize;
                extendedSize = 0;
                if (offset + size > data.size()) throw std::runtime_error("typed gameplay subrecord exceeds payload bounds");
                const char* p = data.data() + offset;

                if (type == "EDID" && out.editorId.empty()) out.editorId = TrimZeroTerminated(std::string(p, size));

                if (recordType == "NPC_") {
                    if (type == "ACBS" && size >= 4) out.actorFlags = ReadValue<std::uint32_t>(p, size, 0);
                    if (type == "CNTO" && size >= 8) out.items.push_back({ ReadValue<std::uint32_t>(p, size, 0), ReadValue<std::int32_t>(p, size, 4) });
                } else if (recordType == "WEAP") {
                    if (type == "DATA" && size >= 10) {
                        out.value = ReadValue<std::uint32_t>(p, size, 0);
                        out.weight = ReadValue<float>(p, size, 4);
                        out.weaponDamage = ReadValue<std::uint16_t>(p, size, 8);
                    }
                } else if (recordType == "ARMO") {
                    if (type == "DATA" && size >= 8) {
                        out.value = ReadValue<std::uint32_t>(p, size, 0);
                        out.weight = ReadValue<float>(p, size, 4);
                    } else if (type == "DNAM" && size >= 4) {
                        out.armorRating = ReadValue<std::uint32_t>(p, size, 0);
                    }
                } else if (recordType == "AMMO") {
                    if (type == "DATA" && size >= 16) {
                        out.projectile = ReadValue<std::uint32_t>(p, size, 0);
                        out.ammoFlags = ReadValue<std::uint32_t>(p, size, 4);
                        out.ammoDamage = ReadValue<float>(p, size, 8);
                        out.ammoValue = ReadValue<std::uint32_t>(p, size, 12);
                    }
                } else if (recordType == "CONT") {
                    if (type == "CNTO" && size >= 8) out.items.push_back({ ReadValue<std::uint32_t>(p, size, 0), ReadValue<std::int32_t>(p, size, 4) });
                } else if (recordType == "LVLI") {
                    if (type == "LVLD" && size >= 1) out.chanceNone = static_cast<std::uint8_t>(p[0]);
                    else if (type == "LVLF" && size >= 1) out.leveledFlags = static_cast<std::uint8_t>(p[0]);
                    else if (type == "LVLO" && size >= 12) {
                        out.leveled.push_back({ ReadValue<std::int16_t>(p, size, 0), ReadValue<std::uint32_t>(p, size, 4), ReadValue<std::int16_t>(p, size, 8) });
                    }
                }
                offset += size;
            }
            if (extendedSize) throw std::runtime_error("dangling XXXX in typed gameplay payload");
        }

        bool IsTypedType(const std::string& type)
        {
            return type == "NPC_" || type == "WEAP" || type == "ARMO" ||
                   type == "AMMO" || type == "CONT" || type == "LVLI";
        }

        bool IsInventoryItemType(const std::string& type)
        {
            static const std::unordered_set<std::string> kInventoryTypes{
                "WEAP", "ARMO", "AMMO", "BOOK", "INGR", "ALCH", "MISC",
                "SLGM", "KEYM", "SCRL", "LIGH", "LVLI"
            };
            return kInventoryTypes.contains(type);
        }

        const WinningRecord& ValidateTarget(
            const CanonicalFormReference& reference,
            const CanonicalRecordDatabase& canonicalDatabase,
            const WinningRecord& sourceRecord,
            const char* field,
            const char* expectedType,
            bool requireInventoryItem,
            TypedGameplayDatabase& typedDatabase)
        {
            const CanonicalRecordKey key{ reference.kind, reference.namespaceIndex, reference.localId };
            const auto target = canonicalDatabase.winners.find(key);
            if (target == canonicalDatabase.winners.end()) {
                ++typedDatabase.missingTargetReferences;
                std::ostringstream message;
                message << "canonical typed reference target missing in " << sourceRecord.sourcePlugin
                        << " record=" << sourceRecord.type << " field=" << field
                        << " target=" << FormNamespaceKindName(reference.kind) << ':' << reference.namespaceIndex
                        << ":0x" << std::hex << std::uppercase << reference.localId;
                throw std::runtime_error(message.str());
            }

            ++typedDatabase.targetReferencesValidated;
            if (expectedType != nullptr) {
                ++typedDatabase.targetTypeChecks;
                if (target->second.type != expectedType) {
                    ++typedDatabase.targetTypeMismatches;
                    std::ostringstream message;
                    message << "canonical typed reference target type mismatch in " << sourceRecord.sourcePlugin
                            << " record=" << sourceRecord.type << " field=" << field
                            << " expected=" << expectedType << " actual=" << target->second.type
                            << " target=" << FormNamespaceKindName(reference.kind) << ':' << reference.namespaceIndex
                            << ":0x" << std::hex << std::uppercase << reference.localId
                            << " targetPlugin=" << target->second.sourcePlugin;
                    throw std::runtime_error(message.str());
                }
            }

            if (requireInventoryItem) {
                ++typedDatabase.itemClassChecks;
                if (!IsInventoryItemType(target->second.type)) {
                    ++typedDatabase.itemClassMismatches;
                    std::ostringstream message;
                    message << "canonical inventory reference target class mismatch in " << sourceRecord.sourcePlugin
                            << " record=" << sourceRecord.type << " field=" << field
                            << " actual=" << target->second.type
                            << " target=" << FormNamespaceKindName(reference.kind) << ':' << reference.namespaceIndex
                            << ":0x" << std::hex << std::uppercase << reference.localId
                            << " targetPlugin=" << target->second.sourcePlugin;
                    throw std::runtime_error(message.str());
                }
            }
            return target->second;
        }

        CanonicalFormReference ResolveReference(
            std::uint32_t rawFormId,
            const PluginStackEntry& sourcePlugin,
            const std::vector<PluginStackEntry>& stack,
            const std::vector<PluginNamespace>& namespaces,
            const CanonicalRecordDatabase& canonicalDatabase,
            const WinningRecord& record,
            const char* field,
            const char* expectedType,
            bool requireInventoryItem,
            TypedGameplayDatabase& database)
        {
            auto resolved = ResolvePluginFormReference(rawFormId, sourcePlugin, stack, namespaces);
            if (resolved.isNull) {
                ++database.nullReferences;
                return resolved;
            }
            if (!resolved.resolved) {
                ++database.unresolvedReferences;
                std::ostringstream message;
                message << "unresolved typed FormID reference in " << record.sourcePlugin
                        << " record=" << record.type << " field=" << field
                        << " raw=0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << rawFormId;
                throw std::runtime_error(message.str());
            }
            ++database.canonicalReferences;
            ValidateTarget(resolved, canonicalDatabase, record, field, expectedType, requireInventoryItem, database);
            return resolved;
        }
    }

    TypedGameplayDatabase BuildTypedGameplayDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack)
    {
        const auto namespaces = BuildFormNamespaces(a_stack);
        TypedGameplayDatabase result;
        std::uint64_t expectedTypedRecords{};
        for (const auto& [key, record] : a_database.winners) {
            (void)key;
            if (IsTypedType(record.type)) ++expectedTypedRecords;
        }

        for (std::size_t stackIndex = 0; stackIndex < a_stack.size(); ++stackIndex) {
            std::ifstream input(a_stack[stackIndex].path, std::ios::binary);
            if (!input) throw std::runtime_error("failed to open plugin for typed gameplay import: " + a_stack[stackIndex].path.string());

            for (const auto& [key, record] : a_database.winners) {
                if (record.sourceStackIndex != stackIndex || !IsTypedType(record.type)) continue;
                bool compressed{};
                auto payload = ReadPayload(input, record, compressed);
                TypedAccumulator parsed;
                ParseTypedSubrecords(record.type, payload, parsed);
                ++result.parsedRecords;
                if (compressed) ++result.compressedRecords;

                TypedRecordBase base{ key, record.sourcePlugin, parsed.editorId };
                if (record.type == "NPC_") {
                    NpcRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.actorFlags = parsed.actorFlags;
                    typed.inventory.reserve(parsed.items.size());
                    for (const auto& item : parsed.items) {
                        typed.inventory.push_back({ ResolveReference(item.rawFormId, a_stack[stackIndex], a_stack, namespaces, a_database, record, "NPC_.CNTO", nullptr, true, result), item.count });
                    }
                    result.inventoryEntries += typed.inventory.size();
                    result.npcs.push_back(std::move(typed));
                } else if (record.type == "WEAP") {
                    WeaponRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.value = parsed.value;
                    typed.weight = parsed.weight;
                    typed.damage = parsed.weaponDamage;
                    result.weapons.push_back(std::move(typed));
                } else if (record.type == "ARMO") {
                    ArmorRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.value = parsed.value;
                    typed.weight = parsed.weight;
                    typed.armorRating = parsed.armorRating;
                    result.armors.push_back(std::move(typed));
                } else if (record.type == "AMMO") {
                    AmmoRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.projectile = ResolveReference(parsed.projectile, a_stack[stackIndex], a_stack, namespaces, a_database, record, "AMMO.DATA.projectile", "PROJ", false, result);
                    typed.flags = parsed.ammoFlags;
                    typed.damage = parsed.ammoDamage;
                    typed.value = parsed.ammoValue;
                    result.ammo.push_back(std::move(typed));
                } else if (record.type == "CONT") {
                    ContainerRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.items.reserve(parsed.items.size());
                    for (const auto& item : parsed.items) {
                        typed.items.push_back({ ResolveReference(item.rawFormId, a_stack[stackIndex], a_stack, namespaces, a_database, record, "CONT.CNTO", nullptr, true, result), item.count });
                    }
                    result.inventoryEntries += typed.items.size();
                    result.containers.push_back(std::move(typed));
                } else if (record.type == "LVLI") {
                    LeveledItemRecord typed;
                    static_cast<TypedRecordBase&>(typed) = base;
                    typed.chanceNone = parsed.chanceNone;
                    typed.flags = parsed.leveledFlags;
                    typed.entries.reserve(parsed.leveled.size());
                    for (const auto& entry : parsed.leveled) {
                        typed.entries.push_back({ entry.level, ResolveReference(entry.rawFormId, a_stack[stackIndex], a_stack, namespaces, a_database, record, "LVLI.LVLO", nullptr, true, result), entry.count });
                    }
                    result.leveledEntries += typed.entries.size();
                    result.leveledItems.push_back(std::move(typed));
                }
            }
        }

        const auto materialized = result.npcs.size() + result.weapons.size() + result.armors.size() +
                                  result.ammo.size() + result.containers.size() + result.leveledItems.size();
        if (materialized != result.parsedRecords || result.parsedRecords != expectedTypedRecords) {
            throw std::runtime_error("typed gameplay database record-count invariant failed");
        }
        if (result.unresolvedReferences != 0 || result.missingTargetReferences != 0 ||
            result.targetTypeMismatches != 0 || result.itemClassMismatches != 0) {
            throw std::runtime_error("typed gameplay canonical-reference semantic invariant failed");
        }
        if (result.targetReferencesValidated != result.canonicalReferences) {
            throw std::runtime_error("typed gameplay target-validation count invariant failed");
        }
        if (result.itemClassChecks != result.inventoryEntries + result.leveledEntries) {
            throw std::runtime_error("typed gameplay item-class validation count invariant failed");
        }

        std::cout << "[TYPED] records=" << result.parsedRecords
                  << " compressed=" << result.compressedRecords
                  << " npc=" << result.npcs.size()
                  << " weap=" << result.weapons.size()
                  << " armo=" << result.armors.size()
                  << " ammo=" << result.ammo.size()
                  << " cont=" << result.containers.size()
                  << " lvli=" << result.leveledItems.size()
                  << " inventoryEntries=" << result.inventoryEntries
                  << " leveledEntries=" << result.leveledEntries
                  << " canonicalRefs=" << result.canonicalReferences
                  << " nullRefs=" << result.nullReferences
                  << " unresolvedRefs=" << result.unresolvedReferences
                  << " targetsValidated=" << result.targetReferencesValidated
                  << " missingTargets=" << result.missingTargetReferences
                  << " typeChecks=" << result.targetTypeChecks
                  << " typeMismatches=" << result.targetTypeMismatches
                  << " itemClassChecks=" << result.itemClassChecks
                  << " itemClassMismatches=" << result.itemClassMismatches << '\n';

        if (!result.weapons.empty()) {
            const auto& r = result.weapons.front();
            std::cout << "[TYPED-SAMPLE] WEAP EDID=" << r.editorId
                      << " value=" << r.value << " weight=" << r.weight << " damage=" << r.damage << '\n';
        }
        if (!result.armors.empty()) {
            const auto& r = result.armors.front();
            std::cout << "[TYPED-SAMPLE] ARMO EDID=" << r.editorId
                      << " value=" << r.value << " weight=" << r.weight << " armorRating=" << r.armorRating << '\n';
        }
        if (!result.ammo.empty()) {
            const auto& r = result.ammo.front();
            std::cout << "[TYPED-SAMPLE] AMMO EDID=" << r.editorId
                      << " damage=" << r.damage << " value=" << r.value;
            if (r.projectile.resolved && !r.projectile.isNull) {
                const CanonicalRecordKey projectileKey{ r.projectile.kind, r.projectile.namespaceIndex, r.projectile.localId };
                const auto target = a_database.winners.find(projectileKey);
                std::cout << " projectile=" << FormNamespaceKindName(r.projectile.kind)
                          << ':' << r.projectile.namespaceIndex
                          << ":0x" << std::hex << std::uppercase << r.projectile.localId
                          << std::dec << std::nouppercase;
                if (target != a_database.winners.end()) std::cout << " targetType=" << target->second.type;
            }
            std::cout << '\n';
        }

        return result;
    }
}
