# 远程渐进式渲染服务设计方案

## 1. 目标与范围

将当前 CUDA 离线路径追踪器改造成一个由远程服务器提供渲染能力的交互式渲染服务：

- 浏览器通过鼠标拖动改变视角。
- 浏览器通过滚轮或触控板改变远近。
- 远程服务器常驻加载 Bistro 场景和 CUDA 资源。
- 相机变化后重新开始 progressive rendering。
- 拖动期间显示低分辨率、低采样数的快速预览。
- 停止操作后持续累积采样，逐步提高画质。
- 浏览器通过 HTTP/WebSocket 获取状态和预览帧。

第一版范围：

- 单 GPU。
- 单服务器进程。
- 每个 session 同时只有一个 active render。
- 首先支持一个已部署的模型，例如 Bistro Interior。
- 不迁移到 WebGPU。
- 不支持客户端上传任意模型或访问任意服务器路径。
- 不承诺远程环境下每次鼠标事件都完成高质量 1080p 路径追踪。

推荐的用户体验是：

```text
拖动中：低分辨率快速预览
停止拖动：服务器持续累积采样，画面逐步变清晰
```

## 2. 总体架构

```text
+----------------------- Browser ------------------------+
| Three.js OrbitControls                                 |
| Camera state, drag/zoom throttle, preview canvas       |
+-------------------------+-----------------------------+
                          |
                 HTTPS / WebSocket
                          |
+-------------------------v-----------------------------+
|                 Remote Render Server                   |
| HTTP API | WebSocket events | Session manager          |
| Request coalescing | Job scheduler | Auth/rate limits  |
+-------------------------+-----------------------------+
                          |
+-------------------------v-----------------------------+
|                  CUDA Render Worker                    |
| GPUScene (resident) | RenderSession | RenderJob        |
| Persistent buffers  | Progressive accumulation        |
+-------------------------+-----------------------------+
                          |
+-------------------------v-----------------------------+
|                    NVIDIA GPU                         |
| BVH | Geometry | Materials | Textures | Light tables    |
+--------------------------------------------------------+
```

浏览器不直接访问 CUDA，也不负责上传模型。浏览器只提交相机和渲染参数，服务器负责模型、GPU 资源和任务调度。

## 3. 当前项目与设计的对应关系

当前实现仍然是一次性离线流程：

1. [src/console.cpp](src/console.cpp) 解析 `render` 命令。
2. 每次 render 都重新遍历模型并打包顶点、材质、纹理和灯光数据。
3. 每次 render 都重新上传 GPU 场景。
4. [src/render_gpu.cu](src/render_gpu.cu) 或 [src/render_wavefront.cu](src/render_wavefront.cu) 分配输出缓冲。
5. 渲染完成后回读 AOV 和图像数据。
6. 释放 GPU 模型和临时资源。

远程交互的第一项工作不是接入网络，而是把这种一次性生命周期拆成可复用的常驻对象。

## 4. 核心对象设计

### 4.1 GPUScene

`GPUScene` 表示已上传到 GPU 的场景，生命周期覆盖多个 render job。

建议保存：

- `GPUModelData`
- 顶点、法线、UV 和材质 ID
- GPU BVH
- GPU 材质 SoA 缓冲
- 纹理 raw buffer、宽度、高度和格式信息
- CUDA texture object
- 天空盒 CUDA texture object
- Sky importance sampling CDF
- 发光三角形
- 光源对象、前缀和及对象中心
- 上传元数据和显存统计

建议使用 RAII owner，避免现有裸指针在服务长期运行时产生泄漏或异常路径未释放的问题。

```cpp
class GPUScene {
public:
    static std::unique_ptr<GPUScene> load(const Model& model);

    GPUScene(const GPUScene&) = delete;
    GPUScene& operator=(const GPUScene&) = delete;

    const GPUModelData& deviceModel() const;
    cudaTextureObject_t skyTexture() const;

private:
    GPUModelData model_{};
    cudaTextureObject_t skyTexture_ = 0;
    // Texture ownership, sky CDF and related CUDA resources.
};
```

必须补齐以下资源的释放：

- CUDA texture object。
- Skybox CUDA array。
- Sky CDF。
- 纹理 GPU buffer。
- 失败上传过程中的已分配资源。

GPUScene 由进程级注册表按 `modelId` 持有一份，常驻、跨 session 共享：

```text
SceneRegistry
└── "bistro-interior" -> GPUScene（常驻，不随 session 释放）
```

禁止每个 session 各自加载场景。per-session 持有在测试球体下无感，换到
Bistro 后是致命的：纹理约占 4.7 GB 显存，两个 session 各持一份即 OOM；
模型打包加 GPU 上传需要 3-5 分钟，第二个 session 不应再等一次。

多 session 对同一 GPUScene 只读；写入只发生在加载阶段，由 GPU worker
串行完成（见 7.1、11）。

### 4.2 RenderSession

`RenderSession` 表示一个浏览器会话：

```text
RenderSession
├── sessionId
├── modelId
├── GPUScene reference
├── CameraState
├── RenderSettings
├── cameraGeneration
├── active RenderJob
└── last preview metadata
```

相机和任何影响辐射结果的参数变化，都必须清空 progressive accumulation，包括：

