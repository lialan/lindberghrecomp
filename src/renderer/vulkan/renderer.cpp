#include "renderer.hpp"
#include "colored_vert.hpp"
#include "colored_frag.hpp"
#include <cstddef>
#include <stdexcept>

namespace lindbergh::vk {

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
        create.format = VK_FORMAT_D32_SFLOAT;
        create.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image(device, create, depth_, depth_memory_);
        depth_view_ = view(device, depth_, create.format, info.layers, VK_IMAGE_ASPECT_DEPTH_BIT);

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
        attachments[1].format = VK_FORMAT_D32_SFLOAT;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
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
        VkImageView views[] = {color_view_, depth_view_, info.density_map};
        VkFramebufferCreateInfo framebuffer_info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        framebuffer_info.renderPass = render_pass;
        framebuffer_info.attachmentCount = pass.attachmentCount;
        framebuffer_info.pAttachments = views;
        framebuffer_info.width = info.width;
        framebuffer_info.height = info.height;
        framebuffer_info.layers = 1; // multiview selects array layers
        check(vkCreateFramebuffer(device.handles.device, &framebuffer_info, nullptr, &framebuffer), "Vulkan world framebuffer");
    } catch (...) { release(); throw; }
}

void Target::release()
{
    const auto d = device_.handles.device;
    if (framebuffer) vkDestroyFramebuffer(d, framebuffer, nullptr);
    if (render_pass) vkDestroyRenderPass(d, render_pass, nullptr);
    if (depth_view_) vkDestroyImageView(d, depth_view_, nullptr);
    if (color_view_) vkDestroyImageView(d, color_view_, nullptr);
    if (depth_) vkDestroyImage(d, depth_, nullptr);
    if (color_memory_) vkDestroyImage(d, color, nullptr);
    if (depth_memory_) vkFreeMemory(d, depth_memory_, nullptr);
    if (color_memory_) vkFreeMemory(d, color_memory_, nullptr);
}
Target::~Target() { release(); }

uint32_t RasterState::key() const
{
    if (cull > VK_CULL_MODE_FRONT_AND_BACK || depth_compare > VK_COMPARE_OP_ALWAYS ||
        (topology != VK_PRIMITIVE_TOPOLOGY_POINT_LIST && topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST &&
         topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST))
        throw std::invalid_argument("Invalid Vulkan raster state");
    return depth_test | (depth_write << 1) | (blend << 2) | (cull << 3) | (depth_compare << 5) | (topology << 8);
}

static ProgramSource colored_program()
{
    return {colored_vert, colored_frag, sizeof colored_vert, sizeof colored_frag,
        {{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX}},
        {{0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
         {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}}, {}};
}

Renderer::Renderer(Device &device, Target &target, PipelineCache &cache)
    : Renderer(device, target, cache, colored_program()) {}

Renderer::Renderer(Device &device, Target &target, PipelineCache &cache, const ProgramSource &program)
    : device_(device), target_(target), cache_(cache), bindings_(program.bindings), attributes_(program.attributes)
{
    if (!program.vertex || !program.fragment || !program.vertex_bytes || !program.fragment_bytes ||
        program.vertex_bytes % 4 || program.fragment_bytes % 4)
        throw std::invalid_argument("Invalid precompiled Vulkan shader modules");
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
    const auto key = state.key();
    if (pipelines_.count(key)) return;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (auto &s : stages) { s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; s.pName = "main"; }
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex_;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fragment_;
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings_.size()); vertex.pVertexBindingDescriptions = bindings_.data();
    vertex.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes_.size()); vertex.pVertexAttributeDescriptions = attributes_.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = state.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = state.cull; raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = state.depth_test; depth.depthWriteEnable = state.depth_write;
    depth.depthCompareOp = state.depth_compare;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 15;
    attachment.blendEnable = state.blend;
    attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1; blend.pAttachments = &attachment;
    VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
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
}

void Renderer::begin(VkCommandBuffer cmd, std::array<float, 4> color)
{
    if (cmd_ || !cmd) throw std::logic_error("Invalid Vulkan render pass begin");
    VkClearValue clear[2]{};
    for (unsigned i = 0; i < 4; i++) clear[0].color.float32[i] = color[i];
    clear[1].depthStencil = {1, 0};
    VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    pass.renderPass = target_.render_pass; pass.framebuffer = target_.framebuffer;
    pass.renderArea.extent = {target_.info.width, target_.info.height};
    pass.clearValueCount = 2; pass.pClearValues = clear;
    vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport{0, 0, float(target_.info.width), float(target_.info.height), 0, 1};
    VkRect2D scissor{{0, 0}, pass.renderArea.extent};
    vkCmdSetViewport(cmd, 0, 1, &viewport); vkCmdSetScissor(cmd, 0, 1, &scissor);
    cmd_ = cmd; bound_ = VK_NULL_HANDLE;
}

void Renderer::draw(FrameRing &ring, const Vertex *vertices, uint32_t count, const Matrices &matrices, RasterState state)
{
    if (!cmd_) throw std::logic_error("Vulkan draw outside render pass");
    if (!vertices || !count || (state.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && count % 3) ||
        (state.topology == VK_PRIMITIVE_TOPOLOGY_LINE_LIST && count % 2))
        throw std::invalid_argument("Invalid Vulkan primitive list");
    auto p = pipelines_.find(state.key());
    if (p == pipelines_.end()) throw std::logic_error("Unprepared Vulkan pipeline encountered during drawing");
    auto data = ring.upload(vertices, size_t(count) * sizeof(Vertex));
    if (bound_ != p->second) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->second); bound_ = p->second; }
    vkCmdBindVertexBuffers(cmd_, 0, 1, &data.buffer, &data.offset);
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof matrices, &matrices);
    vkCmdDraw(cmd_, count, 1, 0, 0);
    ++draws_;
}

void Renderer::draw_indexed(FrameRing &ring, const Vertex *vertices, uint32_t vertex_count,
                            const uint32_t *indices, uint32_t index_count,
                            const Matrices &matrices, RasterState state)
{
    if (!cmd_) throw std::logic_error("Vulkan draw outside render pass");
    if (!vertices || !vertex_count || !indices || !index_count)
        throw std::invalid_argument("Invalid Vulkan indexed draw");
    auto p = pipelines_.find(state.key());
    if (p == pipelines_.end()) throw std::logic_error("Unprepared Vulkan pipeline encountered during drawing");
    auto vertex = ring.upload(vertices, size_t(vertex_count) * sizeof(Vertex));
    auto index = ring.upload(indices, size_t(index_count) * sizeof(uint32_t), 4);
    if (bound_ != p->second) { vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->second); bound_ = p->second; }
    vkCmdBindVertexBuffers(cmd_, 0, 1, &vertex.buffer, &vertex.offset);
    vkCmdBindIndexBuffer(cmd_, index.buffer, index.offset, VK_INDEX_TYPE_UINT32);
    vkCmdPushConstants(cmd_, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof matrices, &matrices);
    vkCmdDrawIndexed(cmd_, index_count, 1, 0, 0, 0);
    ++draws_;
}

void Renderer::end()
{
    if (!cmd_) throw std::logic_error("Vulkan render pass is not recording");
    vkCmdEndRenderPass(cmd_); cmd_ = VK_NULL_HANDLE;
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
