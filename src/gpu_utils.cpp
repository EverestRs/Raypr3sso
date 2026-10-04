// GPU utility functions (host-side, compiled by g++).
// Declares only the CUDA Runtime API functions we need.
// We cannot include any CUDA header due to float2/3/4 type conflicts with glm.
#include "render_gpu.h"

// Forward declarations for CUDA Runtime API
// We match the exact ABI: size_t = unsigned long on x86_64 Linux
#include <stddef.h>  // size_t
extern "C" {
    typedef enum { cudaMemcpyHostToDevice = 1, cudaMemcpyDeviceToHost = 2 } cudaMemcpyKind;
    typedef unsigned int cudaError_t;
    cudaError_t cudaMalloc(void** devPtr, size_t size);
    cudaError_t cudaFree(void* devPtr);
    cudaError_t cudaMemcpy(void* dst, const void* src, size_t count, cudaMemcpyKind kind);
    const char* cudaGetErrorString(cudaError_t error);
}

GPUModelData uploadModelToGPU(const float3* verts, int numVerts,
                               const float3* norms, const float2* uvs,
                               const int* matIds, int numFaces,
                               const LinearBVHNode* bvhNodes, int numBVHNodes,
                               const GPUMaterialBuffer& matBuf,
                               const GPULightTri* lightTris,
                               int numLightTris,
                               const float* triPrefix,
                               const int* objFirst, const float* objArea,
                               const float* objPower, const float3* objCentroid,
                               int numObjs) {
    GPUModelData data = {};  // Zero-initialize ALL fields (null all pointers)
    data.numFaces = numFaces;
    int numV = numFaces * 3;

    fprintf(stderr, "uploadModelToGPU: numV=%d numBVH=%d numMat=%d\n",
            numV, numBVHNodes, matBuf.numMaterials); fflush(stderr);
    // Helper to allocate GPU memory and copy data
    auto allocCpy = [](auto*& gpu, const auto* cpu, size_t bytes, const char* tag) {
        fprintf(stderr, "  alloc %s: %zu bytes (cpu=%p)\n", tag, bytes, (const void*)cpu); fflush(stderr);
        // Only cudaFree non-null GPU pointers to avoid driver issues
        if (gpu) {
            cudaError_t e = cudaFree(gpu);
            if (e != 0) fprintf(stderr, "  free %s FAILED: %s\n", tag, cudaGetErrorString(e));
            gpu = nullptr;
        }
        cudaError_t e;
        if (bytes > 0) {
            e = cudaMalloc((void**)&gpu, bytes);
            if (e != 0) {
                fprintf(stderr, "  alloc %s FAILED: %s\n", tag, cudaGetErrorString(e)); fflush(stderr);
                gpu = nullptr;
                return;
            }
            if (cpu) {
                e = cudaMemcpy(gpu, cpu, bytes, cudaMemcpyHostToDevice);
                if (e != 0) {
                    fprintf(stderr, "  cpy %s FAILED: %s\n", tag, cudaGetErrorString(e)); fflush(stderr);
                }
            }
        } else {
            gpu = nullptr;
        }
        fprintf(stderr, "  alloc %s: done (gpu=%p)\n", tag, (const void*)gpu); fflush(stderr);
    };

    allocCpy(data.vertPositions, verts, numV * sizeof(float3), "verts");
    allocCpy(data.vertNormals,   norms,  numV * sizeof(float3), "norms");
    allocCpy(data.vertUVs,       uvs,    numV * sizeof(float2), "uvs");
    allocCpy(data.materialIds,   matIds, numFaces * sizeof(int), "matIds");

    data.numBVHNodes = numBVHNodes;
    allocCpy(data.bvhNodes, bvhNodes, numBVHNodes * sizeof(LinearBVHNode), "bvh");

    // Material buffers (SoA)
    // CRITICAL: clear ALL material pointers to nullptr BEFORE allocCpy.
    // Otherwise allocCpy's cudaFree(host_ptr) would corrupt the CUDA driver state.
    data.materials = {};
    data.materials.numMaterials = matBuf.numMaterials;
    data.materials.numTextures = matBuf.numTextures;
    data.materials.texObjects = matBuf.texObjects;
    data.materials.texData = matBuf.texData;
    data.materials.texDataWidths = matBuf.texDataWidths;
    data.materials.texDataHeights = matBuf.texDataHeights;
    auto upMat = [&allocCpy](auto*& gpu, const auto* cpu, size_t sz, const char* n) { allocCpy(gpu, cpu, sz, n); };
    upMat(data.materials.baseColorFactors,  matBuf.baseColorFactors,  matBuf.numMaterials * sizeof(float3), "bc");
    upMat(data.materials.roughnessFactors,  matBuf.roughnessFactors,  matBuf.numMaterials * sizeof(float), "r");
    upMat(data.materials.metallicFactors,   matBuf.metallicFactors,   matBuf.numMaterials * sizeof(float), "m");
    upMat(data.materials.specularFactors,   matBuf.specularFactors,   matBuf.numMaterials * sizeof(float), "s");
    upMat(data.materials.opacities,         matBuf.opacities,         matBuf.numMaterials * sizeof(float), "o");
    upMat(data.materials.iors,             matBuf.iors,             matBuf.numMaterials * sizeof(float), "ior");
    upMat(data.materials.emissionFactors,   matBuf.emissionFactors,   matBuf.numMaterials * sizeof(float3), "em");
    upMat(data.materials.transmittingColors, matBuf.transmittingColors, matBuf.numMaterials * sizeof(float3), "tc");
    upMat(data.materials.baseColorTexIds,   matBuf.baseColorTexIds,   matBuf.numMaterials * sizeof(int), "bct");
    upMat(data.materials.normalTexIds,     matBuf.normalTexIds,     matBuf.numMaterials * sizeof(int), "nt");
    upMat(data.materials.roughnessTexIds,   matBuf.roughnessTexIds,   matBuf.numMaterials * sizeof(int), "rt");
    upMat(data.materials.metallicTexIds,    matBuf.metallicTexIds,    matBuf.numMaterials * sizeof(int), "mt");
    upMat(data.materials.emissionTexIds,    matBuf.emissionTexIds,    matBuf.numMaterials * sizeof(int), "emt");
    upMat(data.materials.packedSpecFlags,   matBuf.packedSpecFlags,   matBuf.numMaterials * sizeof(int), "psf");
    upMat(data.materials.texDataOffsets,    matBuf.texDataOffsets,    (matBuf.numTextures + 1) * sizeof(int), "toff");
    upMat(data.materials.texFormats,        matBuf.texFormats,        matBuf.numTextures * sizeof(int), "tfmt");
    upMat(data.materials.texByteOffsets,    matBuf.texByteOffsets,    (matBuf.numTextures + 1) * sizeof(long long), "tboff");
    upMat(data.materials.alphaCutout,       matBuf.alphaCutout,       matBuf.numMaterials * sizeof(int), "aco");
    upMat(data.materials.uvFlipU,           matBuf.uvFlipU,           matBuf.numMaterials * sizeof(int), "ufu");
    upMat(data.materials.shadowOpaque,      matBuf.shadowOpaque,      matBuf.numMaterials * sizeof(int), "sopq");
    upMat(data.materials.emissionAxis,      matBuf.emissionAxis,      matBuf.numMaterials * sizeof(float3), "eax");
    upMat(data.materials.emissionLobeK,     matBuf.emissionLobeK,     matBuf.numMaterials * sizeof(float), "ek");
    upMat(data.materials.emissionLobeU,     matBuf.emissionLobeU,     matBuf.numMaterials * sizeof(float), "eu");
    upMat(data.materials.emissionGamma,     matBuf.emissionGamma,     matBuf.numMaterials * sizeof(float), "egm");

    // Light triangles for GPU NEE (sorted by object for two-level selection)
    data.numLightTriangles = numLightTris;
    data.numLightObjects = numObjs;
    if (numLightTris > 0 && lightTris) {
        allocCpy(data.lightTriangles, lightTris, numLightTris * sizeof(GPULightTri), "light_tris");
        allocCpy(data.lightTriPrefix, triPrefix, numLightTris * sizeof(float), "light_prefix");
        if (numObjs > 0 && objFirst && objArea && objPower && objCentroid) {
            allocCpy(data.lightObjFirst, objFirst, (numObjs + 1) * sizeof(int), "light_objfirst");
            allocCpy(data.lightObjArea, objArea, numObjs * sizeof(float), "light_objarea");
            allocCpy(data.lightObjPower, objPower, numObjs * sizeof(float), "light_objpower");
            allocCpy(data.lightObjCentroid, objCentroid, numObjs * sizeof(float3), "light_objcentroid");
        }
    } else {
        data.lightTriangles = nullptr;
        data.lightTriPrefix = nullptr;
        data.lightObjFirst = nullptr;
        data.lightObjArea = nullptr;
        data.lightObjPower = nullptr;
        data.lightObjCentroid = nullptr;
        data.numLightObjects = 0;
    }
    data.pointLights = nullptr; data.numPointLights = 0;
    return data;
}

