#include "resources.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace lindbergh::vk {

ConstantDensityMap::ConstantDensityMap(Device &d,FrameRing &ring,uint32_t width,uint32_t height,uint32_t layers,float density):device_(d)
{
    if(!d.capabilities.density_map || !d.capabilities.density_map_non_subsampled || !width || !height || (layers!=1&&layers!=2) ||
       !std::isfinite(density)||density<=0||density>1)throw std::invalid_argument("Invalid constant density map");
    try {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};create.imageType=VK_IMAGE_TYPE_2D;
        create.format=VK_FORMAT_R8G8_UNORM;create.extent={width,height,1};create.mipLevels=1;create.arrayLayers=layers;
        create.samples=VK_SAMPLE_COUNT_1_BIT;create.tiling=VK_IMAGE_TILING_OPTIMAL;
        create.usage=VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        check(vkCreateImage(d.handles.device,&create,nullptr,&image_),"constant density image");
        VkMemoryRequirements req;vkGetImageMemoryRequirements(d.handles.device,image_,&req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
        alloc.memoryTypeIndex=d.memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(d.handles.device,&alloc,nullptr,&memory_),"constant density memory");
        check(vkBindImageMemory(d.handles.device,image_,memory_,0),"bind constant density memory");
        VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};v.image=image_;v.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        v.format=create.format;v.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
        check(vkCreateImageView(d.handles.device,&v,nullptr,&view_),"constant density view");
        auto cmd=ring.begin();VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image=image_;b.subresourceRange=v.subresourceRange;b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        VkClearColorValue color{{density,density,0,0}};
        vkCmdClearColorImage(cmd,image_,b.newLayout,&color,1,&v.subresourceRange);
        b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,0,0,nullptr,0,nullptr,1,&b);
        // Static views can be read by the host at BeginRenderPass. Finish this
        // one-time initialization before publishing the view; never per frame.
        ring.wait(ring.submit());
    } catch(...) {release();throw;}
}
void ConstantDensityMap::release()
{
    if(view_)vkDestroyImageView(device_.handles.device,view_,nullptr);
    if(image_)vkDestroyImage(device_.handles.device,image_,nullptr);
    if(memory_)vkFreeMemory(device_.handles.device,memory_,nullptr);
}
ConstantDensityMap::~ConstantDensityMap() {release();}

GeometryBuffer::GeometryBuffer(Device &device, VkDeviceSize bytes) : device_(device), bytes_(bytes)
{
    if (!bytes) throw std::invalid_argument("Empty Vulkan geometry buffer");
    const auto d = device.handles.device;
    try {
        VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        create.size = bytes;
        create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        check(vkCreateBuffer(d, &create, nullptr, &buffer_), "geometry buffer");
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(d, buffer_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = device.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(d, &alloc, nullptr, &memory_), "geometry memory");
        check(vkBindBufferMemory(d, buffer_, memory_, 0), "bind geometry memory");
    } catch (...) { release(); throw; }
}

namespace {
// Rare overflow staging is retired by the same frame fence as ring uploads.
// It avoids flushing/waiting or rejecting a valid persistent geometry upload
// just because earlier commands consumed this slot's fixed upload arena.
struct GeometryStaging {
    Device &device;VkBuffer buffer=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;
    GeometryStaging(Device &d,const void *data,size_t bytes):device(d) {
        try {
            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=bytes;info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            check(vkCreateBuffer(d.handles.device,&info,nullptr,&buffer),"overflow geometry staging");
            VkMemoryRequirements req;vkGetBufferMemoryRequirements(d.handles.device,buffer,&req);
            VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;
            alloc.memoryTypeIndex=d.memory_type(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkAllocateMemory(d.handles.device,&alloc,nullptr,&memory),"overflow geometry staging memory");
            check(vkBindBufferMemory(d.handles.device,buffer,memory,0),"bind overflow geometry staging");
            void *mapped=nullptr;check(vkMapMemory(d.handles.device,memory,0,bytes,0,&mapped),"map overflow geometry staging");
            std::memcpy(mapped,data,bytes);vkUnmapMemory(d.handles.device,memory);
        }catch(...){release();throw;}
    }
    void release(){if(buffer)vkDestroyBuffer(device.handles.device,buffer,nullptr);if(memory)vkFreeMemory(device.handles.device,memory,nullptr);buffer=VK_NULL_HANDLE;memory=VK_NULL_HANDLE;}
    ~GeometryStaging(){release();}
};
}
void GeometryBuffer::upload(VkCommandBuffer cmd, FrameRing &ring, const void *data, size_t bytes, VkDeviceSize offset,
                            bool dedicated_staging)
{
    if (!cmd || !data || !bytes || offset % 4 || bytes % 4 || offset > bytes_ || bytes > bytes_ - offset)
        throw std::invalid_argument("Invalid Vulkan geometry upload");
    Upload source{};
    if(!dedicated_staging&&ring.can_upload(bytes,4))source=ring.upload(data,bytes,4);
    else {auto staging=std::make_shared<GeometryStaging>(device_,data,bytes);source={staging->buffer,0};ring.keep_alive(staging);ring.record_external_upload(bytes);}
    VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = buffer_; barrier.offset = offset; barrier.size = bytes;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &barrier, 0, nullptr);
    VkBufferCopy copy{source.offset, offset, bytes};
    vkCmdCopyBuffer(cmd, source.buffer, buffer_, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
}

