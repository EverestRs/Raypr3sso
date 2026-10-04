#include "model.h"
#include "scene_progress.h"
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

// =========================================================================
// Internal helpers for procedural scene generation
// =========================================================================
namespace {

VertexData makeVertexData(const float3& normal, const float2& uv) {
    VertexData vd;
    vd.normal = normal;
    vd.uv     = uv;
    return vd;
}

} // anonymous namespace

void Model::addQuad(const float3& v0, const float3& v1,
                    const float3& v2, const float3& v3,
                    const float3& normal, const Material* material) {
    // Two triangles per quad: (v0,v1,v2) and (v0,v2,v3)
    const size_t vdIdx = m_vertexData.size();
    m_vertexData.push_back(makeVertexData(normal, float2(0,0)));
    m_vertexData.push_back(makeVertexData(normal, float2(1,0)));
    m_vertexData.push_back(makeVertexData(normal, float2(1,1)));
    m_vertexData.push_back(makeVertexData(normal, float2(0,1)));

    Face f0, f1;
    f0.v[0] = v0; f0.v[1] = v1; f0.v[2] = v2;
    f0.data[0] = &m_vertexData[vdIdx + 0];
    f0.data[1] = &m_vertexData[vdIdx + 1];
    f0.data[2] = &m_vertexData[vdIdx + 2];
    f0.material = material;

    f1.v[0] = v0; f1.v[1] = v2; f1.v[2] = v3;
    f1.data[0] = &m_vertexData[vdIdx + 0];
    f1.data[1] = &m_vertexData[vdIdx + 2];
    f1.data[2] = &m_vertexData[vdIdx + 3];
    f1.material = material;

    m_faces.push_back(f0);
    m_faces.push_back(f1);
}

void Model::addTriangle(const float3& v0, const float3& v1, const float3& v2,
                        const float3& n0, const float3& n1, const float3& n2,
                        const float2& uv0, const float2& uv1, const float2& uv2,
                        const Material* material) {
    const size_t vdIdx = m_vertexData.size();
    m_vertexData.push_back(makeVertexData(n0, uv0));
    m_vertexData.push_back(makeVertexData(n1, uv1));
    m_vertexData.push_back(makeVertexData(n2, uv2));

    Face f;
    f.v[0] = v0; f.v[1] = v1; f.v[2] = v2;
    f.data[0] = &m_vertexData[vdIdx + 0];
    f.data[1] = &m_vertexData[vdIdx + 1];
    f.data[2] = &m_vertexData[vdIdx + 2];
    f.material = material;
    m_faces.push_back(f);
}

void Model::addSphere(const float3& center, float radius,
                      int segments, int stacks,
                      const Material* material) {
    for (int j = 0; j < stacks; ++j) {
        const float theta0 = PI * float(j) / float(stacks);
        const float theta1 = PI * float(j+1) / float(stacks);
        const float sinTheta0 = sinf(theta0), cosTheta0 = cosf(theta0);
        const float sinTheta1 = sinf(theta1), cosTheta1 = cosf(theta1);
        for (int i = 0; i < segments; ++i) {
            const float phi0 = 2.0f * PI * float(i) / float(segments);
            const float phi1 = 2.0f * PI * float(i+1) / float(segments);
            const float sinPhi0 = sinf(phi0), cosPhi0 = cosf(phi0);
            const float sinPhi1 = sinf(phi1), cosPhi1 = cosf(phi1);

            // Four vertices of the quad
            const float3 v00 = center + radius * float3(sinTheta0*cosPhi0, cosTheta0, sinTheta0*sinPhi0);
            const float3 v01 = center + radius * float3(sinTheta0*cosPhi1, cosTheta0, sinTheta0*sinPhi1);
            const float3 v10 = center + radius * float3(sinTheta1*cosPhi0, cosTheta1, sinTheta1*sinPhi0);
            const float3 v11 = center + radius * float3(sinTheta1*cosPhi1, cosTheta1, sinTheta1*sinPhi1);

            // Per-vertex normals (pointing outward from center)
            const float3 n00 = glm::normalize(v00 - center);
            const float3 n01 = glm::normalize(v01 - center);
            const float3 n10 = glm::normalize(v10 - center);
            const float3 n11 = glm::normalize(v11 - center);

            // UVs
            const float u0 = float(i) / float(segments);
            const float u1 = float(i+1) / float(segments);
            const float v0 = float(j) / float(stacks);
            const float v1 = float(j+1) / float(stacks);

            // Two triangles per quad
            addTriangle(v00, v10, v11, n00, n10, n11,
                        float2(u0, v0), float2(u0, v1), float2(u1, v1), material);
            addTriangle(v00, v11, v01, n00, n11, n01,
                        float2(u0, v0), float2(u1, v1), float2(u1, v0), material);
        }
    }
}

void Model::addBox(const float3& center, const float3& half,
                   const Material* material) {
    const float3 c = center;
    const float3 h = half;
    // 6 faces, each as a quad (2 tris)
    // Front (+z)
    addQuad(c + float3(-h.x, -h.y,  h.z),
            c + float3( h.x, -h.y,  h.z),
            c + float3( h.x,  h.y,  h.z),
            c + float3(-h.x,  h.y,  h.z),
            float3(0,0,1), material);
    // Back (-z)
    addQuad(c + float3( h.x, -h.y, -h.z),
            c + float3(-h.x, -h.y, -h.z),
            c + float3(-h.x,  h.y, -h.z),
            c + float3( h.x,  h.y, -h.z),
            float3(0,0,-1), material);
    // Right (+x)
    addQuad(c + float3( h.x, -h.y, -h.z),
            c + float3( h.x, -h.y,  h.z),
            c + float3( h.x,  h.y,  h.z),
            c + float3( h.x,  h.y, -h.z),
            float3(1,0,0), material);
    // Left (-x)
    addQuad(c + float3(-h.x, -h.y,  h.z),
            c + float3(-h.x, -h.y, -h.z),
            c + float3(-h.x,  h.y, -h.z),
            c + float3(-h.x,  h.y,  h.z),
            float3(-1,0,0), material);
    // Top (+y)
    addQuad(c + float3(-h.x,  h.y, -h.z),
            c + float3( h.x,  h.y, -h.z),
            c + float3( h.x,  h.y,  h.z),
            c + float3(-h.x,  h.y,  h.z),
            float3(0,1,0), material);
    // Bottom (-y)
    addQuad(c + float3(-h.x, -h.y,  h.z),
            c + float3( h.x, -h.y,  h.z),
            c + float3( h.x, -h.y, -h.z),
            c + float3(-h.x, -h.y, -h.z),
            float3(0,-1,0), material);
}

