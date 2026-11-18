#include <iostream>
#include "console.h"

int main(int argc, char* argv[]) {
    std::cout << "Raypr3sso v0.1.0 — CUDA Path Tracing Renderer" << std::endl;
    std::cout << "Type 'help' for available commands." << std::endl;

    Console console;
    console.run();

    return 0;
}
