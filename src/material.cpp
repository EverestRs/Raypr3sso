#include "material.h"
#include <algorithm>
#include <cmath>
#include <cctype>
#include <fstream>

// stb_image implementation (define once)
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

// =========================================================================
// DDS loader — supports DXT1/3/5 (BC1/2/3) and BC4/BC5
// =========================================================================
// DDS header structures
#pragma pack(push, 1)
struct DDS_PIXELFORMAT {
    uint32_t dwSize;        // 32
    uint32_t dwFlags;
    uint32_t dwFourCC;
    uint32_t dwRGBBitCount;
    uint32_t dwRBitMask;
    uint32_t dwGBitMask;
    uint32_t dwBBitMask;
    uint32_t dwABitMask;
};
struct DDS_HEADER {
    uint32_t dwSize;              // 124
    uint32_t dwFlags;
    uint32_t dwHeight;
    uint32_t dwWidth;
    uint32_t dwPitchOrLinearSize;
    uint32_t dwDepth;
    uint32_t dwMipMapCount;
    uint32_t dwReserved1[11];
    DDS_PIXELFORMAT ddpf;
    uint32_t dwCaps;
    uint32_t dwCaps2;
    uint32_t dwCaps3;
    uint32_t dwCaps4;
    uint32_t dwReserved2;
};
#pragma pack(pop)

// FourCC codes
#define FOURCC(c0,c1,c2,c3) (uint32_t(c0) | (uint32_t(c1)<<8) | (uint32_t(c2)<<16) | (uint32_t(c3)<<24))
#define DDS_MAGIC      FOURCC('D','D','S',' ')
#define FOURCC_DXT1    FOURCC('D','X','T','1')
#define FOURCC_DXT3    FOURCC('D','X','T','3')
#define FOURCC_DXT5    FOURCC('D','X','T','5')
#define FOURCC_BC4U    FOURCC('B','C','4','U')
#define FOURCC_BC4S    FOURCC('B','C','4','S')
#define FOURCC_ATI1    FOURCC('A','T','I','1')
#define FOURCC_BC5U    FOURCC('B','C','5','U')
#define FOURCC_BC5S    FOURCC('B','C','5','S')
#define FOURCC_ATI2    FOURCC('A','T','I','2')
// D3DFMT enum values (used by some DDS writers instead of FourCC strings)
#define D3DFMT_DXT1  0x00000004
#define D3DFMT_DXT2  0x00000005
#define D3DFMT_DXT3  0x00000006
#define D3DFMT_DXT4  0x00000007
#define D3DFMT_DXT5  0x00000008