void GeometryBuffer::release()
{
    if (buffer_) vkDestroyBuffer(device_.handles.device, buffer_, nullptr);
    if (memory_) vkFreeMemory(device_.handles.device, memory_, nullptr);
}
GeometryBuffer::~GeometryBuffer() { release(); }

Texture::Texture(Device &device, TextureInfo texture) : info(texture), device_(device)
{
    if (!info.width || !info.height || !info.levels || !info.layers ||
        info.width > device.capabilities.properties.limits.maxImageDimension2D ||
        info.height > device.capabilities.properties.limits.maxImageDimension2D ||
        info.layers > device.capabilities.properties.limits.maxImageArrayLayers)
        throw std::invalid_argument("Invalid Vulkan texture dimensions");
    uint32_t max_levels = 1;
    for (uint32_t side = std::max(info.width, info.height); side > 1; side >>= 1) ++max_levels;
    if (info.levels > max_levels) throw std::invalid_argument("Invalid Vulkan mip count");
    const auto identity = [](VkComponentSwizzle value, VkComponentSwizzle channel) {
        return value == VK_COMPONENT_SWIZZLE_IDENTITY || value == channel;
    };
    if (!device.capabilities.image_view_format_swizzle &&
        (!identity(info.components.r, VK_COMPONENT_SWIZZLE_R) ||
         !identity(info.components.g, VK_COMPONENT_SWIZZLE_G) ||
         !identity(info.components.b, VK_COMPONENT_SWIZZLE_B) ||
         !identity(info.components.a, VK_COMPONENT_SWIZZLE_A)))
        throw std::runtime_error("Texture component swizzles were not enabled on this Vulkan device");
    block_width_ = 1;
    switch (info.format) {
    case VK_FORMAT_R8_UNORM: block_bytes_ = 1; break;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R5G6B5_UNORM_PACK16: case VK_FORMAT_R4G4B4A4_UNORM_PACK16: block_bytes_ = 2; break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: block_bytes_ = 4; break;
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB:
    case VK_FORMAT_B8G8R8A8_UNORM: block_bytes_ = 4; break;
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        block_width_ = 4; block_bytes_ = 8; break;
    case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK:
        block_width_ = 4; block_bytes_ = 16; break;
    default: throw std::invalid_argument("Unsupported Vulkan texture format");
    }
    if (block_width_ == 4 && !device.capabilities.texture_compression_bc)
        throw std::runtime_error("BC compression was not enabled; offline texture conversion required");
    VkFormatProperties format;
    vkGetPhysicalDeviceFormatProperties(device.handles.physical, info.format, &format);
    const auto required = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    if ((format.optimalTilingFeatures & required) != required)
        throw std::runtime_error("GPU cannot sample requested texture format; offline asset conversion required");
    initialized_.resize(size_t(info.levels) * info.layers);
    const auto d = device.handles.device;
    try {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D; create.format = info.format;
        create.extent = {info.width, info.height, 1};
        create.mipLevels = info.levels; create.arrayLayers = info.layers;
        create.samples = VK_SAMPLE_COUNT_1_BIT; create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        check(vkCreateImage(d, &create, nullptr, &image_), "sampled texture");
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(d, image_, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = device.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(d, &alloc, nullptr, &memory_), "texture memory");
        check(vkBindImageMemory(d, image_, memory_, 0), "bind texture memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image_; view.format = info.format; view.components = info.components;
        view.viewType = info.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.subresourceRange = {info.format == Target::depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, info.levels, 0, info.layers};
        check(vkCreateImageView(d, &view, nullptr, &view_), "sampled texture view");
    } catch (...) { release(); throw; }
}

VkImageView Texture::array_view()
{
    if (info.layers > 1) return view_;
    if (!array_view_) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=image_;view.format=info.format;view.components=info.components;
        view.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.subresourceRange={info.format==Target::depth_format?VK_IMAGE_ASPECT_DEPTH_BIT:VK_IMAGE_ASPECT_COLOR_BIT,0,info.levels,0,1};
        check(vkCreateImageView(device_.handles.device,&view,nullptr,&array_view_),"one-layer array texture view");
    }
    return array_view_;
}

