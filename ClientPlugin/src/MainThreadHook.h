#pragma once

namespace SkyrimMP
{
    class MainThreadHook
    {
    public:
        static void Install();
        static void ResetActorCache();

    private:
        static void Update(RE::Actor* a_actor, float a_delta);
        static inline REL::Relocation<decltype(Update)> originalUpdate;
    };
}
