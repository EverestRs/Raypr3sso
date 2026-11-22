# 光线追踪渲染器 — 技术设计文档 (ARCH)

> **文档状态（2026-11-22）**：本文档是项目启动期的设计蓝图，接口命名与实现细节已随
> 开发演进（如 GPU 纹理从 cudaArray+textureObject 演进为 BC 块驻显存+内核解码，见
> ADR-14；§15-18 原标注"待实现"的模块均已落地）。**实现现状与全部实测数据以
> [README.md](README.md) 技术报告（§1-§13）为准**；本文保留作设计决策与演进记录。

## 1. 项目结构

```
Raypr3sso/
├── CMakeLists.txt                 # 主构建文件
├── external/                      # 第三方库
│   ├── glm/                       # 数学库 (CPU)
│   ├── stb/                       # stb_image.h (纹理加载)
│   └── tinyexr/                   # EXR 输出 (可选)
├── src/                           # 源码
│   ├── main.cpp                   # 入口 + 交互式控制台
│   ├── geometry.h / .cpp          # 几何工具
│   ├── component.h / .cpp         # 基础组件
│   ├── material.h / .cpp          # 材质 + 纹理
│   ├── model.h / .cpp             # 场景模型加载
│   ├── bvh.h / .cpp               # BVH 构建 + CPU 遍历
│   ├── bvh_gpu.h / .cu            # GPU Linear BVH
│   ├── sampling.h / .cpp          # BSDF + 重要性采样
│   ├── render.h / .cpp            # CPU 渲染引擎
│   ├── render_gpu.h / .cu         # GPU 渲染 Kernel
│   ├── camera.h / .cpp            # 相机模型
│   ├── image.h / .cpp             # G-buffer + 后处理
│   ├── console.h / .cpp           # 交互式控制台
│   └── utils.h / .cpp             # 工具函数
├── include/                       # 公共头文件
│   └── types.h                    # 基础类型别名 + 常量
├── scripts/                       # 工具脚本
│   └── diff.py                    # 渲染对比工具
├── models/                        # Bistro 模型 (需自行下载)
└── output/                        # 渲染输出
```

---

## 2. 基础类型与常量

### 2.1 `types.h` — 全局类型定义

```cpp
#pragma once
#include <cstdint>
#include <cmath>

// ---- 向量类型 ----
// CPU 端使用 glm
#include <glm/glm.hpp>
using float2 = glm::vec2;
using float3 = glm::vec3;
using float4 = glm::vec4;
using uint2  = glm::uvec2;
using uint3  = glm::uvec3;

// GPU 端使用 CUDA 内置向量类型
#ifdef __CUDACC__
// float3/float4 等直接用 CUDA 内置类型
#endif

// ---- 常量 ----
constexpr float EPS_ZERO  = 1e-4f;
constexpr float PI        = 3.14159265358979323846f;
constexpr float INV_PI    = 0.31830988618379067154f;
constexpr float RGB_WEIGHT[3] = {0.299f, 0.587f, 0.114f};  // luminance weights

// ---- 光线 ----
struct Ray {
    float3 origin;
    float3 direction;
};

// ---- 光线微分 ----
struct RayDifferential {
    float3 dPdx{0.0f}, dPdy{0.0f};
    float3 dDdx{0.0f}, dDdy{0.0f};
};

// ---- 包围盒 ----
struct Box {
    float3 v0;  // min
    float3 v1;  // max

    Box() : v0(INFINITY), v1(-INFINITY) {}
    Box(const float3& min, const float3& max) : v0(min), v1(max) {}
    Box operator+(const Box& other) const;
};
```

---

## 3. 几何模块 — `geometry.h/.cpp`

### 3.1 核心接口

```cpp
// ---- 三角形求交 (Möller–Trumbore) ----
// 返回值: 交点 t (INFINITY 表示无交)
float rayTriangleIntersect(const Ray& ray,
                           const float3& v0, const float3& v1, const float3& v2);

// ---- 光线与包围盒求交 ----
// 输入输出: tL/tR 为当前近/远区间
void rayBoxIntersect(const Ray& ray, const Box& box,
                     float& tL, float& tR);

// ---- 重心坐标 ----
float3 barycentric(const float3& v0, const float3& v1,
                   const float3& v2, const float3& P);

// ---- 切线空间 ----
void getTangentSpace(const float3& normal,
                     float3& tangent, float3& bitangent);

// ---- 球面坐标 ----
float2 dirToSphere(const float3& dir);
float3 sphereToDir(const float2& uv);
```

### 3.2 性能要求

- CPU 路径追踪中，三角形求交占比 > 40%，**必须 SIMD 友好**
- GPU Kernel 中，每个 thread 独立求交，**分支尽量少**
- 所有函数标记 `__host__ __device__` 以支持双端编译

---

## 4. 组件模块 — `component.h/.cpp`

### 4.1 核心数据结构

