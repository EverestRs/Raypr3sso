#pragma once

#include "../include/types.h"
#include "component.h"
#include "material.h"
#include "bvh.h"
#include "bvh_gpu.h"
#ifndef __CUDACC__
#include <memory>
#include <string>
#include <vector>
#endif

// ---- GPU Light Structures ----
struct GPUPointLight {
    float3 position;
    float3 color;
    float power;
};

struct GPULightTri {
    float3 v0, v1, v2;
    float3 emission;
    float  area;
    int    faceId;   // source face index; lets implicit hits match NEE picks
    int    matId;    // owning material index; emitter-level MIS matching
};

// ---- GPU Model Data (SoA) ----
struct GPUModelData {
    // SoA triangle data
    float3* vertPositions;    // [numFaces * 3]
    float3* vertNormals;      // [numFaces * 3]
    float2* vertUVs;          // [numFaces * 3]
    int*    materialIds;      // [numFaces]
    int     numFaces;

    // Lights — triangles sorted by object for two-level selection:
    // pick object by power CDF, then a point uniformly over its area
    GPUPointLight* pointLights;
    GPULightTri*   lightTriangles;
    int*           lightObjFirst;    // [numLightObjects+1] tri range per object
    float*         lightObjArea;     // [numLightObjects] total area
    float*         lightObjPower;    // [numLightObjects] luminous power
    float3*        lightObjCentroid; // [numLightObjects] area-weighted center
    float*         lightTriPrefix;   // [numLightTriangles] inclusive area prefix
    int numPointLights;
    int numLightTriangles;
    int numLightObjects;

    // BVH
    LinearBVHNode* bvhNodes;
    int numBVHNodes;

    // Materials (SoA, see 5.3)
    GPUMaterialBuffer materials;
};

#ifndef __CUDACC__
class Model {
public:
    Model() = default;
    Model(Model&&) = default;
    Model& operator=(Model&&) = default;

    // Load FBX scene via assimp
    Model(const std::string& model_folder,
          const std::string& model_name,
          const std::string& skyMap_name);

    // Create a procedural Cornell box test scene
    static Model createTestScene();

    // Create a single sphere test scene (diffuse|rough|glass)
    static Model createSphereTestScene(const std::string& type);

    // Load several FBX files into ONE scene (single BVH/material/texture
    // pool, e.g. Bistro Interior + Exterior so windows look through).
    static Model createCombined(const std::string& model_folder,
                                const std::vector<std::string>& files,
                                const std::string& skyMap_name);

    // Ray intersection: find closest hit along ray
    HitRecord rayHit(const Ray& ray) const;

    // Shadow test: check occlusion between two points
    bool rayOccluded(const Ray& ray, float maxDist) const;

    // Data access
    const std::vector<Face>& faces() const { return m_faces; }
    const std::vector<Material>& materials() const { return m_materials; }
    const std::vector<LightObject>& lightObjects() const { return m_lightObjects; }
    const BVH& bvh() const { return m_bvh; }
    const SkyBox& skyMap() const { return m_skyMap; }
    Box bounds() const { return m_bvh.bounds(); }

    // GPU export — pack model data into GPU-friendly format
    GPUModelData uploadToGPU() const;

private:
    void addQuad(const float3& v0, const float3& v1,
                 const float3& v2, const float3& v3,
                 const float3& normal, const Material* material);
    void addTriangle(const float3& v0, const float3& v1, const float3& v2,
                     const float3& n0, const float3& n1, const float3& n2,
                     const float2& uv0, const float2& uv1, const float2& uv2,
                     const Material* material);
    void addBox(const float3& center, const float3& halfExtents,
                const Material* material);
    void addSphere(const float3& center, float radius,
                   int segments, int stacks,
                   const Material* material);
    bool importFbx(const std::string& model_folder, const std::string& model_name);
    void finalizeScene(const std::string& skyMap_name);

    std::vector<Face> m_faces;
    std::vector<VertexData> m_vertexData;
    std::vector<Material> m_materials;
    std::vector<LightObject> m_lightObjects;
    std::vector<ImageTexture> m_textures;
    BVH m_bvh;
    SkyBox m_skyMap;
    // Per-instance flag: the FBX loader reserves m_vertexData for the whole
    // scene up-front because Face.data[] holds raw pointers into it. This
    // MUST be per-Model — a function-level static made a second Model built
    // in the same process (createCombined's parts!) skip the reserve, and
    // push_back reallocations then left most Face.data pointers dangling.
    bool m_vertsReserved = false;
};
#endif