// =========================================================================
// Cornell box test scene
// =========================================================================
Model Model::createTestScene() {
    Model model;

    // ---- Materials ----
    Material white;
    white.name = "white";
    white.baseColorFactor = float3(0.8f);
    white.roughnessFactor = 0.6f;

    Material red;
    red.name = "red";
    red.baseColorFactor = float3(0.8f, 0.05f, 0.05f);
    red.roughnessFactor = 0.6f;

    Material blue;
    blue.name = "blue";
    blue.baseColorFactor = float3(0.05f, 0.05f, 0.8f);
    blue.roughnessFactor = 0.6f;

    Material light;
    light.name = "light";
    light.baseColorFactor = float3(1.0f);
    light.emissionFactor = float3(80.0f);
    light.roughnessFactor = 0.0f;

    Material metal;
    metal.name = "metal";
    metal.baseColorFactor = float3(0.9f, 0.8f, 0.7f);
    metal.metallicFactor = 0.9f;
    metal.roughnessFactor = 0.2f;

    model.m_materials = {white, red, blue, light, metal};

    const Material* matWhite = &model.m_materials[0];
    const Material* matRed   = &model.m_materials[1];
    const Material* matBlue  = &model.m_materials[2];
    const Material* matLight = &model.m_materials[3];
    const Material* matMetal = &model.m_materials[4];

    // Pre-allocate vertex data so pointers remain valid
    model.m_vertexData.reserve(80);

    // ---- Scene geometry (Cornell box) ----
    const float S = 1.0f;  // half-size

    // Back wall (z = -S)
    model.addQuad(float3(-S, -S, -S), float3( S, -S, -S),
                  float3( S,  S, -S), float3(-S,  S, -S),
                  float3(0,0,1), matWhite);
    // Floor (y = -S)
    model.addQuad(float3(-S, -S, -S), float3( S, -S, -S),
                  float3( S, -S,  S), float3(-S, -S,  S),
                  float3(0,1,0), matWhite);
    // Ceiling (y = S)
    model.addQuad(float3(-S,  S,  S), float3( S,  S,  S),
                  float3( S,  S, -S), float3(-S,  S, -S),
                  float3(0,-1,0), matWhite);
    // Left wall (x = -S)
    model.addQuad(float3(-S, -S,  S), float3(-S, -S, -S),
                  float3(-S,  S, -S), float3(-S,  S,  S),
                  float3(1,0,0), matRed);
    // Right wall (x = S)
    model.addQuad(float3( S, -S, -S), float3( S, -S,  S),
                  float3( S,  S,  S), float3( S,  S, -S),
                  float3(-1,0,0), matBlue);

    // Area light on ceiling (offset 0.01 below ceiling to avoid
    // float-precision overlap with the ceiling quad at y = S)
    model.addQuad(float3(-0.3f,  S-0.01f, -0.3f),
                  float3( 0.3f,  S-0.01f, -0.3f),
                  float3( 0.3f,  S-0.01f,  0.3f),
                  float3(-0.3f,  S-0.01f,  0.3f),
                  float3(0,-1,0), matLight);

    // Short box (left side, diffuse)
    // Offset +0.001 so bottom face sits just above the floor (avoids z-fighting)
    model.addBox(float3(-0.4f, -S + 0.3f + 0.001f, 0.0f),
                 float3(0.3f, 0.3f, 0.3f), matWhite);

    // Tall box (right side, metallic)
    model.addBox(float3(0.4f, -S + 0.5f + 0.001f, -0.2f),
                 float3(0.3f, 0.5f, 0.3f), matMetal);

    // ---- Build BVH (reorders model.m_faces in-place) ----
    model.m_bvh.build(model.m_faces);

    // ---- Register light objects AFTER BVH build (pointers must be valid) ----
    std::unordered_map<const Material*, LightObject> lightMap;
    for (const auto& face : model.faces()) {
        if (face.material && face.material->isEmissive()) {
            auto& lo = lightMap[face.material];
            lo.color = face.material->emissionFactor;
            lo.addFace(face, face.material->emissionFactor);
        }
    }
    model.m_lightObjects.clear();
    for (auto& kv : lightMap) {
        kv.second.center = float3(0.0f, S, 0.0f);
        model.m_lightObjects.push_back(std::move(kv.second));
    }

    return model;
}