```cpp
// ---- 顶点数据 ----
struct VertexData {
    float2 uv;
    float3 normal;
};

// ---- 三角形面 ----
struct alignas(32) Face {
    float3 v[3];            // 世界坐标顶点
    const VertexData* data[3]; // 顶点属性指针
    const Material* material;  // 材质指针
};

// ---- 命中记录 ----
struct HitRecord {
    float t_min;
    float t_max;
    const Face* face = nullptr;

    HitRecord() : t_min(EPS_ZERO), t_max(INFINITY) {}
    explicit HitRecord(float tmin, float tmax) : t_min(tmin), t_max(tmax) {}
};

// ---- 随机数生成器 (CPU) ----
class Generator {
    std::mt19937 mt;
    std::uniform_real_distribution<float> dist;
public:
    explicit Generator(uint32_t seed);
    float operator()();  // [0, 1)
};

// ---- 离散分布采样 ----
class Distribution1D {
    std::vector<float> cdf;
    float sum;
public:
    void build(const std::vector<float>& f);
    float sample(float u, float& pdf) const;
    float pdfAt(float x) const;
};

// ---- 光源面片 ----
struct LightObject {
    float3 center;
    float3 color;
    float power;
};
```

---

## 5. 材质模块 — `material.h/.cpp`

### 5.1 材质参数 (PBR)

```cpp
// ---- 图像纹理 ----
class ImageTexture {
    int width, height, channels;
    std::vector<uint8_t> data;  // CPU: uint8
    // GPU: cudaArray_t
    std::vector<ImageTexture> mipmaps;
public:
    bool load(const std::string& filename);
    float4 sample(float u, float v, float mipLevel = 0.0f) const;
    bool hasAlpha() const;
};

// ---- PBR 材质 ----
class Material {
public:
    std::string name;

    // PBR 参数
    float3 baseColorFactor{1.0f};
    float  roughnessFactor{0.8f};
    float  metallicFactor{0.0f};
    float  specularFactor{0.04f};
    float  opacity{1.0f};
    float  ior{1.0f};
    float3 emissionFactor{0.0f};
    float3 transmittingColor{0.0f};

    // 纹理覆盖
    ImageTexture* baseColorMap   = nullptr;
    ImageTexture* normalMap      = nullptr;
    ImageTexture* roughnessMap   = nullptr;
    ImageTexture* metallicMap    = nullptr;
    ImageTexture* opacityMap     = nullptr;
    ImageTexture* emissionMap    = nullptr;

    bool isEmissive() const;
    bool isTransparent() const;
    bool hasFullyTransparentPart = false;

    // 从纹理采样获取最终参数
    float4 getBaseColor(float u, float v) const;
    float  getRoughness(float u, float v) const;
    float  getMetallic(float u, float v) const;
    float3 getNormal(float u, float v) const;
};
```

### 5.2 纹理加载

```cpp
// ---- 纹理加载器 ----
class TextureLoader {
public:
    // 加载 PNG / HDR，自动生成 mipmap
    static ImageTexture* load(const std::string& filepath);

    // GPU 上传: CPU ImageTexture → cudaArray
    static void uploadToGPU(const ImageTexture& cpuTex,
                            cudaArray_t& gpuArray,
                            cudaTextureObject_t& gpuTexObj);
};
```

### 5.3 GPU 材质表示

GPU 端材质以 SoA 方式存储在全局内存中：

```cpp
// ---- GPU 材质参数数组 (SoA) ----
// 每个场景一个 MaterialBuffer
struct GPUMaterialBuffer {
    float3* baseColorFactors;    // [numMaterials]
    float*  roughnessFactors;
    float*  metallicFactors;
    float*  specularFactors;
    float*  opacities;
    float*  iors;
    float3* emissionFactors;
    float3* transmittingColors;
    int*    baseColorTexIds;     // -1 = 无纹理
    int*    normalTexIds;
    int*    roughnessTexIds;
    int*    metallicTexIds;
    int     numMaterials;

    cudaTextureObject_t* texObjects;  // 纹理对象数组
    int numTextures;
};
```

---

## 6. 模型模块 — `model.h/.cpp`

### 6.1 场景加载

```cpp
class Model {
public:
    // ---- 加载 FBX 场景 ----
    // model_folder: 模型所在目录
    // model_name:   FBX 文件名
    // skyMap_name:  HDR 天空盒文件名 (或 "null")
    Model(const std::string& model_folder,
          const std::string& model_name,
          const std::string& skyMap_name);

    // ---- 光线求交 ----
    // 沿光线寻找最近交点
    HitRecord rayHit(const Ray& ray) const;

    // ---- 阴影测试 ----
    // 判断两点之间是否有遮挡
    bool rayOccluded(const Ray& ray, float maxDist) const;

    // ---- 数据访问 ----
    const std::vector<Face>& faces() const;
    const std::vector<Material>& materials() const;
    const std::vector<LightObject>& lightObjects() const;
    const BVH& bvh() const;
    const SkyBox& skyMap() const;

    // ---- GPU 导出 ----
    // 将模型数据打包为 GPU 可读格式
    GPUModelData uploadToGPU() const;
};
```

