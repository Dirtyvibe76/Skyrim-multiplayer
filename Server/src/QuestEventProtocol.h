#pragma once

#include "CanonicalRecordDatabase.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    enum class QuestEvidenceKind : std::uint8_t
    {
        TalkedToReference = 1,
        EnteredLocation = 2,
        KilledReference = 3,
        CollectedItem = 4
    };

    struct QuestGameplayEvidence
    {
        std::uint64_t sessionId{};
        std::uint64_t sequence{};
        std::string questKey;
        QuestEvidenceKind kind{ QuestEvidenceKind::TalkedToReference };
        CanonicalRecordKey subject;
        std::uint32_t quantity{ 1 };
    };

    struct ClientQuestProjection
    {
        std::uint64_t sessionId{};
        std::string questKey;
        std::uint16_t stage{};
        std::uint64_t revision{};
        bool complete{};
        std::vector<std::uint32_t> activeObjectives;
    };

    std::vector<std::uint8_t> EncodeQuestEvidence(const QuestGameplayEvidence& a_evidence);
    QuestGameplayEvidence DecodeQuestEvidence(const std::vector<std::uint8_t>& a_bytes);
    std::vector<std::uint8_t> EncodeQuestProjection(const ClientQuestProjection& a_projection);
    ClientQuestProjection DecodeQuestProjection(const std::vector<std::uint8_t>& a_bytes);
    void RunQuestEventProtocolSelfTest();
}