- 相机位置。
- 相机方向。
- 相机 up 向量。
- FOV。
- focus 和 coc。
- 分辨率。
- 最大路径深度。
- 天空、太阳、发光参数。
- 材质和纹理。
- 随机种子策略。
- NEE K 值（RENDER_NEE_K）。
- 自适应采样参数（adaptiveMin/adaptiveCheck/adaptiveThresh）。

这条列表的本质：任何"每个 sample 的积分器质量参数"必须在累积生命周期
内锁定。中途把 NEE K=1 和 K=2 的估计平均进同一累积，等于混样两个不同
积分器，结果有偏——参数变化宁可清空重来。

Session 不拥有任何 CUDA 资源：`modelId` 指向注册表中的常驻 `GPUScene`
（见 4.1），session 只保存引用。

Session 生命周期：

```text
loading -> ready -> expired
```

`loading` 表示场景正在 GPU worker 上构建（Bistro 需要数分钟）。此状态
下的 render 请求应排队或返回忙，不得触发第二次加载。

### 4.3 RenderJob

建议状态：

```text
queued -> running -> canceling -> completed
                         |
                         +-> failed
```

任务至少需要保存：

- `renderId`
- `sessionId`
- `cameraGeneration`
- 状态
- 目标分辨率
- sample budget
- 当前 sample 数
- 平均实际 SPP
- 开始时间和结束时间
- 错误信息
- cancel 标记
- 最新 preview sequence

Job 的一次执行只覆盖 K 个 sample（例如 8-32 个），而不是拿满 sample
budget 一口气跑完：

```text
相机/参数不变：
  session 的累积缓冲持续存在
  -> worker 反复调度新的 chunk job 续染，直到 sample budget
相机/参数变化：
  清空累积缓冲 -> 从 sample 0 重新开始
```

理由：

- 单 worker 串行调度下，长 job 在整个预算时间内独占 GPU，其他 session
  的相机更新只能等取消边界。
- job 粒度改为 chunk 后，多 session 公平调度只是 worker 的选取策略
  （round-robin），不需要新的抢占机制。
- chunk 边界天然是发布 preview、检查 cancel、上报 progress 的位置。

`renderId` 必须进程内唯一（单调计数器），不要用 `"r-" + generation`：
同一 generation 会发生多次续染 chunk，浏览器靠 `renderId + generation +
sequence` 丢弃旧帧，撞号会破坏这条规则。

同一 session 只允许运行一个任务。新的相机 generation 到达后：

1. 标记旧任务为 `canceling`。
2. 在 CUDA 安全边界停止旧任务。
3. 确认 GPU kernel 已完成。
4. 保留 GPUScene，不释放或重新上传场景。
5. 清空累积缓冲。
6. 启动最新 generation 的任务。

## 5. GPU 渲染生命周期

### 5.1 常驻资源

下列资源不应随相机变化重新分配：

- 场景几何。
- BVH。
- 材质。
- 纹理。
- 发光三角形。
- 光源采样表。
- Skybox 和 Sky CDF。

### 5.2 可复用 RenderBuffers

按分辨率分配并复用：

- Path state。
- Hit records。
- Live ray index。
- Compaction buffers。
- CUB temporary storage。
- Per-pixel accumulation。
- AOV buffers。
- Preview staging buffer。
- Pinned host buffer。

分辨率改变时重新分配；仅相机改变时只清空 accumulation，不重新上传场景。

这些缓冲由调用方（session 的渲染上下文）持有，不能藏在渲染函数内部。
当前 `render_wavefront.cu` 把 `WavefrontBuffers` 做成函数级 `static`：
单 worker 串行时行为正确，但它是进程全局的，两个 session 交替渲染会把
A 的累积混进 B 的画面。阶段三动工前必须改为调用方持有、以参数传入；
GPU kernel 不需要改动。

累积缓冲语义：

- 跨 chunk 持久，只有 4.2 列表中的参数变化或分辨率变化才 reset。
- 自适应采样的 converged 标记和统计存放在累积缓冲中，随缓冲持久；
  `avgSpp` 按累积口径续算，不按单次调用。
- 单 session 缓冲峰值由分辨率上限决定（1080p 约 1.5 GB），是第 12 节
  显存配额的主要输入。

### 5.3 推荐后端

第一版优先使用 wavefront 后端：

- sample 循环在 host 端。
- 每个 bounce 有独立 kernel。
- 可以在 sample 或 bounce 边界检查取消。
- 更适合定期发布 progressive preview。

当前 megakernel 可以继续作为离线最终渲染后端，但单个 kernel 执行时间较长，不适合第一版的快速取消。

后续如果需要使用 megakernel，需要将大 kernel 拆成较短的批次，否则相机更新只能等当前 kernel 完成后生效。

### 5.4 渐进累积的确定性规则

wavefront 的每 sample 种子是纯函数：`useed = baseSeed + s * 0xABCD`，
随机序列只依赖（像素、sample 序号、baseSeed），与调用历史无关。由此
得到一个强性质，可以直接当验收标准：

> 同一参数下，"分 N 个 chunk 续染到目标 SPP"与"一次性渲染到同一 SPP"
> 的输出字节级一致。现有图像 diff 工具可直接用于渐进渲染验收。

保住这个性质需要遵守：