### 6.2 内部流程

```
assimp::Importer
     │
     ├── aiScene
     │    ├── mMaterials  → Material 数组
     │    ├── mMeshes     → Face 数组 (三角化 + 顶点属性)
     │    └── mRootNode   → 场景层次 (保留或展平)
     │
     ├── processMaterial()  → 加载 PBR 纹理
     ├── processMesh()      → 三角化、顶点合并、光源识别
     │
     └── BVH::build(faces)  → 建立 BVH 加速结构
```

### 6.3 GPU 模型数据

```cpp
struct GPUModelData {
    // SoA 三角形数据
    float3* vertPositions;   // [numFaces * 3]
    float3* vertNormals;     // [numFaces * 3]
    float2* vertUVs;         // [numFaces * 3]
    int*    materialIds;     // [numFaces]
    int     numFaces;

    // 光源
    GPUPointLight*  pointLights;   // [numPointLights]
    GPULightTri*    lightTriangles; // [numLightTris]
    int numPointLights;
    int numLightTriangles;
    int* lightTriDistrib;   // 光源重要性采样 CDF

    // BVH
    LinearBVHNode* bvhNodes;
    int numBVHNodes;

    // 材质 (见 5.3)
    GPUMaterialBuffer materials;
};
```

---

## 7. BVH 模块

### 7.1 CPU BVH — `bvh.h/.cpp`

```cpp
struct BVHNode {
    Box box;
    int leftChild;   // -1 表示叶节点
    int rightChild;
    int faceStart;   // 叶节点: 面片范围起点
    int faceEnd;     // 叶节点: 面片范围终点
};

class BVH {
    BVHNode* nodes = nullptr;
    Face*    faces = nullptr;  // 指向 Model::faces 的引用
    int      numNodes = 0;

public:
    // ---- 构建 ----
    // SAH 启发式构建
    void build(std::vector<Face>& faces);

    // ---- 遍历 (CPU 递归) ----
    void rayHit(const Ray& ray, HitRecord& hit) const;
    bool rayOccluded(const Ray& ray, float maxDist) const;

    // ---- 导出 Linear BVH (GPU) ----
    // 将 CPU 树展平为 GPU 版本
    LinearBVHNode* flattenForGPU() const;
    int numNodesGPU() const;

    // ---- 序列化/反序列化 ----
    bool save(const std::string& path) const;
    bool load(const std::string& path);

    ~BVH();
};
```

### 7.2 GPU Linear BVH — `bvh_gpu.h/.cu`

```cpp
// ---- GPU BVH 节点 ----
// 8-wide 压缩节点，适用于 warp-level 遍历
struct alignas(32) LinearBVHNode {
    float3 bboxMin[2];   // 左右子包围盒
    float3 bboxMax[2];
    union {
        struct { int leftChild, rightChild; }; // 内部节点: 子节点索引
        struct { int faceStart, faceCount; };  // 叶节点: 面片范围
    };
    int isLeaf;     // 0/1
    int pad;
};

// ---- GPU BVH 遍历器 ----
class GPU BVHTraverser {
public:
    // Host 端: 拷贝 BVH 到 GPU
    static void upload(const LinearBVHNode* cpuNodes, int count,
                       LinearBVHNode** gpuNodes);

    // Device 端: 遍历 Kernel — 由渲染 Kernel 调用
    __device__ static HitRecord traverse(const Ray& ray,
                                          const LinearBVHNode* nodes,
                                          const float3* vertPositions,
                                          const int* materialIds,
                                          int numNodes);
};
```

### 7.3 BVH 内存布局

```
CPU (SAH, 指针结构)               GPU (Linear, 数组结构)
┌─────────────────────┐         ┌─────────────────────────┐
│ BVHNode             │         │ LinearBVHNode[0]        │
│  box                │   →    │  bboxMin[0], bboxMax[0] │ ← 左子
│  leftChild*  ───────┤         │  bboxMin[1], bboxMax[1] │ ← 右子
│  rightChild* ───────┤         │  childIndices[2]        │
│  faceStart/End      │         │  isLeaf / faceStart ... │
└─────────────────────┘         └─────────────────────────┘
                                        ▲
                                        │
展平算法: DFS 顺序 → 数组，指针 → 数组索引
```

### 7.4 BVH 序列化格式

```cpp
// 磁盘缓存 (*.bvh 二进制文件)
struct BVHFileHeader {
    char magic[4] = {'B', 'V', 'H', 1};
    uint32_t numFaces;
    uint32_t numNodes;
    uint64_t faceDataOffset;   // 面片数据起始偏移
    uint64_t nodeDataOffset;   // 节点数据起始偏移
};
```

---

