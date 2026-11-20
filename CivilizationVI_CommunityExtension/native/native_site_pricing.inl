// Read-only native district site accounting, deliberately independent of a
// mod's building/transaction forecast. NetValue minus intrinsic Value keeps
// GameCore's harvest, improvement, resource-copy and land-purchase accounting
// while excluding the district yields that the caller prices itself. Neither
// function performs a placement. The ordinary AI uses these same evaluators.
// This include follows economy_pricing.inl; all borrowed objects die at return.
using DistrictIntrinsicFn = int* (__cdecl *)(int*, unsigned, int*, int, int,
    const void*, const void*, void*);

namespace SitePricingLayout {
// The 15 November trace recovered this NetValue/Value ABI. Use the Steam
// 26595377 addresses below and the identity in target_identity.h.
// NetValue passes Value an empty 24-byte Types::Vector<AiMapChange>.
// GameCore allocates its buffer, so release it through GameCore's own import.
constexpr uintptr_t IntrinsicDistrictRva = 0x52d4e0;
constexpr uintptr_t DistrictDefinitionsRva = 0x7d0920;
constexpr uintptr_t EditMapRva = 0x44d40, PlotLookupRva = 0x52d10;
constexpr uintptr_t PlotAttributesRva = 0x5f090, PlotDistrictTypeRva = 0x5e720;
constexpr uintptr_t AlignedFreeImportRva = 0x9b7680;
constexpr uintptr_t GlobalsPointerRva = 0xb8aa68;
constexpr size_t GlobalsAiOffset = 0x250, AiDiscountOffset = 0x120;
constexpr size_t AiHorizonsPointerOffset = 0x138, AttributeOwnerOffset = 0x1c;
// Preserve the readable range required by the plot and attributes quotes.
constexpr size_t PlotQuoteReadableBytes = 0x90;
constexpr int OrdinaryDistrictHorizonIndex = 1;
constexpr int WonderHorizonIndex = 2;
constexpr int MaxHorizon = 300;

struct Coordinates { int x, y; };
struct MapChanges {
    void* data{};
    // Native vector bookkeeping is opaque to CE. Only GameCore initializes
    // these16 bytes; the owning buffer is the first pointer, as in NetValue.
    std::array<unsigned char,16> bookkeeping{};
    MapChanges() = default;
    MapChanges(const MapChanges&) = delete;
    MapChanges& operator=(const MapChanges&) = delete;
    ~MapChanges() {
        if (data) (*Runtime::GetGameCoreGlobalAt<void (**)(void*)>(AlignedFreeImportRva))(data);
    }
};
static_assert(sizeof(MapChanges)==24);
}

bool site_pricing_ready{};
bool qualify_site_pricing() {
    using namespace SitePricingLayout;
    struct Prefix { uintptr_t rva; const char* bytes; size_t length; };
    const Prefix expected[]{
        {IntrinsicDistrictRva,"\x44\x89\x4c\x24\x20\x4c\x89\x44\x24\x18\x89\x54\x24\x10\x48\x89",16},
        {DistrictDefinitionsRva,"\x48\x83\xec\x28\xe8\xa7\x44\x87\xff\x48\x05\x70\x0b\x00\x00\x48",16},
        {SitePricingLayout::EditMapRva,"\x48\x8b\x05\x21\x5d\xb4\x00\x48",8},
        {PlotLookupRva,"\x44\x8b\x02\x41\x81\xf8\xf1\xd8\xff\xff\x0f\x84\x92\x00\x00\x00",16},
        {PlotAttributesRva,"\x0f\xb7\x49\x4e\xe9\x87\xd1\xfe\xff",9},
        {PlotDistrictTypeRva,"\x48\x83\xec\x38\x0f\xbe\x41\x1c\x48\x8d\x54\x24\x20\x66\x89\x44",16},
    };
    if (!economy_pricing_ready) return false;
    for (const auto& p:expected) {
        const auto* address=Runtime::GetGameCoreGlobalAt<const unsigned char*>(p.rva);
        if (!readable(address,p.length,false) || std::memcmp(address,p.bytes,p.length)) return false;
    }
    const auto* allocator=Runtime::GetGameCoreGlobalAt<void* const*>(AlignedFreeImportRva);
    return readable(allocator,sizeof(void*),false) && *allocator;
}