void Texture::upload(VkCommandBuffer cmd, FrameRing &ring, uint32_t level, uint32_t layer,
                     const void *data, size_t bytes)
{
    if (!cmd || !data || level >= info.levels || layer >= info.layers)
        throw std::invalid_argument("Invalid Vulkan texture upload subresource");
    const uint32_t width = std::max(1u, info.width >> level), height = std::max(1u, info.height >> level);
    const auto expected = size_t((width + block_width_ - 1) / block_width_) *
                          ((height + block_width_ - 1) / block_width_) * block_bytes_;
    if (bytes != expected) throw std::invalid_argument("Vulkan texture upload byte count mismatch");
    auto source = ring.upload(data, bytes, std::max(4u, block_bytes_));
    const auto index = size_t(layer) * info.levels + level;
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = initialized_[index] ? VK_ACCESS_SHADER_READ_BIT : 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.oldLayout = initialized_[index] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image_; barrier.subresourceRange = {VkImageAspectFlags(info.format == Target::depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT), level, 1, layer, 1};
    const auto shaders = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    vkCmdPipelineBarrier(cmd, initialized_[index] ? shaders : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    VkBufferImageCopy copy{};
    copy.bufferOffset = source.offset;
    copy.imageSubresource = {info.format == Target::depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1}; copy.imageExtent = {width, height, 1};
    vkCmdCopyBufferToImage(cmd, source.buffer, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, shaders, 0, 0, nullptr, 0, nullptr, 1, &barrier);
    initialized_[index] = true;
}

void Texture::copy_from(VkCommandBuffer cmd, Target &source, uint32_t level, int32_t dx, int32_t dy,
                         int32_t sx, int32_t sy, uint32_t width, uint32_t height,
                         uint32_t source_layer, uint32_t destination_layer, uint32_t layer_count)
{
    if (!layer_count || source_layer >= source.info.layers || layer_count > source.info.layers-source_layer ||
        destination_layer >= info.layers || layer_count > info.layers-destination_layer ||
        level >= info.levels || !width || !height || dx < 0 || dy < 0 || sx < 0 || sy < 0 ||
        uint64_t(dx)+width > std::max(1u,info.width>>level) || uint64_t(dy)+height > std::max(1u,info.height>>level) ||
        uint64_t(sx)+width > source.info.width || uint64_t(sy)+height > source.info.height)
        throw std::invalid_argument("Invalid Vulkan texture copy rectangle");
    const bool depth = info.format == Target::depth_format;
    const auto aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageAspectFlags all_aspects = depth ? VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const auto old = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : source.info.final_layout;
    // Render targets have one or two layers. Both eyes share one GPU copy
    // command; only destination layout transitions need per-layer state.
    VkImageMemoryBarrier barriers[3]{};
    const uint32_t barrier_count=1+layer_count;
    for (auto &b : barriers) { b.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; }
    auto &from=barriers[0]; from.image=depth?source.depth_image():source.color;
    from.subresourceRange={all_aspects,0,1,source_layer,layer_count}; from.oldLayout=old; from.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    from.srcAccessMask=depth?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    from.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    for (uint32_t layer=0;layer<layer_count;++layer) {
        const uint32_t destination=destination_layer+layer;
        const bool initialized=initialized_[size_t(destination)*info.levels+level];
        auto &to=barriers[1+layer];to.image=image_;
        to.subresourceRange={all_aspects,level,1,destination,1};
        to.oldLayout=initialized?VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:VK_IMAGE_LAYOUT_UNDEFINED;
        to.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;to.srcAccessMask=initialized?VK_ACCESS_SHADER_READ_BIT:0;
        to.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,barrier_count,barriers);
    if(source.info.guest_bottom_origin && (depth || source.info.color_format==info.format)) {
        VkImageCopy copy{}; copy.srcSubresource={aspect,0,source_layer,layer_count}; copy.srcOffset={sx,sy,0};
        copy.dstSubresource={aspect,level,destination_layer,layer_count}; copy.dstOffset={dx,dy,0}; copy.extent={width,height,1};
        vkCmdCopyImage(cmd,from.image,from.newLayout,image_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    } else {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(device_.handles.physical,info.format,&props);
        if(!(props.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT)) throw std::runtime_error("Texture copy requires supported Vulkan blit format");
        VkImageBlit blit{}; blit.srcSubresource={aspect,0,source_layer,layer_count}; blit.dstSubresource={aspect,level,destination_layer,layer_count};
        blit.srcOffsets[0]={sx,source.info.guest_bottom_origin?sy:int32_t(source.info.height)-sy,0};
        blit.srcOffsets[1]={sx+int32_t(width),source.info.guest_bottom_origin?sy+int32_t(height):int32_t(source.info.height)-sy-int32_t(height),1};
        blit.dstOffsets[0]={dx,dy,0}; blit.dstOffsets[1]={dx+int32_t(width),dy+int32_t(height),1};
        vkCmdBlitImage(cmd,from.image,from.newLayout,image_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    }
    from.oldLayout=from.newLayout; from.newLayout=old; from.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
    from.dstAccessMask=depth?VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT:
                           VK_ACCESS_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    for (uint32_t layer=0;layer<layer_count;++layer) {
        auto &to=barriers[1+layer];
        to.oldLayout=to.newLayout;to.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;to.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        initialized_[size_t(destination_layer+layer)*info.levels+level]=true;
    }
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT,0,0,nullptr,0,nullptr,barrier_count,barriers);
}

void Texture::release()
{
    if (array_view_) vkDestroyImageView(device_.handles.device, array_view_, nullptr);
    if (view_) vkDestroyImageView(device_.handles.device, view_, nullptr);
    if (image_) vkDestroyImage(device_.handles.device, image_, nullptr);
    if (memory_) vkFreeMemory(device_.handles.device, memory_, nullptr);
}
Texture::~Texture() { release(); }

Sampler::Sampler(Device &device, SamplerInfo info) : device_(device)
{
    if (!std::isfinite(info.min_lod) || !std::isfinite(info.max_lod) || info.min_lod < 0 || info.max_lod < info.min_lod)
        throw std::invalid_argument("Invalid Vulkan sampler LOD");
    VkSamplerCreateInfo create{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    create.magFilter = info.mag_filter; create.minFilter = info.min_filter; create.mipmapMode = info.mip_filter;
    create.addressModeU = info.u; create.addressModeV = info.v; create.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    create.compareEnable = info.compare; create.compareOp = info.compare_op; create.borderColor = info.border;
    create.minLod = info.min_lod; create.maxLod = info.max_lod;
    check(vkCreateSampler(device.handles.device, &create, nullptr, &sampler_), "texture sampler");
}
Sampler::~Sampler() { if (sampler_) vkDestroySampler(device_.handles.device, sampler_, nullptr); }

SampledSet::SampledSet(Device &device, const std::vector<SampledImage> &images) : device_(device)
{
    if (images.size() > device.capabilities.properties.limits.maxPerStageDescriptorSamplers)
        throw std::invalid_argument("Invalid Vulkan sampled image count");
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    std::vector<VkDescriptorImageInfo> descriptors;
    for (const auto &image : images) {
        if (!image.view || !image.sampler || (!bindings.empty() && image.binding <= bindings.back().binding))
            throw std::invalid_argument("Invalid or unsorted Vulkan sampled image binding");
        bindings.push_back({image.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        descriptors.push_back({image.sampler, image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    }
    const auto d = device.handles.device;
    try {
        VkDescriptorSetLayoutCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        create.bindingCount = static_cast<uint32_t>(bindings.size()); create.pBindings = bindings.data();
        check(vkCreateDescriptorSetLayout(d, &create, nullptr, &layout_), "sampled image layout");
        if (images.empty()) return;
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(images.size())};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pool.maxSets = 1; pool.poolSizeCount = 1; pool.pPoolSizes = &size;
        check(vkCreateDescriptorPool(d, &pool, nullptr, &pool_), "material descriptor pool");
        VkDescriptorSetAllocateInfo alloc{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        alloc.descriptorPool = pool_; alloc.descriptorSetCount = 1; alloc.pSetLayouts = &layout_;
        check(vkAllocateDescriptorSets(d, &alloc, &set_), "material descriptor set");
        std::vector<VkWriteDescriptorSet> writes;
        for (size_t i = 0; i < images.size(); ++i) {
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = set_; write.dstBinding = images[i].binding;
            write.descriptorCount = 1; write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            write.pImageInfo = &descriptors[i]; writes.push_back(write);
        }
        vkUpdateDescriptorSets(d, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    } catch (...) { release(); throw; }
}
void SampledSet::release()
{
    if (pool_) vkDestroyDescriptorPool(device_.handles.device, pool_, nullptr);
    if (layout_) vkDestroyDescriptorSetLayout(device_.handles.device, layout_, nullptr);
}
SampledSet::~SampledSet() { release(); }

} // namespace lindbergh::vk
