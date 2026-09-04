#include "QuestEventProtocol.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace SkyrimMP::Server
{
    namespace
    {
        constexpr std::uint32_t kQuestMagic = 0x31545351u; // QST1
        constexpr std::uint16_t kQuestProtocolVersion = 1;
        constexpr std::size_t kMaximumQuestKey = 64;
        constexpr std::size_t kMaximumObjectives = 128;

        template <class T>
        void Append(std::vector<std::uint8_t>& out, T value)
        {
            static_assert(std::is_trivially_copyable_v<T>);
            const auto offset = out.size();
            out.resize(offset + sizeof(T));
            std::memcpy(out.data() + offset, &value, sizeof(T));
        }

        template <class T>
        T Read(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            if (offset + sizeof(T) > bytes.size()) throw std::runtime_error("truncated quest protocol field");
            T value{};
            std::memcpy(&value, bytes.data() + offset, sizeof(T));
            offset += sizeof(T);
            return value;
        }

        void AppendString(std::vector<std::uint8_t>& out, const std::string& value)
        {
            if (value.empty() || value.size() > kMaximumQuestKey) throw std::runtime_error("invalid quest protocol key length");
            Append(out, static_cast<std::uint8_t>(value.size()));
            out.insert(out.end(), value.begin(), value.end());
        }

        std::string ReadString(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto size = Read<std::uint8_t>(bytes, offset);
            if (!size || size > kMaximumQuestKey || offset + size > bytes.size()) throw std::runtime_error("invalid quest protocol key");
            std::string value(reinterpret_cast<const char*>(bytes.data() + offset), size);
            offset += size;
            return value;
        }

        void AppendHeader(std::vector<std::uint8_t>& out, std::uint8_t kind)
        {
            Append(out, kQuestMagic);
            Append(out, kQuestProtocolVersion);
            Append(out, kind);
        }

        void ReadHeader(const std::vector<std::uint8_t>& bytes, std::size_t& offset, std::uint8_t expectedKind)
        {
            if (Read<std::uint32_t>(bytes, offset) != kQuestMagic ||
                Read<std::uint16_t>(bytes, offset) != kQuestProtocolVersion ||
                Read<std::uint8_t>(bytes, offset) != expectedKind) {
                throw std::runtime_error("invalid quest protocol header");
            }
        }

        void AppendKey(std::vector<std::uint8_t>& out, const CanonicalRecordKey& key)
        {
            Append(out, static_cast<std::uint8_t>(key.kind == FormNamespaceKind::Light ? 1 : 0));
            Append(out, key.namespaceIndex);
            Append(out, key.localId);
        }

        CanonicalRecordKey ReadKey(const std::vector<std::uint8_t>& bytes, std::size_t& offset)
        {
            const auto kind = Read<std::uint8_t>(bytes, offset);
            if (kind > 1) throw std::runtime_error("invalid quest protocol namespace kind");
            CanonicalRecordKey key{ kind ? FormNamespaceKind::Light : FormNamespaceKind::Full,
                Read<std::uint32_t>(bytes, offset), Read<std::uint32_t>(bytes, offset) };
            if (!key.localId) throw std::runtime_error("quest protocol subject cannot be null");
            return key;
        }

        void EnsureConsumed(const std::vector<std::uint8_t>& bytes, std::size_t offset)
        {
            if (offset != bytes.size()) throw std::runtime_error("quest protocol packet has trailing bytes");
        }
    }

    std::vector<std::uint8_t> EncodeQuestEvidence(const QuestGameplayEvidence& evidence)
    {
        if (!evidence.sessionId || !evidence.sequence || evidence.quantity == 0 || evidence.quantity > 1000000 ||
            static_cast<std::uint8_t>(evidence.kind) < 1 || static_cast<std::uint8_t>(evidence.kind) > 4) {
            throw std::runtime_error("invalid quest gameplay evidence");
        }
        std::vector<std::uint8_t> out;
        AppendHeader(out, 1);
        Append(out, evidence.sessionId);
        Append(out, evidence.sequence);
        AppendString(out, evidence.questKey);
        Append(out, static_cast<std::uint8_t>(evidence.kind));
        AppendKey(out, evidence.subject);
        Append(out, evidence.quantity);
        return out;
    }

    QuestGameplayEvidence DecodeQuestEvidence(const std::vector<std::uint8_t>& bytes)
    {
        std::size_t offset{};
        ReadHeader(bytes, offset, 1);
        QuestGameplayEvidence result;
        result.sessionId = Read<std::uint64_t>(bytes, offset);
        result.sequence = Read<std::uint64_t>(bytes, offset);
        result.questKey = ReadString(bytes, offset);
        const auto kind = Read<std::uint8_t>(bytes, offset);
        if (!result.sessionId || !result.sequence || kind < 1 || kind > 4) throw std::runtime_error("invalid decoded quest evidence identity or kind");
        result.kind = static_cast<QuestEvidenceKind>(kind);
        result.subject = ReadKey(bytes, offset);
        result.quantity = Read<std::uint32_t>(bytes, offset);
        if (!result.quantity || result.quantity > 1000000) throw std::runtime_error("invalid decoded quest evidence quantity");
        EnsureConsumed(bytes, offset);
        return result;
    }

    std::vector<std::uint8_t> EncodeQuestProjection(const ClientQuestProjection& projection)
    {
        if (!projection.sessionId || !projection.revision || projection.activeObjectives.size() > kMaximumObjectives) {
            throw std::runtime_error("invalid client quest projection");
        }
        std::vector<std::uint8_t> out;
        AppendHeader(out, 2);
        Append(out, projection.sessionId);
        AppendString(out, projection.questKey);
        Append(out, projection.stage);
        Append(out, projection.revision);
        Append(out, static_cast<std::uint8_t>(projection.complete ? 1 : 0));
        Append(out, static_cast<std::uint8_t>(projection.activeObjectives.size()));
        for (const auto objective : projection.activeObjectives) Append(out, objective);
        return out;
    }

    ClientQuestProjection DecodeQuestProjection(const std::vector<std::uint8_t>& bytes)
    {
        std::size_t offset{};
        ReadHeader(bytes, offset, 2);
        ClientQuestProjection result;
        result.sessionId = Read<std::uint64_t>(bytes, offset);
        result.questKey = ReadString(bytes, offset);
        result.stage = Read<std::uint16_t>(bytes, offset);
        result.revision = Read<std::uint64_t>(bytes, offset);
        const auto complete = Read<std::uint8_t>(bytes, offset);
        const auto count = Read<std::uint8_t>(bytes, offset);
        if (!result.sessionId || !result.revision || complete > 1 || count > kMaximumObjectives) throw std::runtime_error("invalid decoded quest projection");
        result.complete = complete != 0;
        result.activeObjectives.reserve(count);
        for (std::uint8_t i = 0; i < count; ++i) result.activeObjectives.push_back(Read<std::uint32_t>(bytes, offset));
        EnsureConsumed(bytes, offset);
        return result;
    }

    void RunQuestEventProtocolSelfTest()
    {
        const QuestGameplayEvidence evidence{ 7, 11, "F:0:123", QuestEvidenceKind::KilledReference,
            { FormNamespaceKind::Full, 0, 0x1234 }, 1 };
        const auto evidenceRoundTrip = DecodeQuestEvidence(EncodeQuestEvidence(evidence));
        const ClientQuestProjection projection{ 7, "F:0:123", 20, 4, false, { 10, 20 } };
        const auto projectionRoundTrip = DecodeQuestProjection(EncodeQuestProjection(projection));
        if (evidenceRoundTrip.questKey != evidence.questKey || evidenceRoundTrip.kind != evidence.kind ||
            evidenceRoundTrip.subject != evidence.subject || projectionRoundTrip.questKey != projection.questKey ||
            projectionRoundTrip.activeObjectives != projection.activeObjectives) {
            throw std::runtime_error("quest event protocol self-test failed");
        }
        std::cout << "[QUEST-WIRE-SELFTEST] typedEvidence=true bounded=true noClientCompletion=true projection=true roundTrip=true\n";
    }
}