## 8. BSDF 模块 — `sampling.h/.cpp`

### 8.1 Disney BSDF 实现

```cpp
// ---- 命中时表面信息 ----
struct HitInfo {
    float3 position;
    float3 shapeNormal;
    float3 surfaceNormal;
    float3 baseColor;
    float3 emission;
    float  specular;
    float  roughness;
    float  metallic;
    float  opacity;
    float  eta;         // 折射率 (1.0 = 空气)
    int    materialID;
    bool   entering;    // 是否从外部进入
};

// ---- BSDF 评估与采样 ----
class BSDF {
public:
    float3 inDir;        // 入射方向 (指向表面)
    HitInfo surface;

    BSDF(const float3& inDir, const HitInfo& info);

    // ---- BRDF / BTDF 评估 ----
    float3 eval(const float3& outDir) const;
    float  pdf(const float3& outDir) const;

    // ---- 重要性采样 ----
    // 返回 出射方向 + BSDF 值/PDF
    float3 sample(float u1, float u2, float3& outDir, float& pdf) const;

    // ---- Glass: 精确折射方向 + Fresnel ----
    bool refract(float3& outDir, float& fresnel) const;

private:
    float GTR1(float NdotH, float a) const;
    float GTR2(float NdotH, float a) const;
    float smithG(float NdotV, float alpha) const;
    float schlickFresnel(float u) const;
};
```

### 8.2 采样策略

| 材质类型 | 采样策略 | PDF 分布 |
|----------|----------|----------|
| Diffuse (`metallic=0, roughness>0`) | 余弦加权半球采样 | `cos(θ)/π` |
| Glossy (`metallic=0, roughness<1`) | GTR2 微表面采样 | GTR2 * SmithG |
| Metal (`metallic=1`) | GTR2 微表面采样 (无折射) | GTR2 * SmithG |
| Glass (`opacity=0`) | 反射/折射概率混合 (Fresnel) | 反射: GTR2, 折射: BTDF |
| Transparent (`opacity<1`) | 反射 + 折射 (同上) | 同上 |

---

## 9. 相机模块 — `camera.h/.cpp`

```cpp
struct Camera {
    float3 position;
    float3 direction;   // 朝向
    float3 up;          // 上方向
    float3 right;       // 右方向 (自动计算)

    float  fov;         // 垂直视场角 (度)
    float  focus;       // 焦距 (景深)
    float  coc;         // 最大弥散圆半径 (像素)

    // 生成光线 (含 anti-aliasing + 景深)
    Ray generateRay(float px, float py,
                    float offsetU, float offsetV,  // AA jitter
                    float lensU, float lensV,      // DoF jitter
                    float2 filmSize) const;
};
```

---

## 10. 渲染模块 — CPU

### 10.1 CPU 路径追踪主循环 — `render.h/.cpp`

```cpp
// ---- 渲染参数 ----
struct RenderArgs {
    Camera  camera;
    int     width, height;       // 分辨率
    int     spp;                 // 每像素采样数
    float   pDirect;             // 直接光照采样比例
    int     maxDepth = 16;       // 最大路径深度
    int     numThreads;          // 线程数
    std::string outputPath;      // 输出目录
};

// ---- 每个像素的渲染数据 ----
struct PerPixelData {
    float3 radiance_Dd;  // Direct Diffuse
    float3 radiance_Ds;  // Direct Specular
    float3 radiance_Id;  // Indirect Diffuse
    float3 radiance_Is;  // Indirect Specular
    float3 baseColor;    // 命中点基础色
    float3 normal;       // 命中点法线
    float3 emission;     // 自发光/天空盒贡献
};

// ---- 多线程渲染入口 ----
void renderCPU(const Model& model, const RenderArgs& args,
               std::vector<PerPixelData>& output);

// ---- 单像素渲染 ----
void renderPixel(const Model& model, const RenderArgs& args,
                 int x, int y, uint32_t seed, PerPixelData& out);

// ---- 路径追踪递归 ----
// 返回沿路径累积的 radiance 及分类
void samplePath(const Model& model, Ray ray,
                uint32_t& seed, int depth, bool excludeDirect,
                float3& radiance, float3& throughput,
                PerPixelData& split);
```

### 10.2 CPU 渲染管线图

```
for each pixel (x, y):
  for each sample s in [0..spp):
    ray = camera.generateRay(x, y, jitter_u, jitter_v, ...)
    hit = model.rayHit(ray)
    if miss:
        emission += skyMap.get(ray.direction)
        continue
    getHitInfo(hit, ..., hitInfo)
    sampleDirectLight(hitInfo)     → radiance_Dd, _Ds
    sampleIndirectLight(hitInfo)   → radiance_Id, _Is
    accumulate(PerPixelData)
  end
  atomicAdd to output buffer
```

---

## 11. 渲染模块 — GPU

### 11.1 GPU Kernel 设计 — `render_gpu.h/.cu`

