#include "../GeometryCoverageLayout.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

void require(bool value) { if (!value) throw std::runtime_error("Coverage layout check failed"); }
int main()
{
    try
    {
        using namespace GlassFg;
        std::array<GeometryCoverageRegion, 3> regions {{{7,9,33,2,4}, {}, {90,20,1,3,8}}};
        std::array<GeometryInstance, 3> map {};
        std::uint32_t words = 99;
        require(PackGeometryCoverage(regions, map, 128, 64, 6, words) && words == 6);
        require(map[0].validCoverage(words) && map[1].inactive() && map[2].validCoverage(words));
        require(map[0].pixelBase == 32 && map[2].pixelBase == 160 && map[2].statusIndex == 4);
        std::array<std::uint32_t, 8> storage {};
        storage.front() = storage.back() = 0xdeadbeef;
        for (auto index : {0,2})
        {
            const auto& m = map[index];
            for (unsigned y=0; y<m.height; ++y)
                for (unsigned x=0; x<m.width; ++x)
                {
                    const auto bit = m.pixelBase + y*m.stride+x;
                    require(bit/32 != m.statusIndex && bit/32 < words);
                    storage[1+bit/32] |= 1u << (bit%32);
                }
        }
        require(storage.front()==0xdeadbeef && storage.back()==0xdeadbeef);
        require(storage[1]==0 && storage[5]==0 && storage[2]==UINT32_MAX &&
                storage[3]==UINT32_MAX && storage[4]==3 && storage[6]==7);
        const auto before=map;
        require(!PackGeometryCoverage(regions,map,128,64,5,words));
        require(words==6 && std::memcmp(before.data(),map.data(),sizeof(map))==0);
        regions[2].left=UINT32_MAX;
        require(!PackGeometryCoverage(regions,map,128,64,6,words));
        require(std::memcmp(before.data(),map.data(),sizeof(map))==0);
        regions={};
        require(PackGeometryCoverage(regions,map,128,64,0,words) && words==0);
        for (auto& m:map) require(m.inactive());
        require(!PackGeometryCoverage(regions,map,128,64,std::uint64_t(UINT32_MAX)/32+1,words));
        std::cout << "COVERAGE_LAYOUT_OK packed_rows status_guards inactive capacity transaction\n";
        return 0;
    }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