// Little-endian helpers
inline uint16_t ImageTexture::le16(const uint8_t* p) {
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}
inline uint32_t ImageTexture::le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// ---- DXT1/BC1 decompress (RGB + 1-bit alpha, 8 bytes per 4x4 block) ----
void ImageTexture::decompressDXT1(const uint8_t* block, uint8_t* out,
                                   int w, int h, int rowPitch) {
    uint16_t c0 = le16(block);
    uint16_t c1 = le16(block + 2);

    // Expand RGB565 to RGB888
    auto expand565 = [](uint16_t c, uint8_t* rgb) {
        rgb[0] = ((c >> 11) & 0x1F) * 255 / 31;
        rgb[1] = ((c >>  5) & 0x3F) * 255 / 63;
        rgb[2] = ( c        & 0x1F) * 255 / 31;
    };

    uint8_t colors[4][4];  // [index][R,G,B,A]
    expand565(c0, colors[0]);
    expand565(c1, colors[1]);
    colors[0][3] = 255;
    colors[1][3] = 255;

    if (c0 > c1) {
        // Derived colors: 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1
        for (int c = 0; c < 3; ++c) {
            colors[2][c] = (2 * colors[0][c] + colors[1][c] + 1) / 3;
            colors[3][c] = (colors[0][c] + 2 * colors[1][c] + 1) / 3;
        }
        colors[2][3] = 255;
        colors[3][3] = 255;
    } else {
        // Transparent: c0, c1, blend, 0
        for (int c = 0; c < 3; ++c) {
            colors[2][c] = (colors[0][c] + colors[1][c] + 1) / 2;
            colors[3][c] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }

    // 4x4 pixel indices (2 bits each, 8 bytes total, indices start at byte 4)
    const uint8_t* idxBits = block + 4;
    for (int y = 0; y < 4 && y < h; ++y) {
        for (int x = 0; x < 4 && x < w; ++x) {
            int idx = (idxBits[y] >> (2 * x)) & 3;
            int offset = y * rowPitch + x * 4;
            out[offset + 0] = colors[idx][0];
            out[offset + 1] = colors[idx][1];
            out[offset + 2] = colors[idx][2];
            out[offset + 3] = colors[idx][3];
        }
    }
}

// ---- DXT3/BC2 decompress (explicit 4-bit alpha, 16 bytes per 4x4 block) ----
void ImageTexture::decompressDXT3(const uint8_t* block, uint8_t* out,
                                   int w, int h, int rowPitch) {
    // Alpha: 4 bits per pixel, first 8 bytes
    for (int y = 0; y < 4 && y < h; ++y) {
        for (int x = 0; x < 4 && x < w; ++x) {
            uint8_t a = (block[y * 2 + x / 2] >> (4 * (x % 2))) & 0xF;
            out[y * rowPitch + x * 4 + 3] = a * 17;  // 4-bit → 8-bit
        }
    }
    // Color: same as DXT1 (bytes 8-15)
    decompressDXT1(block + 8, out, w, h, rowPitch);
}

// ---- DXT5/BC3 decompress (interpolated 3-bit alpha, 16 bytes per 4x4 block) ----
void ImageTexture::decompressDXT5(const uint8_t* block, uint8_t* out,
                                   int w, int h, int rowPitch) {
    uint8_t a0 = block[0], a1 = block[1];

    // Build 8-entry alpha lookup table
    uint8_t alpha[8];
    alpha[0] = a0;
    alpha[1] = a1;
    if (a0 > a1) {
        for (int i = 2; i < 8; ++i)
            alpha[i] = ((8 - i) * a0 + (i - 1) * a1 + 3) / 7;
    } else {
        for (int i = 2; i < 6; ++i)
            alpha[i] = ((6 - i) * a0 + (i - 1) * a1 + 2) / 5;
        alpha[6] = 0;
        alpha[7] = 255;
    }

    // 3-bit alpha indices packed in 6 bytes (48 bits)
    const uint8_t* idxBits = block + 2;
    for (int y = 0; y < 4 && y < h; ++y) {
        for (int x = 0; x < 4 && x < w; ++x) {
            int bitOff = 3 * (y * 4 + x);
            int byteOff = bitOff / 8;
            int bitShift = bitOff % 8;
            int idx = (idxBits[byteOff] >> bitShift) & 7;
            if (byteOff + 1 < 6 && bitShift > 5)
                idx |= (idxBits[byteOff + 1] << (8 - bitShift)) & 7;
            out[y * rowPitch + x * 4 + 3] = alpha[idx & 7];
        }
    }
    // Color: same as DXT1 (bytes 8-15)
    decompressDXT1(block + 8, out, w, h, rowPitch);
}

// ---- BC4 decompress (single channel, 8 bytes per 4x4 block) ----
void ImageTexture::decompressBC4(const uint8_t* block, uint8_t* out,
                                  int w, int h, int rowPitch, int channel) {
    uint8_t r0 = block[0], r1 = block[1];
    uint8_t vals[8];
    vals[0] = r0;
    vals[1] = r1;
    if (r0 > r1) {
        for (int i = 2; i < 8; ++i)
            vals[i] = ((8 - i) * r0 + (i - 1) * r1 + 3) / 7;
    } else {
        for (int i = 2; i < 6; ++i)
            vals[i] = ((6 - i) * r0 + (i - 1) * r1 + 2) / 5;
        vals[6] = 0;
        vals[7] = 255;
    }

    const uint8_t* idxBits = block + 2;
    for (int y = 0; y < 4 && y < h; ++y) {
        for (int x = 0; x < 4 && x < w; ++x) {
            int bitOff = 3 * (y * 4 + x);
            int byteOff = bitOff / 8;
            int bitShift = bitOff % 8;
            int idx = (idxBits[byteOff] >> bitShift) & 7;
            if (byteOff + 1 < 6 && bitShift > 5)
                idx |= (idxBits[byteOff + 1] << (8 - bitShift)) & 7;
            out[y * rowPitch + x * 4 + channel] = vals[idx & 7];
        }
    }
}

// ---- BC5/ATI2 decompress (two BC4 channels, 16 bytes per 4x4 block) ----
void ImageTexture::decompressBC5(const uint8_t* block, uint8_t* out,
                                  int w, int h, int rowPitch) {
    // R channel from first BC4 (bytes 0-7), G channel from second BC4 (bytes 8-15)
    decompressBC4(block,     out, w, h, rowPitch, 0);  // R
    decompressBC4(block + 8, out, w, h, rowPitch, 1);  // G
    // Normal maps: compute B from R,G
    for (int y = 0; y < 4 && y < h; ++y) {
        for (int x = 0; x < 4 && x < w; ++x) {
            int offset = y * rowPitch + x * 4;
            float r = out[offset + 0] / 255.0f * 2.0f - 1.0f;
            float g = out[offset + 1] / 255.0f * 2.0f - 1.0f;
            float b = sqrtf(fmaxf(0.0f, 1.0f - r*r - g*g));
            out[offset + 2] = uint8_t((b * 0.5f + 0.5f) * 255.0f);
            out[offset + 3] = 255;
        }
    }
}

// ---- DDS file loading ----

void ImageTexture::bakeAverageTexel() {
    if (width <= 0 || height <= 0 || channels < 3 || data.empty()) return;
    // Average of the LINEARIZED texels (sRGB -> linear, pow 2.2): this value
    // feeds NEE flat light emission and must live in the same linear radiance
    // domain as the per-pixel shading paths (which convert with powf(x, 2.2)).
    // Averaging the raw sRGB bytes overestimated NEE light ~2.1x on the Bistro
    // lamp maps.
    // Sample a 2D GRID instead of a 1D stride: a stride of n/4096 on a
    // 2048-wide map aliases to the two columns x=0 and x=1024 only, which
    // produced a zero average (and thus zero NEE light) for mostly-dark maps
    // whose highlights lie off those columns.
    double r = 0, g = 0, b = 0, r22 = 0, g22 = 0, b22 = 0;
    const int GRID = 64;
    const int sx = std::max(1, width / GRID);
    const int sy = std::max(1, height / GRID);
    long samples = 0;
    // EMISSIVE CONTRAST: bake with gamma 5 (not the 2.2 linearization). The
    // official Bistro look has dark lamp frames/stems and only the bulbs and
    // glass glowing — with gamma 2.2 the map's mid-gray texels (~0.25) lit up
    // at radiance ~23 and even the metal stems glowed white. The total flux
    // total lamp flux is lower than a gamma-2.2 bake would give; the scene
    // exposure / sky-scale knobs are calibrated with gamma 5 in effect. The
    // kernel's per-pixel emission uses the same gamma (default 5), so NEE
    // and implicit hits share one radiance domain.
    const float EG = 5.0f;
    for (int y = 0; y < height; y += sy) {
        for (int x = 0; x < width; x += sx) {
            const uint8_t* px = &data[(size_t(y) * width + x) * channels];
            r += std::pow(px[0] / 255.0, EG);
            g += std::pow(px[1] / 255.0, EG);
            b += std::pow(px[2] / 255.0, EG);
            r22 += std::pow(px[0] / 255.0, 2.2);
            g22 += std::pow(px[1] / 255.0, 2.2);
            b22 += std::pow(px[2] / 255.0, 2.2);
            samples++;
        }
    }
    if (samples > 0) {
        emissionTexAvg = float3(float(r / samples),
                                float(g / samples),
                                float(b / samples));
        emissionTexAvg22 = float3(float(r22 / samples),
                                  float(g22 / samples),
                                  float(b22 / samples));
    }
}

void ImageTexture::desaturate() {
    if (data.empty() || channels < 3) return;
    cpuModified = true;   // force the RGBA8 upload path: the GPU cannot get
                          // these pixels from the raw BC blocks anymore
    for (size_t p = 0; p < data.size(); p += channels) {
        uint8_t r = data[p], g = data[p+1], b = data[p+2];
        uint8_t gray = static_cast<uint8_t>(std::min(255.0f,
            0.299f * r + 0.587f * g + 0.114f * b + 0.5f));
        data[p] = gray; data[p+1] = gray; data[p+2] = gray;
    }
    bakeAverageTexel();
}

bool ImageTexture::loadDDS(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) return false;

    // Read magic
    uint32_t magic;
    file.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != DDS_MAGIC) return false;

    // Read header
    DDS_HEADER header;
    file.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (header.dwSize != 124) return false;

    width  = static_cast<int>(header.dwWidth);
    height = static_cast<int>(header.dwHeight);
    channels = 4;  // Always decompress to RGBA

    fprintf(stderr, "  DDS: %s  %dx%d  mips=%u  fourCC=0x%08x\n",
            filename.c_str(), width, height,
            header.dwMipMapCount, header.ddpf.dwFourCC);

    // Sanity check
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        fprintf(stderr, "  DDS: invalid dimensions!\n");
        return false;
    }

    // Determine format
    uint32_t fourCC = header.ddpf.dwFourCC;
    // Normalize fourCC: D3DFMT enum values (0-0x100) → FourCC string
    uint32_t fmt = fourCC;
    if (fmt <= 0x100) {
        switch (fmt) {
            case D3DFMT_DXT1: fmt = FOURCC_DXT1; break;
            case D3DFMT_DXT2: case D3DFMT_DXT3: fmt = FOURCC_DXT3; break;
            case D3DFMT_DXT4: case D3DFMT_DXT5: fmt = FOURCC_DXT5; break;
            default: break;
        }
    }
    // DX10 extension: read header and default to DXT5
    if (fmt == FOURCC('D','X','1','0')) {
        fmt = FOURCC_DXT5;
    }

    // Calculate number of mip levels
    int mipCount = (header.dwMipMapCount > 0) ? static_cast<int>(header.dwMipMapCount) : 1;

    // Read and decompress each mip level
    data.resize(size_t(width) * height * 4);

    int mw = width, mh = height;
    for (int mip = 0; mip < mipCount; ++mip) {
        int bw = (mw + 3) / 4;
        int bh = (mh + 3) / 4;

        int blockSize = 0;
        bool isCompressed = true;
        bool isBC5 = false;
        switch (fmt) {
            case FOURCC_DXT1: blockSize = 8;  break;
            case FOURCC_DXT3: blockSize = 16; break;
            case FOURCC_DXT5: blockSize = 16; break;
            case FOURCC_ATI2: case FOURCC_BC5U: case FOURCC_BC5S:
                blockSize = 16; isBC5 = true; break;
            case FOURCC_ATI1: case FOURCC_BC4U: case FOURCC_BC4S:
                blockSize = 8; break;
            default:
                isCompressed = false;
                break;
        }

        size_t mipDataSize = size_t(bw) * bh * blockSize;

        if (isCompressed) {
            std::vector<uint8_t> compressed(mipDataSize);
            file.read(reinterpret_cast<char*>(compressed.data()), mipDataSize);

            if (mip == 0) {
                // Keep the raw blocks for the BC1/BC3/BC5 formats the kernel can
                // decode, so the GPU can hold the texture compressed (see
                // GPUMaterialBuffer::texFormats). Other BC formats (e.g. DXT3 /
                // BC4) keep the RGBA8 upload path.
                if (fmt == FOURCC_DXT1) { compressedBlocks = compressed; bcFormat = 1; }
                else if (fmt == FOURCC_DXT5) { compressedBlocks = compressed; bcFormat = 2; }
                else if (isBC5) { compressedBlocks = compressed; bcFormat = 3; }
                // Decompress to data array (only top mip)
                uint8_t* dst = data.data();
                int rowPitch = width * 4;

                for (int by = 0; by < bh; ++by) {
                    for (int bx = 0; bx < bw; ++bx) {
                        int blockIdx = (by * bw + bx) * blockSize;
                        int px = bx * 4, py = by * 4;
                        int blockW = std::min(4, width - px);
                        int blockH = std::min(4, height - py);
                        uint8_t* blockDst = dst + py * rowPitch + px * 4;

                        if (isBC5) {
                            decompressBC5(compressed.data() + blockIdx,
                                          blockDst, blockW, blockH, rowPitch);
                        } else {
                            switch (fourCC) {
                                case FOURCC_DXT1:
                                    decompressDXT1(compressed.data() + blockIdx,
                                                   blockDst, blockW, blockH, rowPitch);
                                    break;
                                case FOURCC_DXT3:
                                    decompressDXT3(compressed.data() + blockIdx,
                                                   blockDst, blockW, blockH, rowPitch);
                                    break;
                                case FOURCC_DXT5:
                                    decompressDXT5(compressed.data() + blockIdx,
                                                   blockDst, blockW, blockH, rowPitch);
                                    break;
                                case FOURCC_BC4U: case FOURCC_BC4S: case FOURCC_ATI1: {
                                    // Single channel → replicate to RGB
                                    decompressBC4(compressed.data() + blockIdx,
                                                  blockDst, blockW, blockH, rowPitch, 0);
                                    for (int py2 = 0; py2 < blockH; ++py2)
                                        for (int px2 = 0; px2 < blockW; ++px2) {
                                            int off = py2 * rowPitch + px2 * 4;
                                            blockDst[off + 1] = blockDst[off + 0];
                                            blockDst[off + 2] = blockDst[off + 0];
                                            blockDst[off + 3] = 255;
                                        }
                                    break;
                                }
                                default: break;
                            }
                        }
                    }
                }
                channels = 4;
            }
            // Skip lower mips (we'll generate them ourselves)
        } else {
            // Uncompressed format
            if (mip == 0) {
                int bytesPerPixel;
                if (header.ddpf.dwRGBBitCount == 32) bytesPerPixel = 4;
                else if (header.ddpf.dwRGBBitCount == 24) bytesPerPixel = 3;
                else if (header.ddpf.dwRGBBitCount == 8) bytesPerPixel = 1;
                else { file.close(); return false; }

                size_t rowSize = size_t(mw) * bytesPerPixel;
                size_t pitch = (header.dwFlags & 0x8) ? header.dwPitchOrLinearSize : rowSize;
                // Some DDS store pitch in dwPitchOrLinearSize, others rowSize
                if (pitch < rowSize) pitch = rowSize;

                std::vector<uint8_t> rawData(pitch * mh);
                file.read(reinterpret_cast<char*>(rawData.data()), pitch * mh);

                data.resize(size_t(width) * height * 4);
                for (int y = 0; y < height; ++y) {
                    for (int x = 0; x < width; ++x) {
                        const uint8_t* src = rawData.data() + y * pitch + x * bytesPerPixel;
                        uint8_t* dst = data.data() + (y * width + x) * 4;
                        if (bytesPerPixel == 4) {
                            // BGRA → RGBA (DDS stores in BGRA order)
                            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = src[3];
                        } else if (bytesPerPixel == 3) {
                            // BGR → RGB
                            dst[0] = src[2]; dst[1] = src[1]; dst[2] = src[0]; dst[3] = 255;
                        } else {
                            dst[0] = dst[1] = dst[2] = src[0]; dst[3] = 255;
                        }
                    }
                }
            } else {
                // Skip lower mips
                size_t rowSize = size_t(mw) * (header.ddpf.dwRGBBitCount / 8);
                file.seekg(mipDataSize, std::ios::cur);
            }
        }



        // Next mip level
        mw = std::max(1, mw / 2);
        mh = std::max(1, mh / 2);
    }

    file.close();

    if (data.empty() || size_t(width) * height * 4 != data.size()) {
        fprintf(stderr, "  DDS: invalid data size %zu (expected %zu)\n",
                data.size(), size_t(width) * height * 4);
        return false;
    }

    generateMipmaps();
    bakeAverageTexel();
    return true;
}

