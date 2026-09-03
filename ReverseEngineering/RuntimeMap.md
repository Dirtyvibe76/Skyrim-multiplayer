# Skyrim Runtime Map

## RE-0.1 — Local Player Observation

Status: IMPLEMENTED / NOT YET RUNTIME VERIFIED

Observed through CommonLibSSE-NG:

- RE::PlayerCharacter::GetSingleton()
- TESForm::GetFormID()
- TESObjectREFR::GetPosition()
- TESObjectREFR::GetAngle()
- TESObjectREFR::GetParentCell()
- TESObjectCELL::GetWorldSpace()

Expected output:

- Skyrim runtime version
- Player FormID
- Current cell FormID
- Current worldspace FormID
- Position XYZ
- Rotation XYZ

## Design rules

1. Server-authoritative state is the project invariant.
2. Skyrim engine access stays behind the client adapter/reverse-engineering layer.
3. Server and protocol code must not contain Skyrim memory offsets.
4. Raw offsets must be documented before use.
5. Read-only observation precedes hooks or writes.
6. Every runtime interface must be experimentally verified in-game.
