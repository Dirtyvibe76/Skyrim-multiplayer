#include "WorldReferenceDatabase.h"
#include "WorldSpatialContext.h"

#include <zlib.h>

#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000u;
        constexpr std::uint32_t kDeletedRecordFlag = 0x00000020u;

        template <class T>
        T ReadValue(const char* data, std::size_t size, std::size_t offset)
        {
            if (offset + sizeof(T) > size) {
                throw std::runtime_error("world reference field exceeds subrecord bounds");
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

        std::vector<char> ReadPayload(std::ifstream& input, const WinningRecord& record)
        {
            std::vector<char> raw(record.dataSize);
            if (!raw.empty()) {
                input.seekg(static_cast<std::streamoff>(record.dataOffset), std::ios::beg);
                input.read(raw.data(), static_cast<std::streamsize>(raw.size()));
                if (!input) throw std::runtime_error("truncated world reference payload");
            }
            if ((record.recordFlags & kCompressedRecordFlag) == 0) return raw;
            if (raw.size() < 4) throw std::runtime_error("compressed world reference missing size prefix");

            std::uint32_t expectedSize{};
            std::memcpy(&expectedSize, raw.data(), sizeof(expectedSize));
            std::vector<char> output(expectedSize);
            uLongf outputSize = static_cast<uLongf>(output.size());
            const auto status = ::uncompress(
                reinterpret_cast<Bytef*>(output.data()), &outputSize,
                reinterpret_cast<const Bytef*>(raw.data() + 4),
                static_cast<uLong>(raw.size() - 4));
            if (status != Z_OK || outputSize != expectedSize) {
                throw std::runtime_error("world reference zlib decompression failed or size mismatched");
            }
            return output;
        }

        struct ParsedWorldReference
        {
            std::string editorId;
            std::uint32_t baseRawFormId{};
            WorldTransform transform;
            bool hasBase{};
            bool hasTransform{};
        };

        ParsedWorldReference ParseWorldReferencePayload(const std::vector<char>& data)
        {
            ParsedWorldReference out;
            std::size_t offset = 0;
            std::uint32_t extendedSize = 0;
            while (offset < data.size()) {
                if (offset + 6 > data.size()) throw std::runtime_error("world reference payload ends with partial subrecord header");
                const std::string type(data.data() + offset, 4);
                offset += 4;
                std::uint16_t shortSize{};
                std::memcpy(&shortSize, data.data() + offset, sizeof(shortSize));
                offset += 2;

                if (type == "XXXX") {
                    if (shortSize != 4 || offset + 4 > data.size()) throw std::runtime_error("invalid XXXX in world reference payload");
                    std::memcpy(&extendedSize, data.data() + offset, sizeof(extendedSize));
                    offset += 4;
                    continue;
                }

                const std::uint32_t size = extendedSize ? extendedSize : shortSize;
                extendedSize = 0;
                if (offset + size > data.size()) throw std::runtime_error("world reference subrecord exceeds payload bounds");
                const char* p = data.data() + offset;

                if (type == "EDID" && out.editorId.empty()) {
                    out.editorId = TrimZeroTerminated(std::string(p, size));
                } else if (type == "NAME" && size >= 4 && !out.hasBase) {
                    out.baseRawFormId = ReadValue<std::uint32_t>(p, size, 0);
                    out.hasBase = true;
                } else if (type == "DATA" && size >= 24 && !out.hasTransform) {
                    out.transform.x = ReadValue<float>(p, size, 0);
                    out.transform.y = ReadValue<float>(p, size, 4);
                    out.transform.z = ReadValue<float>(p, size, 8);
                    out.transform.pitch = ReadValue<float>(p, size, 12);
                    out.transform.yaw = ReadValue<float>(p, size, 16);
                    out.transform.roll = ReadValue<float>(p, size, 20);
                    out.hasTransform = true;
                }
                offset += size;
            }
            if (extendedSize) throw std::runtime_error("dangling XXXX in world reference payload");
            return out;
        }

        CanonicalFormReference ResolveAndValidateBase(
            std::uint32_t rawFormId,
            const PluginStackEntry& sourcePlugin,
            const std::vector<PluginStackEntry>& stack,
            const std::vector<PluginNamespace>& namespaces,
            const CanonicalRecordDatabase& database,
            const WinningRecord& sourceRecord,
            WorldReferenceDatabase& world)
        {
            auto resolved = ResolvePluginFormReference(rawFormId, sourcePlugin, stack, namespaces);
            if (!resolved.resolved || resolved.isNull) {
                std::ostringstream message;
                message << "world reference base FormID did not resolve in " << sourceRecord.sourcePlugin
                        << " record=" << sourceRecord.type << " raw=0x" << std::hex << std::uppercase
                        << std::setw(8) << std::setfill('0') << rawFormId;
                throw std::runtime_error(message.str());
            }

            ++world.baseReferences;
            const CanonicalRecordKey key{ resolved.kind, resolved.namespaceIndex, resolved.localId };
            const auto target = database.winners.find(key);
            if (target == database.winners.end()) {
                ++world.missingBaseTargets;
                std::ostringstream message;
                message << "world reference base target missing in " << sourceRecord.sourcePlugin
                        << " record=" << sourceRecord.type
                        << " target=" << FormNamespaceKindName(resolved.kind) << ':' << resolved.namespaceIndex
                        << ":0x" << std::hex << std::uppercase << resolved.localId;
                throw std::runtime_error(message.str());
            }
            ++world.baseTargetsValidated;

            if (sourceRecord.type == "ACHR") {
                ++world.actorTypeChecks;
                if (target->second.type != "NPC_") {
                    ++world.actorTypeMismatches;
                    std::ostringstream message;
                    message << "ACHR base target is not NPC_ in " << sourceRecord.sourcePlugin
                            << " actual=" << target->second.type
                            << " target=" << FormNamespaceKindName(resolved.kind) << ':' << resolved.namespaceIndex
                            << ":0x" << std::hex << std::uppercase << resolved.localId;
                    throw std::runtime_error(message.str());
                }
            } else {
                ++world.referenceBaseTypeCounts[target->second.type];
            }
            return resolved;
        }
    }

    WorldReferenceDatabase BuildWorldReferenceDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack)
    {
        const auto namespaces = BuildFormNamespaces(a_stack);
        WorldReferenceDatabase world;

        for (std::size_t stackIndex = 0; stackIndex < a_stack.size(); ++stackIndex) {
            std::ifstream input(a_stack[stackIndex].path, std::ios::binary);
            if (!input) throw std::runtime_error("failed to open plugin for world reference import: " + a_stack[stackIndex].path.string());

            for (const auto& [key, record] : a_database.winners) {
                if (record.sourceStackIndex != stackIndex || (record.type != "REFR" && record.type != "ACHR")) continue;

                ++world.parsedRecords;
                const bool deleted = (record.recordFlags & kDeletedRecordFlag) != 0;
                if (deleted) ++world.deletedRecords;

                const auto payload = ReadPayload(input, record);
                const auto parsed = ParseWorldReferencePayload(payload);

                WorldReferenceRecord typed;
                typed.key = key;
                typed.plugin = record.sourcePlugin;
                typed.editorId = parsed.editorId;
                typed.transform = parsed.transform;
                typed.hasTransform = parsed.hasTransform;
                typed.deleted = deleted;

                if (parsed.hasBase) {
                    typed.baseObject = ResolveAndValidateBase(
                        parsed.baseRawFormId, a_stack[stackIndex], a_stack, namespaces,
                        a_database, record, world);
                    typed.hasBaseObject = true;
                } else if (!deleted) {
                    ++world.missingBaseObjects;
                }

                if (parsed.hasTransform) ++world.transformRecords;
                else if (!deleted) ++world.missingTransforms;

                if (record.type == "ACHR") world.actors.push_back(std::move(typed));
                else world.references.push_back(std::move(typed));
            }
        }

        if (world.baseReferences != world.baseTargetsValidated || world.missingBaseTargets != 0 || world.actorTypeMismatches != 0) {
            throw std::runtime_error("world reference canonical base-target invariant failed");
        }

        std::cout << "[WORLD] records=" << world.parsedRecords
                  << " refr=" << world.references.size()
                  << " achr=" << world.actors.size()
                  << " deleted=" << world.deletedRecords
                  << " baseRefs=" << world.baseReferences
                  << " baseTargetsValidated=" << world.baseTargetsValidated
                  << " missingBaseTargets=" << world.missingBaseTargets
                  << " actorTypeChecks=" << world.actorTypeChecks
                  << " actorTypeMismatches=" << world.actorTypeMismatches
                  << " transforms=" << world.transformRecords
                  << " missingBase=" << world.missingBaseObjects
                  << " missingTransform=" << world.missingTransforms
                  << " refrBaseTypes=" << world.referenceBaseTypeCounts.size() << '\n';

        if (!world.actors.empty()) {
            const auto& r = world.actors.front();
            std::cout << "[WORLD-SAMPLE] ACHR base=" << FormNamespaceKindName(r.baseObject.kind)
                      << ':' << r.baseObject.namespaceIndex << ":0x" << std::hex << std::uppercase << r.baseObject.localId
                      << std::dec << std::nouppercase
                      << " pos=(" << r.transform.x << ',' << r.transform.y << ',' << r.transform.z << ')'
                      << " rot=(" << r.transform.pitch << ',' << r.transform.yaw << ',' << r.transform.roll << ")\n";
        }
        if (!world.references.empty()) {
            const auto& r = world.references.front();
            std::cout << "[WORLD-SAMPLE] REFR";
            if (r.hasBaseObject) {
                std::cout << " base=" << FormNamespaceKindName(r.baseObject.kind)
                          << ':' << r.baseObject.namespaceIndex << ":0x" << std::hex << std::uppercase << r.baseObject.localId
                          << std::dec << std::nouppercase;
            }
            if (r.hasTransform) {
                std::cout << " pos=(" << r.transform.x << ',' << r.transform.y << ',' << r.transform.z << ')'
                          << " rot=(" << r.transform.pitch << ',' << r.transform.yaw << ',' << r.transform.roll << ')';
            }
            std::cout << '\n';
        }

        const auto spatial = BuildWorldSpatialContextDatabase(a_database, a_stack);
        if (spatial.referenceRecords != world.parsedRecords) {
            throw std::runtime_error("world reference/spatial-context record-count invariant failed");
        }

        return world;
    }
}
