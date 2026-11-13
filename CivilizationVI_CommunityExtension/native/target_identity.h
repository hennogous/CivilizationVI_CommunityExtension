#pragma once
#include <cstddef>
#include <cstdint>

namespace AI::DistrictScores::GameCoreLayout {
// XP2 Windows Steam November 2026 hotfix (build 26407122). Its .text and
// section layout match the previous image. See native/README.md for the
// recovered signatures, caller/register usage and fixture. Requalify the offsets
// and instruction prefixes together before adding support for another image.
// Generic economic quote helpers, rechecked against the text-identical November
// XP2 profile. NormalizeYields and timed income use Q8 pointers/return buffers;
// AmenityValue also receives the actual player and recipient city. No pointers
// escape a quote. Requalify these functions together with the layout below.
inline constexpr uintptr_t NativePlayerRegistryRva = 0x306b80;
inline constexpr uintptr_t NativeCityFindRva = 0x2c4360;
inline constexpr uintptr_t NormalizeYieldIncomeRva = 0x52b110;
inline constexpr uintptr_t DiscountTimedIncomeRva = 0x528660;
inline constexpr uintptr_t AmenityIncomeValueRva = 0x5272a0;
inline constexpr uintptr_t YieldDefinitionsRva = 0x7d1ce0;
// NativeCityFind reads the city collection through its end pointer at +0xa0.
inline constexpr size_t CityCollectionReadableBytes = 0xa8;
inline constexpr size_t RegistryPlayerArrayOffset = 0x20;
inline constexpr size_t PlayerCitiesPointerOffset = 0x6d0;
inline constexpr uintptr_t RegisterScriptDataRva = 0x5bdac0;
inline constexpr uintptr_t DistrictNetValueRva = 0x52a440;
inline constexpr uintptr_t NetValueFromPlotAnalysisReturnRva = 0x3e30be;
inline constexpr uintptr_t CalculateDistrictValuesRva = 0x3dfee0;
// DoTurnActivate calls CalculateDistrictValues here, then checks for a district.
// The pinned late-placement replay confirms this return boundary feeds both readers.
inline constexpr uintptr_t DistrictAggregationCallRva = 0x3e293c;
inline constexpr uintptr_t DistrictAggregationReturnRva = 0x3e2941;
inline constexpr unsigned char X64CallRel32 = 0xe8;
inline constexpr uintptr_t GetBestBuildingRva = 0x3e4710;
inline constexpr uintptr_t OrdinaryBuildingNominationCallRva = 0x3e1605;
inline constexpr uintptr_t OrdinaryBuildingNominationReturnRva = 0x3e160a;
// Both missing-prerequisite checks have passed at the flag restore.
inline constexpr uintptr_t BuildingNominationRestoreFlagRva = 0x3e49d5;
inline constexpr uintptr_t BuildingNominationComparisonRva = 0x3e49da;
inline constexpr unsigned char BuildingNominationRestoreFlag[] = {0x44, 0x0f, 0xb6, 0x6d, 0x48};
inline constexpr unsigned char BuildingNominationCompare[] = {0x44, 0x3b, 0x36, 0x7e, 0x11, 0x44, 0x89, 0x36};
inline constexpr uintptr_t GetBestDistrictRva = 0x3e4a50;
inline constexpr uintptr_t DistrictPresentValueRva = 0x3e7f40;
// Current XP2 SHA below; REA rechecked 13 Nov. Location takes (CityBuild*,
// district, allowFrontierFlag) and returns borrowed Plot* or null. Request
// Update takes (UnitRequestContract*, CityBuild*) and returns native success.
// It retains max(stored bid, current cache), so managed Quarter requests need
// an explicit rebase after that update. No other unit/building request is owned.
inline constexpr uintptr_t DistrictLocationRva = 0x3e5550;
inline constexpr uintptr_t RequestUpdateRva = 0x3fa5e0;
inline constexpr uintptr_t AssignRequestValueRva = 0x2ca0f0;
inline constexpr size_t RequestValueWrapperOffset = 0x148;
inline constexpr size_t RequestValueOffset = 0x158;
// CityDistricts::count(type, onlyComplete=false, includeReplacements=true), also
// used by DistrictLocation before it returns an existing repair footprint.
inline constexpr uintptr_t CountDistrictTypeRva = 0x11ec40;
inline constexpr size_t ManagedCityDistrictsOffset = 0xb58;
inline constexpr size_t RequestUnitOffset = 0x178;
inline constexpr size_t RequestBuildingOffset = 0x190;
inline constexpr size_t RequestDistrictOffset = 0x1a8;
inline constexpr size_t RequestCoordinatesOffset = 0x1c0;
inline constexpr size_t RequestReadableBytes = 0x1c8;

// CityBuild owns a bounded array of plot-analysis rows. Row zero is the city
// center. City and row identities are checked before observing a score.
inline constexpr size_t CityBuildCityOffset = 0x90;
inline constexpr size_t CityBuildRowsOffset = 0x4c8;
inline constexpr size_t CityBuildRowCountOffset = 0x4d8;
// FAutoVariable's editable district matrix, separate from row bindings.
inline constexpr size_t DistrictMatrixDataOffset = 0x518;
inline constexpr size_t DistrictMatrixCapacityOffset = 0x520;
inline constexpr size_t DistrictMatrixLengthOffset = 0x528;
inline constexpr size_t CityIdOffset = 0xa8;
inline constexpr size_t CityOwnerOffset = 0xd8;
inline constexpr size_t PlotXOffset = 0x28;
inline constexpr size_t PlotYOffset = 0x2a;
inline constexpr size_t PlotAnalysisRowSize = 0x78;
inline constexpr unsigned MaxCityPlotRows = 37;
inline constexpr size_t RowXOffset = 0x08;
inline constexpr size_t RowYOffset = 0x0c;
inline constexpr size_t RowDistrictScoresOffset = 0x20;
// The referenced native reservation starts with a signed request ID.
inline constexpr size_t RowReservationOffset = 0x48;
// Evaluator3e2aa0 reads Plot+44 (improvement type), not a district here.
// +51 is completed/unpillaged district only: neither flag can identify an
// unfinished foundation or a repair. Use the actual district collection.
inline constexpr size_t RowHasImprovementOffset = 0x50;
inline constexpr size_t RowHasCompletedDistrictOffset = 0x51;
// Already used by supplemental repair: CityDistricts::GetDistrictAt(xy).
// The collection includes unfinished and pillaged objects. Return is borrowed.
inline constexpr uintptr_t DistrictAtRva = 0x11e520;
inline constexpr size_t DistrictTypeOffset = 0xf0;
inline constexpr int UnreservedPlot = -1;
inline constexpr size_t RowDistrictEligibleOffset = 0x55;
inline constexpr size_t RowFrontierOffset = 0x57;
inline constexpr size_t RowOwnedOffset = 0x5a;
inline constexpr size_t RowOwnerOffset = 0x70;
inline constexpr size_t RowCityIdOffset = 0x74;

inline constexpr char GameCoreSha256[] = "092abfbb2176f8df04b5212f819dc269dd83aea866ff05aaf489904a1ff7c41f";
inline constexpr uint32_t GameCoreTimestamp = 1793649108u;
inline constexpr uint32_t GameCoreImageSize = 12976128u;
inline constexpr unsigned char fingerprint_register[32] = { 0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x45, 0x33, 0xc0, 0x48, 0x8d, 0x15, 0x90, 0x0a, 0x00, 0x00, 0x48, 0x8b, 0xd9, 0xe8, 0xa8, 0xfd, 0x3e, 0x00, 0x45, 0x33, 0xc0, 0x48, 0x8d, 0x15, 0x9e, 0x0b };
inline constexpr unsigned char fingerprint_net[32] = { 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x48, 0x8d, 0x6c, 0x24, 0xf1, 0x48, 0x81, 0xec, 0xa0, 0x00, 0x00, 0x00, 0x44, 0x8b, 0x75, 0x6f, 0x33, 0xc0 };
} // namespace AI::DistrictScores::GameCoreLayout