```cpp
// ---- Kernel 启动参数 ----
struct GPURenderArgs {
    Camera      camera;
    int         width, height;
    int         spp;
    float       pDirect;
    int         maxDepth;
    uint32_t    baseSeed;
};

// ---- 主 Kernel ----
// GridDim: (tilesX, tilesY)
// BlockDim: (TILE_W, TILE_H)
__global__ void renderKernel(
    GPURenderArgs      args,
    GPUModelData       model,
    cudaTextureObject_t skyTex,
    float4*            outputColor,       // [width * height]
    float4*            outputDirectDiff,  // AOV 通道
    float4*            outputDirectSpec,
    float4*            outputIndirectDiff,
    float4*            outputIndirectSpec
);

// ---- 单路径追踪 (device 函数) ----
__device__ float3 tracePath(
    uint32_t&      seed,
    const Ray&     ray,
    const GPUModelData& model,
    cudaTextureObject_t skyTex,
    int            maxDepth,
    bool           excludeDirect,
    PerPixelData&  split
);

// ---- BVH 遍历 + 求交 (device) ----
__device__ HitRecord traceRay(
    const Ray&     ray,
    const GPUModelData& model
);
```

### 11.2 Kernel 启动配置

```cpp
// Tile-based 渲染
constexpr int TILE_W = 16;
constexpr int TILE_H = 16;

// 每个 tile 一个 block
dim3 blockDim(TILE_W, TILE_H);
dim3 gridDim((width + TILE_W - 1) / TILE_W,
             (height + TILE_H - 1) / TILE_H);

// Shared memory 分配
// 每个 thread 需要 ~48 字节路径栈
// blockDim.x * blockDim.y * 48 = 256 * 48 = 12KB
size_t sharedMemBytes = TILE_W * TILE_H * 48;

renderKernel<<<gridDim, blockDim, sharedMemBytes>>>(
    args, model, skyTex, output, ...
);
```

### 11.3 GPU 数据流

```
Host (CPU)                           Device (GPU)
─────────                            ────────────
assimp → Model
  │
  ├── BVH::build()                   │
  ├── BVH::flattenForGPU() ─────────→│  cudaMemcpy → LinearBVHNode[]
  ├── TextureLoader::uploadToGPU() ─→│  cudaArray + textureObject
  ├── Face → SoA ──────────────────→│  float3* vertPositions 等
  ├── Material → GPUMaterialBuffer →│  GPUMaterialBuffer
  └── LightObject[] ───────────────→│  GPUPointLight[]
                                     │
cudaMemcpyAsync(args → GPU)          │
                                     │
renderKernel<<<grid, block>>>()      │  ← 完全在 GPU 执行
                                     │
cudaMemcpyAsync(output → Host)       │
  │                                  │
  └── Photo::postProcess()
  └── Photo::save()
```

---

## 12. 后处理 & 图像输出 — `image.h/.cpp`

### 12.1 G-buffer 与输出

```cpp
class Photo {
public:
    int width, height;
    float exposure;

    // AOV 通道
    struct Channels {
        float4* finalColor;        // 最终合成图
        float4* directDiffuse;
        float4* directSpecular;
        float4* indirectDiffuse;
        float4* indirectSpecular;
        float4* baseColor;        // 用于调试
        float4* emission;         // 用于调试
    } channels;

    enum ShadeOption {
        BaseColor        = 1 << 0,
        Emission         = 1 << 1,
        DirectLight      = 1 << 2,
        IndirectLight    = 1 << 3,
        Diffuse          = 1 << 4,
        Specular         = 1 << 5,
        Full             = DirectLight|IndirectLight|Diffuse|Specular|BaseColor|Emission,
        DoBloom          = 1 << 6,
        DoFXAA           = 1 << 7,
        DoDoF            = 1 << 8,
    };

    void postProcess(int shadeOptions);
    void save(const std::string& filename);

private:
    void tonemap();        // Reinhard / ACES
    void gammaCorrect();   // sRGB
    void bloom();
    void fxaa();
    void depthOfField(const float* depthBuffer, float focus, float coc);
};
```

### 12.2 Tone Mapping

```cpp
// Reinhard
float3 reinhard(float3 hdr) {
    return hdr / (hdr + 1.0f);
}

// ACES Filmic (Narkowicz 2015)
float3 acesFilm(float3 x) {
    return (x * (2.51f * x + 0.03f))
         / (x * (2.43f * x + 0.59f) + 0.14f);
}
```

---

## 13. 天空盒 — `component.h/.cpp`

```cpp
class SkyBox {
    int width, height;
    std::vector<float3> data;   // RGB HDR 数据
    Distribution1D distrib;     // 重要性采样分布

public:
    void load(const std::string& hdrFile);

    // 采样: 给定方向返回 radiance
    float3 sample(const float3& dir) const;

    // 重要性采样: 按亮度分布采样
    float3 sampleImportance(float u1, float u2, float& pdf) const;

    bool empty() const { return width == 0; }

    // GPU 上传
    cudaTextureObject_t uploadToGPU() const;
};
```