// ---- ImageTexture::load (auto-detect DDS vs regular image) ----
bool ImageTexture::load(const std::string& filename) {
    // Detect DDS by extension
    if (filename.size() > 4) {
        std::string ext;
        for (int i = (int)filename.size() - 4; i < (int)filename.size(); ++i)
            ext += std::tolower(static_cast<unsigned char>(filename[i]));
        if (ext == ".dds") {
            if (loadDDS(filename)) { bakeAverageTexel(); return true; }
            return false;
        }
    }

    // Try loading as HDR first
    float* hdrData = stbi_loadf(filename.c_str(), &width, &height, &channels, 0);
    if (hdrData) {
        data.resize(size_t(width) * height * channels);
        for (int i = 0; i < width * height * channels; ++i) {
            data[i] = static_cast<uint8_t>(
                std::min(255.0f, hdrData[i] * 255.0f));
        }
        stbi_image_free(hdrData);
        generateMipmaps();
        return true;
    }

    // Fall back to LDR
    int reqChannels = 4;
    unsigned char* imgData = stbi_load(filename.c_str(), &width, &height, &channels, reqChannels);
    if (!imgData) return false;

    channels = reqChannels;
    data.assign(imgData, imgData + size_t(width) * height * channels);
    stbi_image_free(imgData);

    generateMipmaps();
    return true;
}

