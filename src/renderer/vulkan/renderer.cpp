#include "renderer.hpp"
#include "colored_vert.hpp"
#include "colored_frag.hpp"
#include <cstddef>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <cstdlib>

namespace lindbergh::vk {
// Like command recording, optional counters have one renderer-thread owner.
// The host reads them only after draining the worker at shutdown.
namespace { Renderer::CommandStats command_totals; }
bool Renderer::command_stats_enabled() {
    static const bool enabled=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_COMMAND_STATS");return v&&*v=='1';}();return enabled;
}
Renderer::CommandStats Renderer::global_command_stats() {return command_totals;}
const char *Renderer::command_name(Command kind) {
    static const char *names[]={"pipeline","viewport","scissor","stencil-reference","stencil-compare","stencil-write",
        "depth-bias","blend-constants","descriptors","vertex-buffer","index-buffer","push-constants"};
    return names[size_t(kind)];
}
bool Renderer::emit(Command kind,bool redundant,bool always_filter) {
    const auto i=size_t(kind);
    if((filter_||always_filter)&&redundant){if(count_commands_){++commands_.filtered[i];++command_totals.filtered[i];}return false;}
    if(count_commands_){++commands_.issued[i];++command_totals.issued[i];}return true;
}
void Renderer::invalidate_state() {
    // Another renderer can have changed any binding since our last attachment.
    // Within one attachment our variants share the same pipeline layout and
    // dynamic-state declarations, so their cached values remain applicable.
    bound_=VK_NULL_HANDLE;viewport_valid_=scissor_valid_=streams_valid_=index_valid_=matrices_valid_=false;
    dynamic_valid_=0;for(auto &d:descriptors_)d.valid=false;
}
void Renderer::filter_state(bool enabled) {
    if(cmd_)throw std::logic_error("Cannot change state filtering during render pass");
    filter_=enabled;invalidate_state();
}


static void image(Device &device, const VkImageCreateInfo &info, VkImage &image, VkDeviceMemory &memory)
{
    const auto d = device.handles.device;
    check(vkCreateImage(d, &info, nullptr, &image), "create render image");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = device.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(d, &alloc, nullptr, &memory), "render image memory");
    check(vkBindImageMemory(d, image, memory, 0), "bind render image");
}

static VkImageView view(Device &device, VkImage image, VkFormat format, uint32_t layers, VkImageAspectFlags aspect)
{
    VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    info.format = format;
    info.subresourceRange = {aspect, 0, 1, 0, layers};
    VkImageView result;
    check(vkCreateImageView(device.handles.device, &info, nullptr, &result), "render image view");
    return result;
}

