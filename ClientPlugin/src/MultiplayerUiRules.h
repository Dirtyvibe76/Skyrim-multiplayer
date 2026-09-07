#pragma once

namespace SkyrimMP::MultiplayerUiRules
{
    bool InstallMenuRules();
    bool InstallInteractionRules();
    void SetActive(bool a_active) noexcept;
    bool IsActive() noexcept;
}