void freeGPUModel(GPUModelData& data) {
    auto cuf = [](auto*& p) { if (p) { cudaFree((void*)p); p = nullptr; } };
    cuf(data.vertPositions); cuf(data.vertNormals); cuf(data.vertUVs);
    cuf(data.materialIds); cuf(data.bvhNodes);
    cuf(data.materials.baseColorFactors); cuf(data.materials.roughnessFactors);
    cuf(data.materials.metallicFactors); cuf(data.materials.specularFactors);
    cuf(data.materials.opacities); cuf(data.materials.iors);
    cuf(data.materials.emissionFactors); cuf(data.materials.transmittingColors);
    cuf(data.materials.baseColorTexIds); cuf(data.materials.normalTexIds);
    cuf(data.materials.roughnessTexIds); cuf(data.materials.metallicTexIds);
    cuf(data.materials.emissionTexIds);
    cuf(data.materials.packedSpecFlags);
    cuf(data.materials.texDataOffsets);
    cuf(data.materials.texFormats);
    cuf(data.materials.texByteOffsets);
    cuf(data.materials.alphaCutout);
    cuf(data.materials.uvFlipU);
    cuf(data.materials.shadowOpaque);
    cuf(data.materials.emissionAxis); cuf(data.materials.emissionLobeK); cuf(data.materials.emissionLobeU); cuf(data.materials.emissionGamma);
    // texObjects is freed separately (needs cudaDestroyTextureObject)
    data.materials.texObjects = nullptr;
    data.materials.texData = nullptr;
    data.materials.texDataWidths = nullptr;
    data.materials.texDataHeights = nullptr;
    data.materials.numTextures = 0;
    cuf(data.pointLights); cuf(data.lightTriangles); cuf(data.lightTriPrefix);
    cuf(data.lightObjFirst); cuf(data.lightObjArea);
    cuf(data.lightObjPower); cuf(data.lightObjCentroid);
}