// =========================================================================
// Single sphere test scene
// =========================================================================
Model Model::createSphereTestScene(const std::string& type) {
    Model model;

    // ---- Materials ----
    Material white;
    white.name = "white";
    white.baseColorFactor = float3(0.8f);
    white.roughnessFactor = 0.6f;

    Material light;
    light.name = "light";
    light.baseColorFactor = float3(1.0f);
    light.emissionFactor = float3(2000.0f);
    light.roughnessFactor = 0.0f;

    // Sphere material based on type
    Material sphereMat;
    sphereMat.name = "sphere";
    sphereMat.baseColorFactor = float3(0.9f);
    sphereMat.metallicFactor = 0.0f;

    if (type == "diffuse") {
        // Diffuse white ball — test basic shading and shadows
        sphereMat.roughnessFactor = 0.6f;
        sphereMat.opacity = 1.0f;
        sphereMat.specularFactor = 0.04f;
    } else if (type == "rough") {
        // Glossy ball with specular highlight — test BRDF
        sphereMat.roughnessFactor = 0.1f;
        sphereMat.opacity = 1.0f;
        sphereMat.specularFactor = 0.04f;
    } else if (type == "glass") {
        // Glass ball — test refraction / transmission
        // baseColor = (1,1,1) → clear glass (no Beer's-law absorption tint).
        // Setting it to e.g. 0.9 would tint the glass heavily with the current
        // omega_absorb=32 over the sphere diameter.
        sphereMat.baseColorFactor = float3(1.0f);
        sphereMat.roughnessFactor = 0.0f;
        sphereMat.opacity = 0.0f;       // Fully transparent (dielectric)
        sphereMat.ior = 1.5f;
        sphereMat.specularFactor = 0.04f;
    } else if (type == "checker") {
        // Glass ball with checkerboard background for refraction verification
        // Clear glass: baseColor = (1,1,1) so the bright back lights and
        // checkerboard are clearly visible through the sphere.
        sphereMat.baseColorFactor = float3(1.0f);
        sphereMat.roughnessFactor = 0.0f;
        sphereMat.opacity = 0.0f;
        sphereMat.ior = 1.5f;
        sphereMat.specularFactor = 0.04f;
    } else {
        std::cerr << "Unknown sphere type '" << type << "', using diffuse.\n";
        sphereMat.roughnessFactor = 0.6f;
        sphereMat.opacity = 1.0f;
        sphereMat.specularFactor = 0.04f;
    }

    model.m_materials = {white, light, sphereMat};
    // Reserve enough capacity for all materials that will be added later
    // (checker type adds red, cyan, backLight = 3 more). This prevents
    // vector reallocation from invalidating the material pointers below.
    model.m_materials.reserve(6);

    const Material* matWhite  = &model.m_materials[0];
    const Material* matLight  = &model.m_materials[1];
    const Material* matSphere = &model.m_materials[2];

    // Pre-allocate vertex data
    model.m_vertexData.reserve(20000);

    // ---- Ground plane ----
    // Large white diffuse plane at y = -1
    model.addQuad(float3(-3, -1, -3), float3( 3, -1, -3),
                  float3( 3, -1,  3), float3(-3, -1,  3),
                  float3(0, 1, 0), matWhite);

    if (type == "checker") {
        // ---- Emissive checkerboard for unambiguous refraction test ----
        // Squares emit their own color (red/cyan), so the pattern is always
        // brightly visible through the glass sphere regardless of lighting.
        Material red, cyan;
        red.name = "red"; red.baseColorFactor = float3(1.0f, 0.0f, 0.0f); red.emissionFactor = float3(2.0f, 0.0f, 0.0f); red.roughnessFactor = 0.6f;
        cyan.name = "cyan"; cyan.baseColorFactor = float3(0.0f, 1.0f, 1.0f); cyan.emissionFactor = float3(0.0f, 2.0f, 2.0f); cyan.roughnessFactor = 0.6f;
        model.m_materials.push_back(red);
        model.m_materials.push_back(cyan);
        const Material* matRed = &model.m_materials[model.m_materials.size() - 2];
        const Material* matCyan = &model.m_materials.back();

        const float cs = 0.5f;
        const int cols = 12, rows = 8;
        const float zPlane = -2.5f;
        for (int r = 0; r < rows; ++r) {
            for (int c = 0; c < cols; ++c) {
                float x0 = -cols * cs / 2 + c * cs;
                float x1 = x0 + cs;
                float y0 = -rows * cs / 2 + r * cs;
                float y1 = y0 + cs;
                const Material* mat = ((r + c) % 2 == 0) ? matRed : matCyan;
                model.addQuad(float3(x0, y0, zPlane), float3(x1, y0, zPlane),
                              float3(x1, y1, zPlane), float3(x0, y1, zPlane),
                              float3(0, 0, 1), mat);
            }
        }

        // ---- Back light behind the sphere to illuminate the checkerboard ----
        Material backLight;
        backLight.name = "backLight";
        backLight.baseColorFactor = float3(1.0f);
        backLight.emissionFactor = float3(500.0f);
        backLight.roughnessFactor = 0.0f;
        model.m_materials.push_back(backLight);
        const Material* matBackLight = &model.m_materials.back();
        // Two back lights behind the sphere to uniformly illuminate the checkerboard
        for (float bx = -0.6f; bx <= 0.6f; bx += 1.2f) {
            const float3 blPos(bx, 0.5f, -0.8f);
            const float bls = 0.25f;
            model.addQuad(blPos + float3(-bls, -bls, 0), blPos + float3(bls, -bls, 0),
                          blPos + float3(bls,  bls, 0), blPos + float3(-bls,  bls, 0),
                          float3(0, 0, -1), matBackLight);
        }
    }

    // ---- Sphere ----
    // Center offset above (0, -0.5, 0) to avoid self-intersection with ground
    // at y = -1.0. Offset by 0.05 (500x EPS_ZERO) ensures shadow acne
    // and contact-point artifacts are fully eliminated.
    model.addSphere(float3(0, -0.5f + 0.05f, 0), 0.5f, 64, 32, matSphere);

    // ---- Point-like light source ----
    // Small emissive quad positioned above and to the left
    // to cast a visible shadow on the ground behind/right of the sphere
    const float3 lightCenter(-1.2f, 1.8f, 1.0f);
    const float ls = 0.15f;  // light quad half-size
    model.addQuad(lightCenter + float3(-ls, 0, -ls),
                  lightCenter + float3( ls, 0, -ls),
                  lightCenter + float3( ls, 0,  ls),
                  lightCenter + float3(-ls, 0,  ls),
                  float3(0, -1, 0), matLight);

    // ---- Build BVH (reorders model.m_faces in-place) ----
    model.m_bvh.build(model.m_faces);

    // ---- Register light objects AFTER BVH build ----
    std::unordered_map<const Material*, LightObject> lightMap;
    for (const auto& face : model.faces()) {
        if (face.material && face.material->isEmissive()) {
            auto& lo = lightMap[face.material];
            lo.color = face.material->emissionFactor;
            lo.addFace(face, face.material->emissionFactor);
        }
    }
    model.m_lightObjects.clear();
    for (auto& kv : lightMap) {
        model.m_lightObjects.push_back(std::move(kv.second));
    }

    return model;
}

// =========================================================================
// FBX loading via assimp (optional, requires RENDERER_HAS_ASSIMP)
// =========================================================================
#ifdef RENDERER_HAS_ASSIMP
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <chrono>
#include <assimp/material.h>
#endif

Model::Model(const std::string& model_folder,
             const std::string& model_name,
             const std::string& skyMap_name) {
#ifndef RENDERER_HAS_ASSIMP
    (void)model_folder;
    (void)model_name;
    std::cerr << "Warning: assimp not available. Use createTestScene() instead.\n";
#else
    if (importFbx(model_folder, model_name))
        finalizeScene(skyMap_name);
#endif
}

