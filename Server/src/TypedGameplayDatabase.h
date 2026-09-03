#pragma once

#include "CanonicalRecordDatabase.h"
#include "PluginStack.h"

#include <cstdint>
#include <string>
#include <vector>

namespace SkyrimMP::Server
{
    struct ItemRef
    {
        CanonicalFormReference form;
        std::int32_t count{};
    };

    struct LeveledEntry
    {
        std::int16_t level{};
        CanonicalFormReference form;
        std::int16_t count{};
    };

    struct TypedRecordBase
    {
        CanonicalRecordKey key;
        std::string plugin;
        std::string editorId;
    };

    struct NpcRecord : TypedRecordBase
    {
        std::uint32_t actorFlags{};
        std::vector<ItemRef> inventory;
    };

    struct WeaponRecord : TypedRecordBase
    {
        std::uint32_t value{};
        float weight{};
        std::uint16_t damage{};
    };

    struct ArmorRecord : TypedRecordBase
    {
        std::uint32_t value{};
        float weight{};
        std::uint32_t armorRating{};
    };

    struct AmmoRecord : TypedRecordBase
    {
        CanonicalFormReference projectile;
        std::uint32_t flags{};
        float damage{};
        std::uint32_t value{};
    };

    struct ContainerRecord : TypedRecordBase
    {
        std::vector<ItemRef> items;
    };

    struct LeveledItemRecord : TypedRecordBase
    {
        std::uint8_t chanceNone{};
        std::uint8_t flags{};
        std::vector<LeveledEntry> entries;
    };

    struct TypedGameplayDatabase
    {
        std::vector<NpcRecord> npcs;
        std::vector<WeaponRecord> weapons;
        std::vector<ArmorRecord> armors;
        std::vector<AmmoRecord> ammo;
        std::vector<ContainerRecord> containers;
        std::vector<LeveledItemRecord> leveledItems;
        std::uint64_t parsedRecords{};
        std::uint64_t compressedRecords{};
        std::uint64_t inventoryEntries{};
        std::uint64_t leveledEntries{};
        std::uint64_t canonicalReferences{};
        std::uint64_t nullReferences{};
        std::uint64_t unresolvedReferences{};
    };

    TypedGameplayDatabase BuildTypedGameplayDatabase(
        const CanonicalRecordDatabase& a_database,
        const std::vector<PluginStackEntry>& a_stack);
}
