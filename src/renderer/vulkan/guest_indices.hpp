#pragma once
#include <cstdint>
#include <cstddef>
#include <map>
#include <tuple>
#include <vector>

namespace lindbergh::vk {
// One cache per guest buffer. Every BufferData/SubData revision invalidates
// its bounds; unchanged EBO ranges need neither decoding nor another upload.
class GuestIndexRanges {
public:
    uint32_t vertices(const void *shadow,size_t bytes,uint64_t revision,uint64_t offset,
                      uint32_t count,uint32_t type);
    static uint32_t decode(const void *data,uint32_t count,uint32_t type,std::vector<uint32_t> &out);
    static bool native(uint32_t mode,uint32_t type,uint64_t offset);
    uint64_t hits() const { return hits_; }
    uint64_t scans() const { return scans_; }
private:
    const void *shadow_=nullptr;
    size_t bytes_=0;
    uint64_t revision_=0,hits_=0,scans_=0;
    bool valid_=false;
    std::map<std::tuple<uint64_t,uint32_t,uint32_t>,uint32_t> ranges_;
};
} // namespace lindbergh::vk