Target::Target(Device &device, TargetInfo target) : info(target), device_(device)
{
    if (!info.width || !info.height || (info.layers != 1 && info.layers != 2))
        throw std::invalid_argument("Invalid Vulkan target dimensions");
    if (info.layers == 2 && !device.capabilities.multiview)
        throw std::runtime_error("Stereo Vulkan target requires enabled multiview");
    if (info.density_map && (!device.capabilities.density_map || !device.capabilities.density_map_non_subsampled))
        throw std::runtime_error("Density-map target requires enabled non-subsampled fragment density maps");
    try {
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        create.imageType = VK_IMAGE_TYPE_2D;
        create.format = info.color_format;
        create.extent = {info.width, info.height, 1};
        create.mipLevels = 1;
        create.arrayLayers = info.layers;
        create.samples = VK_SAMPLE_COUNT_1_BIT;
        create.tiling = VK_IMAGE_TILING_OPTIMAL;
        create.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        color = info.color;
        if (!color) image(device, create, color, color_memory_);
        color_view_ = view(device, color, info.color_format, info.layers, VK_IMAGE_ASPECT_COLOR_BIT);
        create.format = Target::depth_format;
        create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        image(device, create, depth_, depth_memory_);
        depth_view_ = view(device, depth_, create.format, info.layers, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);

        VkAttachmentDescription attachments[3]{};
        attachments[0].format = info.color_format;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = info.final_layout;
        attachments[1] = attachments[0];
        attachments[1].format = Target::depth_format;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        VkAttachmentReference col{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &col;
        subpass.pDepthStencilAttachment = &depth;
        VkSubpassDependency dependencies[2]{};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                       VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        uint32_t mask = info.layers == 2 ? 3 : 1;
        VkRenderPassMultiviewCreateInfo multiview{VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
        multiview.subpassCount = 1;
        multiview.pViewMasks = &mask;
        multiview.correlationMaskCount = 1;
        multiview.pCorrelationMasks = &mask;
        VkRenderPassFragmentDensityMapCreateInfoEXT foveation{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
        if (info.density_map) {
            attachments[2].format = VK_FORMAT_R8G8_UNORM;
            attachments[2].samples = VK_SAMPLE_COUNT_1_BIT;
            attachments[2].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachments[2].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachments[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachments[2].initialLayout = attachments[2].finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
            foveation.fragmentDensityMapAttachment = {2, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
            multiview.pNext = &foveation;
            dependencies[0].srcStageMask |= VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
            dependencies[0].dstStageMask |= VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT;
            dependencies[0].srcAccessMask |= VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
            dependencies[0].dstAccessMask |= VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
        }
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.pNext = &multiview;
        pass.attachmentCount = info.density_map ? 3 : 2;
        pass.pAttachments = attachments;
        pass.subpassCount = 1;
        pass.pSubpasses = &subpass;
        pass.dependencyCount = 2;
        pass.pDependencies = dependencies;
        check(vkCreateRenderPass(device.handles.device, &pass, nullptr, &render_pass), "Vulkan world render pass");
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].initialLayout = info.final_layout;
        attachments[1].loadOp = attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        check(vkCreateRenderPass(device.handles.device, &pass, nullptr, &load_pass), "Vulkan preserving render pass");
        framebuffer=make_framebuffer(info.density_map);
        if(info.density_map)density_framebuffers_.push_back({info.density_map,framebuffer});
    } catch (...) { release(); throw; }
}

VkFramebuffer Target::make_framebuffer(VkImageView map)
{
    VkImageView views[]={color_view_,depth_view_,map};
    VkFramebufferCreateInfo create{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    create.renderPass=render_pass;create.attachmentCount=info.density_map?3:2;
    create.pAttachments=views;create.width=info.width;create.height=info.height;create.layers=1;
    VkFramebuffer result;
    check(vkCreateFramebuffer(device_.handles.device,&create,nullptr,&result),"Vulkan world framebuffer");
    return result;
}
void Target::prepare_density_maps(const std::vector<VkImageView> &maps)
{
    if(!info.density_map || maps.empty())throw std::invalid_argument("Target has no density-map attachment");
    density_framebuffers_.reserve(density_framebuffers_.size()+maps.size());
    for(auto map:maps) {
        if(!map)throw std::invalid_argument("Null density-map view");
        const auto found=std::find_if(density_framebuffers_.begin(),density_framebuffers_.end(),[&](const auto &e){return e.first==map;});
        if(found==density_framebuffers_.end())density_framebuffers_.push_back({map,make_framebuffer(map)});
    }
}
void Target::select_density_map(VkImageView map)
{
    const auto found=std::find_if(density_framebuffers_.begin(),density_framebuffers_.end(),[&](const auto &e){return e.first==map;});
    if(found==density_framebuffers_.end())throw std::invalid_argument("Density-map framebuffer was not prepared");
    framebuffer=found->second; // handle selection only: no Vulkan allocation/wait
}

void Target::release()
{
    const auto d = device_.handles.device;
    if(density_framebuffers_.empty()) {
        if(framebuffer)vkDestroyFramebuffer(d,framebuffer,nullptr);
    } else for(const auto &entry:density_framebuffers_)vkDestroyFramebuffer(d,entry.second,nullptr);
    if (load_pass) vkDestroyRenderPass(d, load_pass, nullptr);
    if (render_pass) vkDestroyRenderPass(d, render_pass, nullptr);
    if (depth_view_) vkDestroyImageView(d, depth_view_, nullptr);
    if (color_view_) vkDestroyImageView(d, color_view_, nullptr);
    if (depth_) vkDestroyImage(d, depth_, nullptr);
    if (color_memory_) vkDestroyImage(d, color, nullptr);
    if (depth_memory_) vkFreeMemory(d, depth_memory_, nullptr);
    if (color_memory_) vkFreeMemory(d, color_memory_, nullptr);
}
Target::~Target() { release(); }

RasterKey RasterState::key() const
{
    if (cull>3 || depth_compare>7 || topology>VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
        src_rgb>VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA || dst_rgb>VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
        src_alpha>VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA || dst_alpha>VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
        blend_rgb>VK_BLEND_OP_MAX || blend_alpha>VK_BLEND_OP_MAX || stencil_compare>7 ||
        stencil_fail>7 || stencil_depth_fail>7 || stencil_pass>7 || color_mask>15 || alpha_func>8 || fixed_texture>255 || fog_mode>3)
        throw std::invalid_argument("Invalid Vulkan raster state");
    // Disabled tests do not affect rendering. Canonical keys avoid creating
    // separate pipelines when the guest changes their dormant parameters.
    const auto compare=depth_test?depth_compare:VK_COMPARE_OP_ALWAYS;
    const uint64_t stencil_bits=stencil ? (uint64_t(1)<<16) | (uint64_t(stencil_compare)<<17) |
        (uint64_t(stencil_fail)<<20) | (uint64_t(stencil_depth_fail)<<23) | (uint64_t(stencil_pass)<<26)
        : (uint64_t(VK_COMPARE_OP_ALWAYS)<<17);
    const uint64_t blend_bits=blend ? uint64_t(src_rgb) | (uint64_t(dst_rgb)<<5) | (uint64_t(src_alpha)<<10) |
        (uint64_t(dst_alpha)<<15) | (uint64_t(blend_rgb)<<20) | (uint64_t(blend_alpha)<<23)
        : uint64_t(VK_BLEND_FACTOR_ONE) | (uint64_t(VK_BLEND_FACTOR_ONE)<<10);
    uint64_t a=depth_test | (uint64_t(depth_test && depth_write)<<1) | (uint64_t(blend)<<2) | (uint64_t(cull)<<3) |
        (uint64_t(compare)<<5) | (uint64_t(topology)<<8) | (uint64_t(color_mask)<<12) |
        stencil_bits | (uint64_t(depth_bias)<<29) |
        (uint64_t(clockwise)<<30) | (uint64_t(full_hud)<<31);
    uint64_t b=blend_bits | (uint64_t(alpha_func)<<26) |
        (uint64_t(fixed_texture)<<30) | (uint64_t(fixed_env)<<38) | (uint64_t(fog_mode)<<62);
    return {a,b};
}

static ProgramSource colored_program()
{
    return {colored_vert, colored_frag, sizeof colored_vert, sizeof colored_frag,
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}},
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}, {}};
}

static bool full_hud_capable(const ProgramSource &source)
{
    const auto count=source.vertex_bytes/4;
    if(!source.vertex||count<5||source.vertex[0]!=0x07230203)return false;
    for(size_t at=5;at<count;) {
        const auto words=source.vertex[at]>>16,op=source.vertex[at]&0xffff;
        if(!words||words>count-at)return false;
        // OpDecorate <id> SpecId 5: shared offline wrapper's full-HUD switch.
        if(op==71&&words==4&&source.vertex[at+2]==1&&source.vertex[at+3]==5)return true;
        at+=words;
    }
    return false;
}

Renderer::Renderer(Device &device, Target &target, PipelineCache &cache)
    : Renderer(device, target, cache, colored_program()) {}

Renderer::Renderer(Device &device, Target &target, PipelineCache &cache, const ProgramSource &program)
    : device_(device), target_(target), cache_(cache), bindings_(program.bindings), attributes_(program.attributes),
      descriptor_count_(static_cast<uint32_t>(program.descriptor_layouts.size()))
{
    if (!program.vertex || !program.fragment || !program.vertex_bytes || !program.fragment_bytes ||
        program.vertex_bytes % 4 || program.fragment_bytes % 4)
        throw std::invalid_argument("Invalid precompiled Vulkan shader modules");
    full_hud_capable_=full_hud_capable(program);
    static const bool filtering=[] {const auto *v=std::getenv("LINDBERGH_VULKAN_STATE_FILTER");return !v||*v!='0';}();
    filter_=filtering;count_commands_=command_stats_enabled();
    streams_.resize(bindings_.size());descriptors_.resize(descriptor_count_);
    try {
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = program.vertex_bytes;
        module.pCode = program.vertex;
        check(vkCreateShaderModule(device.handles.device, &module, nullptr, &vertex_), "vertex SPIR-V module");
        module.codeSize = program.fragment_bytes;
        module.pCode = program.fragment;
        check(vkCreateShaderModule(device.handles.device, &module, nullptr, &fragment_), "fragment SPIR-V module");
        VkPushConstantRange matrices{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Matrices)};
        VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        info.pushConstantRangeCount = 1;
        info.pPushConstantRanges = &matrices;
        info.setLayoutCount = static_cast<uint32_t>(program.descriptor_layouts.size());
        info.pSetLayouts = program.descriptor_layouts.data();
        check(vkCreatePipelineLayout(device.handles.device, &info, nullptr, &layout_), "draw pipeline layout");
    } catch (...) { release(); throw; }
}

void Renderer::prepare(RasterState state)
{
    if (cmd_) throw std::logic_error("Pipeline creation attempted during Vulkan draw recording");
    if(state.full_hud&&!full_hud_capable_)
        throw std::runtime_error("Offline vertex shader predates full-screen HUD depth support; rebuild the shader pack");
    const auto key = state.key();
    if (pipelines_.count(key)) return;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex_;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment_;
    const uint32_t constants[]={uint32_t(target_.info.guest_bottom_origin),state.alpha_func,state.fixed_texture,state.fixed_env,state.fog_mode,uint32_t(state.full_hud)};
    const VkSpecializationMapEntry entries[]={{0,0,4},{1,4,4},{2,8,4},{3,12,4},{4,16,4},{5,20,4}};
    VkSpecializationInfo specialization{6,entries,sizeof constants,constants};
    stages[0].pSpecializationInfo=stages[1].pSpecializationInfo=&specialization;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings_.size()); vertex.pVertexBindingDescriptions = bindings_.data();
    vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes_.size()); vertex.pVertexAttributeDescriptions = attributes_.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = state.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = state.cull; raster.frontFace = (target_.info.guest_bottom_origin != state.clockwise) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    raster.depthBiasEnable=state.depth_bias;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = state.depth_test; depth.depthWriteEnable = state.depth_test && state.depth_write;
    depth.depthCompareOp = state.depth_test?state.depth_compare:VK_COMPARE_OP_ALWAYS;
    depth.stencilTestEnable=state.stencil;
    depth.front=state.stencil ? VkStencilOpState{state.stencil_fail,state.stencil_pass,state.stencil_depth_fail,state.stencil_compare,~0u,~0u,0}
                             : VkStencilOpState{VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_STENCIL_OP_KEEP,VK_COMPARE_OP_ALWAYS,~0u,~0u,0};
    depth.back=depth.front;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = state.color_mask;
    attachment.blendEnable = state.blend;
    attachment.srcColorBlendFactor = state.blend?state.src_rgb:VK_BLEND_FACTOR_ONE;
    attachment.srcAlphaBlendFactor = state.blend?state.src_alpha:VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = state.blend?state.dst_rgb:VK_BLEND_FACTOR_ZERO;
    attachment.dstAlphaBlendFactor = state.blend?state.dst_alpha:VK_BLEND_FACTOR_ZERO;
    attachment.colorBlendOp = state.blend?state.blend_rgb:VK_BLEND_OP_ADD;
    attachment.alphaBlendOp = state.blend?state.blend_alpha:VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = uint32_t(sizeof dynamic_states / sizeof *dynamic_states); dynamic.pDynamicStates = dynamic_states;
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pipeline.stageCount = 2; pipeline.pStages = stages;
    pipeline.pVertexInputState = &vertex; pipeline.pInputAssemblyState = &assembly;
    pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster;
    pipeline.pMultisampleState = &samples; pipeline.pDepthStencilState = &depth;
    pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic;
    pipeline.layout = layout_; pipeline.renderPass = target_.render_pass;
    VkPipeline result;
    check(vkCreateGraphicsPipelines(device_.handles.device, cache_.handle(), 1, &pipeline, nullptr, &result), "prepare graphics pipeline");
    pipelines_.emplace(key, result);
    if (pipeline_created) pipeline_created(state);
}