// Load ONE FBX file into the member arrays (faces/materials/textures).
// BVH build, light-object grouping and sky loading live in finalizeScene so
// createCombined() can import several files before finalizing once.
bool Model::importFbx(const std::string& model_folder,
                      const std::string& model_name) {
#ifndef RENDERER_HAS_ASSIMP
    (void)model_folder;
    (void)model_name;
    return false;
#else
    const std::string path = model_folder + "/" + model_name;

    auto tStart = std::chrono::steady_clock::now();
    sceneProgress::report("parse", 0, 1);
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path,
        aiProcess_Triangulate |
        aiProcess_PreTransformVertices |
        aiProcess_OptimizeMeshes |
        aiProcess_FixInfacingNormals);

    if (!scene || !scene->mRootNode) {
        std::cerr << "Failed to load: " << importer.GetErrorString() << std::endl;
        return false;
    }
    fprintf(stderr, "TIMER assimp-parse: %.1fs\n",
            std::chrono::duration<float>(std::chrono::steady_clock::now() - tStart).count()); fflush(stderr);
    sceneProgress::report("parse", 1, 1);

    // ---- Helper: URL decode for FBX texture paths ----
    auto urlDecode = [](const std::string& src) -> std::string {
        std::string decoded;
        for (size_t i = 0; i < src.length(); ++i) {
            if (src[i] == '%' && i + 2 < src.length()) {
                int code;
                std::sscanf(src.substr(i + 1, 2).c_str(), "%x", &code);
                decoded += static_cast<char>(code);
                i += 2;
            } else {
                decoded += src[i];
            }
        }
        return decoded;
    };

    // ---- Helper: resolve texture path from FBX ----
    auto resolveTexPath = [&](const aiString& texPath) -> std::string {
        std::string path = urlDecode(texPath.C_Str());
        // Replace backslashes with forward slashes
        std::replace(path.begin(), path.end(), '\\', '/');
        // If the path is absolute, try to use it directly; otherwise relative to model folder
        if (path.empty()) return "";
        if (path[0] == '/' || path[0] == '\\') {
            // Absolute path — strip leading slash for local filesystem
            if (path.size() > 1) path = path.substr(1);
            return path;
        }
        return model_folder + "/" + path;
    };

    // ---- Helper: load a texture and register it ----
    // Returns pointer to the loaded texture (lifetime managed by m_textures vector).
    // Falls back to nullptr on failure.
    double texTotal = 0.0; int texCnt = 0;  // texture decode timing
    auto loadTexture = [&](const std::string& filepath) -> ImageTexture* {
        if (filepath.empty()) return nullptr;
        // Check if already loaded
        for (auto& tex : m_textures) {
            // Quick check via filename (not perfect but sufficient for Bistro)
            if (tex.getDebugName() == filepath) return &tex;
        }
        m_textures.emplace_back();
        m_textures.back().setDebugName(filepath);
        auto t0 = std::chrono::steady_clock::now();
        bool ok = m_textures.back().load(filepath);
        texTotal += std::chrono::duration<float>(
            std::chrono::steady_clock::now() - t0).count();
        texCnt++;
        if (texCnt % 40 == 0) {
            fprintf(stderr, "TIMER texture-load: %.1fs (n=%d)\n", texTotal, texCnt);
            // 纹理总数在 FBX 材质遍历中才发现，total=0 表示阶段内计数
            sceneProgress::report("decode", texCnt, 0);
        }
        if (ok) {
            return &m_textures.back();
        }
        m_textures.pop_back();
        return nullptr;
    };


    // ---- Pre-reserve texture storage to prevent pointer invalidation ----
    // count unique texture references from all materials
    m_textures.reserve(512);  // Bistro has ~200 unique textures

    // ---- Load materials ----
    m_materials.resize(scene->mNumMaterials);
    for (unsigned i = 0; i < scene->mNumMaterials; ++i) {
        const aiMaterial* aim = scene->mMaterials[i];
        Material& mat = m_materials[i];
        mat.name = aim->GetName().C_Str();

        aiColor3D c; float f;
        if (aim->Get(AI_MATKEY_COLOR_DIFFUSE, c) == AI_SUCCESS)
            mat.baseColorFactor = float3(c.r, c.g, c.b);
        if (aim->Get(AI_MATKEY_COLOR_EMISSIVE, c) == AI_SUCCESS) {
            mat.emissionFactor = float3(c.r, c.g, c.b);
            // Bistro FBX stores emissive in arbitrary units; boost for path tracing
            mat.emissionFactor = mat.emissionFactor * 1000.0f;
        }
        // If no explicit emissive color but an emissive texture exists,
        // set a default factor so the GPU path (which doesn't sample
        // emissive textures per-pixel) still gets light contribution.
        // The CPU path handles this by sampling the texture × 1000.
        // We'll apply this after loading textures below.
        if (aim->Get(AI_MATKEY_METALLIC_FACTOR, f) == AI_SUCCESS)
            mat.metallicFactor = f;
        if (aim->Get(AI_MATKEY_ROUGHNESS_FACTOR, f) == AI_SUCCESS)
            mat.roughnessFactor = f;
        if (aim->Get(AI_MATKEY_REFRACTI, f) == AI_SUCCESS)
            mat.ior = f;
        if (aim->Get(AI_MATKEY_OPACITY, f) == AI_SUCCESS)
            mat.opacity = f;

        // ---- Load textures ----
        // Texture type mapping for PBR materials
        struct TexTypeMapping {
            aiTextureType aiType;
            ImageTexture* Material::* destPtr;  // pointer-to-member for the texture pointer
        };
        const TexTypeMapping texMappings[] = {
            {aiTextureType_DIFFUSE,       &Material::baseColorMap},
            {aiTextureType_BASE_COLOR,    &Material::baseColorMap},
            {aiTextureType_NORMALS,       &Material::normalMap},
            {aiTextureType_METALNESS,     &Material::metallicMap},
            {aiTextureType_DIFFUSE_ROUGHNESS, &Material::roughnessMap},
            {aiTextureType_EMISSIVE,      &Material::emissionMap},
            {aiTextureType_OPACITY,       &Material::opacityMap},
            // Fallback: also check for roughness in SPECULAR/GLOSSINESS
            {aiTextureType_SPECULAR,      &Material::roughnessMap},
        };

        for (const auto& mapping : texMappings) {
            aiString texPath;
            if (aim->GetTexture(mapping.aiType, 0, &texPath) != AI_SUCCESS)
                continue;
            std::string resolvedPath = resolveTexPath(texPath);
            if (resolvedPath.empty()) continue;

            // Try .dds extension (Bistro uses DDS textures)
            ImageTexture* loadedTex = nullptr;
            // First try the path as-is
            loadedTex = loadTexture(resolvedPath);
            if (!loadedTex) {
                // Try replacing extension with .dds
                size_t dotPos = resolvedPath.rfind('.');
                if (dotPos != std::string::npos) {
                    std::string ddsPath = resolvedPath.substr(0, dotPos) + ".dds";
                    loadedTex = loadTexture(ddsPath);
                }
            }
            if (!loadedTex) {
                // Try common Bistro filename variations
                // Many Bistro textures have _BaseColor, _Normal, etc. suffixes
                // If the original path didn't work, try without subdirectory
                size_t slashPos = resolvedPath.rfind('/');
                if (slashPos != std::string::npos) {
                    std::string basename = resolvedPath.substr(slashPos + 1);
                    // Try in the model folder root
                    std::string altPath = model_folder + "/Textures/" + basename;
                    loadedTex = loadTexture(altPath);
                }
            }
            if (loadedTex) {
                mat.*(mapping.destPtr) = loadedTex;
            }
        }

        // Packed SPECULAR map (Bistro format): G=roughness, B=metallic
        bool hasSpecularTex = false;
        {
            aiString texPath;
            if (aim->GetTexture(aiTextureType_SPECULAR, 0, &texPath) == AI_SUCCESS)
                hasSpecularTex = true;
        }
        mat.packedSpecularMap = hasSpecularTex && mat.roughnessMap != nullptr;

        // DIAG: dump liquid/glass material params after glass heuristics
        {
            std::string ln2 = mat.name;
            std::transform(ln2.begin(), ln2.end(), ln2.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (ln2.find("wine") != std::string::npos ||
                ln2.find("beer") != std::string::npos ||
                ln2.find("water") != std::string::npos ||
                ln2.find("ice") != std::string::npos ||
                ln2.find("glass") != std::string::npos)
                fprintf(stderr, "  LIQ[%s]: bc=(%.3f,%.3f,%.3f) op=%.3f ior=%.3f\n",
                        mat.name.c_str(), mat.baseColorFactor.x,
                        mat.baseColorFactor.y, mat.baseColorFactor.z,
                        mat.opacity, mat.ior);
        }

        // Bistro FBX has no REFRACTI keys and glass panes ship opacity=1, so
        // without this the GPU treats all glass/liquids as opaque plastic.
        {
            std::string lname = mat.name;
            std::transform(lname.begin(), lname.end(), lname.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (lname.find("glass") != std::string::npos) {
                mat.ior = 1.5f;
                mat.opacity = fminf(mat.opacity, 0.9f);
            } else if (mat.opacity < 0.99f && fabsf(mat.ior - 1.0f) < 0.01f) {
                mat.ior = 1.5f;   // Water/Beer/Wine/Ice — no IOR in FBX
            }
        }

        // The ceiling-fan light emits WHITE in the official Bistro render,
        // but the asset's CeilingFan_Emissive map is warm (avg 0.79,0.68,0.46).
        // Desaturate it (luminance-preserving) so the fan light is white.
        {
            std::string fn = mat.name;
            std::transform(fn.begin(), fn.end(), fn.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (fn.find("ceilingfan") != std::string::npos &&
                mat.emissionMap && !mat.emissionMap->empty())
                mat.emissionMap->desaturate();
        }

        // GPU path fallback: if material has an emissive texture but no explicit
        // emissive color (common in Bistro), set a default factor so the GPU
        // path (which doesn't sample emissive textures per-pixel) still gets light.
        if (mat.emissionMap && !mat.emissionMap->empty()) {
            const float3 em = mat.emissionFactor;
            if (glm::length(em) < 1e-6f) {
                // No emissive color in FBX — use the texture's average color × 1000
                // to match the CPU path's boost logic
                const auto* d = mat.emissionMap->getData();
                int tw = mat.emissionMap->getWidth();
                int th = mat.emissionMap->getHeight();
                int tc = mat.emissionMap->getChannels();
                if (d && tw > 0 && th > 0) {
                    double r = 0, g = 0, b = 0;
                    int samples = 0;
                    int stride = std::max(1, tw * th / 256);
                    for (int p = 0; p < tw * th; p += stride) {
                        r += d[p * tc + 0];
                        g += d[p * tc + 1];
                        b += d[p * tc + 2];
                        samples++;
                    }
                    if (samples > 0) {
                        mat.emissionFactor = float3(float(r / samples / 255.0 * 1000.0),
                                                     float(g / samples / 255.0 * 1000.0),
                                                     float(b / samples / 255.0 * 1000.0));
                    }
                }
            }
        }

        // Log material info
        std::cout << "  Material '" << mat.name << "': "
                  << "bc=" << (mat.baseColorMap ? "tex" : "factor")
                  << " nrm=" << (mat.normalMap ? "tex" : "none")
                  << " rgh=" << (mat.roughnessMap ? "tex" : "factor")
                  << " met=" << (mat.metallicMap ? "tex" : "factor")
                  << " em=" << (mat.isEmissive() ? "yes" : "no")
                  << " emFac=" << mat.emissionFactor.x << "," << mat.emissionFactor.y << "," << mat.emissionFactor.z
                  << " op=" << mat.opacity
                  << std::endl;
        if (i + 1 == scene->mNumMaterials)
            fprintf(stderr, "TIMER texture-load total: %.1fs (n=%d)\n", texTotal, texCnt);
    }

    // ---- Process meshes ----
    // Store emissive face indices per-material for later light object creation
    std::unordered_map<const Material*, std::vector<int>> emissiveFaceIds;

    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* mesh = scene->mMeshes[mi];
        const Material* mat = (mesh->mMaterialIndex < m_materials.size())
                            ? &m_materials[mesh->mMaterialIndex] : nullptr;
        // DEBUG: attribute meshes to materials + print AABB center for
        // landmark meshes (locate the exterior corner restaurant)
        {
            std::string mn = mesh->mName.C_Str();
            std::string mtl = mat ? mat->name : std::string("(none)");
            std::string mnL = mn, mtlL = mtl;
            std::transform(mnL.begin(), mnL.end(), mnL.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::transform(mtlL.begin(), mtlL.end(), mtlL.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (mnL.find("glass") != std::string::npos || mnL.find("rack") != std::string::npos ||
                mnL.find("cabinet") != std::string::npos || mnL.find("wine") != std::string::npos ||
                mtlL.find("glass") != std::string::npos || mtlL.find("rack") != std::string::npos ||
                mnL.find("awn") != std::string::npos || mtlL.find("awn") != std::string::npos ||
                mnL.find("sign") != std::string::npos || mnL.find("vespa") != std::string::npos ||
                mnL.find("lamp") != std::string::npos || mnL.find("lantern") != std::string::npos ||
                mnL.find("menu") != std::string::npos || mnL.find("hang") != std::string::npos ||
                mnL.find("rod") != std::string::npos || mnL.find("wire") != std::string::npos ||
                mnL.find("cord") != std::string::npos || mnL.find("chain") != std::string::npos ||
                mtlL.find("menu") != std::string::npos || mtlL.find("sign") != std::string::npos ||
                mnL.find("scoot") != std::string::npos || mtlL.find("scoot") != std::string::npos ||
                mnL.find("petit") != std::string::npos || mnL.find("coin") != std::string::npos ||
                mnL.find("foliage") != std::string::npos || mnL.find("linde") != std::string::npos ||
                mnL.find("leaves") != std::string::npos) {
                static int glassDbg = 0;
                if (glassDbg++ < 200) {
                    aiVector3D lo(1e9f), hi(-1e9f);
                    for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
                        const aiVector3D& p = mesh->mVertices[vi];
                        lo = aiVector3D(std::min(lo.x,p.x), std::min(lo.y,p.y), std::min(lo.z,p.z));
                        hi = aiVector3D(std::max(hi.x,p.x), std::max(hi.y,p.y), std::max(hi.z,p.z));
                    }
                    // mean face normal (first 64 faces) + face0 UV extent +
                    // diffuse texture name — for orientation/mirroring forensics
                    float3 nsum(0.0f);
                    unsigned nn = std::min<unsigned>(mesh->mNumFaces, 64);
                    for (unsigned fi = 0; fi < nn && mesh->HasFaces(); ++fi) {
                        const aiFace& fc = mesh->mFaces[fi];
                        if (fc.mNumIndices < 3) continue;
                        const aiVector3D& a = mesh->mVertices[fc.mIndices[0]];
                        const aiVector3D& b = mesh->mVertices[fc.mIndices[1]];
                        const aiVector3D& c = mesh->mVertices[fc.mIndices[2]];
                        aiVector3D n(b.x-a.x, b.y-a.y, b.z-a.z);
                        n = n ^ aiVector3D(c.x-a.x, c.y-a.y, c.z-a.z);
                        float L = std::max(n.Length(), 1e-6f);
                        nsum += float3(n.x/L, n.y/L, n.z/L);
                    }
                    float3 nm = (nn && glm::length(nsum) > 1e-6f) ? nsum / float(nn) : float3(0);
                    float u0=0,u1=0,v0=0,v1=0; float tnorm = 0;
                    std::string texName;
                    if (mat && mat->emissionMap && !mat->emissionMap->empty()) texName = "EM:";
                    texName += (mat && mat->baseColorMap) ? "bc" : "-";
                    if (mesh->mNumFaces && mesh->HasTextureCoords(0)) {
                        const aiFace& fc = mesh->mFaces[0];
                        for (unsigned k2 = 0; k2 < fc.mNumIndices; ++k2) {
                            const aiVector3D& t = mesh->mTextureCoords[0][fc.mIndices[k2]];
                            u0 = std::min(u0, t.x); u1 = std::max(u1, t.x);
                            v0 = std::min(v0, t.y); v1 = std::max(v1, t.y);
                        }
                    }
                    fprintf(stderr, "  MESHMAP: '%s' -> '%s' (op=%.2f em=%s) aabb=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) meann=(%.2f,%.2f,%.2f) uv0=[%.2f..%.2f,%.2f..%.2f]\n",
                            mesh->mName.C_Str(), mtl.c_str(), mat ? mat->opacity : -1.f,
                            (mat && mat->isEmissive()) ? "yes" : "no",
                            lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, nm.x, nm.y, nm.z, u0, u1, v0, v1);
                }
            }
        }

        // DEBUG (temporary): dump sign/banner vertex positions + UVs so the
        // UV->screen orientation (mirroring) can be verified numerically.
        {
            std::string mn = mesh->mName.C_Str();
            std::string mtl = mat ? mat->name : std::string("(none)");
            std::string mnL = mn, mtlL = mtl;
            std::transform(mnL.begin(), mnL.end(), mnL.begin(), [](unsigned char ch) { return std::tolower(ch); });
            std::transform(mtlL.begin(), mtlL.end(), mtlL.begin(), [](unsigned char ch) { return std::tolower(ch); });
            if (mnL.find("banner") != std::string::npos || mtlL.find("sign") != std::string::npos) {
                fprintf(stderr, "  SIGNDUMP: mesh='%s' mtl='%s' verts=%u faces=%u\n",
                        mn.c_str(), mtl.c_str(), mesh->mNumVertices, mesh->mNumFaces);
                unsigned vlim = std::min<unsigned>(mesh->mNumVertices, 24);
                for (unsigned vi = 0; vi < vlim; ++vi) {
                    const aiVector3D& p = mesh->mVertices[vi];
                    float uu = 0, vv = 0;
                    if (mesh->HasTextureCoords(0)) { uu = mesh->mTextureCoords[0][vi].x; vv = mesh->mTextureCoords[0][vi].y; }
                    fprintf(stderr, "    SV %u p=(%.3f,%.3f,%.3f) uv=(%.4f,%.4f)\n", vi, p.x, p.y, p.z, uu, vv);
                }
                unsigned flim = std::min<unsigned>(mesh->mNumFaces, 8);
                for (unsigned fi = 0; fi < flim; ++fi) {
                    const aiFace& fc = mesh->mFaces[fi];
                    fprintf(stderr, "    SF %u:", fi);
                    for (unsigned k = 0; k < fc.mNumIndices; ++k) fprintf(stderr, " %u", fc.mIndices[k]);
                    fprintf(stderr, "\n");
                }
            }
        }

        // Pre-count total vertices across ALL meshes for one big reservation
        // (vertex data is small but pointer stability is critical for
        // Face.data[]). Per-INSTANCE flag — see the member comment in model.h.
        if (!m_vertsReserved) {
            uint64_t totalVerts = 0;
            for (unsigned mm = 0; mm < scene->mNumMeshes; ++mm)
                totalVerts += scene->mMeshes[mm]->mNumVertices;
            m_vertexData.reserve(m_vertexData.size() + static_cast<size_t>(totalVerts));
            m_vertsReserved = true;
        }
        const size_t vdBase = m_vertexData.size();
        for (unsigned vi = 0; vi < mesh->mNumVertices; ++vi) {
            VertexData vd;
            vd.normal = mesh->HasNormals()
                ? float3(mesh->mNormals[vi].x, mesh->mNormals[vi].y, mesh->mNormals[vi].z)
                : float3(0,1,0);
            vd.uv = mesh->HasTextureCoords(0)
                ? float2(mesh->mTextureCoords[0][vi].x, mesh->mTextureCoords[0][vi].y)
                : float2(0);
            m_vertexData.push_back(vd);
        }

        for (unsigned fi = 0; fi < mesh->mNumFaces; ++fi) {
            const aiFace& face = mesh->mFaces[fi];
            if (face.mNumIndices != 3) continue;

            m_faces.emplace_back();
            Face& f = m_faces.back();
            for (int k = 0; k < 3; ++k) {
                unsigned idx = face.mIndices[k];
                const aiVector3D& v = mesh->mVertices[idx];
                f.v[k] = float3(v.x, v.y, v.z);
                f.data[k] = &m_vertexData[vdBase + idx];
            }
            f.material = mat;

            // Collect emissive face indices (use index, not pointer, to avoid
            // invalidation from future vector reallocation)
            if (mat && mat->isEmissive()) {
                emissiveFaceIds[mat].push_back(static_cast<int>(m_faces.size() - 1));
            }
        }
    }

    std::cout << "Loaded " << m_faces.size() << " faces" << std::endl;
    // Bake emissive texture averages NOW — data verified intact here. Light
    // collection later reads these instead of re-scanning bytes (large host
    // allocations in the render path were observed zeroing some buffers).
    for (auto& mat : m_materials) {
        if (mat.emissionMap && !mat.emissionMap->empty()) {
            const uint8_t* d = mat.emissionMap->getData();
            int tw = mat.emissionMap->getWidth();
            int th = mat.emissionMap->getHeight();
            int tc = mat.emissionMap->getChannels();
            if (d && tw > 0 && th > 0 && tc >= 3) {
                // bakeAverageTexel() already produced the linear-domain
                // average at load time; copy it per material.
                mat.emissionTexAvg = mat.emissionMap->emissionTexAvg;
                mat.emissionTexAvg22 = mat.emissionMap->emissionTexAvg22;
            }
        }
    }
    return true;
#endif
}

// BVH build + light-object grouping + sky loading — shared by the single-file
// constructor and createCombined().
void Model::finalizeScene(const std::string& skyMap_name) {
    if (!m_faces.empty()) {
        m_bvh.build(m_faces);
        // Print scene bounding box for camera setup reference
        Box b = m_bvh.bounds();
        float3 center = (b.v0 + b.v1) * 0.5f;
        float3 size = b.v1 - b.v0;
        std::cout << "  Scene bounds: min=(" << b.v0.x << "," << b.v0.y << "," << b.v0.z
                  << ") max=(" << b.v1.x << "," << b.v1.y << "," << b.v1.z
                  << ") center=(" << center.x << "," << center.y << "," << center.z
                  << ") size=(" << size.x << "," << size.y << "," << size.z << ")\n";
    }

    // ---- Create light objects AFTER BVH build (BVH reorders faces in-place,
    //      so pointers stored in LightObject must refer to the final order) ----
    std::unordered_map<const Material*, LightObject> lightMap;
    for (const auto& face : m_faces) {
        if (face.material && face.material->isEmissive()) {
            auto& lo = lightMap[face.material];
            // Use emission factor if set; otherwise use a default (texture-based)
            float3 em = face.material->emissionFactor;
            // NEE must use the LINEAR-domain average of the emissive texture
            // (same domain as per-pixel implicit hits and the GPU light list);
            // a flat un-textured factor makes every lamp radiate at full 500.
            if (face.material->emissionMap && !face.material->emissionMap->empty())
                em = em * face.material->emissionTexAvg;
            if (glm::length(em) < 1e-6f)
                em = float3(1000.0f);  // textured emissive: assume bright
            lo.color = em;
            lo.addFace(face, em);
        }
    }
    m_lightObjects.clear();
    for (auto& kv : lightMap) {
        m_lightObjects.push_back(std::move(kv.second));
    }

    std::cout << "  Lights: " << m_lightObjects.size() << " light objects" << std::endl;

    // ---- Load sky map ----
    if (skyMap_name != "null" && !skyMap_name.empty()) {
        m_skyMap.load(skyMap_name);
        // Art-direction sky intensity (the official Bistro interior look has
        // strong window light on the floor; the stock HDRI alone is weaker).
        if (const char* ss = std::getenv("RENDER_SKY_SCALE")) {
            float s = std::strtof(ss, nullptr);
            if (s > 0.0f && s != 1.0f) {
                m_skyMap.scaleRadiance(s);
                std::cout << "  Sky radiance scaled x" << s << std::endl;
            }
        }
        // Art-direction sky color (RENDER_SKY_TINT="r g b", default off):
        // daytime plates need a blue ambient (see SkyMap::tintRadiance).
        if (const char* st = std::getenv("RENDER_SKY_TINT")) {
            float3 t(1.0f);
            std::istringstream ss(st);
            ss >> t.x >> t.y >> t.z;
            if (t.x > 0.0f && t.y > 0.0f && t.z > 0.0f) {
                m_skyMap.tintRadiance(t);
                std::cout << "  Sky tint: (" << t.x << ", " << t.y << ", " << t.z << ")" << std::endl;
            }
        }
        // Directional ambient attenuation (street-canyon falloff): dim the
        // sky near the horizon so camera-facing facades lose their ambient
        // fill while the zenith keeps lighting the street. RENDER_SKY_HORIZON
        // = the horizon factor (default 1 = off), RENDER_SKY_HTRANS = the
        // sin(el) transition elevation.
        {
            float hd = 1.0f, ht = 0.35f;
            if (const char* z = std::getenv("RENDER_SKY_HORIZON")) hd = std::strtof(z, nullptr);
            if (const char* t = std::getenv("RENDER_SKY_HTRANS")) ht = std::strtof(t, nullptr);
            if (hd >= 0.0f && hd < 1.0f) {
                m_skyMap.applyElevationFalloff(hd, ht);
                std::cout << "  Sky horizon falloff: dim=" << hd
                          << " trans(sin el)=" << ht << std::endl;
            }
        }
        // Sun-cone focus (the official "dark environment + strong sun" rig):
        // dim the whole dome to RENDER_SKY_FOCUS_DIM except a smooth cone
        // around the sun direction (RENDER_SUN_AZ/EL). Requires the sun env
        // vars to be set; otherwise no-op.
        if (const char* fd = std::getenv("RENDER_SKY_FOCUS_DIM")) {
            float dim = std::strtof(fd, nullptr);
            float cone = 25.0f;
            if (const char* cc = std::getenv("RENDER_SKY_FOCUS_CONE"))
                cone = std::strtof(cc, nullptr);
            if (dim >= 0.0f && dim < 1.0f) {
                float az = 0.0f, el = 45.0f;
                if (const char* a = std::getenv("RENDER_SUN_AZ")) az = std::strtof(a, nullptr);
                if (const char* e = std::getenv("RENDER_SUN_EL")) el = std::strtof(e, nullptr);
                float azR = az * float(M_PI) / 180.0f, elR = el * float(M_PI) / 180.0f;
                float3 sd = float3(std::cos(elR) * std::cos(azR), std::sin(elR),
                                   std::cos(elR) * std::sin(azR));
                m_skyMap.applySunConeFocus(sd, cone * float(M_PI) / 180.0f, dim);
                std::cout << "  Sky sun-cone focus: dim=" << dim
                          << " cone=" << cone << "deg" << std::endl;
            }
        }
        // Azimuthal darkening of the behind-camera sky hemisphere: the
        // facades' broad specular reflection of that bright region is what
        // keeps them lit in our render while the official rig has a dark
        // environment there. RENDER_SKY_BACKDIM = the dim factor (1 = off).
        // RENDER_SKY_BACKAZ = the center azimuth in the RENDER_SUN_AZ
        // convention (degrees from +x toward +z, default 180 = -x, behind
        // the camera looking +x). NOTE: the old hardcoded center (phi=M_PI)
        // landed on -z, not the documented -x — the az parameter is now
        // converted through the kernel's equirect convention (2026-09-23).
        {
            float bd = 1.0f, bw = 60.0f, baz = 180.0f;
            if (const char* z = std::getenv("RENDER_SKY_BACKDIM")) bd = std::strtof(z, nullptr);
            if (const char* w = std::getenv("RENDER_SKY_BACKW")) bw = std::strtof(w, nullptr);
            if (const char* a = std::getenv("RENDER_SKY_BACKAZ")) baz = std::strtof(a, nullptr);
            if (bd >= 0.0f && bd < 1.0f) {
                // dir(az, el=0) = (cos az, 0, sin az); the equirect texel
                // azimuth (geometry.h convention) is phi = atan2(-x, z).
                const float azR = baz * float(M_PI) / 180.0f;
                const float phi = atan2f(-cosf(azR), sinf(azR));
                m_skyMap.applyAzimuthDarken(phi, bw * float(M_PI) / 180.0f, bd);
                std::cout << "  Sky azimuth darken: dim=" << bd
                          << " halfwidth=" << bw << "deg centerAz=" << baz << std::endl;
            }
        }
    }
}

// Load multiple FBX files into ONE scene (e.g. Bistro Interior+Exterior):
// a single BVH / material / texture pool with one finalize pass. Pointer
// remaps use index arithmetic against each part's element base; the texture
// elements are move-constructed after their base is captured (vector move
// keeps per-element heap buffers intact), so no dedup or reload is needed.
Model Model::createCombined(const std::string& model_folder,
                            const std::vector<std::string>& files,
                            const std::string& skyMap_name) {
    std::vector<std::unique_ptr<Model>> parts;
    for (const auto& name : files) {
        auto part = std::make_unique<Model>(model_folder, name, std::string("null"));
        if (part->m_faces.empty()) {
            std::cerr << "Combined load failed for " << name << std::endl;
            return Model();
        }
        parts.push_back(std::move(part));
    }
    Model merged;
    size_t tM = 0, tT = 0, tF = 0, tV = 0;
    for (const auto& p : parts) {
        tM += p->m_materials.size(); tT += p->m_textures.size();
        tF += p->m_faces.size();     tV += p->m_vertexData.size();
    }
    merged.m_materials.reserve(tM);
    merged.m_textures.reserve(tT);
    merged.m_vertexData.reserve(tV);
    merged.m_faces.reserve(tF);

    std::vector<const ImageTexture*> texBases;
    std::vector<const Material*> matBases;
    std::vector<const VertexData*> vertBases;
    std::vector<size_t> texOffs, matOffs, vertOffs;
    for (const auto& p : parts) {
        texBases.push_back(p->m_textures.data());
        matBases.push_back(p->m_materials.data());
        vertBases.push_back(p->m_vertexData.data());
        texOffs.push_back(merged.m_textures.size());
        matOffs.push_back(merged.m_materials.size());
        vertOffs.push_back(merged.m_vertexData.size());
        for (auto& t : p->m_textures) merged.m_textures.push_back(std::move(t));
        merged.m_materials.insert(merged.m_materials.end(),
                                  p->m_materials.begin(), p->m_materials.end());
        merged.m_vertexData.insert(merged.m_vertexData.end(),
                                   p->m_vertexData.begin(), p->m_vertexData.end());
    }
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        for (size_t j = matOffs[pi]; j < matOffs[pi] + parts[pi]->m_materials.size(); ++j) {
            Material& m = merged.m_materials[j];
            ImageTexture* maps[] = {m.baseColorMap, m.normalMap, m.roughnessMap,
                                    m.metallicMap, m.opacityMap, m.emissionMap};
            ImageTexture** remapped[] = {&m.baseColorMap, &m.normalMap, &m.roughnessMap,
                                         &m.metallicMap, &m.opacityMap, &m.emissionMap};
            for (int k = 0; k < 6; ++k) {
                if (maps[k]) {
                    const size_t idx = static_cast<size_t>(maps[k] - texBases[pi]);
                    if (idx >= parts[pi]->m_textures.size()) {
                        std::cerr << "combined: material texture idx " << idx
                                  << " out of range (part " << pi << ")\n";
                        *remapped[k] = nullptr;
                        continue;
                    }
                    *remapped[k] = &merged.m_textures[texOffs[pi] + idx];
                }
            }
        }
    }
    for (size_t pi = 0; pi < parts.size(); ++pi) {
        auto& p = *parts[pi];
        for (const Face& f : p.m_faces) {
            Face nf = f;
            if (f.material)
                nf.material = &merged.m_materials[matOffs[pi] +
                    static_cast<size_t>(f.material - matBases[pi])];
            for (int k = 0; k < 3; ++k) {
                if (!f.data[k]) continue;
                // Defensive: a stale (dangling) source pointer would compute
                // a wild offset — null it so packGeometry's null-guard
                // substitutes defaults instead of segfaulting downstream.
                const ptrdiff_t off = f.data[k] - vertBases[pi];
                if (off < 0 || off >= (ptrdiff_t)p.m_vertexData.size()) {
                    std::cerr << "combined: face data ptr outside part pool"
                              << " (part " << pi << ", offset " << off
                              << ") — nulled\n";
                    nf.data[k] = nullptr;
                    continue;
                }
                nf.data[k] = &merged.m_vertexData[vertOffs[pi] +
                    static_cast<size_t>(off)];
            }
            merged.m_faces.push_back(nf);
        }
    }
    std::cerr << "combined: finalize begin (" << merged.m_faces.size() << " faces, "
              << merged.m_materials.size() << " materials, "
              << merged.m_textures.size() << " textures)" << std::endl;
    // The merge copies materials/vertices/faces into `merged` (textures were
    // moved), and all part pointers were remapped above — the originals are
    // dead weight from here on. Free them BEFORE finalizeScene builds the
    // BVH: keeping ~4M duplicate faces alive through the build peaked host
    // memory hard enough to OOM-kill the server on the combined bistro load.
    parts.clear();
    const VertexData* vdBefore = merged.m_vertexData.data();
    merged.finalizeScene(skyMap_name);
    // Diagnostics (combined load segfaulted in packGeometry): did
    // finalizeScene reallocate the vertex pool, and do all face data
    // pointers land inside it?
    std::cerr << "combined: vertexData "
              << (merged.m_vertexData.data() == vdBefore ? "stable" : "REALLOCATED")
              << " size=" << merged.m_vertexData.size() << std::endl;
    {
        const VertexData* vb = merged.m_vertexData.data();
        const VertexData* ve = vb + merged.m_vertexData.size();
        size_t bad = 0;
        for (const Face& f : merged.m_faces)
            for (int k = 0; k < 3; ++k)
                if (f.data[k] && (f.data[k] < vb || f.data[k] >= ve)) ++bad;
        std::cerr << "combined: vertex ptr validation: " << bad
                  << " out-of-range / " << merged.m_faces.size() << " faces"
                  << std::endl;
    }
    std::cerr << "combined: finalize done" << std::endl;
    return merged;
}

HitRecord Model::rayHit(const Ray& ray) const {
    HitRecord hit;
    m_bvh.rayHit(ray, hit);
    return hit;
}

bool Model::rayOccluded(const Ray& ray, float maxDist) const {
    return m_bvh.rayOccluded(ray, maxDist);
}
