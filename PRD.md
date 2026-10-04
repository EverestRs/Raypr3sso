# 光线追踪渲染器 — 产品需求文档 (PRD)

## 1. 概述

### 1.1 项目目标

开发一款 **离线光线追踪渲染器**，支持 CPU + GPU (CUDA/HIP) 双后端，能够处理 Amazon Lumberyard Bistro 开源场景（酒馆内景 ~1.3M 面、街道外景 ~2.8M 面），实现高质量的物理正确渲染。

### 1.2 参考项目

- **[Raym0nade](https://github.com/lemonchu/Raym0nade)** (MIT License)：CPU 多线程路径追踪器，使用 assimp + glm + libpng，支持 Disney BSDF、BVH、多级 G-buffer 输出、Bloom/FXAA/景深等后处理。

### 1.3 技术栈选择

| 维度 | 选择 |
|------|------|
| 编程语言 | C++17 / C++20 |
| GPU 后端 | CUDA (NVIDIA) / HIP (AMD) |
| 数学库 | glm (CPU) / CUDA 内置数学 (GPU) |
| 模型加载 | assimp (FBX 解析 + 三角化) |
| 图像输出 | stb_image_write / libpng |
| 构建系统 | CMake |
| 环境贴图 | HDR (OpenEXR / Radiance HDR) |
| 纹理格式 | .png, .dds (Python 脚本转码) |

### 1.4 目标 GPU 硬件

| 项目 | 规格 |
|------|------|
| **GPU 型号** | NVIDIA Tesla V100 (Volta 架构) |
| **CUDA Cores** | 5120 |
| **显存** | 16GB / 32GB HBM2 |
| **Compute Capability** | 7.0 |
| **RT Cores** | ❌ 无（纯软件 BVH 遍历） |
| **Tensor Cores** | 有（可用于降噪辅助，非必须） |

> **注意**：V100 无硬件 RT Core，光线求交完全由 CUDA Kernel 软件实现，符合本项目手写 BVH 遍历 + 三角形求交的设计目标。

#### Bistro 场景显存估算

```
场景: BistroExterior (2.8M 面, 最坏情况)
  三角形顶点数据: 2.8M × 3顶点 × 3 floats × 4B ≈ 100 MB
  BVH 节点: ~5.6M 节点 × 32B ≈ 180 MB
  PBR 纹理贴图 (估算)                         ≈ 2 GB
  帧缓冲 + G-buffer (1920×1080 × 4通道 × 4B × 5) ≈ 165 MB
  临时路径栈 + 随机数状态                      ≈ 100 MB
  ──────────────────────────────────────────
  总计估算: ≈ 2.5 GB << 16 GB ✅
```

### 1.5 GPU 跨代兼容性分析

#### 核心结论

本项目基于 V100 开发纯软件 BVH + 三角形求交，代码在 **所有 NVIDIA GPU 上完全兼容**，无需任何修改即可运行在带 RT Core 的 GPU 上。

#### 跨代支持矩阵

| 架构 | 代表 GPU | Compute Cap. | RT Core | 本项目兼容性 | 纯软件性能 (vs V100) |
|------|----------|--------------|---------|-------------|---------------------|
| Kepler | K80 | 3.7 | ❌ | ✅ | ~0.2x |
| Maxwell | M40 | 5.2 | ❌ | ✅ | ~0.3x |
| Pascal | P100 | 6.1 | ❌ | ✅ | ~0.6x |
| **Volta** | **V100** | **7.0** | **❌** | **✅ 基准** | **1x (baseline)** |
| Turing | RTX 2080 Ti | 7.5 | ✅ 1st gen | ✅ | ~1.5-2x |
| Ampere | RTX 3090 / A100 | 8.0 / 8.0 | ✅ 2nd gen | ✅ | ~2-3x |
| Ada Lovelace | RTX 4090 | 8.9 | ✅ 3rd gen | ✅ | ~3-4x |
| Hopper | H100 | 9.0 | ✅ 3rd gen | ✅ | ~3-4x |

#### 为什么无需担心兼容性

1. **纯 CUDA C++** — BVH 遍历、三角形求交、BSDF 采样都是标准 CUDA 代码，不属于任何硬件专有指令
2. **无 PTX 内联汇编** — 不使用特定架构的 PTX 指令，编译器向后兼容
3. **Compute Capability 抽象** — CUDA 工具链自动为不同架构编译，`-gencode` 参数可一次生成多个架构的 cubin

#### RT Core GPU 上的性能表现

即使有 RT Core，你的纯软件路径仍然有**竞争力**：

```
路径追踪各阶段耗时分布 (RTX 4090):
  ┌─────────────────────────────────────┐
  │ BVH 遍历 + 三角形求交     ~35%      │ ← 纯软件 vs RT Core
  │ BSDF 评估与采样           ~30%      │ ← 不受 RT Core 影响
  │ 光照采样                  ~20%      │ ← 不受 RT Core 影响
  │ 材质纹理采样              ~10%      │ ← 不受 RT Core 影响
  │ 路径生成与随机数          ~5%       │ ← 不受 RT Core 影响
  └─────────────────────────────────────┘
```

即使 BVH 遍历快 5x（RT Core），整体只提速 ~28%。**更大的收益来自更多 CUDA Core + 更高频率 + 更大显存带宽**。

#### 后续 RT Core 升级路径（可选）

```
// 当前：纯软件路径
void traceRay(Ray ray, HitRecord &hit) {
    bvh.traverse(ray, hit);   // 手写 BVH
    intersectTriangle(ray, hit);
}

// 后续：OptiX 混合路径
void traceRay(Ray ray, HitRecord &hit) {
#ifdef USE_OPTIX
    optixTrace(...);           // RT Core 硬件加速
    if (optixResult == MISS)
        bvh.traverse(ray, hit); // 回退到自定义几何求交
#else
    bvh.traverse(ray, hit);    // 纯软件 fallback
#endif
}
```

架构上只需新增一个 **OptiX 适配层**，现有代码无需重构。OptiX 支持混合遍历（硬件 RT Core + 自定义求交），可在同一条路径中混用。

#### CPU 依赖分析

| 阶段 | 依赖 CPU | 说明 |
|------|----------|------|
| 模型加载 (assimp FBX) | ✅ 必须 | CPU-only，但仅启动时执行一次 |
| BVH 构建 | ✅ 必须 | 也可在 GPU 做 (LBVH)，但 CPU 构建更简单 |
| 纹理上传 | ⚡ 可选 | 可通过 CUDA 异步传输，CPU 几乎无开销 |
| **渲染主循环** | **❌ 不依赖** | **100% 在 GPU，与 RT Core 无关** |
| 后处理 | ⚡ 可选 | Bloom/FXAA/DoF 可完全在 GPU 实现 |
| 文件写入 | ✅ 必须 | PNG/EXR 写入，输出时瞬间完成 |

> 无论 GPU 有无 RT Core，**渲染核心不依赖 CPU**。CPU 只做预处理和 I/O。

#### 推荐的编译配置

```cmake
# 一次编译，多架构兼容
set(CUDA_ARCHS "70;75;80;86;89;90")
foreach(arch ${CUDA_ARCHS})
    list(APPEND CUDA_FLAGS -gencode arch=compute_${arch},code=sm_${arch})
endforeach()
```

这样编译出的二进制可在 V100 (SM70) 到 H100 (SM90) 上无缝运行。

---

## 2. 场景需求 (Scene Setup)

### 2.1 Amazon Lumberyard Bistro 模型

| 场景 | 面数 | 光源数量 | 材质挑战 |
|------|------|----------|----------|
| **酒馆内景** BistroInterior | ~1.3M 三角面 | ~20 个光源 | 酒类液体、玻璃器皿、金属吧台、粗糙木纹 |
| **街道外景** BistroExterior | ~2.8M 三角面 | ~120 个 (夜晚) / 天空盒 (白天) | 大面积远距离场景、大量点光源夜景、复杂植被 |

### 2.2 场景加载要求

1. **FBX 格式支持** — 使用 assimp 解析 `BistroInterior_Wine.fbx` 和 `BistroExterior.fbx`
2. **材质系统** — 完整解析 FBX 中嵌入的 PBR 材质参数（baseColor、roughness、metallic、specular、opacity、normal map、emissive）
3. **光源检测** — 自动识别 emissive 材质面片作为光源，支持灯光实例化和重要性采样
4. **天空盒** — 支持 HDR 环境贴图（.hdr 格式），白天外景使用 `san_giuseppe_bridge_4k.hdr`
5. **单位统一** — FBX 场景使用米制单位，相机参数与之对齐

---

## 3. 功能需求 (Functional Requirements)

### 3.1 核心渲染管线

| 优先级 | 模块 | 描述 |
|--------|------|------|
| P0 | **路径追踪 (Path Tracing)** | 基于物理的无偏路径追踪，支持间接光照、多反弹 |
| P0 | **BVH 加速结构** | SAH 优化的 BVH 树，CPU/GPU 双版本 |
| P0 | **Disney BSDF 材质** | Diffuse + Glossy + Metal + Glass (Dielectric) |
| P0 | **直接光照采样** | 对光源面片进行重要性采样（NEE） |
| P1 | **纹理映射** | 支持 baseColor / normal / roughness / metallic 贴图，mipmap |
| P1 | **多光源采样** | 对大量光源（~120个）使用 BVH/KD-tree 加速查找或 Light BVH |
| P1 | **介质渲染** | 支持透明材质吸收（beer's law），折射/反射 |
| P2 | **面光源 / 区域光源** | 物理正确的 emissive 面片采样 |
| P2 | **景深 (DoF)** | 薄透镜相机模型，弥散圆散焦 |
| P2 | **运动模糊 (Motion Blur)** | 时间采样，支持变换运动 |
| P2 | **体积光 / 雾效** | 均匀介质 / 异质介质 (participating media) |

### 3.2 输出与后处理

| 优先级 | 功能 | 描述 |
|--------|------|------|
| P0 | **多级 G-buffer 输出** | Direct Diffuse/Specular, Indirect Diffuse/Specular, Normal, BaseColor, Emission 分离输出 |
| P0 | **PNG 输出** | 8-bit/channel, PNG 格式 |
| P1 | **Tone Mapping** | Reinhard / ACES Filmic Tone Mapping |
| P1 | **Gamma 校正** | sRGB 标准 Gamma 校正 |
| P1 | **Bloom** | 高亮区域泛光效果 |
| P1 | **FXAA** | 快速近似抗锯齿 |
| P2 | **EXR 输出** | 16-bit float EXR 保留 HDR 数据 |

### 3.3 渲染参数控制

| 参数 | 说明 |
|------|------|
| 分辨率 | `width` / `height` |
| 每像素采样数 | `spp` (samples per pixel) |
| 直接/间接采样比 | `P_Direct` 分配比例 |
| 线程数 | CPU 后端渲染线程数 |
| 景深参数 | `focus` 焦距 / `CoC` 弥散圆半径 |
| 曝光值 | `exposure` |
| 输出路径 | 渲染结果保存目录 |

---

## 4. 架构设计

### 4.1 双后端架构

```
                    +---------------------+
                    |   Scene / Model     |
                    | (assimp + FBX)      |
                    +----------+----------+
                               |
                    +----------+----------+
                    |   BVH Builder       |
                    | (SAH, CPU build)    |
                    +----+----------+-----+
                         |          |
              +----------+--+  +----+----------+
              | CPU Backend  |  | GPU Backend   |
              | (std::thread) |  | (CUDA/HIP)    |
              | Path Tracing  |  | Path Tracing  |
              | on CPU        |  | on GPU (grid) |
              +-------+------+  +-------+-------+
                      |                 |
              +-------+------+  +-------+-------+
              | Photo (CPU)  |  | Photo (CPU)    |
              | Post-process |  | Post-process   |
              +-------+------+  +-------+-------+
                      |                 |
              +-------+-----------------+-------+
              |         PNG / EXR Export         |
              +----------------------------------+
```

### 4.2 CPU 后端设计 (参考 Raym0nade)

- **多线程渲染**：任务队列 + 线程池，每个线程独立随机数生成器
- **BVH 遍历**：DFS + 近轴优先优化
- **路径追踪**：递归采样（最大深度 16），Russian Roulette 终止
- **BSDF 采样**：Disney BSDF (GTR1/GTR2, Smith GGX)
- **G-buffer 累加**：直接/间接、漫反射/高光 四通道各自独立累加

### 4.3 GPU 后端设计

- **CUDA Kernel**：每像素一个 thread，wavefront 方式发射路径
- **BVH**：将 BVH 结构拷贝到 GPU 全局内存，Shared memory 优化遍历
- **路径追踪**：使用栈式遍历替代递归（栈在 local memory）
- **Tile-based 渲染**：分块渲染以避免 GPU 超时
- **Stream 并行**：多个 CUDA stream 同时处理不同 tile

### 4.4 模块划分

| 模块 | 文件 (参考 Raym0nade) | 说明 |
|------|----------------------|------|
| 几何工具 | `geometry.h/.cpp` | Ray, Box, 三角形求交 (Möller–Trumbore), 重心坐标 |
| 组件 | `component.h/.cpp` | VertexData, Face, Generator, RandomDistribution, SkyBox |
| 材质 | `material.h/.cpp` | ImageData, Material (PBR 纹理加载 + 属性) |
| 模型 | `model.h/.cpp` | 模型加载 (assimp), BVH 构建, rayHit |
| BVH | `bvh.h/.cpp` | SAH BVH 构建 + 遍历 |
| BSDF | `sampling.h/.cpp` | Disney BSDF, 重要性采样 |
| 渲染核心 | `render.h/.cpp` | Path Tracing 主循环, 直接/间接光照采样 |
| 图像 | `image.h/.cpp` | G-buffer, Post-processing (Bloom/FXAA/DoF) |
| 控制台 | `myconsole.h/.cpp` | 交互式控制台 (model/args 管理 + 渲染命令) |

---

## 5. 性能目标

### 5.1 渲染时间目标

| 场景 | 分辨率 | SPP | CPU (12核+) | GPU (V100) | GPU 加速比 |
|------|--------|-----|-------------|------------|------------|
| 酒馆内景 (1.3M 面) | 1920x1080 | 256 | < 30 min | ~1-2 min | ~15-30x |
| 街道外景 (2.8M 面) | 1920x1080 | 256 | < 60 min | ~3-5 min | ~12-20x |
| 街道夜景 (120 光源) | 1920x1080 | 512 | < 120 min | ~8-12 min | ~10-15x |

### 5.2 优化策略

- **CPU 后端**：充分利用多核（12 线程+），SIMD 优化三角形求交（SSE/AVX）
- **GPU 后端**：利用 Shared Memory 优化 BVH 遍历，Coalesced Memory 访问，Occupancy 优化

### 5.3 GPU 特别注意事项

| 考量点 | 说明 |
|--------|------|
| **无 RT Core** | V100 纯软件求交，需优化 BVH 布局和 warp-level 并行 |
| **Occupancy** | 路径追踪每个 thread 需要私有栈，限制 thread 数量，需平衡 block size |
| **显存带宽** | V100 HBM2 900 GB/s，适合大纹理场景，需注意 texture cache 命中 |
| **Tile 大小** | V100 建议 16×16 tile，避免 local memory 溢出并保持高 occupancy |

---

## 6. 实施路线图

### Phase 1：基础框架 (Week 1-2)

- [x] 项目 CMake 搭建
- [ ] 基础几何工具 (Ray, Box, 三角形求交)
- [ ] assimp 模型加载，FBX -> 三角面片
- [ ] BVH 构建 (SAH)
- [ ] CPU 单线程 rayHit 测试

### Phase 2：CPU 路径追踪 (Week 3-4)

- [ ] Disney BSDF (Diffuse + Glossy + Metal + Glass)
- [ ] 直接光照采样 (NEE)
- [ ] 多线程渲染引擎
- [ ] PNG 输出 + Gamma 校正
- [ ] Bistro 内景渲染验证

### Phase 3：材质与纹理 (Week 5-6)

- [ ] 纹理加载 (PNG/DDS)
- [ ] 法线贴图
- [ ] 透明材质 (Opacity + IOR)
- [ ] 介质吸收 (Beer's Law)
- [ ] 完整 Disney BSDF (subsurface + sheen + clearcoat)
- [ ] 射线微分 (RayDifferential) 实现各向异性纹理过滤
- [ ] 多光源重要性采样 (Light BVH)

### Phase 4：后处理 (Week 7-8)

- [ ] Tone Mapping
- [ ] Bloom
- [ ] FXAA
- [ ] 景深 (Depth of Field)
- [ ] 多级 G-buffer 分离输出

### Phase 5：GPU 后端 (Week 9-12)

- [ ] CUDA BVH 移植（Linear BVH / 压缩节点以适应 V100 显存带宽）
- [ ] CUDA Path Tracing Kernel（stack-based 遍历替代递归）
- [ ] Tile-based 渲染（16×16 tile，适配 V100 occupancy）
- [ ] GPU occupancy 调优（block size / shared memory / register 平衡）
- [ ] Bistro 场景在 V100 上的正确性验证 + 性能 baseline
- [ ] HIP (AMD) 后端适配

### Phase 6：高级特性 (Week 13-16)

- [ ] 运动模糊
- [ ] 体积光 / 雾效
- [ ] EXR 输出
- [ ] 街道外景（白天 ~120 光源 + 天空盒）验证
- [ ] 街道夜景渲染验证

### Phase 7：画质提升 (Week 17-20)

- [ ] 光源面片重要性分布 (RandomDistribution per LightObject)
- [ ] 辐射度方差追踪 (Variance tracking for spatial denoising)
- [ ] 空间滤波降噪 (NL-Means 联合双边滤波)
- [ ] 曝光控制参数 (exposure slider)
- [ ] Disney BSDF 补充：subsurface 散射层
- [ ] Disney BSDF 补充：sheen 织物质感层
- [ ] Disney BSDF 补充：clearcoat 清漆层
- [ ] 射线微分各向异性纹理过滤 (RayDifferential dPdx/dPdy)
- [ ] 视差校正环境贴图 (Parallax-corrected cubemap)
- [ ] Taylor 级数展开的 BSDF 正则化 (粗糙度路径正则化)

---

## 7. 风险评估 (Risk Assessment)

### 🔴 高风险（编码前必须解决）

#### R1：CPU / GPU BVH 格式不兼容

| 项 | 说明 |
|----|------|
| **问题** | CPU 用 SAH 递归指针结构（节点含 `left`/`right` 指针），GPU 需要 Linear BVH（压缩节点、数组索引、宽度优先）。两者遍历路径完全不同，不能共享同一份 Builder 输出 |
| **影响** | 若 Phase 1 按 CPU 方式构建 BVH，到 Phase 5 移植 GPU 时可能需要重写 Builder + 重测全部场景 |
| **解决方案** | Builder 输出中间表示 → 分别展平为 CPU/GPU 两种布局 |
| **决策** | **CPU 构建 SAH → 展平为 Linear BVH（SOA 格式）供 GPU 使用。** CPU 端保留指针结构便于递归遍历，GPU 端使用 8-wide / 4-wide 压缩节点。Builder 内部实现两种 layout 输出 |

#### R2：GPU 纹理采样方案缺失

| 项 | 说明 |
|----|------|
| **问题** | Bistro 有大量 PBR 纹理（baseColor / normal / roughness / metallic），部分为 4K 分辨率。GPU 上使用何种方式采样直接影响 Kernel 性能和画质 |
| **影响** | 若后期更换纹理方案，需要重写 Material 模块和 Kernel 纹理读取逻辑 |
| **解决方案** | 提前确定纹理管线 |
| **决策** | **使用 `cudaArray` + `textureObject`**，利用硬件纹理单元做双线性过滤 + mipmap 自动降级。纹理上传使用 `cudaMemcpyToArray` 异步传输，mipmap 链由 CUDA 自动生成或预计算 |

#### R3：GPU 随机数生成器未指定

| 项 | 说明 |
|----|------|
| **问题** | 路径追踪每个像素每次反弹都需要高质量随机数。RNG 的质量影响噪点分布，性能影响 Kernel 总耗时 |
| **影响** | 手写 RNG 可能周期不够导致条纹伪影；cuRAND API 使用不当会引入额外寄存器压力 |
| **解决方案** | |
| **决策** | **使用 cuRAND 的 Philox 4×32-10 算法**（`curand_init` + `curand_uniform`），每个 thread 独立 seed + sequence。Philox 在 GPU 上周期长、质量高、寄存器占用可接受。CPU 端使用 `std::mt19937`（同参考项目） |

#### R4：CPU / GPU 双后端正确性验证

| 项 | 说明 |
|----|------|
| **问题** | 同一场景在 CPU 和 GPU 上渲染结果需一致，但 Monte Carlo 路径追踪本身有随机噪声，不能简单做像素精确匹配 |
| **影响** | 若到 Phase 5 才发现不一致，排查 root cause 成本极高 |
| **解决方案** | 建立自动化验证 Pipeline |
| **决策** | **实现 `render_diff` 工具**：固定随机种子、同路径数，逐像素计算 MSE / PSNR / MaxError。对 diffuse 通道允许误差 < 0.5%，specular 通道因浮点精度差异允许 < 1%。CI 中集成回归测试（小场景 100 spp） |

---

### 🟡 中风险（实施前需补充）

#### R5：HIP / CUDA 双后端维护策略

| 项 | 说明 |
|----|------|
| **问题** | PRD 承诺 HIP (AMD) 支持，但 HIP 与 CUDA 存在细微差异：`__shfl_sync` 行为、动态并行能力、`cudaGraph` 等 |
| **方案 A** | 使用 **hipify-perl** 脚本自动转换 CUDA → HIP，最小化手写差异代码 |
| **方案 B** | 核心 Kernel 用 HIP 书写（兼容 CUDA 编译），平台特定 API 用 `#ifdef __HIP_PLATFORM__` 隔离 |
| **推荐** | **方案 A + B 混合**：基础框架用 hipify 做一次性转换，平台特有优化手写 `#ifdef` |
| **时机** | Phase 5 末期实施，先保证 CUDA 正确，再 HIP 移植 |

#### R6：显存溢出处理 (Out-of-Core)

| 项 | 说明 |
|----|------|
| **问题** | Bistro 外景 2.8M 面 + 高分辨率纹理可能 > 8GB，若在 16GB V100 上不足，需降级策略 |
| **触发条件** | 纹理分辨率过高（8K × 8K）或场景未来扩展 |
| **方案 A** | 纹理动态流式加载（仅加载当前可见区域纹理，参考 MegaTextures） |
| **方案 B** | 显存不足时自动回退到 CPU 后端 |
| **推荐** | **方案 B**（简单可靠）：渲染前预检显存，超限时提示用户切换 CPU 后端或降低纹理分辨率 |

#### R7：降噪方案缺失

| 项 | 说明 |
|----|------|
| **问题** | 256 SPP 对 Bistro 夜景（120 光源）噪声仍明显，需降噪才能达到视觉可用级别 |
| **可选方案** | |
| | **OptiX Denoiser** — 利用 Tensor Core，质量高，但增加 OptiX 依赖 |
| | **OpenCV NLM / BM3D** — CPU 降噪，无需额外 GPU 依赖 |
| | **SVGF / A-SVGF** — 开源时空降噪，适合路径追踪 |
| **推荐** | Phase 6 评估 OptiX Denoiser（最优质量），BM3D 作为 CPU fallback |

#### R8：BVH 构建时间

| 项 | 说明 |
|----|------|
| **问题** | 2.8M 面的 SAH BVH 在 CPU 上构建需 20-60 秒 |
| **影响** | 仅启动时一次构建，可接受。但若后续引入动态物体或场景编辑，则不可接受 |
| **方案** | Phase 1 使用 CPU SAH 构建（简单）。未来需要时可实现 GPU LBVH（~1 秒内构建） |
| **决策** | **CPU SAH 构建，缓存 BVH 到磁盘文件**，下次加载直接反序列化，避免重复构建 |

#### R9：AOV / 渲染通道未精确定义

| 项 | 说明 |
|----|------|
| **问题** | "多级 G-buffer 输出"含义模糊：四通道（Direct Diffuse/Specular, Indirect Diffuse/Specular）如何从路径贡献中分离？ |
| **定义** | |
| | **Direct Diffuse** — NEE 采样 + diffuse BSDF 评估 |
| | **Direct Specular** — NEE 采样 + specular BSDF/BRDF 评估 |
| | **Indirect Diffuse** — 间接反弹 + diffuse BSDF |
| | **Indirect Specular** — 间接反弹 + specular/glossy BSDF |
| | **BaseColor** — 首次命中 `baseColor`（无光照） |
| | **Emission** — 自发光/天空盒直接贡献 |
| **输出格式** | 单帧渲染输出 6 张独立 PNG（或 1 张多图层 EXR） |

---

### 🔵 低风险（建议补充）

#### R10：Bistro 模型许可证

| 项 | 说明 |
|----|------|
| **许可证** | Amazon Lumberyard Bistro 使用 **Apache 2.0** 许可，允许商业使用但需保留版权声明 |
| **要求** | 在项目中放置 `LICENSE` 文件并在文档中注明："Bistro scene model copyright Amazon.com, Inc. or its affiliates. Licensed under Apache 2.0." |

#### R11：Python 依赖必要性

| 项 | 说明 |
|----|------|
| **问题** | 参考项目使用 Python 调用 `dds_to_array` / `hdr_to_array` 脚本做纹理转码 |
| **建议** | 用 **`stb_image.h`** 直接加载 HDR/PNG，消除 Python 运行时依赖。DDS 格式可通过 `tiny_dds_loader` 或 `DirectXTex` 处理 |
| **决策** | **去除 Python 依赖**，改用 C/C++ 库。简化构建环境配置 |

#### R12：浮点精度策略

| 项 | 说明 |
|----|------|
| **策略** | BVH 遍历 / 三角形求交：`float`（足够） |
| | BSDF 评估 / 颜色累加：`float`（参考项目已验证） |
| | 高贡献路径 / 焦散：`double`（可选，遇到 Firefly artifact 时启用） |
| | 帧缓冲累加：`float` + 原子操作或 `float4` 向量化 |

#### R13：GPU 数据布局 (AoS → SoA)

| 项 | 说明 |
|----|------|
| **问题** | CPU 端用 `Face { vec3 v[3], VertexData* data[3], Material* }`（AoS）方便遍历，但 GPU 需要 **SoA** 以获得 coalesced memory 访问 |
| **决策** | Builder 输出两种格式：CPU 保留 AoS 结构，GPU 使用 SoA 变体（顶点位置数组、法线数组、UV 数组分离存储）。此转换在 BVH 展平阶段一并完成 |

#### R14：Phase 1 缺少 GPU 前期准备

| 项 | 说明 |
|----|------|
| **问题** | Phase 1 全为 CPU 工作，但 CUDA 项目构建、SoA 数据布局设计应从初期开始 |
| **建议** | Phase 1 增加： |
| | - CUDA 工具链检测 + 编译配置 (`find_package(CUDA)`) |
| | - GPU 设备查询模块（显存大小、Compute Capability） |
| | - SoA 数据结构设计文档 |
| | - 简单的 GPU 内存分配 / 传输测试（热身） |

---

### 风险缓解行动计划

| ID | 行动 | 截止阶段 | 负责人 |
|----|------|----------|--------|
| R1 | BVH Builder 设计文档：CPU SAH + Linear BVH 展平方案定稿 | Phase 1 前 | 架构 |
| R2 | GPU 纹理管线原型：cudaArray + textureObject 示例验证 | Phase 1 | 工程 |
| R3 | cuRAND Philox 采样测试 + 与 CPU Mersenne Twister 结果对比 | Phase 1 | 工程 |
| R4 | `render_diff` 验证工具实现 + 回归测试场景定义 | Phase 2 | QA |
| R5 | HIP 兼容性测试计划制定 | Phase 5 前 | 架构 |
| R6 | 显存预检模块实现 | Phase 5 | 工程 |
| R7 | 降噪方案技术选型报告 | Phase 6 前 | 架构 |
| R8 | BVH 磁盘缓存序列化实现 | Phase 1 | 工程 |
| R9 | AOV 通道定义文档 + 分离实现 | Phase 2 | 架构 |
| R10 | 确认 Bistro 许可证并添加声明 | Phase 1 | PM |
| R11 | 用 C/C++ 库替代 Python 依赖 | Phase 1 | 工程 |
| R13 | SoA 数据布局结构体定义 | Phase 1 前 | 架构 |

---

## 8. Bistro 场景参考配置

参考 Raym0nade 的已有参数：

```
# 酒馆内景 (Interior)
create model bistro
model/Bistro_v5_2/
BistroInterior_Wine.fbx
null

# 街道外景 - 白天 (Exterior Day)
create model bistro
model/Bistro_v5_2/
BistroExterior.fbx
san_giuseppe_bridge_4k.hdr

# 街道外景 - 夜晚 (Exterior Night)
create model bistro
model/Bistro_v5_2/
BistroExterior.fbx
null
```

---

## 9. 成功标准

1. **内景场景**：正确渲染玻璃器皿的折射/反射、金属表面的高光、酒液的透射吸收、多个区域光源的柔和阴影
2. **外景白天**：天空盒环境光照、大面积间接光照、植被透射 shadow
3. **外景夜晚**：~120 个点光源的正确采样、无噪点/低噪点渲染、光衰减正确
4. **GPU 性能**：比 CPU 后端快 10-15x 以上
5. **图像质量**：与参考项目 Raym0nade 输出进行视觉对比，PSNR > 35dB