void Renderer::begin(VkCommandBuffer cmd, std::array<float, 4> color, bool preserve, float depth, uint32_t stencil,
                     const VkRect2D *area)
{
    if (cmd_ || !cmd) throw std::logic_error("Invalid Vulkan render pass begin");
    VkClearValue clear[2]{};
    for (unsigned i = 0; i < 4; i++) clear[0].color.float32[i] = color[i];
    clear[1].depthStencil = {depth, stencil};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = preserve ? target_.load_pass : target_.render_pass; pass.framebuffer = target_.framebuffer;
    pass.renderArea.extent = {target_.info.width, target_.info.height};
    if (area) pass.renderArea = *area; // the caller guarantees nothing is drawn outside it
    pass.clearValueCount = 2; pass.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, target_.info.guest_bottom_origin ? float(target_.info.height) : 0,
                        float(target_.info.width), target_.info.guest_bottom_origin ? -float(target_.info.height) : float(target_.info.height), 0, 1};
    VkRect2D scissor = pass.renderArea;
    cmd_ = cmd;invalidate_state();
    if(emit(Command::Viewport,false))vkCmdSetViewport(cmd,0,1,&viewport);
    if(emit(Command::Scissor,false))vkCmdSetScissor(cmd,0,1,&scissor);
    viewport_=viewport;scissor_=scissor;viewport_valid_=scissor_valid_=true;
    const float blend_constants[4]{}; dynamic_state(0,~0u,~0u,0,0,blend_constants);
}

