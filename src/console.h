#pragma once

#include <map>
#include <memory>
#include <string>

#include "model.h"
#include "render.h"
#include "gpu_scene.h"
#include "render_gpu.h"

class Console {
    std::map<std::string, std::unique_ptr<Model>>     models;
    std::map<std::string, std::unique_ptr<GPUScene>>  gpuScenes;
    // Per-model wavefront pipeline state; buffers are reused across renders
    // of the same model, accumulation is reset before every CLI render.
    std::map<std::string, std::unique_ptr<WavefrontRenderContext>> wavefrontContexts;
    std::map<std::string, RenderArgs> renderArgs;
public:
    void run();  // Interactive loop

    // Commands:
    //   create model <id>
    //   create args <id>
    //   render <model_id> <args_id>
    //   view model <id>
    //   view args <id>
    //   exit
};
