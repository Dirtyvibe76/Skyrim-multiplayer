#include "GameplayPayloadImporter.h"
#include "TypedGameplayDatabase.h"
#include "WorldReferenceDatabase.h"
#include "ImportedQuestDatabase.h"
#include "RuntimeEntityRegistry.h"
#include "ServerQuestProgram.h"

#include <zlib.h>

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kCompressedRecordFlag = 0x00040000u;

        bool IsGameplayType(const std::string& type)
        {
            static const std::unordered_set<std::string> kTypes{
                "NPC_", "ACHR", "REFR", "CONT", "LVLI",
                "WEAP", "ARMO", "AMMO", "SPEL", "QUST"
            };
            return kTypes.contains(type);
        }

        std::string TrimZeroTerminated(std::string value)
        {
            const auto zero = value.find('\0');
            if (zero != std::string::npos) {
                value.resize(zero);
            }
            return value;
        }

        std::uint16_t ReadU16(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + sizeof(std::uint16_t) > data.size()) {
                throw std::runtime_error("truncated gameplay subrecord size");
            }
            std::uint16_t value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        std::uint32_t ReadU32(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + sizeof(std::uint32_t) > data.size()) {
                throw std::runtime_error("truncated gameplay extended subrecord size");
            }
            std::uint32_t value{};
            std::memcpy(&value, data.data() + offset, sizeof(value));
            return value;
        }

        std::string ReadSignature(const std::vector<char>& data, std::size_t offset)
        {
            if (offset + 4 > data.size()) {
                throw std::runtime_error("truncated gameplay subrecord signature");
            }
            return std::string(data.data() + offset, 4);
        }

        std::vector<char> DecompressBethesdaRecord(
            const std::vector<char>& stored,
            const std::string& plugin,
            const std::string& type)
        {
            if (stored.size() < sizeof(std::uint32_t)) {
                throw std::runtime_error("compressed gameplay record is missing uncompressed-size prefix in " + plugin);
            }

            const auto expectedSize = ReadU32(stored, 0);
            if (expectedSize == 0) {
                return {};
            }
            if (expectedSize > static_cast<std::uint32_t>(std::numeric_limits<uLongf>::max())) {
                throw std::runtime_error("compressed gameplay record is too large for zlib in " + plugin);
            }

            std::vector<char> output(expectedSize);
            uLongf outputSize = static_cast<uLongf>(output.size());
            const auto* compressed = reinterpret_cast<const Bytef*>(stored.data() + sizeof(std::uint32_t));
            const auto compressedSize = static_cast<uLong>(stored.size() - sizeof(std::uint32_t));

            const int status = ::uncompress(
                reinterpret_cast<Bytef*>(output.data()),
                &outputSize,
                compressed,
                compressedSize);
            if (status != Z_OK) {
                throw std::runtime_error(
                    "zlib failed to decompress " + type + " gameplay record in " + plugin +
                    " status=" + std::to_string(status));
            }
            if (outputSize != expectedSize) {
                throw std::runtime_error(
                    "decompressed gameplay record size mismatch in " + plugin +
                    ": expected=" + std::to_string(expectedSize) +
                    " actual=" + std::to_string(outputSize));
            }
            return output;
        }

        struct ParsedPayload
        {
            std::uint32_t subrecordCount{};
            std::string editorId;
        };

        ParsedPayload ParsePayload(const std::vector<char>& data)
        {
            ParsedPayload result;
            std::size_t offset = 0;
            std::uint32_t extendedSize = 0;

            while (offset < data.size()) {
                if (offset + 6 > data.size()) {
                    throw std::runtime_error("gameplay payload ends with partial subrecord header");
                }

                const auto type = ReadSignature(data, offset);
                offset += 4;
                const auto shortSize = ReadU16(data, offset);
                offset += 2;

                if (type == "XXXX") {
                    if (shortSize != 4 || offset + 4 > data.size()) {
                        throw std::runtime_error("invalid XXXX gameplay subrecord");
                    }
                    extendedSize = ReadU32(data, offset);
                    offset += 4;
                    continue;
                }

                const std::uint32_t size = extendedSize != 0 ? extendedSize : shortSize;
                extendedSize = 0;
                if (offset + size > data.size()) {
                    throw std::runtime_error("gameplay subrecord exceeds record payload");
                }

                ++result.subrecordCount;
                if (type == "EDID" && result.editorId.empty()) {
                    result.editorId = TrimZeroTerminated(std::string(data.data() + offset, size));
                }
                offset += size;
            }

            if (extendedSize != 0) {
                throw std::runtime_error("dangling XXXX gameplay subrecord");
            }
            return result;
        }
    }

    GameplayPayloadSummary ImportGameplayPayloads(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack,
        std::size_t a_sampleLimit,
        RuntimeEntityRegistry* a_runtimeRegistry)
    {
        GameplayPayloadSummary summary;

        for (std::size_t stackIndex = 0; stackIndex < a_stack.size(); ++stackIndex) {
            std::ifstream input(a_stack[stackIndex].path, std::ios::binary);
            if (!input) {
                throw std::runtime_error("failed to open plugin for gameplay payload import: " + a_stack[stackIndex].path.string());
            }

            for (const auto& [key, record] : a_database.winners) {
                if (record.sourceStackIndex != stackIndex || !IsGameplayType(record.type)) {
                    continue;
                }

                ++summary.candidateRecords;
                ++summary.typeCounts[record.type];

                std::vector<char> stored(record.dataSize);
                if (!stored.empty()) {
                    input.seekg(static_cast<std::streamoff>(record.dataOffset), std::ios::beg);
                    input.read(stored.data(), static_cast<std::streamsize>(stored.size()));
                    if (!input) {
                        throw std::runtime_error("truncated gameplay record payload in " + a_stack[stackIndex].path.string());
                    }
                }

                const bool compressed = (record.recordFlags & kCompressedRecordFlag) != 0;
                std::vector<char> payload;
                if (compressed) {
                    ++summary.compressedRecords;
                    summary.compressedBytes += stored.size();
                    payload = DecompressBethesdaRecord(stored, record.sourcePlugin, record.type);
                    summary.decompressedBytes += payload.size();
                } else {
                    payload = std::move(stored);
                }

                const auto parsed = ParsePayload(payload);
                ++summary.parsedRecords;
                summary.subrecords += parsed.subrecordCount;
                if (!parsed.editorId.empty()) {
                    ++summary.editorIds;
                }

                if (summary.samples.size() < a_sampleLimit) {
                    summary.samples.push_back(GameplayPayloadSample{
                        record.type,
                        record.sourcePlugin,
                        parsed.editorId,
                        key,
                        parsed.subrecordCount,
                        compressed
                    });
                }
            }
        }

        if (summary.parsedRecords != summary.candidateRecords) {
            throw std::runtime_error(
                "gameplay payload import incomplete: candidates=" + std::to_string(summary.candidateRecords) +
                " parsed=" + std::to_string(summary.parsedRecords));
        }

        // Independently materialize and validate typed authoritative gameplay definitions.
        const auto typed = BuildTypedGameplayDatabase(a_database, a_stack);
        (void)typed;

        summary.questDefinitions = std::make_shared<ImportedQuestDatabase>(
            BuildImportedQuestDatabase(a_database, a_stack));
        summary.questPrograms = std::make_shared<ServerQuestProgramDatabase>(
            CompileServerQuestPrograms(*summary.questDefinitions));
        if (a_runtimeRegistry) {
            a_runtimeRegistry->questDefinitions = summary.questDefinitions;
            a_runtimeRegistry->questPrograms = summary.questPrograms;
        }

        // Materialize authoritative placed-world references. REFR/ACHR NAME targets are
        // canonicalized against the winning-record database and ACHR bases must resolve to NPC_.
        const auto world = BuildWorldReferenceDatabase(a_database, a_stack, a_runtimeRegistry);
        (void)world;

        return summary;
    }
}