- 累积只在 sample 边界有效。chunk 在 sample 中途被取消时，该 sample
  作废，恢复后从原 sample 序号重跑；wfAccumKernel 未执行，累积缓冲
  不受影响。
- 预览导出不改变累积域：累积始终是线性 HDR 均值，tone mapping 只发生
  在导出/编码阶段（见 10.4），不回写累积缓冲。
- 每 sample 质量参数（NEE K、深度、自适应参数、种子策略）在一个累积
  生命周期内锁定，变化即 reset（见 4.2）。

## 6. 相机协议与校验

相机请求示例：

```json
{
  "generation": 17,
  "position": [-0.833, 3.099, -1.471],
  "direction": [0.994, -0.105, -0.036],
  "up": [0.105, 0.994, -0.004],
  "fov": 40.5,
  "focus": 0.0,
  "coc": 0.0
}
```

服务端必须校验：

- 向量分量都是有限数。
- `direction` 长度大于最小阈值。
- `up` 长度大于最小阈值。
- `direction` 和 `up` 不近似平行。
- FOV 在合理范围内，例如 `1..150`。
- 相机位置在允许场景范围或安全距离内。
- generation 不得倒退覆盖更新的相机状态。

服务端应归一化方向和 up，并重新构造正交相机基向量。

## 7. HTTP API

建议所有接口使用 `/api` 前缀。

### 7.1 创建会话

```text
POST /api/sessions
```

请求：

```json
{
  "model": "bistro-interior",
  "preset": "interior"
}
```

响应：

```json
{
  "sessionId": "s-123",
  "model": "bistro-interior",
  "state": "loading",
  "capabilities": {
    "maxWidth": 1920,
    "maxHeight": 1080,
    "wavefront": true,
    "aov": false
  }
}
```

创建 session 不等于场景就绪。模型打包和 GPU 上传（Bistro 需要数分钟）
由 GPU worker 执行，HTTP 线程只登记 session 并立即返回：

```text
POST /api/sessions -> state = loading
GPU worker 完成加载 -> WebSocket session-ready -> state = ready
```

HTTP 请求线程内禁止模型加载、`cudaMalloc`、纹理上传等任何 CUDA 操作。
当前骨架的 `createSession` 在 HTTP 线程同步 `GPUScene::load`，绑定
Bistro 前必须改掉。模型已在注册表常驻时，`state` 直接是 `ready`。

### 7.2 更新相机

```text
POST /api/sessions/{sessionId}/camera
```

请求包含相机参数和 generation。服务端收到后应合并快速连续到达的更新，只保留最新 generation。

响应：

```json
{
  "accepted": true,
  "generation": 17,
  "renderId": "r-456"
}
```

### 7.3 启动或继续渲染

```text
POST /api/sessions/{sessionId}/render
```

请求：

```json
{
  "cameraGeneration": 17,
  "width": 640,
  "height": 360,
  "sppBudget": 512,
  "maxDepth": 8,
  "backend": "wavefront",
  "progressive": true,
  "resetAccumulation": true,
  "seed": 4660
}
```

### 7.4 查询任务

```text
GET /api/renders/{renderId}
```

响应：

```json
{
  "renderId": "r-456",
  "sessionId": "s-123",
  "generation": 17,
  "state": "running",
  "sample": 96,
  "sampleBudget": 512,
  "avgSpp": 91.4,
  "previewSequence": 12,
  "previewUrl": "/api/renders/r-456/preview?seq=12"
}
```

### 7.5 取消任务

```text
POST /api/renders/{renderId}/cancel
```

取消操作应幂等。已完成或已取消的任务重复 cancel 不应导致错误状态转换。

### 7.6 获取图像

```text
GET /api/renders/{renderId}/preview
GET /api/renders/{renderId}/image
```

建议通过 HTTP 二进制返回 PNG、JPEG 或 WebP，不要把大图编码为 JSON/base64。

## 8. WebSocket 事件

```text
GET /api/sessions/{sessionId}/events
```

事件示例：

```json
{
  "type": "progress",
  "renderId": "r-456",
  "generation": 17,
  "sequence": 12,
  "sample": 96,
  "sampleBudget": 512,
  "avgSpp": 91.4,
  "previewUrl": "/api/renders/r-456/preview?seq=12",
  "width": 640,
  "height": 360
}
```

其他事件类型：

```text
session-ready
render-started
preview-ready
render-completed
render-canceled
render-failed
server-error
```

浏览器必须根据以下字段丢弃旧帧：

- `renderId`
- `generation`
- `sequence`

WebSocket 只传事件和元数据，图像本身通过 HTTP 下载，便于缓存、断线重连和 CDN/反向代理处理。

连接管理要求：

- 握手校验路径与会话：仅 `/api/sessions/{sessionId}/events` 且 session
  存在时才升级。当前骨架对任何带 `Upgrade: websocket` 头的请求都升级、
  不解析路径，需要收口。
- 升级成功后把连接注册进对应 session；断开或 session 过期时注销。
- GPU worker 是事件的唯一生产者：progress、preview-ready 等事件写入
  per-session 有界队列，WS 连接线程只消费发送，worker 不直接操作
  socket。
- 队列有界：慢消费者只保留最新 preview 事件，丢弃积压，不阻塞 worker。

## 9. 浏览器端交互策略