---

## 14. 控制台 — `console.h/.cpp`

```cpp
class Console {
    std::map<std::string, Model>     models;
    std::map<std::string, RenderArgs> renderArgs;
public:
    void run();  // 交互式循环

    // 命令: create model <id>
    // 命令: create args <id>
    // 命令: render <model_id> <args_id>
    // 命令: view model <id>
    // 命令: view args <id>
    // 命令: exit
};
```

---

## 15. 射线微分模块 — `render.h/.cpp` (已实现：CPU 后端)

> **状态**：CPU 后端已落地（`render.cpp` 的 `calc_dPdxy/calc_dDdxy` +
> `ImageTexture::sample(mipLevel)` 的 mip 链，渲染主循环已传入初始 ray differential）。
> GPU kernel 为扁平缓冲手动双线性采样、无 mip，斜视角纹理过滤仍是潜在改进项。

### 15.1 设计目标

实现各向异性纹理过滤（MIP 映射），消除斜视角下的纹理模糊/锯齿。

```cpp
// ---- 射线微分计算 ----
// 从相机光线推导出屏幕空间导数 dPdx / dPdy
void calc_dPdxy(const Ray &ray, float hit_t, const float3 &normal,
                const RayDifferential &base_diff,
                float3 &hit_dPdx, float3 &hit_dPdy);

// ---- 射线方向微分 ----
void calc_dDdxy(const Ray &ray, const float3 &normal,
                const RayDifferential &base_diff,
                float3 &hit_dDdx, float3 &hit_dDdy);

// ---- 纹理坐标微分 (UV 导数) ----
float2 getDuv(const Face &face, const float3 &hit_dPdx);
```

### 15.2 Mip Level 计算

```
duv_dx = |getDuv(face, hit_dPdx)|
duv_dy = |getDuv(face, hit_dPdy)|
mipLevel = log2(max(duv_dx, duv_dy))

// 在 ImageTexture::sample(u, v, mipLevel) 中使用
```

### 15.3 BSDF 正则化

```cpp
// 粗糙度路径正则化：防止光滑表面在低 spp 下产生 firefly
// 随着路径深度增加，粗糙度下限逐渐提高
constexpr float regularizationFactor = 0.01f;
void regularizeRoughness(float &roughness, float roughnessFactor, int depth) {
    float minRoughness = roughnessFactor + regularizationFactor * depth;
    roughness = std::max(roughness, minRoughness);
}
```

---

## 16. 完整 Disney BSDF (已实现)

### 16.1 当前已实现

| 层 | 状态 |
|----|------|
| Diffuse (Lambertian) | ✅ 已实现 |
| Specular (GTR2/GGX) | ✅ 已实现 |
| Fresnel (Schlick) | ✅ 已实现 |
| Metallic 混合 | ✅ 已实现 |

### 16.2 Subsurface / Sheen / Clearcoat（已实现）

> **状态**：四层全部落地，CPU/GPU 同公式（`sampling.cpp` 的 `BSDF::eval` 与
> `render_gpu.cu` 的 diffuse/sheen 分支）；clearcoat GTR1 逆 CDF 的潜伏 bug 已在
> 2026-09-13 全代码审计修复（README §13.1）。Bistro 资产 clearcoat=0，画面无感。

```cpp
// ---- Subsurface 散射层 ----
// Disney BSDF 论文 5.3: 基于 diffuse lobe 的次表面散射近似
// 控制参数: subsurface [0, 1]
float3 Fd = (1.0f / PI) * mix(FdLambertian, ss, subsurface) * Cdlin;

// ---- Sheen 织物质感层 ----
// Disney BSDF 论文 5.4: 模拟布料/织物表面的纤维散射
// Csheen = 混入 baseColor 的色调
// 控制参数: sheen [0, 1], sheenTint [0, 1]
float3 Fsheen = FH * sheen * Csheen;

// ---- Clearcoat 清漆层 ----
// Disney BSDF 论文 5.5: 模拟漆面/透明涂层
// 使用 GTR1 分布（比 GTR2 尾部更宽）
// 控制参数: clearcoat [0, 1], clearcoatGloss [0, 1]
float Dr = GTR1(NdotH, mix(0.1f, 0.001f, clearcoatGloss));
float Fr = mix(0.04f, 1.0f, FH);  // 清漆 F0 = 0.04 (IOR=1.5)
float Gr = smithG_GGX(NdotL, 0.25f) * smithG_GGX(NdotV, 0.25f);
float3 clearcoat = Dr * Fr * Gr / 4.0f;
```

### 16.3 BSDF 求值管线图