void Renderer::attach(VkCommandBuffer cmd)
{
    if (cmd_ || !cmd) throw std::logic_error("Invalid Vulkan pipeline attach");
    cmd_ = cmd;invalidate_state();
}
void Renderer::detach() { cmd_ = VK_NULL_HANDLE;invalidate_state(); }

void Renderer::viewport(float x, float y, float width, float height, VkRect2D scissor,float min_depth,float max_depth)
{
    if (!cmd_) throw std::logic_error("Viewport outside render pass");
    VkViewport vp{x, target_.info.guest_bottom_origin ? y + height : float(target_.info.height) - y - height,
                  width, target_.info.guest_bottom_origin ? -height : height, min_depth, max_depth};
    if(emit(Command::Viewport,viewport_valid_&&!std::memcmp(&viewport_,&vp,sizeof vp)))vkCmdSetViewport(cmd_,0,1,&vp);
    if(emit(Command::Scissor,scissor_valid_&&!std::memcmp(&scissor_,&scissor,sizeof scissor)))vkCmdSetScissor(cmd_,0,1,&scissor);
    viewport_=vp;scissor_=scissor;viewport_valid_=scissor_valid_=true;
}

void Renderer::dynamic_state(uint32_t ref, uint32_t compare_mask, uint32_t write_mask, float factor, float units, const float constants[4])
{
    if (!cmd_) throw std::logic_error("Dynamic state outside render pass");
    if(emit(Command::StencilReference,(dynamic_valid_&1)&&stencil_ref_==ref))vkCmdSetStencilReference(cmd_,VK_STENCIL_FACE_FRONT_AND_BACK,ref);
    if(emit(Command::StencilCompare,(dynamic_valid_&2)&&stencil_compare_==compare_mask))vkCmdSetStencilCompareMask(cmd_,VK_STENCIL_FACE_FRONT_AND_BACK,compare_mask);
    if(emit(Command::StencilWrite,(dynamic_valid_&4)&&stencil_write_==write_mask))vkCmdSetStencilWriteMask(cmd_,VK_STENCIL_FACE_FRONT_AND_BACK,write_mask);
    const std::array<float,2> bias{factor,units};
    if(emit(Command::DepthBias,(dynamic_valid_&8)&&!std::memcmp(depth_bias_.data(),bias.data(),sizeof bias)))vkCmdSetDepthBias(cmd_,units,0,factor);
    if(emit(Command::BlendConstants,(dynamic_valid_&16)&&!std::memcmp(blend_constants_.data(),constants,sizeof blend_constants_)))vkCmdSetBlendConstants(cmd_,constants);
    stencil_ref_=ref;stencil_compare_=compare_mask;stencil_write_=write_mask;depth_bias_=bias;
    std::copy(constants,constants+4,blend_constants_.begin());dynamic_valid_=31;
}

