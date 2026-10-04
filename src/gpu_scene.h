#pragma once

#include "render_gpu.h"
#include <memory>
#include <unordered_map>

// Owns all GPU resources associated with one uploaded scene. The scene data
// can outlive an individual render job; camera changes must not recreate it.
class GPUScene {
public:
    struct GeometryData {
        std::vector<float3> vertices;
        std::vector<float3> normals;
        std::vector<float2> uvs;
        std::vector<int> materialIds;
    };

    struct TextureData {
        std::vector<ImageTexture*> unique;
        std::unordered_map<ImageTexture*, int> ids;
        std::vector<int> widths;
        std::vector<int> heights;
        std::vector<int> channels;
        std::vector<const unsigned char*> data;
    };

    static GeometryData packGeometry(const Model& model);
    static TextureData collectTextures(const Model& model);
    static std::unique_ptr<GPUScene> load(const Model& model);

    struct MaterialData {
        std::vector<float3> baseColorFactors;
        std::vector<float3> emissionFactors;
        std::vector<float> roughnessFactors, metallicFactors, specularFactors;
        std::vector<float> opacities, iors;
        std::vector<int> baseColorTexIds, normalTexIds;
        std::vector<int> roughnessTexIds, metallicTexIds, emissionTexIds;
        std::vector<int> packedSpecFlags, alphaCutout, uvFlipU, shadowOpaque;
        std::vector<float3> emissionAxis;
        std::vector<float> emissionLobeK, emissionLobeU, emissionGamma;
    };

    static MaterialData createMaterialData(const Model& model,
                                            const TextureData& textures,
                                            float emissionScale);
    static void applyMaterialTuning(const Model& model, MaterialData& materials);
    static std::vector<int> classifyOpaqueMaterials(const MaterialData& materials,
                                                     const TextureData& textures);

    struct LightData {
        std::vector<GPULightTri> triangles;
        std::vector<float> trianglePrefix;
        std::vector<int> objectFirst;
        std::vector<float> objectArea, objectPower;
        std::vector<float3> objectCentroid;
    };

    static LightData collectLights(const Model& model,
                                   const MaterialData& materials,
                                   float emissionScale);

    struct TextureUploadData {
        std::vector<int> formats;
        std::vector<const unsigned char*> blocks;
        std::vector<long long> byteOffsets;
        std::vector<int> texelOffsets;
        unsigned long long* objects = nullptr;
        int* widths = nullptr;
        int* heights = nullptr;
        unsigned char* rawData = nullptr;
        int uploaded = 0;
    };

    static TextureUploadData uploadTextures(const TextureData& textures);
    static LinearBVHNode* flattenBVH(const Model& model);

    struct SkyData {
        cudaTextureObject_t texture = 0;
        float* cdf = nullptr;
        float cdfSum = 0.0f;
        int width = 0;
        int height = 0;
    };

    static SkyData uploadSky(const Model& model);

    static GPUModelData uploadModel(const GeometryData& geometry,
                                    const LinearBVHNode* bvhNodes,
                                    int numBVHNodes,
                                    const GPUMaterialBuffer& materials,
                                    const GPULightTri* lightTriangles,
                                    int numLightTriangles,
                                    const float* trianglePrefix,
                                    const int* objectFirst,
                                    const float* objectArea,
                                    const float* objectPower,
                                    const float3* objectCentroid,
                                    int numObjects);

    struct UploadedResources {
        GPUModelData model{};
        unsigned long long* textureObjects = nullptr;
        int textureCount = 0;
        unsigned char* textureData = nullptr;
        int* textureWidths = nullptr;
        int* textureHeights = nullptr;
        cudaTextureObject_t skyTexture = 0;
        float* skyCDF = nullptr;
        float skyCDFSum = 0.0f;
        int skyWidth = 0;
        int skyHeight = 0;
    };

    GPUScene() = default;
    GPUScene(const GPUScene&) = delete;
    GPUScene& operator=(const GPUScene&) = delete;

    GPUScene(GPUScene&& other) noexcept { adoptFrom(other); }
    GPUScene& operator=(GPUScene&& other) noexcept {
        if (this != &other) {
            release();
            adoptFrom(other);
        }
        return *this;
    }

    ~GPUScene() { release(); }

    // Takes ownership of resources produced by the upload/build stage.
    // After this call, the caller must not free any field in resources.
    void attach(UploadedResources resources) {
        release();
        model_ = resources.model;
        textureObjects_ = resources.textureObjects;
        textureCount_ = resources.textureCount;
        textureData_ = resources.textureData;
        textureWidths_ = resources.textureWidths;
        textureHeights_ = resources.textureHeights;
        skyTexture_ = resources.skyTexture;
        skyCDF_ = resources.skyCDF;
        skyCDFSum_ = resources.skyCDFSum;
        skyWidth_ = resources.skyWidth;
        skyHeight_ = resources.skyHeight;
    }

    bool ready() const { return model_.bvhNodes != nullptr; }
    GPUModelData& model() { return model_; }
    const GPUModelData& model() const { return model_; }
    cudaTextureObject_t skyTexture() const { return skyTexture_; }
    float* skyCDF() const { return skyCDF_; }
    float skyCDFSum() const { return skyCDFSum_; }
    int skyWidth() const { return skyWidth_; }
    int skyHeight() const { return skyHeight_; }

    void release() {
        freeGPUModel(model_);
        if (textureObjects_)
            freeGPUTextures(textureObjects_, textureCount_);
        freeGPUTextureBuffers(textureData_, textureWidths_, textureHeights_);
        if (skyTexture_)
            freeSkyBoxTexture(skyTexture_);
        if (skyCDF_)
            freeGPUCDF(skyCDF_);
        textureObjects_ = nullptr;
        textureCount_ = 0;
        textureData_ = nullptr;
        textureWidths_ = nullptr;
        textureHeights_ = nullptr;
        skyTexture_ = 0;
        skyCDF_ = nullptr;
        skyCDFSum_ = 0.0f;
        skyWidth_ = 0;
        skyHeight_ = 0;
    }

private:
    void adoptFrom(GPUScene& other) {
        model_ = other.model_;
        textureObjects_ = other.textureObjects_;
        textureCount_ = other.textureCount_;
        textureData_ = other.textureData_;
        textureWidths_ = other.textureWidths_;
        textureHeights_ = other.textureHeights_;
        skyTexture_ = other.skyTexture_;
        skyCDF_ = other.skyCDF_;
        other.model_ = {};
        other.textureObjects_ = nullptr;
        other.textureCount_ = 0;
        other.textureData_ = nullptr;
        other.textureWidths_ = nullptr;
        other.textureHeights_ = nullptr;
        other.skyTexture_ = 0;
        other.skyCDF_ = nullptr;
        skyCDFSum_ = other.skyCDFSum_;
        skyWidth_ = other.skyWidth_;
        skyHeight_ = other.skyHeight_;
        other.skyCDFSum_ = 0.0f;
        other.skyWidth_ = 0;
        other.skyHeight_ = 0;
    }

    GPUModelData model_{};
    unsigned long long* textureObjects_ = nullptr;
    int textureCount_ = 0;
    unsigned char* textureData_ = nullptr;
    int* textureWidths_ = nullptr;
    int* textureHeights_ = nullptr;
    cudaTextureObject_t skyTexture_ = 0;
    float* skyCDF_ = nullptr;
    float skyCDFSum_ = 0.0f;
    int skyWidth_ = 0;
    int skyHeight_ = 0;
};