#include "pch.h"

#include "NativeEntityRegistry.h"

#include <mutex>
#include <unordered_map>

namespace SkyrimMP
{
    namespace
    {
        struct NativeBinding
        {
            RE::ObjectRefHandle handle;
            std::uint32_t runtimeFormId{};
            NativeEntityKind kind{ NativeEntityKind::StaticReference };
        };

        std::mutex g_bindingMutex;
        std::unordered_map<WorldEntityId, NativeBinding> g_byEntity;
        std::unordered_map<std::uint32_t, WorldEntityId> g_byForm;

        bool Bind(WorldEntityId id, std::uint32_t formId, RE::ObjectRefHandle handle, NativeEntityKind kind)
        {
            if (id == 0 || formId == 0) return false;
            std::scoped_lock lock(g_bindingMutex);

            const auto entityIt = g_byEntity.find(id);
            if (entityIt != g_byEntity.end() && entityIt->second.runtimeFormId != formId) {
                logs::error("[NATIVE-ENTITY] rejected identity remap worldEntity={:016X} oldForm={:08X} newForm={:08X}",
                    id, entityIt->second.runtimeFormId, formId);
                return false;
            }
            const auto formIt = g_byForm.find(formId);
            if (formIt != g_byForm.end() && formIt->second != id) {
                logs::error("[NATIVE-ENTITY] rejected native form collision form={:08X} oldWorldEntity={:016X} newWorldEntity={:016X}",
                    formId, formIt->second, id);
                return false;
            }

            g_byEntity[id] = NativeBinding{ handle, formId, kind };
            g_byForm[formId] = id;
            logs::debug("[NATIVE-ENTITY] bound worldEntity={:016X} form={:08X} kind={}",
                id, formId, static_cast<unsigned>(kind));
            return true;
        }
    }

    bool NativeEntityRegistry::BindStatic(WorldEntityId id, std::uint32_t runtimeFormId)
    {
        if (IsDynamicWorldEntity(id)) return false;
        return Bind(id, runtimeFormId, {}, NativeEntityKind::StaticReference);
    }

    bool NativeEntityRegistry::BindRuntime(WorldEntityId id, RE::TESObjectREFR& reference, NativeEntityKind kind)
    {
        if (!IsDynamicWorldEntity(id)) return false;
        return Bind(id, reference.GetFormID(), reference.GetHandle(), kind);
    }

    RE::TESObjectREFR* NativeEntityRegistry::Resolve(WorldEntityId id)
    {
        NativeBinding binding;
        {
            std::scoped_lock lock(g_bindingMutex);
            const auto it = g_byEntity.find(id);
            if (it == g_byEntity.end()) return nullptr;
            binding = it->second;
        }
        if (auto reference = binding.handle.get()) return reference.get();
        auto* form = RE::TESForm::LookupByID(binding.runtimeFormId);
        return form ? form->As<RE::TESObjectREFR>() : nullptr;
    }

    WorldEntityId NativeEntityRegistry::FindByRuntimeFormId(std::uint32_t runtimeFormId)
    {
        std::scoped_lock lock(g_bindingMutex);
        const auto it = g_byForm.find(runtimeFormId);
        return it == g_byForm.end() ? 0 : it->second;
    }

    void NativeEntityRegistry::Unbind(WorldEntityId id)
    {
        std::scoped_lock lock(g_bindingMutex);
        const auto it = g_byEntity.find(id);
        if (it == g_byEntity.end()) return;
        g_byForm.erase(it->second.runtimeFormId);
        g_byEntity.erase(it);
    }

    void NativeEntityRegistry::Reset()
    {
        std::scoped_lock lock(g_bindingMutex);
        g_byEntity.clear();
        g_byForm.clear();
    }

    void NativeEntityRegistry::ResetStatic()
    {
        std::scoped_lock lock(g_bindingMutex);
        for (auto it = g_byEntity.begin(); it != g_byEntity.end();) {
            if (it->second.kind != NativeEntityKind::StaticReference) {
                ++it;
                continue;
            }
            g_byForm.erase(it->second.runtimeFormId);
            it = g_byEntity.erase(it);
        }
    }

    std::size_t NativeEntityRegistry::Size()
    {
        std::scoped_lock lock(g_bindingMutex);
        return g_byEntity.size();
    }
}