int l_evaluation_context(lua_State* L) {
    using namespace SitePricingLayout;
    if (!site_pricing_ready) return 0;
    const auto* globalSlot=Runtime::GetGameCoreGlobalAt<const unsigned char* const*>(GlobalsPointerRva);
    if (!readable(globalSlot,sizeof(void*),false)) return 0;
    const auto* globals=*globalSlot;
    if (!readable(globals,GlobalsAiOffset+sizeof(void*),false)) return 0;
    const auto* ai=read_field<const unsigned char*>(globals,GlobalsAiOffset);
    if (!readable(ai,AiHorizonsPointerOffset+sizeof(void*),false)) return 0;
    const auto* horizons=read_field<const int*>(ai,AiHorizonsPointerOffset);
    if (!readable(horizons, (WonderHorizonIndex + 1) * sizeof(int), false)) return 0;
    const int ordinary = horizons[OrdinaryDistrictHorizonIndex];
    const int wonder = horizons[WonderHorizonIndex];
    const int discount = read_field<int>(ai, AiDiscountOffset);
    if (ordinary<=0 || ordinary>MaxHorizon || discount<=0 || discount>64*Q8Units) return 0;
    char output[192];
    snprintf(output,sizeof output,"{\"ordinaryHorizon\":%d,\"wonderHorizon\":%d,\"discountQ8\":%d,\"maxHorizon\":%d}",
        ordinary,wonder,discount,MaxHorizon);
    push_json(L,output,std::strlen(output));
    return 1;
}

int l_district_site_adjustment(lua_State* L) {
    using namespace SitePricingLayout;
    const int owner=hks::checkinteger(L,1),cityId=hks::checkinteger(L,2),district=hks::checkinteger(L,3);
    const Coordinates xy{hks::checkinteger(L,4),hks::checkinteger(L,5)};
    const int ready=hks::checkinteger(L,6),horizon=hks::checkinteger(L,7),discount=hks::checkinteger(L,8);
    const unsigned char *player{},*city{};
    if (!site_pricing_ready || district<0 || district>=128 || xy.x<0 || xy.x>=256 || xy.y<0 || xy.y>=256 ||
        !quote_times(ready,horizon,discount) || !quote_objects(owner,cityId,player,city)) return 0;
    const auto* definitions=Runtime::GetGameCoreGlobalAt<DefinitionsFn>(DistrictDefinitionsRva)();
    if (!readable(definitions,2*sizeof(uintptr_t),false) || definitions[1]<definitions[0] ||
        static_cast<uintptr_t>(district)>=(definitions[1]-definitions[0])/sizeof(void*)) return 0;
    const auto* map=Runtime::GetGameCoreGlobalAt<const void* (*)()>(SitePricingLayout::EditMapRva)();
    if (!readable(map,sizeof(void*),false)) return 0;
    const auto* plot=Runtime::GetGameCoreGlobalAt<const unsigned char* (*)(const void*,const Coordinates*)>(PlotLookupRva)(map,&xy);
    if (!readable(plot, PlotQuoteReadableBytes, false) ||
        read_field<int16_t>(plot, PlotXOffset) != xy.x ||
        read_field<int16_t>(plot, PlotYOffset) != xy.y)
        return 0;
    const auto* attributes=Runtime::GetGameCoreGlobalAt<const unsigned char* (*)(const void*)>(PlotAttributesRva)(plot);
    if (!readable(attributes,AttributeOwnerOffset+1,false)) return 0;
    const int actualOwner=read_field<int8_t>(attributes,AttributeOwnerOffset);
    // This quote is only for new owned/frontier footprints. Repairs and
    // existing districts have separate native premiums and remain separate APIs.
    if ((actualOwner!=-1 && actualOwner!=owner) ||
        Runtime::GetGameCoreGlobalAt<int (*)(const void*)>(PlotDistrictTypeRva)(attributes)!=-1) return 0;
    int rate=discount,net{},intrinsic{};
    if (original_net(&net,static_cast<unsigned>(district),&rate,ready,horizon,
        const_cast<unsigned char*>(plot),const_cast<unsigned char*>(city))!=&net) return 0;
    MapChanges changes;
    rate=discount;
    if (Runtime::GetGameCoreGlobalAt<DistrictIntrinsicFn>(IntrinsicDistrictRva)(
        &intrinsic,static_cast<unsigned>(district),&rate,ready,horizon,plot,city,&changes)!=&intrinsic) return 0;
    // NetValue zero can be a feature/resource hard rejection. Do not convert
    // that short-circuit into an attractive mod forecast. No result is unknown.
    if (net==0 || net<=InvalidScore || intrinsic<=InvalidScore) return 0;
    const auto adjustment=static_cast<int64_t>(net)-intrinsic;
    if (adjustment < -MaxScore || adjustment > MaxScore) return 0;
    char output[192];
    snprintf(output,sizeof output,"{\"netQ8\":%d,\"intrinsicQ8\":%d,\"adjustmentQ8\":%lld,\"includesPurchase\":%d}",
        net,intrinsic,static_cast<long long>(adjustment),actualOwner==-1);
    push_json(L,output,std::strlen(output));
    return 1;
}