void Renderer::clear(VkImageAspectFlags aspects, std::array<float,4> color, float depth, uint32_t stencil, VkRect2D rect)
{
    if (!cmd_) throw std::logic_error("Clear outside render pass");
    VkClearAttachment attachments[2]{}; uint32_t count = 0;
    if (aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        auto &a = attachments[count++]; a.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        std::copy(color.begin(), color.end(), a.clearValue.color.float32);
    }
    if (aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        auto &a = attachments[count++]; a.aspectMask = aspects & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        a.clearValue.depthStencil = {depth, stencil};
    }
    VkClearRect area{rect,0,1};
    if (!count || !rect.extent.width || !rect.extent.height) return;
    vkCmdClearAttachments(cmd_,count,attachments,1,&area);
}

void Renderer::bind_descriptors(uint32_t first_set, uint32_t count, const VkDescriptorSet *sets,
                                uint32_t offset_count, const uint32_t *dynamic_offsets)
{
    if (!cmd_) throw std::logic_error("Descriptor binding outside Vulkan render pass");
    if (!count || !sets || first_set > descriptor_count_ || count > descriptor_count_ - first_set ||
        (offset_count && !dynamic_offsets))
        throw std::invalid_argument("Invalid Vulkan descriptor binding");
    auto &saved=descriptors_[first_set];
    const bool same=saved.valid&&saved.sets.size()==count&&saved.offsets.size()==offset_count&&
        std::equal(saved.sets.begin(),saved.sets.end(),sets)&&
        (!offset_count||std::equal(saved.offsets.begin(),saved.offsets.end(),dynamic_offsets));
    if(!emit(Command::Descriptors,same))return;
    vkCmdBindDescriptorSets(cmd_,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,first_set,count,sets,offset_count,dynamic_offsets);
    // A later partial bind invalidates any cached group overlapping its sets.
    // Offsets are kept with the complete bind, never guessed per descriptor.
    for(uint32_t set=0;set<descriptors_.size();++set) {
        auto &d=descriptors_[set];
        if(d.valid&&set<first_set+count&&first_set<set+d.sets.size())d.valid=false;
    }
    saved.sets.assign(sets,sets+count);
    if(offset_count)saved.offsets.assign(dynamic_offsets,dynamic_offsets+offset_count);else saved.offsets.clear();
    saved.valid=true;
}

