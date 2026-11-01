#include "guest_indices.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace lindbergh::vk {
namespace {
size_t width(uint32_t type) {
    switch(type){case 0x1401:return 1;case 0x1403:return 2;case 0x1405:return 4;}
    throw std::invalid_argument("Unsupported guest index type");
}
template<class T> uint32_t scan(const void *data,uint32_t count,uint32_t *out) {
    if(count&&!data)throw std::invalid_argument("Null guest index storage");
    const auto *p=static_cast<const uint8_t *>(data);uint32_t maximum=0;
    // Constant-width memcpy supports unaligned guest storage and allows the
    // compiler to vectorize the max reduction instead of calling memcpy per index.
    for(uint32_t i=0;i<count;++i){T v;std::memcpy(&v,p+size_t(i)*sizeof(T),sizeof(T));
        const auto value=uint32_t(v);maximum=std::max(maximum,value);if(out)out[i]=value;}
    if(maximum==UINT32_MAX)throw std::out_of_range("Guest index range overflow");
    return count?maximum+1:0;
}
uint32_t scan_type(const void *data,uint32_t count,uint32_t type,uint32_t *out) {
    switch(type){case 0x1401:return scan<uint8_t>(data,count,out);
        case 0x1403:return scan<uint16_t>(data,count,out);case 0x1405:return scan<uint32_t>(data,count,out);}
    throw std::invalid_argument("Unsupported guest index type");
}
}
bool GuestIndexRanges::native(uint32_t mode,uint32_t type,uint64_t offset) {
    if(type!=0x1403&&type!=0x1405)return false;
    if(offset%(type==0x1403?2:4))return false;
    // Points/lines/strips/triangles/fans retain their original order. Legacy
    // line loops, quads, quad strips and polygons still use explicit lowering.
    return mode==0||mode==1||mode==3||mode==4||mode==5||mode==6;
}
uint32_t GuestIndexRanges::decode(const void *data,uint32_t count,uint32_t type,std::vector<uint32_t> &out) {
    width(type);out.resize(count);return scan_type(data,count,type,out.data());
}
uint32_t GuestIndexRanges::vertices(const void *shadow,size_t bytes,uint64_t revision,uint64_t offset,
                                   uint32_t count,uint32_t type) {
    const auto size=width(type);
    if(offset>bytes||count>(bytes-size_t(offset))/size||(count&&!shadow))
        throw std::out_of_range("Guest element range exceeds buffer storage");
    if(!valid_||revision_!=revision||shadow_!=shadow||bytes_!=bytes) {
        ranges_.clear();revision_=revision;shadow_=shadow;bytes_=bytes;valid_=true;
    }
    const auto key=std::make_tuple(offset,count,type);
    const auto found=ranges_.find(key);if(found!=ranges_.end()){++hits_;return found->second;}
    const auto *data=count?static_cast<const uint8_t *>(shadow)+size_t(offset):nullptr;
    const auto bound=scan_type(data,count,type,nullptr);++scans_;
    // Bound metadata independently of a title's number of draw ranges.
    if(ranges_.size()>=1024)ranges_.clear();
    ranges_.emplace(key,bound);return bound;
}
} // namespace lindbergh::vk