现有 [threejs/js/main.js](threejs/js/main.js) 已经使用 `OrbitControls`，可以复用鼠标和触控板输入逻辑。

浏览器端需要维护：

```text
sessionId
renderId
cameraGeneration
lastPreviewSequence
pendingCameraUpdate
```

### 拖动期间

- 监听 `controls.change`。
- 将相机位置、方向、up 和 FOV转换为请求。
- 以约 10 到 15 Hz throttle 请求。
- 服务器端只保留最新 generation。
- 使用 320×180 或 640×360。
- `sppBudget=1..4`。
- `maxDepth=2..4`。
- `RENDER_NEE_K=1`。

### 停止拖动后

- debounce 约 250 到 500 ms。
- 提交 640×360 或 1280×720 的 progressive render。
- 逐步增加 sample budget。
- 必要时在最终任务中开启更高质量的 NEE、降噪和 AOV。

远程渲染的体验目标应是低延迟预览，而不是让服务器为每个鼠标事件完成一张完整高质量图像。

## 10. 网络与图像传输

### 10.1 传输格式

预览帧建议使用：

- JPEG：兼容性好，适合快速预览。
- WebP：通常体积更小，适合现代浏览器。
- PNG：只用于需要无损的最终结果。
- EXR：只用于专业后处理，不作为普通浏览器预览格式。

640×360 预览应尽量控制在几十到几百 KB，具体取决于图像内容和质量参数。

### 10.2 服务器端缓存

服务端保留：

- 最近一张 preview。
- 当前 render 的最终图像。
- 最近的 frame sequence。

浏览器重连后先请求当前 preview，再重新建立 WebSocket。

### 10.3 延迟组成

需要分别测量：

```text
鼠标事件
 -> 浏览器 throttle
 -> 网络 RTT
 -> 服务器排队
 -> GPU 首个 sample
 -> 回读与编码
 -> HTTP 下载
 -> 浏览器解码与显示
```

重点指标：

- 相机更新到首帧预览的延迟。
- 拖动期间稳定 FPS。
- 停止操作后画面收敛时间。
- 平均预览帧大小。
- 网络 RTT 和丢包后的恢复时间。

### 10.4 预览与最终图的色彩管线

累积器中是线性 HDR 均值，浏览器 `<img>` 无法直接显示，编码前必须在
服务器端完成 tone mapping。这一层决定预览与最终图的观感差异：

- 服务进程复用 CLI 的同一条后处理代码路径（exposure、sRGB、bloom、
  grade），不在 server 里重写第二套。当前 `render_server` 目标没有
  链接 `image.cpp`，后处理链不在服务进程里，预览导出前必须补上。
- Bistro 内景的标定参数（发光 ×1000、exposure 1.1、sRGB EOTF、bloom）
  是交付观感的组成部分；预览跳过 bloom 会与最终图明显不一致。
- 推荐策略：拖动预览只做 exposure + sRGB，渐进精修与最终图走完整
  管线；两档差异在验收时明示。
- tone mapping 只作用于导出副本，不回写累积缓冲（保持线性域续染，
  见 5.4）。

## 11. 取消与并发模型

第一版建议：

```text
一个 GPU
  -> 一个 RendererService
      -> 一个 GPU worker thread
          -> 多个 session 排队
          -> 每个 session 最多一个 active job
```

HTTP 线程不能直接调用 `cudaFree`、销毁 texture object 或释放正在使用的 GPU buffer。

同一边界也适用于加载：模型打包、`cudaMalloc`、纹理上传只发生在 GPU
worker 线程（见 7.1），HTTP 线程登记的 session 停留在 `loading`。

chunk 化的 job（见 4.3）让 worker 调度循环变得简单：

```text
循环：
  从有 pending 工作的 session 中按 round-robin 取一个 chunk
  执行 K 个 sample（sample 边界响应 cancel）
  发布 progress / preview
  继续
```

session 销毁时：cancel 活跃 chunk -> worker 确认完成 -> 注销 WS 连接
-> 移除 session 对场景的引用。场景本身常驻注册表，不随 session 释放。

取消流程：

1. HTTP 线程设置 job 的 cancel 请求。
2. GPU worker 在 sample/bounce 边界读取 cancel 状态。
3. 当前 kernel 正常完成。
4. worker 更新 job 状态为 `canceled`。
5. 清空或保留合适的 accumulation。
6. 启动最新 generation。

GPU 场景资源只有在 session 销毁且 GPU worker 确认没有使用时才能释放。

## 12. 远程部署

### 开发环境

渲染服务默认监听：

```text
127.0.0.1:9000
```

通过 SSH tunnel 访问：

```bash
ssh -N -L 9000:127.0.0.1:9000 user@remote-server
```

浏览器访问本地：

```text
http://127.0.0.1:9000
```

### 生产环境

推荐：

```text
Browser
  -> HTTPS/WSS reverse proxy
  -> Render service on 127.0.0.1
  -> NVIDIA GPU
```

反向代理负责：

- TLS。
- WebSocket upgrade。
- 请求体大小限制。
- 连接超时。
- 基础访问日志。

渲染服务负责：

- session token。
- Origin 校验。
- 模型白名单。
- 分辨率/SPP/depth 上限。
- 任务超时。
- 速率限制。
- 显存和任务配额。