void Renderer::draw(FrameRing &ring, const Vertex *vertices, uint32_t count, const Matrices &matrices, RasterState state)
{
    if (!cmd_) throw std::logic_error("Vulkan draw outside render pass");
    if (!vertices || !count || (state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && count % 3) ||
        (state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST && count % 2))
        throw std::invalid_argument("Invalid Vulkan primitive list");
    auto data = ring.upload(vertices, size_t(count) * sizeof(Vertex));
    draw_buffers(&data, 1, count, matrices, state);
}

void Renderer::draw_indexed(FrameRing &ring, const Vertex *vertices, uint32_t vertex_count,
                            const uint32_t *indices, uint32_t index_count,
                            const Matrices &matrices, RasterState state)
{
    if (!cmd_) throw std::logic_error("Vulkan draw outside render pass");
    if (!vertices || !vertex_count || !indices || !index_count)
        throw std::invalid_argument("Invalid Vulkan indexed draw");
    auto vertex = ring.upload(vertices, size_t(vertex_count) * sizeof(Vertex));
    auto index = ring.upload(indices, size_t(index_count) * sizeof(uint32_t), 4);
    const IndexStream indexed{index.buffer, index.offset, VK_INDEX_TYPE_UINT32};
    draw_buffers(&vertex, 1, index_count, matrices, state, &indexed);
}