float4 ImageTexture::sample(float u, float v, float mipLevel) const {
    if (data.empty()) return float4(1.0f);

    // Wrap UV to [0,1)
    u = u - floorf(u);
    v = v - floorf(v);

    // Determine mip level
    // When mipLevel <= 0, use base level (no mipmapping).
    // When mipLevel > 0, interpret as duv and compute level via log2(duv * maxDimension).
    int level = 0, nextLevel = 0;
    float levelBlend = 0.0f;
    int numLevels = 1 + static_cast<int>(mipmaps.size());

    if (mipLevel > 0.0f) {
        float mapDepth = log2f(mipLevel * fmaxf(float(width), float(height)));
        mapDepth = fmaxf(0.0f, fminf(mapDepth, float(numLevels - 1)));
        level = static_cast<int>(mapDepth);
        nextLevel = std::min(level + 1, numLevels - 1);
        levelBlend = mapDepth - float(level);
    }

    // Helper: bilinear sample at a given mip level
    auto sampleLevel = [&](int lvl) -> float4 {
        int w = width, h = height;
        const uint8_t* src = data.data();
        if (lvl > 0) {
            w = std::max(1, width >> lvl);
            h = std::max(1, height >> lvl);
            src = mipmaps[lvl - 1].data.data();
        }
        // Map UV to pixel coordinates
        const float fx = u * w - 0.5f;
        const float fy = v * h - 0.5f;
        int ix = static_cast<int>(floorf(fx));
        int iy = static_cast<int>(floorf(fy));
        const float tx = fx - floorf(fx);
        const float ty = fy - floorf(fy);
        int x0 = (ix + w) % w;
        int x1 = (ix + 1 + w) % w;
        int y0 = (iy + h) % h;
        int y1 = (iy + 1 + h) % h;
        auto texel = [&](int x, int y, int c) -> float {
            return src[(size_t(y) * w + x) * channels + c] / 255.0f;
        };
        float4 result;
        for (int c = 0; c < 4; ++c) {
            float c00 = texel(x0, y0, c);
            float c10 = texel(x1, y0, c);
            float c01 = texel(x0, y1, c);
            float c11 = texel(x1, y1, c);
            float col = (c00 * (1 - tx) + c10 * tx) * (1 - ty)
                      + (c01 * (1 - tx) + c11 * tx) * ty;
            result[c] = col;
        }
        return result;
    };

    float4 r0 = sampleLevel(level);
    if (levelBlend > 0.0f && level != nextLevel) {
        float4 r1 = sampleLevel(nextLevel);
        for (int c = 0; c < 4; ++c)
            r0[c] = r0[c] * (1.0f - levelBlend) + r1[c] * levelBlend;
    }
    return r0;
}

