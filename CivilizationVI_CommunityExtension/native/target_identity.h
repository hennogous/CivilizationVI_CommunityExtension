#pragma once
#include <cstddef>
#include <cstdint>

namespace AI::DistrictScores::GameCoreLayout {
// XP2 Windows Steam November 2026 hotfix (build 26407122). Its .text and
// section layout match the previous image. See native/README.md for the
// recovered signatures, caller/register usage and fixture. Requalify the offsets
// and instruction prefixes together before adding support for another image.
inline constexpr uintptr_t RegisterScriptDataRva = 0x5bdac0;
inline constexpr uintptr_t DistrictNetValueRva = 0x52a440;
inline constexpr uintptr_t NetValueFromPlotAnalysisReturnRva = 0x3e30be;
inline constexpr uintptr_t CalculateDistrictValuesRva = 0x3dfee0;
// DoTurnActivate calls CalculateDistrictValues here, then checks for a district.
// The pinned late-placement replay confirms this return boundary feeds both readers.
inline constexpr uintptr_t DistrictAggregationCallRva = 0x3e293c;
inline constexpr uintptr_t DistrictAggregationReturnRva = 0x3e2941;
inline constexpr unsigned char X64CallRel32 = 0xe8;
inline constexpr uintptr_t GetBestDistrictRva = 0x3e4a50;
inline constexpr uintptr_t DistrictPresentValueRva = 0x3e7f40;

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