void Renderer::draw_buffers(const Upload *streams, uint32_t stream_count, uint32_t count,
                            const Matrices &matrices, RasterState state, const IndexStream *indices)
{
    if (!cmd_) throw std::logic_error("Vulkan draw outside render pass");
    if (!count || stream_count != bindings_.size() || (stream_count && !streams) ||
        (state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && count % 3) ||
        (state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST && count % 2))
        throw std::invalid_argument("Invalid Vulkan geometry streams or primitive count");
    for (uint32_t i = 0; i < stream_count; ++i)
        if (!streams[i].buffer) throw std::invalid_argument("Null Vulkan vertex buffer");
    if (indices && (!indices->buffer ||
        (indices->type != VK_INDEX_TYPE_UINT16 && indices->type != VK_INDEX_TYPE_UINT32) ||
        indices->offset % (indices->type == VK_INDEX_TYPE_UINT16 ? 2 : 4)))
        throw std::invalid_argument("Invalid Vulkan index stream");
    auto p = pipelines_.find(state.key());
    if (p == pipelines_.end()) throw std::logic_error("Unprepared Vulkan pipeline encountered during drawing");
    if(emit(Command::Pipeline,bound_==p->second,true))vkCmdBindPipeline(cmd_,VK_PIPELINE_BIND_POINT_GRAPHICS,p->second);
    bound_=p->second;
    for(uint32_t i=0;i<stream_count;++i) {
        if(emit(Command::VertexBuffer,streams_valid_&&streams_[i].buffer==streams[i].buffer&&streams_[i].offset==streams[i].offset))
            vkCmdBindVertexBuffers(cmd_,bindings_[i].binding,1,&streams[i].buffer,&streams[i].offset);
        streams_[i]=streams[i];
    }
    streams_valid_=true;
    if(emit(Command::PushConstants,matrices_valid_&&!std::memcmp(&matrices_,&matrices,sizeof matrices)))
        vkCmdPushConstants(cmd_,layout_,VK_SHADER_STAGE_VERTEX_BIT,0,sizeof matrices,&matrices);
    matrices_=matrices;matrices_valid_=true;
    if(indices) {
        if(emit(Command::IndexBuffer,index_valid_&&index_.buffer==indices->buffer&&index_.offset==indices->offset&&index_.type==indices->type))
            vkCmdBindIndexBuffer(cmd_,indices->buffer,indices->offset,indices->type);
        index_=*indices;index_valid_=true;
        vkCmdDrawIndexed(cmd_, count, 1, 0, 0, 0);
    } else vkCmdDraw(cmd_, count, 1, 0, 0);
    ++draws_;
}

void Renderer::end()
{
    if (!cmd_) throw std::logic_error("Vulkan render pass is not recording");
    vkCmdEndRenderPass(cmd_); cmd_ = VK_NULL_HANDLE;invalidate_state();
}

void Renderer::release()
{
    const auto d = device_.handles.device;
    for (auto &[key, p] : pipelines_) vkDestroyPipeline(d, p, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (vertex_) vkDestroyShaderModule(d, vertex_, nullptr);
    if (fragment_) vkDestroyShaderModule(d, fragment_, nullptr);
}
Renderer::~Renderer() { release(); }

} // namespace lindbergh::vk