bool ImageTexture::hasAlpha() const {
    return channels == 4;
}

void ImageTexture::generateMipmaps() {
    mipmaps.clear();
    std::vector<uint8_t> prevData = data;
    int pw = width, ph = height;
    while (pw > 1 || ph > 1) {
        int nw = std::max(1, pw / 2);
        int nh = std::max(1, ph / 2);
        ImageTexture mip;
        mip.width = nw;
        mip.height = nh;
        mip.channels = channels;
        mip.data.resize(size_t(nw) * nh * channels);

        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < channels; ++c) {
                    int sum = 0, cnt = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            int sx = x * 2 + dx;
                            int sy = y * 2 + dy;
                            if (sx < pw && sy < ph) {
                                sum += prevData[(size_t(sy) * pw + sx) * channels + c];
                                ++cnt;
                            }
                        }
                    }
                    mip.data[(size_t(y) * nw + x) * channels + c] =
                        static_cast<uint8_t>(cnt > 0 ? sum / cnt : 0);
                }
            }
        }
        // Store the mip data before moving mip into the vector
        // (after move, mip.data is empty, so we copy from the stored element)
        mipmaps.push_back(std::move(mip));
        prevData = mipmaps.back().data;  // Copy — NOT move — from the stored mip
        pw = nw;
        ph = nh;
    }
}