```
getBSDF(outDir)
  │
  ├── Diffuse (Lambertian)
  │     └── Cdlin = (1 - metallic) * baseColor
  │     └── Fd = (1/π) * mix(1, ss, subsurface) * Cdlin
  │     └── Fsheen = FH * sheen * Csheen
  │
  ├── Specular (GTR2/GGX)
  │     └── Cspec0 = mix(specular, baseColor, metallic)
  │     └── D = GTR2(NdotH, roughness²)
  │     └── F = Cspec0 + (1 - Cspec0) * pow5(1 - LdotH)
  │     └── G = smithG(NdotV, roughness²) * smithG(NdotL, roughness²)
  │     └── specular = D * F * G / (4 * NdotV * NdotL)
  │
  └── Clearcoat
        └── Dr = GTR1(NdotH, clearcoatGloss)
        └── Fr = 0.04 + 0.96 * pow5(1 - LdotH)
        └── Gr = smithG_GGX(NdotL, 0.25) * smithG_GGX(NdotV, 0.25)
        └── clearcoat = Dr * Fr * Gr / 4
```

---

## 17. 空间滤波降噪 (已实现，且已超出本设计)

> **状态**：落地实现远超本节蓝图——7 遍多尺度 log 域 NL-means、5 项边缘感知权重、
> firefly 双门、SPP 自适应门限、方差引导混合与 albedo 细节回注，另集成 Intel OIDN
> 双路降噪。完整设计与实测见 README §8 与 §13.5/§13.12。

### 17.1 设计目标

参考 Raym0nade 的 `filter()` 实现，基于 NL-Means 联合双边滤波，利用 G-buffer 的几何信息（法线、位置）做边缘感知平滑。

```cpp
// ---- 方差跟踪 ----
// 在 RadianceData 中维护辐射度的方差估计
struct RadianceData {
    float3 radiance;   // 累积辐射度
    float  Var;        // 方差估计 (平方的累积 - 均值的平方)
};

void accumulateInwardRadiance(RadianceData &rd, float3 inradiance, float weight) {
    rd.radiance += inradiance * weight;
    rd.Var += dot(inradiance, inradiance) * weight;
}

// ---- 空间滤波 ----
// 对每个像素，在其邻域内搜索相似像素做加权平均
// 权重考虑: 位置距离、法线夹角、基色差异、辐射度方差
float getWeight(const HitInfo &Gp, const HitInfo &Gq,
                const RadianceData &Lp, const RadianceData &Lq, bool specular) {
    float dn = 1 - dot(Gp.shapeNormal, Gq.shapeNormal);
    float dc = dot(Gp.baseColor, Gq.baseColor) / (dot(Gp.baseColor, Gp.baseColor) + 1e-6f);
    float dv = fabsf(Lp.Var - Lq.Var) / max(Lp.Var, Lq.Var);
    return exp(-dn * dn / sigmaN - dc * dc / sigmaC - dv * dv / sigmaV);
}

void filterRadiance(RadianceData *radiance, const HitInfo *Gbuffer,
                    int width, int height, bool specular);
```

### 17.2 滤波管线

```
渲染完成
  │
  ├── spatialClamp()         ← 裁剪异常值 / firefly
  ├── filter()               ← 4 次迭代 NL-Means
  │     ├── 对 radiance_Dd / _Id (diffuse)  联合滤波
  │     └── 对 radiance_Ds / _Is (specular) 联合滤波
  ├── shade(Full)            ← 合成最终色彩
  ├── bloom()                ← 泛光
  ├── gammaCorrection()      ← Gamma 校正
  ├── FXAA()                 ← 抗锯齿
  └── save()                 ← PNG 输出
```

---

## 18. 光源面片重要性分布 (已实现)

> **状态**：两级重要性采样已落地——对象级按功率 CDF（距离加权 `power/(dist²+area)`）、
> 对象内按三角形面积前缀和均匀采样；K 多样本平均与 power-heuristic MIS 配对见
> README §5.1。

### 18.1 设计目标

Raym0nade 的每个 `LightObject` 包含多个发光面片，按面积/亮度做重要性采样。当场景有大量光源（Bistro 夜景 ~120 光源）时，需要先选择光源，再在面片内采样。

```cpp
// ---- 光源对象改进 ----
class LightObject {
    float3 center;
    float3 color;
    float  power;
    RandomDistribution faceDist;  // 按面积/亮度的面片分布
    std::vector<const Face*> faces;

    // 采样: 返回面上的一个点 + 方向 + PDF
    LightSample sample(Generator &gen) const;
    float pdf(const float3 &point, const float3 &wi) const;
};

// ---- 光源采样管线 ----
struct LightSample {
    float3 position;   // 光源面上的采样点
    float3 wi;         // 从着色点到采样点的方向
    float  pdf;        // 采样 PDF
    float3 emission;   // 该面的发光强度
};

std::vector<LightSample> sampleDirectLight(
    const BSDF &bsdf, const Model &model,
    Generator &gen, int spp);
```

---

## 19. GPU SoA 数据打包流程