## 13. 安全要求

必须满足：

- 默认只监听回环地址。
- 不允许客户端传入任意模型路径。
- 只允许访问预配置模型 ID。
- 限制最大宽、高、像素数、SPP 和最大深度。
- 校验 JSON 数字，拒绝 NaN、Infinity 和异常大数。
- 对 WebSocket 连接设置空闲超时和最大消息大小。
- 校验 `Origin` 和访问 token。
- 对创建 session、更新相机和启动 render 做速率限制。
- 清理过期 session 和临时图像。
- 服务器不向客户端返回内部路径、CUDA 指针或诊断堆栈。

## 14. CMake 与目录建议

建议新增独立服务目标，不把 HTTP 逻辑塞入 `Console`：

```text
src/server/
├── render_server_main.cpp
├── http_server.cpp
├── websocket_session.cpp
├── session_manager.cpp
├── render_scheduler.cpp
├── render_session.cpp
├── render_job.cpp
├── gpu_scene.cpp
├── render_buffers.cpp
└── protocol.cpp
```

构建目标：

```text
Raypr3sso      # 保留现有 CLI
render_server     # 远程服务进程
```

HTTP/WebSocket 依赖可以选择提供两者的成熟 C++ 库，例如 Boost.Beast，或 standalone Asio 配合 Beast。服务层应保持与 CUDA 核心解耦。

## 15. 分阶段实施

### 阶段一：GPU 场景资源所有权

- 新建 `GPUScene`。
- 新增 `SceneRegistry`：场景按 modelId 进程内唯一、常驻、跨 session
  共享，session 只持引用（见 4.1；当前 `SessionInfo` 的 per-session
  持有需要改）。
- 模型加载移到 GPU worker：session 创建只登记，加载完成后经 WebSocket
  发 `session-ready`（见 7.1）。
- 把 `console.cpp` 中的模型打包和上传逻辑移入 scene builder。
- 补齐 sky texture、Sky CDF、texture buffer 的释放。
- 使用 RAII 处理失败路径。
- 保持 CLI 输出和图像不变。

验收：同一模型连续两次改变相机，第二次不再重复上传几何和纹理；两个
session 共享同一份场景，第二个 session 创建不触发重新加载。

### 阶段二：RenderBuffers 和 RenderJob

- 把 wavefront 的临时 buffer 从函数级 `static` 改为调用方持有的对象，
  以参数传入渲染函数，按 session 复用（见 5.2；kernel 不动）。
- 增加 job 状态、generation 和 cancel 标记。
- `renderId` 改为进程内唯一计数器（见 4.3）。
- 在 sample/bounce 边界支持取消。
- 增加 preview 导出点（导出线性均值副本，编码在后处理/编码线程做）。

验收：快速连续相机更新时，旧任务能够安全取消，GPU 不崩溃，显存不持续
增长；两个 session 交替渲染互不串扰。

### 阶段三：Progressive accumulation

- 累积缓冲与自适应 converged 状态归属 session，跨 chunk 持久（见 5.2）。
- job 改为 K-sample 分块，worker round-robin 调度（见 4.3、11）。
- 相机/参数变化 reset，固定相机续染（确定性规则见 5.4）。
- 每 chunk 边界导出预览，走 10.4 的色彩管线。
- 增加 sample、avgSpp 和 sequence 统计（累积口径）。

验收：固定相机时图像逐步变清晰，改变相机后不会混入旧视角内容；分块
续染与一次性渲染字节级一致（现有 diff 工具验收）。

### 阶段四：HTTP/WebSocket 服务

- 创建独立 `render_server`。
- 实现 session、camera、render、cancel、status、image API。
- 加入单 GPU scheduler。
- WebSocket 推送进度和 preview-ready。

验收：浏览器可以创建 session、提交相机、收到 preview 和完成事件。

### 阶段五：浏览器端接入

- 复用 `OrbitControls`。
- 发送相机状态。
- 显示服务器 preview。
- 实现 throttle、debounce、generation 和旧帧丢弃。
- 增加网络断线重连。

验收：拖动视角和滚轮缩放可以稳定更新服务器画面。

### 阶段六：远程部署与安全

- 增加 token 和 Origin 校验。
- 增加参数上限和任务配额。
- 配置 HTTPS/WSS 反向代理。
- 配置 systemd 或 Docker。
- 使用 SSH tunnel 完成开发环境验证。

验收：未授权请求、超限任务、断线、重复取消和过期 session 都能正确处理。

## 16. 验证计划

### 图像一致性

1. 运行现有 CLI interior/counter GPU 渲染。
2. 保存基准图像和主要 AOV。
3. 重构 GPUScene 后重新渲染。
4. 使用现有图像 diff 脚本比较结果。

### 资源常驻

1. 加载模型一次。
2. 连续提交多个相机。
3. 检查日志确认纹理、BVH 和材质只上传一次。
4. 观察显存是否稳定。

### 取消与 generation

1. 连续发送 20 次相机更新。
2. 确认只保留最新 generation。
3. 确认旧帧不会覆盖新帧。
4. 确认 cancel 后 CUDA 资源仍然有效。

### 网络与体验

记录：

- 首帧预览延迟。
- 拖动期间 FPS。
- 预览帧大小。
- 网络 RTT。
- 停止操作后的收敛时间。
- GPU 利用率和显存。
- session 数量增加时的排队时间。