void ImageTexture::rebuildMipmapsSrgb(float gamma) {
    // Same geometry as generateMipmaps, but RGB averaging happens in the
    // pow(2.2) domain the shader decodes with (gpu_toLinear), and alpha
    // stays linear. Re-encode with pow(1/2.2) back into the stored bytes so
    // the sampling path is unchanged.
    mipmaps.clear();
    std::vector<uint8_t> prevData = data;
    int pw = width, ph = height;
    while (pw > 1 || ph > 1) {
        int nw = std::max(1, pw / 2);
        int nh = std::max(1, ph / 2);
        ImageTexture mip;
        mip.width = nw;
        mip.height = nh;
        mip.channels = channels;
        mip.data.resize(size_t(nw) * nh * channels);

        for (int y = 0; y < nh; ++y) {
            for (int x = 0; x < nw; ++x) {
                for (int c = 0; c < channels; ++c) {
                    double sum = 0.0;
                    int cnt = 0;
                    for (int dy = 0; dy < 2; ++dy) {
                        for (int dx = 0; dx < 2; ++dx) {
                            int sx = x * 2 + dx;
                            int sy = y * 2 + dy;
                            if (sx < pw && sy < ph) {
                                const double v =
                                    prevData[(size_t(sy) * pw + sx) * channels + c] / 255.0;
                                sum += (c == 3) ? v : std::pow(v, double(gamma));  // alpha linear
                                ++cnt;
                            }
                        }
                    }
                    const double mean = cnt > 0 ? sum / cnt : 0.0;
                    const double out = (c == 3) ? mean : std::pow(mean, 1.0 / double(gamma));
                    mip.data[(size_t(y) * nw + x) * channels + c] =
                        static_cast<uint8_t>(std::min(255.0, std::max(0.0, out * 255.0 + 0.5)));
                }
            }
        }
        mipmaps.push_back(std::move(mip));
        prevData = mipmaps.back().data;
        pw = nw;
        ph = nh;
    }
}