```
assimp 加载完成后:
  Face[] faces (AoS)
    │
    ├── 提取 vertices  → float3 vertBuffer[faces*3]
    ├── 提取 normals   → float3 normalBuffer[faces*3]
    ├── 提取 uvs       → float2 uvBuffer[faces*3]
    ├── 提取 material  → int materialIdBuffer[faces]
    │
    └── BVH flatten   → LinearBVHNode bvhNodeBuffer[nodes]
                          │
                          ├── 每个节点存储 2 个子 AABB (float3 × 2)
                          ├── 子节点索引 (int × 2)
                          └── 叶节点: faceStart/faceCount

  所有 buffer → cudaMalloc → cudaMemcpy → GPU
```

---

## 20. 性能预算

| 模块 | CPU 预算 (per ray) | GPU 预算 (per ray per thread) |
|------|-------------------|------------------------------|
| BVH 遍历 | ~50 节点访问 | ~30 节点访问 (warp-level) |
| 三角形求交 | ~6 次 | ~4 次 (提前剔除) |
| BSDF 采样 | ~200 cycles | ~40 cycles |
| 纹理采样 | ~100 cycles | ~20 cycles (硬件纹理单元) |
| 随机数 | ~50 cycles | ~10 cycles (Philox) |

---

## 21. 关键设计决策记录 (ADR)

| ID | 决策 | 理由 | 影响 |
|----|------|------|------|
| ADR-01 | 使用 `cudaArray` + `textureObject` 而非 global memory 采样 | 硬件双线性过滤 + mipmap 自动管理 + 纹理缓存优化 | 增加纹理上传复杂度，但 Kernel 性能高 2-3x。**已取代 → ADR-14** |
| ADR-02 | BVH 用 SAH CPU 构建 + 展平为 Linear BVH | 构建简单，GPU 遍历高效 | 构建时间 20-60s，通过磁盘缓存避免重复构建 |
| ADR-03 | GPU RNG 使用 cuRAND Philox | 质量高、周期长、寄存器友好 | 增加 cuRAND 依赖，但这是 CUDA 标准库 |
| ADR-04 | AOV 分离在路径追踪时完成，而非后处理 | 避免二次遍历场景，减少带宽 | Kernel 需要维护 5 个累加器，增加寄存器压力 |
| ADR-05 | Tile size = 16×16 | V100 occupancy 平衡 + shared memory 限制 | 256 threads/block，shared memory 12KB |
| ADR-06 | 去除 Python 依赖，使用 stb_image.h | 简化构建，减少外部依赖 | 需要处理 DDS 格式（可用 tiny_dds_loader） |
| ADR-07 | Disney BSDF 分 4 层实现（diffuse/sheen/specular/clearcoat）而非单一 lobe | 逐层叠加更灵活，可独立开关每层做调试 | 增加 BSDF 评估计算量 ~20%，但 Visual 质量提升显著 |
| ADR-08 | BSDF 参数使用 HitInfo SoA 传入，而非 Material 指针间接引用 | GPU 上减少指针追逐，提高 cache 命中率 | 增加每线程寄存器压力，但减少 global memory 流量 |
| ADR-09 | 降噪使用 NL-Means 联合双边滤波，而非深度学习方法 | 无需额外依赖（OptiX/TensorRT），结果确定可复现 | 滤波耗时 ~3-5s/帧 (1920×1080)，比 OptiX Denoiser 慢但可调参 |
| ADR-10 | 辐射度方差在路径追踪时在线累积，而非后处理估计 | 避免二次遍历，方差数据无额外开销 | Kernel 需要额外维护 Var 累加器，增加 ~8 字节/thread |
| ADR-11 | 光源采样采用两级分布：光源级 + 面片级 | Bistro 夜景有 ~120 光源，先选光源再选面片比全部面片统一采样更高效 | 增加采样代码复杂度，但收敛速度明显提升 |
| ADR-12 | BSDF 粗糙度随路径深度正则化（regularization） | 防止光滑表面在低采样率下产生 firefly（极高亮度异常像素） | 轻微增加偏差（bias），但显著改善视觉效果 |
| ADR-13 | 射线微分从相机光线推导，而非从屏幕空间差分计算 | 光线方向微分可直接给出纹理坐标的各向异性导数 | 增加 generateRay 计算量 ~10%，但纹理过滤精度更高 |
| ADR-14 | GPU 纹理改为 uchar4 扁平大缓冲手动双线性采样，后进一步演进为 BC 块驻显存 + 内核软件解码（取代 ADR-01） | Bistro 213 张 2048² 贴图双份拷贝需 ~28GB 且 OOM 静默白屏；扁平缓冲 ~3.6GB，BC 驻显存 ~0.73GB（纹理显存 -80%）；整数取整逐位复刻 CPU 解码，与 CPU 渲染字节级一致 | 失去硬件过滤与 mip（斜视角过滤为遗留改进项）；上传失败由静默跳过改为显式告警中止 |