### 安全

测试：

- 无 token 请求。
- 错误 token。
- 未授权 Origin。
- 非法模型 ID。
- 任意路径输入。
- 超大分辨率。
- 超大 spp。
- 负数或 NaN 相机参数。
- 重复 cancel。
- WebSocket 断线重连。

## 17. 推荐的第一版默认参数

| 场景 | 分辨率 | SPP | 最大深度 | NEE K | 用途 |
|---|---:|---:|---:|---:|---|
| 拖动预览 | 320×180 | 1-2 | 2-3 | 1 | 低延迟交互 |
| 普通预览 | 640×360 | 4-16 | 4-6 | 1 | 停止拖动后的快速清晰化 |
| 渐进精修 | 640×360 | 128-512 | 8-12 | 1-2 | 中等质量远程显示 |
| 最终图像 | 1280×720 或更高 | 按需 | 按需 | 2-4 | 高质量输出 |

这些参数需要在远程服务器上根据 GPU 型号、网络 RTT、模型复杂度和带宽重新标定。
NEE K 已是 `/render` 的请求参数（`neeK`，1..8，变化即重置累积，见 4.2/19）。

## 18. 关键设计决策

- 选择远程服务器控制 CUDA，而不是立即重写为 WebGPU。
- GPU 场景常驻，避免相机变化时重复上传大模型。
- 第一版独立服务进程，不复用交互式 Console。
- 第一版使用单 GPU 串行调度。
- 优先 wavefront，以获得更细的取消和 progressive 控制边界。
- 浏览器只负责相机输入、任务控制和图像显示。
- 使用低质量预览加停止后的渐进精修，而不是追求每个鼠标事件都完成完整高质量渲染。
- 场景按 modelId 进程内唯一、常驻共享；session 只持引用，绝不 per-session 加载。
- job = K-sample 分块，累积状态归 session；多 session 公平性来自分块轮转，不引入抢占机制。
- 模型加载与所有 CUDA 操作只发生在 GPU worker 线程，HTTP 线程零 CUDA。
- 渐进续染遵守 sample 边界 + 纯函数种子的确定性规则（5.4），分块与一次性渲染字节级一致。
- 预览编码复用 CLI 后处理代码路径，tone mapping 只在导出阶段，累积保持线性域。

## 19. 当前实现状态

仓库中已加入独立的 `render_server` 进程，默认监听 `127.0.0.1:9000`，可通过命令行覆盖地址和端口：

```bash
./build/render_server 127.0.0.1 9000
```

当前已实现（比早期记录更进一步）：

