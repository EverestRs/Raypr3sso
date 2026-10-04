#include "render_server.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>

namespace {
RenderServer* activeServer = nullptr;
void handleSignal(int) {
    if (activeServer) activeServer->stop();
}
}

int main(int argc, char** argv) {
    const char* bindAddress = argc > 1 ? argv[1] : "127.0.0.1";
    const uint16_t port = argc > 2 ? static_cast<uint16_t>(std::atoi(argv[2])) : 9000;

    // 访问 token（设计 13）：RENDER_TOKEN 覆盖；否则生成随机 token 并打印
    // 到 stderr（操作者通道，不进入任何 HTTP 响应）。
    std::string token;
    if (const char* fromEnv = std::getenv("RENDER_TOKEN")) token = fromEnv;
    if (token.empty()) {
        std::random_device rd;
        static const char* hex = "0123456789abcdef";
        for (int i = 0; i < 32; ++i) token += hex[rd() & 0xf];
    }
    std::fprintf(stderr, "auth token: %s\n", token.c_str());
    std::fflush(stderr);

    RenderServer server(bindAddress, port, token);
    activeServer = &server;
    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    return server.run();
}
