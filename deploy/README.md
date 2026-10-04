# 部署指南（设计 12）

## 开发环境（SSH tunnel）

```bash
# 远程服务器上
TOKEN=$(openssl rand -hex 16)
RENDER_TOKEN=$TOKEN ./build/render_server 127.0.0.1 9000 &

# 本地
ssh -N -L 9000:127.0.0.1:9000 user@remote-server
```

浏览器打开（threejs 目录由任意静态服务器提供）：

```text
http://127.0.0.1:8080/threejs/index.html?remote=http://127.0.0.1:9000&model=bistro-interior&token=$TOKEN
```

token 走页面 URL 参数，客户端自动附加到全部 HTTP/WS/图片请求；也可以直接用
`Authorization: Bearer <token>` 头访问 API（curl/脚本）。

## 生产环境

```text
Browser -> HTTPS/WSS (nginx, TLS) -> render_server (127.0.0.1:9000) -> GPU
```

- systemd：`deploy/render_server.service`（非特权用户、ProtectSystem、自动重启）。
- 反向代理：`deploy/nginx-render.conf`（TLS、WS upgrade、1MB body 限制、超时）。
- 容器：`deploy/Dockerfile`（nvidia-container-toolkit，模型走挂载卷）。

token 分发：`Environment=RENDER_TOKEN=`（多实例固定）或从
`journalctl -u render_server | grep 'auth token'` 读取；页面 URL 由前端门派发，
不要把 token 提交进公共仓库。

## 安全基线（设计 13）核对单

- 服务默认只监听 127.0.0.1；TLS 由代理负责。
- 全部 API（除 `/api/health`）要求 token（头或 query 双通道，WS 用 query）。
- WebSocket 校验 Origin：回环来源 / `file://`（null）/ `RENDER_ORIGINS` 白名单。
- 模型白名单（`sphere-test` / `bistro-interior`），不接受任意路径。
- 分辨率 ≤1920×1080、SPP ≤4096、深度 ≤32、请求体 ≤1MB。
- 速率限制：建 session 10/分钟、相机 30/秒、渲染 20/秒；session 上限 16。
- session TTL（默认 900s，`RENDER_SESSION_TTL`）过期后 GPU 资源在 worker 上释放；
  渲染链超时（默认 600s，`RENDER_RENDER_TIMEOUT`）在 sample 边界中止。
- 错误响应只含受控错误码，不返回内部路径、CUDA 指针或堆栈。