- `GET /api/health`、`POST /api/sessions`、`GET /api/sessions/{id}`、`POST /api/sessions/{id}/cancel`
- `POST /api/sessions/{id}/camera`：position/direction/up/fov/focus/coc 校验、归一化，generation 递增并请求取消旧 active job
- `POST /api/sessions/{id}/render`：已真实提交 wavefront 可取消任务（`renderGPUWavefrontCancelable`），含分辨率/SPP/深度上限校验。**2026-09-29 起为分块渐进链**：请求支持 `chunkSpp`、`resetAccumulation`、`seed`；worker 按 chunk 续提交直到预算，`renderId` 为进程内唯一计数器；`GET /api/sessions/{id}` 暴露 `accumSamples`/`accumGeneration`。设置 `RENDER_SERVER_DUMP_DIR` 可导出原始 radiance 通道用于字节级 A/B（`scripts/run_server_chunk_ab.sh`）
- `WavefrontRenderContext`（阶段三前置 #3/#4 已落地）：wavefront 累积缓冲与绝对 sample 位置由调用方持有（session/console model 各一份），chunk 状态机含"中断 sample 不累积"规则；已验收 one-shot 与 8×8 分块五通道字节一致 + 链中途取消安全；CLI 一次性渲染字节级回归通过
- **场景注册表与异步加载（阶段三前置 #1/#2 已落地，2026-09-29）**：`POST /api/sessions` 接 `{"model","preset"}`，模型白名单 `sphere-test`/`bistro-interior`，未知模型 400、未知 preset 400；session 只持 modelId，GPU worker 加载常驻 `SceneEntry`（loading→ready/failed，失败自动重试），加载中渲染 409 `scene_not_ready`；两个 session 共享同一份 Bistro 场景、日志确认只加载一次；preset 相机来自共享表 `scene_presets.h`（console `preset` 命令同源）；服务端与 CLI 的 Bistro 内景 AOVSTAT 逐项一致（`Dd=0.0077 Id=0.1497 Em=0.0663`，`GPUScene::load` 与 console 手工管线等价得到实证），Bistro 上分块 A/B 字节一致（`scripts/run_server_bistro_check.sh`）
- **预览导出与色彩管线（阶段三收尾 + #6 已落地，2026-09-29）**：GPU worker 在每个有产出的 chunk 边界把当前累积编码为预览 PNG——直接构建 `Photo` 并调用与 CLI 相同的 `postProcess(Full|DoBloom)`（不设 radiance 指针即自然跳过降噪，bloom/tonemap/gamma/despeckle 与 CLI 同源）；`GET /api/sessions/{id}/preview` 二进制返回 PNG，session 状态暴露 `previewSequence/previewSpp/宽高`，`/render` 支持 display-only 的 `exposure`（不重置累积）。验收：球体 256spp/8spp 分块发布 32 个 sequence，噪声梯度 10.79→5.21 单调收敛；Bistro 内景服务端预览与 CLI `RENDER_DENOISE=0` 输出 **sha256 逐字节一致**（`scripts/run_server_preview_check.sh`）
- **事件通路与 /api/renders 路由（阶段四 + #5 已落地，2026-09-29）**：WebSocket 握手校验路径（仅 `/api/sessions/{id}/events`，其余 404），连接注册进 session 的 per-connection 有界事件队列（容量 64，慢消费者丢旧帧不阻塞 worker，GPU worker 是唯一生产者、不碰 socket）；事件 `session-ready`（连接快照 + 加载完成扇出）/`render-started`/`progress`/`preview-ready`（含 renderId/generation/sequence/previewUrl）/`render-completed`/`render-canceled`/`render-failed`/`server-error`；客户端帧解析（Ping→Pong、Close、1MB 上限）+ 200ms recv 超时。`/api/renders/{id}` 状态（sample/sampleBudget/avgSpp/previewUrl）、`/cancel`（幂等：终态任务重复取消无状态破坏）、`/preview`、`/image`（链完成时的最终图）。验收：python stdlib WS 客户端收全 18 事件链（sample 8..64 单调）、错误路径 404、幂等取消、链中途取消停在 120/4096、重连快照（`scripts/run_server_events_check.py`）。CORS 预检头（Allow-Methods/Allow-Headers）已补——无头 Chrome E2E 实测跨源 POST 需要
- **浏览器端接入（阶段五已落地，2026-09-29）**：`threejs/js/main.js` 的 `createRemoteClient` 重写为两档状态机——`start..end` 交互区间内的 change 走拖动档（320×180×2spp，throttle 120ms），`end`（拖动/滚轮释放）后 400ms 触发精修链（640×360×128spp×8spp 分块，被 in-flight 跳过时自动重试）；显示由 WS 事件驱动（preview-ready/progress），旧帧按 renderId+generation+sequence 丢弃；断线自动重连（指数退避，session 丢失则重建）；`?remote=&model=&dragMs=&idleMs=` 参数化。注意：damping 会使 change 事件在输入停止后长尾持续，debounce 必须挂在 'end' 上而非 change 静默——首轮 E2E 实测踩坑
- **部署与安全（阶段六已落地，2026-09-30）**：token 认证（启动生成随机 token 打印 stderr 或 `RENDER_TOKEN` 指定；`Authorization: Bearer` 头 + `?token=` query 双通道，WS/`<img>` 走 query；`/api/health` 开放）；WS Origin 校验（回环/`null`/`RENDER_ORIGINS` 白名单外 403）；速率限制（建 session 10/分、相机 30/秒、渲染 20/秒，超限 429）+ session 上限 16；session TTL（`RENDER_SESSION_TTL` 默认 900s）过期回收——GC 线程标记+cancel，CUDA 资源在 GPU worker 上释放（队列串行无竞争）；渲染链超时（`RENDER_RENDER_TIMEOUT` 默认 600s）由 GC 监视器 cancel、在 sample 边界中止后记 failed；renders 记录 >64 淘汰终态项；浏览器客户端 token 全链路（fetch/WS/图片 URL）。部署件：`deploy/render_server.service`（systemd，非特权+ProtectSystem）、`deploy/nginx-render.conf`（TLS+WS upgrade+body 限制）、`deploy/Dockerfile`、`deploy/README.md`（含安全核对单）。验收：`scripts/run_server_security_check.py` 全绿——401/403/429、4096-spp 链 6s 超时于 sample 885 中止、TTL 过期后 session/renders/WS 全 404 且配额回收、重复取消幂等；带 token 回归：chunk A/B 字节一致、18 事件链、Bistro 预览 sha256 与 CLI 一致、CDP E2E naturalWidth 实证 overlay 真实加载（拖动帧 320 / 精修帧 640）
- **路由 query 潜伏 bug（阶段六修复）**：路由此前不处理 `?query`，事件里的 previewUrl（`?seq=`）一直 404——阶段四/五的"overlay 已显示"证据曾被本地场景亮度变化污染；修复后 E2E 以 `naturalWidth > 0` 断言图片真实加载
- **浏览器实机反馈修复（2026-09-30）**：(1) 远程模式下页面不再构建/渲染本地雨夜街景（`remoteMode` 跳过场景/光照/动效/目标点钳制），画布只做输入捕获；(2) session-ready 后**从服务端反向同步标定相机**到本地轨道——此前页面把雨夜街景的默认机位 POST 给服务端，覆盖了 Bistro 内景机位导致相机在墙外、画面全黑；(3) **太阳 env 组装提取为共享 `applySunFromEnv`**（RENDER_SUN_INT/AZ/EL/TINT/SOFT + SKY_FOCUS_*）——服务端 /render 此前从未设置 hasSun，内景缺主直接光（Dd 假暗 20×）；修复后带交付 env（az=90 el=12 int=85 fixture=2.0）的服务端渲染 AOVSTAT Dd=0.1493 与 CLI 基准吻合，预览亮度 mean 67/255（此前近全黑）；(4) 页面新增 `&exposure=` 参数（缺省跟随 preset，内景建议 1.75 匹配交付观感）
- **预览降噪（实机反馈第二轮修复，2026-10-01）**：无降噪的低 spp 预览被火焰噪声毁掉只剩高光轮廓——精修链（`/render` 的 `denoise:1`，前端 IDLE 档自动携带）的预览编码接入 **CLI 同源 NL-means 降噪器**：`encodePreview` 按 console 同样方式填充 radiance 分量+方差+G-buffer（PerPixelData 现成携带全部输入），`postProcess` 附加 `DoDenoise`；拖动档保持原始快速帧（延迟优先）。实测 128spp@640×360 降噪后内景结构完整清晰（墙壁/吧台/吊灯/桌椅全部可辨），降噪耗时百毫秒级不影响 chunk 节奏
- **NEE K 参数化（2026-09-30 落地）**：`GPURenderArgs.neeK` 取代全局设备符号 `d_neeK`（kernel 只读解析后的 args，host 入口把 0 解析为进程默认 `defaultNeeK()`，`RENDER_NEE_K`/`setNeeK` 语义不变）；`/render` 支持 `neeK`（1..8），且 NEE K 变化按 4.2 触发累积重置（`accumNeeK` 追踪）；前端两档 K=1（拖动）/K=2（精修）。验证：CLI 默认 K 字节回归通过、K=1/2 的 AOVSTAT 不同、服务端 K 变化自动重置（accum 4→4）与同 K 续染（4→8）、chunk A/B 字节一致回归通过、E2E 带两档 K 跑满 128spp
- **BC 纹理压缩验证（实现已在库，2026-09-30 实测收口）**：DDS 的 BC1/BC3/BC5 块直驻显存、kernel 内 `bcDecodeTexel` 软件解码（逐位复刻 CPU 解码舍入，含 DXT5 alpha 被 BC1 1-bit 覆盖的怪癖），`RENDER_BC_TEX` 默认开（`RENDER_BC_FMT` 掩码可选）。实测 Bistro 内景：纹理显存 **2.00 GB（213 RGBA8）→ 0.40 GB（212 BC + 1 RGBA8），省 1.6 GB/80%**；BC 开/关 AOVSTAT 与 PNG **逐字节一致**（3ed99275…，零画质代价）；服务端经共享 `GPUScene::load` 自动受益。备忘旧估算"省 3.0GB"按 4.69GB 峰值计，当前子集实测 1.6GB
- session 已绑定 `GPUScene`（当前是球体测试场景，非 Bistro）
- 单 GPU `RenderScheduler` 线程隔离，HTTP 线程不触碰渲染中的 CUDA 资源
- WebSocket `101` 握手、`session-ready` 文本事件、客户端帧消费
- HTTP 请求大小上限、本地回环冒烟测试
- 浏览器端雏形：`threejs/js/main.js` 的 `createRemoteClient`（session 创建、相机 80ms 节流、WebSocket 连接、previewUrl overlay 钩子）；浏览器尚未调用 `/render`

