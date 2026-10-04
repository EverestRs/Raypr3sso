# RenderEngine 渲染服务容器（设计 12；需要 nvidia-container-toolkit）
#
#   docker build -t renderengine-server -f deploy/Dockerfile .
#   docker run --gpus all -p 127.0.0.1:9000:9000 \
#     -e RENDER_TOKEN=<token> -v bistro-cache:/opt/renderengine/models \
#     renderengine-server
#
# 注意：模型文件（models/Bistro_v5_2）不入镜像，挂载只读卷提供。

FROM nvidia/cuda:13.0.0-devel-ubuntu22.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake g++ libassimp-dev zlib1g-dev libjpeg-dev libpng-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY src ./src
COPY include ./include
COPY external ./external
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j"$(nproc)" --target render_server

FROM nvidia/cuda:13.0.0-runtime-ubuntu22.04
RUN apt-get update && apt-get install -y --no-install-recommends \
        libassimp5 zlib1g libjpeg8 libpng16-16 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 900 render
WORKDIR /opt/renderengine
COPY --from=build /src/build/render_server ./build/render_server
COPY models ./models
USER render
EXPOSE 9000
# 只监听容器内 0.0.0.0 由 docker -p 限定宿主侧回环；TLS 交给反向代理
ENTRYPOINT ["./build/render_server", "0.0.0.0", "9000"]
