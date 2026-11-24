#pragma once

#include "../include/types.h"
#include <cstdint>
#ifndef __CUDACC__
#include <string>
#include <vector>
#endif

#ifndef __CUDACC__
// ---- Image Texture ----
class ImageTexture {
    int width = 0, height = 0, channels = 0;
    std::vector<uint8_t> data;
    std::vector<ImageTexture> mipmaps;
    std::string m_debugName;  // for debugging / deduplication
public:
    // Raw mip-0 BC blocks (BC1/BC3/BC5) kept from load so the GPU can store the
    // texture COMPRESSED and decode in-kernel: 0.5-1 byte/texel instead of 4.
    // Empty when the source was uncompressed (those keep the RGBA8 upload path).
    std::vector<uint8_t> compressedBlocks;
    int bcFormat = -1;   // -1 = none (RGBA8 upload), 1 = BC1(DXT1), 2 = BC3(DXT5), 3 = BC5(ATI2/BC5U)
    // Set when the CPU post-processes the decoded pixels (desaturate()): the GPU
    // must then upload the modified RGBA8 data instead of the raw BC blocks,
    // otherwise the two paths disagree on every texel of that texture.
    bool cpuModified = false;
    // Average RGB texel [0..1], baked immediately after decode while the
    // buffer is guaranteed intact (later heap activity may stomp it)
    float3 emissionTexAvg{1.0f, 1.0f, 1.0f};
    // gamma-2.2 domain average: string lights render their emissive map
    // at pow 2.2 (saturated bulb colors survive); their NEE flat emission
    // must use the same domain as the implicit per-pixel path.
    float3 emissionTexAvg22{1.0f, 1.0f, 1.0f};
    bool load(const std::string& filename);
    bool loadDDS(const std::string& filename);
    void bakeAverageTexel();
    // Rebuild levels 1..N with box averaging in the pow(2.2) domain — the
    // domain the GPU shader decodes baseColor/emission texels in. Averaging
    // raw sRGB bytes biases every mip darker (Jensen: mean(s^2.2) >=
    // mean(s)^2.2 — measured as a 9% interior luma drop). Data maps
    // (normal/roughness/metallic) keep the linear-byte chain: their texels
    // are not gamma-decoded at sampling time.
    void rebuildMipmapsSrgb(float gamma = 2.2f);
    float4 sample(float u, float v, float mipLevel = 0.0f) const;
    bool hasAlpha() const;
    // Luminance-preserving desaturation of the stored RGB data + re-bake of
    // the emissive average. Used for the ceiling-fan light whose emissive
    // map is warm but whose official look is white.
    void desaturate();
    bool empty() const { return width == 0; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
    int getChannels() const { return channels; }
    const uint8_t* getData() const { return data.data(); }

    // GPU mipmap chain access (P0 texture clarity fix): levels 1..N built by
    // generateMipmaps() at load, RGBA8, for trilinear sampling in-kernel.
    // Level 0 stays in its native storage (BC blocks or RGBA8) — the chain
    // is the coarser tail only.
    int getMipLevels() const { return int(mipmaps.size()); }
    int getMipWidth(int level) const { return mipmaps[level - 1].width; }
    int getMipHeight(int level) const { return mipmaps[level - 1].height; }
    const uint8_t* getMipData(int level) const { return mipmaps[level - 1].data.data(); }

    // Debug/dedup helpers
    const std::string& getDebugName() const { return m_debugName; }
    void setDebugName(const std::string& name) { m_debugName = name; }

private:
    void generateMipmaps();

    // DDS decompression helpers
    static void decompressDXT1(const uint8_t* block, uint8_t* out, int w, int h, int rowPitch);
    static void decompressDXT3(const uint8_t* block, uint8_t* out, int w, int h, int rowPitch);
    static void decompressDXT5(const uint8_t* block, uint8_t* out, int w, int h, int rowPitch);
    static void decompressBC4(const uint8_t* block, uint8_t* out, int w, int h, int rowPitch, int channel);
    static void decompressBC5(const uint8_t* block, uint8_t* out, int w, int h, int rowPitch);
    static uint16_t le16(const uint8_t* p);
    static uint32_t le32(const uint8_t* p);
};

// ---- PBR Material ----
class Material {
public:
    std::string name;

    float3 baseColorFactor{1.0f};
    float  roughnessFactor{0.8f};
    float  metallicFactor{0.0f};
    float  specularFactor{0.04f};
    float  opacity{1.0f};
    float  ior{1.0f};
    float3 emissionFactor{0.0f};
    float3 transmittingColor{0.0f};

    ImageTexture* baseColorMap   = nullptr;
    ImageTexture* normalMap      = nullptr;
    ImageTexture* roughnessMap   = nullptr;
    ImageTexture* metallicMap    = nullptr;
    ImageTexture* opacityMap     = nullptr;
    ImageTexture* emissionMap    = nullptr;
    // Bistro-style packed SPECULAR map (G=roughness, B=metallic)
    bool packedSpecularMap  = false;
    // Average emissive-map texel (baked at load time); NEE uses it to match
    // the kernel's per-point texture sampling without re-reading bytes later
    float3 emissionTexAvg   = float3(1.0f);
    float3 emissionTexAvg22 = float3(1.0f);   // gamma-2.2 domain (string lights)

    // Disney BSDF additional parameters
    float  subsurfaceFactor   = 0.0f;  // [0,1] sub-surface scattering
    float  sheenFactor        = 0.0f;  // [0,1] fabric/sheen
    float  sheenTintFactor    = 0.5f;  // [0,1] sheen color tint
    float  clearcoatFactor    = 0.0f;  // [0,1] clear coat
    float  clearcoatGlossFactor = 0.0f; // [0,1] clear coat gloss

