#pragma once
#include "shader_pack.hpp"
#include <map>
#include <set>

namespace lindbergh::vk {

// Portable descriptions, never driver cache bytes or guest resources. A seed
// can be packaged with a title and prepared against the current GPU at load.
// Retained renderers use the actual target and layouts reflected from its
// current offline shaders. Runtime descriptor sets have compatible layouts.
class PipelineRecipes {
  public:
    PipelineRecipes(Device &, PipelineCache &, std::vector<const ShaderPack *>, std::string writable_path,
                    std::string seed_path = {});
    ~PipelineRecipes();
    PipelineRecipes(const PipelineRecipes &) = delete;
    PipelineRecipes &operator=(const PipelineRecipes &) = delete;
    // Portable RGBA8 descriptions can rebuild for sRGB/FDM target variants;
    // every retained pipeline is created against the destination render pass.
    void prepare(Target &);
    std::shared_ptr<Renderer> renderer(Target &, const ShaderRecord &,
                                       const std::vector<VkVertexInputBindingDescription> &,
                                       const std::vector<VkVertexInputAttributeDescription> &);
    // Transfer retained renderers to the owner for fence-delayed destruction.
    std::vector<std::shared_ptr<Renderer>> retire(Target &);
    void save();
    size_t recipe_count() const { return recipes_.size(); }
    size_t first_use_count() const { return first_uses_; }
    size_t prepared_count() const { return prepared_; }

  private:
    struct Recipe {
        SourceDigest shader;
        std::array<uint32_t, 5> target;
        std::vector<VkVertexInputBindingDescription> bindings;
        std::vector<VkVertexInputAttributeDescription> attributes;
        RasterState raster;
    };
    Device &device_;
    PipelineCache &cache_;
    std::string path_;
    std::map<SourceDigest, const ShaderRecord *> shaders_;
    std::map<const ShaderRecord *, SourceDigest> identities_;
    std::map<std::string, Recipe> recipes_;
    std::map<std::pair<Target *, std::string>, std::shared_ptr<Renderer>> renderers_;
    std::shared_ptr<int> lifetime_ = std::make_shared<int>(0);
    bool dirty_ = false, preparing_ = false;
    size_t first_uses_ = 0, prepared_ = 0;
    static std::array<uint32_t, 5> target_class(const Target &);
    static std::string key(const Recipe &, bool raster);
    void load(const std::string &);
    void validate(const Recipe &) const;
};
} // namespace lindbergh::vk