// ---- Material ----
// isEmissive() is now inline in material.h

bool Material::isTransparent() const {
    return opacity < 1.0f || hasFullyTransparentPart;
}

float4 Material::getBaseColor(float u, float v, float mipLevel) const {
    if (baseColorMap && !baseColorMap->empty()) {
        float4 c = baseColorMap->sample(u, v, mipLevel);
        // BaseColor maps are sRGB-encoded: convert to linear before lighting
        // (mirrors reference gammaPow). Without this, texture contrast is
        // compressed ~2.2x on display and wood/fabric grain disappears.
        return float4(powf(c.x, 2.2f), powf(c.y, 2.2f), powf(c.z, 2.2f), c.w);
    }
    return float4(baseColorFactor, 1.0f);
}

static bool glossMode() {
    // RENDER_GLOSS=1: interpret the packed SPECULAR G channel as GLOSSINESS
    // (roughness = 1 - G) instead of roughness. A/B switch for the Bistro
    // matte-vs-glossy question; defaults to the roughness interpretation.
    static const bool g = [] {
        const char* e = std::getenv("RENDER_GLOSS");
        return e && e[0] == '1';
    }();
    return g;
}

float Material::getRoughness(float u, float v, float mipLevel) const {
    if (roughnessMap && !roughnessMap->empty()) {
        float4 s = roughnessMap->sample(u, v, mipLevel);
        // Packed SPECULAR format stores roughness in G
        float r = packedSpecularMap ? s.y : s.x;
        return glossMode() ? 1.0f - r : r;
    }
    return roughnessFactor;
}

float Material::getMetallic(float u, float v, float mipLevel) const {
    if (metallicMap && !metallicMap->empty()) {
        return metallicMap->sample(u, v, mipLevel).x;
    }
    if (packedSpecularMap && roughnessMap && !roughnessMap->empty()) {
        // Packed SPECULAR format stores metallic in B
        return glm::min(roughnessMap->sample(u, v, mipLevel).z, 0.99f);
    }
    return metallicFactor;
}

float3 Material::getNormal(float u, float v, float mipLevel) const {
    if (normalMap && !normalMap->empty()) {
        float4 n = normalMap->sample(u, v, mipLevel);
        return glm::normalize(float3(n.x * 2.0f - 1.0f,
                                      n.y * 2.0f - 1.0f,
                                      n.z * 2.0f - 1.0f));
    }
    return float3(0.0f, 1.0f, 0.0f);
}

// ---- TextureLoader ----
ImageTexture* TextureLoader::load(const std::string& filepath) {
    auto* tex = new ImageTexture();
    if (tex->load(filepath)) {
        return tex;
    }
    delete tex;
    return nullptr;
}