当前尚未完成：

- token、速率限制、Origin 校验、session 过期清理（阶段六安全项）
- 生产 HTTPS/WSS 反向代理与部署配置

在这些项目完成前，服务只适合作为本机/SSH tunnel 下的 API 骨架，不应直接暴露到公网。

### 结构修正（阶段三动工前的硬性前置）

以下是已写代码与 4.1/4.3/5.2/7.1 的偏差。不在阶段三前修正，后续代码
会建立在错误的所有权和调度语义上：

1. ✅ 场景共享（2026-09-29 完成）：`SceneEntry` 注册表按 modelId 常驻
   `shared_ptr<Model>+GPUScene`，session 只持 modelId；Bistro 全进程只
   加载一次，多 session 共享（验收见 §19"当前已实现"）。
2. ✅ 异步加载（2026-09-29 完成）：`createSession` 只登记并提交 GPU
   worker 加载任务，session 状态 loading→ready/failed，加载中渲染
   409；HTTP 线程零 CUDA。
3. ✅ 累积缓冲归属（2026-09-29 完成）：`WavefrontBuffers` 从函数级 static
   改为 `WavefrontRenderContext` 调用方持有、参数传入，累积跨 chunk 持久；
   中断 sample 不累积；one-shot vs 分块字节级一致已验收。
4. ✅ job 语义（2026-09-29 完成）：K-sample 分块 + worker 续提交链；
   `renderId` 已改为进程内唯一计数器。
5. ✅ 事件通路（2026-09-29 完成）：WebSocket 握手校验路径并注册进
   session 的 per-connection 有界队列；GPU worker 是唯一生产者，经队列
   发布全部渲染事件（见 8），慢消费者丢旧帧不阻塞。
6. ✅ 色彩管线（2026-09-29 完成）：`render_server` 已链接 `image.cpp`，
   预览编码复用 CLI 后处理路径（`Photo::postProcess` + `rgbBytes`），
   tone mapping 只在导出阶段；服务端预览与 CLI 裸输出 sha256 一致。

另：孤儿代码 `render_session.h/cpp` 已删除（逻辑由 `RenderServer` 覆盖），
陈旧的 `render_backend_not_bound` fallback 已移除。