    bool isEmissive() const {
        return glm::length(emissionFactor) > 0.0f ||
               (emissionMap != nullptr && !emissionMap->empty());
    }
    bool isTransparent() const;
    bool hasFullyTransparentPart = false;

    float4 getBaseColor(float u, float v, float mipLevel = 0.0f) const;
    float  getRoughness(float u, float v, float mipLevel = 0.0f) const;
    float  getMetallic(float u, float v, float mipLevel = 0.0f) const;
    float3 getNormal(float u, float v, float mipLevel = 0.0f) const;
};

// ---- Texture Loader ----
class TextureLoader {
public:
    static ImageTexture* load(const std::string& filepath);
};
#endif

// ---- GPU Material Buffer (SoA) ----
// Each scene has one MaterialBuffer on the GPU.
struct GPUMaterialBuffer {
    float3* baseColorFactors;    // [numMaterials]
    float*  roughnessFactors;
    float*  metallicFactors;
    float*  specularFactors;
    float*  opacities;
    float*  iors;
    float3* emissionFactors;
    float3* transmittingColors;
    int*    baseColorTexIds;     // -1 = no texture
    int*    normalTexIds;
    int*    roughnessTexIds;
    int*    metallicTexIds;
    int*    emissionTexIds;      // -1 = no emissive texture
    int*    packedSpecFlags;     // 1 = rough/metal read from packed G/B channels
    int     numMaterials;

#ifndef __CUDACC__
    typedef unsigned long long cudaTextureObject_t;
#endif
    cudaTextureObject_t* texObjects;  // Texture object array (nullptr if unused) -- BROKEN, use texData
    int numTextures;
    // RAW texture data (float4 RGBA) for manual sampling — works reliably
    int*    texDataOffsets; // prefix-sum start offset per texture (texels)
    unsigned char* texData; // [numTextures][w*h] flattened uchar4 RGBA data
    float3* emissionAxis;   // per-material emission direction axis (0,0,0 = uniform)
    float*  emissionLobeK;  // per-material emission lobe exponent (0 = uniform)
    float*  emissionLobeU;  // uniform fraction: lobe = U + (1-U)*dot^k
    float*  emissionGamma;
    int*    alphaCutout;    // alpha-cutout foliage: texel alpha < 0.5 = hole (pass through)
                            // (emission gamma: 5 = contrast bake for interior lamps;
                            //  2.2 for colored string lights whose pow5 colors crush)
    int*    uvFlipU;        // 1 = mirror u for this material's texture sampling
                            // (RENDER_UVFLIP_U comma list — e.g. the MenuSign board
                            //  whose authored UV reads mirrored from the camera side)
    int*    shadowOpaque;   // 1 = provably opaque blocker: shadow rays can treat
                            // ANY hit on this material as blocking without running
                            // the full hit-info evaluation (baseColor/roughness/
                            // metallic/emission texture fetches + powf). Proven at
                            // load: !alphaCutout && opacityFactor*minAlpha >= 0.99
                            //  => the dielectric test (opacity < 0.99) is false for
                            // every texel, so the old path always returned
                            // blocked=true. Bit-identical, skips the fetches.
    int*    texDataWidths;  // [numTextures]
    int*    texDataHeights; // [numTextures]
    // Compressed-texture support: textures whose source DDS was BC stay BC in
    // VRAM and are decoded in-kernel (0.5-1 B/texel instead of 4 → the Bistro's
    // 3.76 GB texture set becomes 0.73 GB). texFormats[texId]: 0 = RGBA8 (use
    // texDataOffsets/texDataWidths as before), 1 = BC1, 2 = BC3, 3 = BC5.
    // texByteOffsets[texId] is the byte offset of that texture's data inside
    // texData (BC blocks or RGBA8 texels), with [numTextures] holding the end.
    int*    texFormats;     // [numTextures]
    long long* texByteOffsets; // [numTextures + 1] (bytes — can exceed 2^31)

    // P0 mipmap chain (texture clarity): levels 1..N of every texture, RGBA8,
    // concatenated in mipData; per-texture byte ranges in mipByteOffsets
    // (prefix, [numTextures+1]); per-texture level counts in mipLevels
    // (0 = no chain → sampling falls back to level 0 = byte-identical old
    // path). Level dimensions are mipWidths/mipHeights as FLAT per-level
    // arrays indexed by mipLevelStart[t]+(level-1) — kept simple: we store
    // them per level in a parallel flat layout.
    unsigned char* mipData = nullptr;         // RGBA8 texels, all levels
    long long*     mipByteOffsets = nullptr;  // [numTextures + 1]
    int*           mipLevels = nullptr;       // [numTextures] chain length (0..N)
    int*           mipWidths = nullptr;       // [totalMipLevels] per-level width
    int*           mipHeights = nullptr;      // [totalMipLevels] per-level height
    int*           mipLevelStart = nullptr;   // [numTextures+1] index into widths
    // ---- P0 diagnostics: LOD distribution probe (RENDER_MIP_PROBE=texId) ----
    // When set, gpu_textureLod atomicMax-es the per-axis texel-footprint
    // ratios of that texture so the host can see whether the analytic LOD
    // ever reaches the deep levels (0 = disabled).
    int            mipProbe = 0;
    int            mipProbeTexId = -1;
    float*         mipProbeMaxU = nullptr;    // device float
    float*         mipProbeMaxV = nullptr;    // device float
};
