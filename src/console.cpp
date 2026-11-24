#include "console.h"
#include "model.h"
#include "render.h"
#include "render_gpu.h"
#include "gpu_scene.h"
#include "render_job.h"
#include "scene_presets.h"
#include "image.h"
#include "utils.h"
#include <unordered_map>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <sys/stat.h>
#include <cstdlib>

void Console::run() {
    std::string line;
    while (true) {
        std::cout << "> ";
        if (!std::getline(std::cin, line)) break;

        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "exit" || cmd == "quit") {
            break;

        } else if (cmd == "setenv") {
            // setenv KEY [VALUE...] — set/unset an environment variable at
            // runtime. Lets ONE loaded session run a multi-config A/B sweep
            // (wavefront on/off, adaptive on/off) without paying the 3-5 min
            // model load per configuration.
            std::string key;
            iss >> key;
            std::string value;
            std::getline(iss, value);
            if (!value.empty() && value[0] == ' ') value.erase(0, 1);
            if (key.empty()) {
                std::cout << "usage: setenv KEY VALUE\n";
            } else {
                if (value.empty()) ::unsetenv(key.c_str());
                else ::setenv(key.c_str(), value.c_str(), 1);
                std::cout << "env " << key << " = \"" << value << "\"\n";
            }

        } else if (cmd == "help") {
            std::cout << "Raypr3sso v0.1.0 Commands:\n"
                      << "  create model <id> [folder model_name sky] - Cornell box, or load FBX\n"
                      << "  create model <id> ball [diffuse|rough|glass] - Single sphere test\n"
                      << "  create args <id>                         - Create render args\n"
                      << "  preset <name> <args_id>                  - Bistro camera preset\n"
                      << "    Presets: interior, interior_quick, daylight, exterior, counter, wide, front, ball\n"
                      << "    daylight/exterior/interior also install their calibrated rig env values\n"
                      << "  setenv KEY [VALUE]                       - Set env var at runtime (A/B without reload)\n"
                      << "  render <model_id> <args_id> [cpu|gpu]   - Render scene\n"
                      << "  list models|args                        - List created objects\n"
                      << "  view model|args <id>                    - Show details\n"
                      << "  findgeom <model_id>                     - Test ray hits from various positions\n"
                      << "  exit                                    - Exit the program\n";

        } else if (cmd == "list") {
            std::string type;
            iss >> type;
            if (type == "models") {
                if (models.empty()) {
                    std::cout << "No models created.\n";
                } else {
                    for (const auto& kv : models)
                        std::cout << "  " << kv.first << " (" << kv.second->faces().size() << " faces)\n";
                }
            } else if (type == "args") {
                if (renderArgs.empty()) {
                    std::cout << "No arg sets created.\n";
                } else {
                    for (const auto& kv : renderArgs)
                        std::cout << "  " << kv.first << " (" << kv.second.width << "x" << kv.second.height << ", " << kv.second.spp << " spp)\n";
                }
            } else {
                std::cout << "Unknown type '" << type << "'. Try 'models' or 'args'.\n";
            }

        } else if (cmd == "create") {
            std::string type, id;
            iss >> type >> id;
            if (type == "model") {
                if (models.find(id) != models.end()) {
                    std::cout << "Model '" << id << "' already exists.\n";
                } else {
                    // Check if user provides a model folder path
                    std::string extra;
                    iss >> extra;
                    if (extra.empty() || extra == "cornell") {
                        std::cout << "Creating Cornell box test scene '" << id << "'...\n";
                        gpuScenes.erase(id);
                        wavefrontContexts.erase(id);
                        models[id] = std::make_unique<Model>(Model::createTestScene());
                        std::cout << "Done. " << models[id]->faces().size() << " triangles.\n";
                    } else if (extra == "ball") {
                        // Single sphere test scene
                        std::string ballType;
                        iss >> ballType;
                        if (ballType.empty()) ballType = "diffuse";
                        std::cout << "Creating single sphere test scene '" << id
                                  << "' (type: " << ballType << ")...\n";
                        gpuScenes.erase(id);
                        wavefrontContexts.erase(id);
                        models[id] = std::make_unique<Model>(Model::createSphereTestScene(ballType));
                        std::cout << "Done. " << models[id]->faces().size() << " triangles.\n";
                    } else {
                        // Load from file: create model <id> <model_folder> <model_name> <skyMap>
                        std::string model_folder = extra;
                        std::string model_name, skyMap_name = "null";
                        iss >> model_name >> skyMap_name;
                        std::cout << "Loading model from " << model_folder << "/"
                                  << model_name << "..." << std::endl;
                        auto m = std::make_unique<Model>(model_folder, model_name, skyMap_name);
                        if (m->faces().empty()) {
                            std::cout << "Failed to load model (0 faces).\n";
                        } else {
                            gpuScenes.erase(id);
                            wavefrontContexts.erase(id);
                        wavefrontContexts.erase(id);
                            models[id] = std::move(m);
                            std::cout << "Done. " << models[id]->faces().size() << " faces, "
                                      << models[id]->lightObjects().size() << " lights.\n";
                        }
                    }
                }
            } else if (type == "args") {
                if (renderArgs.find(id) != renderArgs.end()) {
                    std::cout << "Args '" << id << "' already exists.\n";
                } else {
                    RenderArgs args;
                    std::string input;
                    auto prompt = [&](const std::string& msg, auto& val) {
                        std::cout << msg << " [" << val << "]: ";
                        std::getline(std::cin, input);
                        if (!input.empty()) {
                            std::istringstream(input) >> val;
                        }
                    };
                    prompt("Width",  args.width);
                    prompt("Height", args.height);
                    prompt("Samples per pixel", args.spp);
                    prompt("Max depth", args.maxDepth);
                    prompt("Exposure", args.exposure);
                    {
                        float3 defaultPos = args.camera.position;
                        std::cout << "Camera position (x y z) ["
                                  << defaultPos.x << " " << defaultPos.y << " " << defaultPos.z << "]: ";
                        std::getline(std::cin, input);
                        if (!input.empty()) {
                            std::istringstream(input) >> args.camera.position.x
                                                    >> args.camera.position.y
                                                    >> args.camera.position.z;
                        }
                    }
                    {
                        float3 defaultDir = args.camera.direction;
                        std::cout << "Camera direction (x y z) ["
                                  << defaultDir.x << " " << defaultDir.y << " " << defaultDir.z << "]: ";
                        std::getline(std::cin, input);
                        if (!input.empty()) {
                            std::istringstream(input) >> args.camera.direction.x
                                                    >> args.camera.direction.y
                                                    >> args.camera.direction.z;
                        }
                    }
                    prompt("FOV (degrees)", args.camera.fov);
                    prompt("Output path", args.outputPath);
                    renderArgs[id] = args;
                    std::cout << "Render args '" << id << "' created.\n";
                }
            } else {
                std::cout << "Unknown type: " << type << "\n";
            }

        } else if (cmd == "render") {
            std::string modelId, argsId, backend = "cpu";
            iss >> modelId >> argsId >> backend;

            auto mit = models.find(modelId);
            if (mit == models.end()) {
                std::cout << "Model '" << modelId << "' not found.\n";
                continue;
            }
            auto ait = renderArgs.find(argsId);
            if (ait == renderArgs.end()) {
                std::cout << "Args '" << argsId << "' not found.\n";
                continue;
            }

            const Model& model = *mit->second;
            const RenderArgs& args = ait->second;

            std::cout << "Rendering '" << modelId << "' on " << backend
                      << " with " << args.spp << " spp...\n";
            Timer timer;

            // Render (CPU or GPU)
            std::vector<PerPixelData> output;
            bool useGPU = (backend == "gpu");
            bool useWavefront = false;   // RENDER_WAVEFRONT=1
            bool adaptiveSppOn = false;  // RENDER_ADAPTIVE_SPP=1
            RenderJob renderJob;
            renderJob.start();
            if (useGPU) {
#ifdef RENDERER_HAS_CUDA
                std::cout << "Starting GPU render path...\n" << std::flush;
                if (const char* wf = std::getenv("RENDER_WAVEFRONT");
                    wf && wf[0] == '1') {
                    useWavefront = true;
                }
                // --- GPU render path ---
                const int pixelCount = args.width * args.height;
                output.resize(pixelCount);
                GPURenderArgs gpuArgs{};
                auto& gpuScenePtr = gpuScenes[modelId];
                if (!gpuScenePtr)
                    gpuScenePtr = std::make_unique<GPUScene>();
                GPUScene& gpuScene = *gpuScenePtr;

                // Pack model data for GPU upload
                if (!gpuScene.ready()) {
                const auto& faces = model.faces();
                GPUScene::GeometryData geometry = GPUScene::packGeometry(model);

                // Build material pointer -> index map
                const auto& matls = model.materials();
                std::unordered_map<const Material*, int> matMap;
                for (int i = 0; i < int(matls.size()); ++i)
                    matMap[&matls[i]] = i;

                // Build GPU material buffer from model materials
                int nM = int(matls.size());

                GPUScene::TextureData textureData = GPUScene::collectTextures(model);
                auto& uniqueTextures = textureData.unique;
                auto& texIdMap = textureData.ids;
                auto& texW = textureData.widths;
                auto& texH = textureData.heights;
                auto& texC = textureData.channels;
                auto& texD = textureData.data;

                // Second pass: build the material buffer arrays.
                // Emission scale moved to per-render args
                // (applyPhysicalEnvFromEnv); the upload stays raw so the
                // same resident scene can switch profiles per view.
                const float emiScale = 1.0f;
                GPUScene::MaterialData materialData =
                    GPUScene::createMaterialData(model, textureData, emiScale);
                auto& bcF = materialData.baseColorFactors;
                auto& emF = materialData.emissionFactors;
                auto& rF = materialData.roughnessFactors;
                auto& mF = materialData.metallicFactors;
                auto& sF = materialData.specularFactors;
                auto& opF = materialData.opacities;
                auto& ioF = materialData.iors;
                auto& bcTexIds = materialData.baseColorTexIds;
                auto& nrmTexIds = materialData.normalTexIds;
                auto& rghTexIds = materialData.roughnessTexIds;
                auto& metTexIds = materialData.metallicTexIds;
                auto& emTexIds = materialData.emissionTexIds;
                auto& pSF = materialData.packedSpecFlags;
                auto& emAxis = materialData.emissionAxis;
                auto& emK = materialData.emissionLobeK;
                auto& emU = materialData.emissionLobeU;
                auto& emGamma = materialData.emissionGamma;
                auto& aco = materialData.alphaCutout;
                auto& ufu = materialData.uvFlipU;
                GPUScene::applyMaterialTuning(model, materialData);
                // Use raw baseColorFactor from FBX. GPU will sample textures on its own,
                // so baking the average into the factor would cause double-multiplication.
                // Global lamp dimmer for daytime art (RENDER_EMISSION_SCALE,
                // default 1): scales EVERY emissive material's factor and the
                // NEE light-triangle emission — the exterior sconces/globes/
                // lanterns are not covered by RENDER_FIXTURE_GAIN's interior
                // name list, and a daytime plate needs all lamps subdued
                // (official daytime: bulbs glow as small saturated dots,
                // wall sconces read OFF, no blown white rectangles).
                fprintf(stderr, "  Emission scale: x%.2f\n", emiScale);
                // Debug: verify texture IDs for first 10 materials
                for (int di = 0; di < std::min(10, nM); ++di)
                    fprintf(stderr, "  TID[%d] '%s': texId=%d nrmId=%d\n", di,
                            matls[di].name.c_str(), bcTexIds[di], nrmTexIds[di]);

                // ---- Shadow fast-path classification (per material) ----
                // A shadow ray that hits a material flagged here is provably
                // BLOCKED: the kernel can skip the full hit-info evaluation
                // (baseColor/roughness/metallic/emission fetches + powf) and
                // return "blocked" directly, bit-identically to the old path.
                // Proof: the old path blocks unless (a) alphaCutout && opacity
                // < 0.5 (pass-through) or (b) isDielectric = metallic < 0.5 &&
                // opacity < 0.99 && |eta-1| > 0.01. The flag requires
                // !alphaCutout and opacityFactor * minAlpha >= 0.99, and the
                // interpolated alpha is >= minAlpha everywhere, so opacity >=
                // 0.99 for every texel => both branches are unreachable.
                // minAlpha is measured by scanning the decoded texture (3-channel
                // sources have implicit alpha 255 — same as the upload path).
                std::vector<int> sopq =
                    GPUScene::classifyOpaqueMaterials(materialData, textureData);


                // Upload textures to GPU (convert ImageTexture* to raw data arrays)
                GPUScene::TextureUploadData textureUpload =
                    GPUScene::uploadTextures(textureData);
                unsigned long long* gpuTexObjs = textureUpload.objects;
                int* gpuTexWidths = textureUpload.widths;
                int* gpuTexHeights = textureUpload.heights;
                unsigned char* gpuRawTexData = textureUpload.rawData;
                const int nTexUploaded = textureUpload.uploaded;
                auto& texFmt = textureUpload.formats;
                auto& texByteOff = textureUpload.byteOffsets;
                auto& texOff = textureUpload.texelOffsets;

                // Build host material buffer
                GPUMaterialBuffer hostMat;
                hostMat.baseColorFactors = bcF.data();
                hostMat.roughnessFactors = rF.data();
                hostMat.metallicFactors  = mF.data();
                hostMat.specularFactors  = sF.data();
                hostMat.opacities        = opF.data();
                hostMat.iors             = ioF.data();
                hostMat.emissionFactors  = emF.data();
                hostMat.transmittingColors = nullptr;
                hostMat.baseColorTexIds  = bcTexIds.data();
                hostMat.normalTexIds     = nrmTexIds.data();
                hostMat.roughnessTexIds  = rghTexIds.data();
                hostMat.metallicTexIds   = metTexIds.data();
                hostMat.emissionTexIds   = emTexIds.data();
                hostMat.packedSpecFlags  = pSF.data();
                hostMat.emissionAxis     = emAxis.data();
                hostMat.emissionLobeK    = emK.data();
                hostMat.emissionLobeU    = emU.data();
                hostMat.emissionGamma    = emGamma.data();
                hostMat.alphaCutout      = aco.data();
                hostMat.uvFlipU          = ufu.data();
                hostMat.shadowOpaque     = sopq.data();
                // Texture texel offsets were prepared by GPUScene::uploadTextures.
                hostMat.numMaterials     = nM;
                hostMat.texObjects       = gpuTexObjs;
                hostMat.numTextures      = int(uniqueTextures.size());
                hostMat.texData          = gpuRawTexData;
                hostMat.texDataOffsets   = texOff.data();
                hostMat.texDataWidths    = gpuTexWidths;   // reuse width array
                hostMat.texDataHeights   = gpuTexHeights;  // reuse height array
                hostMat.texFormats       = texFmt.data();
                hostMat.texByteOffsets   = texByteOff.data();
                // P0 mipmap chain (device pointers from uploadTextures).
                hostMat.mipData          = textureUpload.mipRawData;
                hostMat.mipByteOffsets   = textureUpload.mipByteOffsetsGpu;
                hostMat.mipLevels        = textureUpload.mipLevelsGpu;
                hostMat.mipWidths        = textureUpload.mipWidthsGpu;
                hostMat.mipHeights       = textureUpload.mipHeightsGpu;
                hostMat.mipLevelStart    = textureUpload.mipStartGpu;

                // Collect emissive faces for GPU NEE, grouped per light object
                GPUScene::LightData lightData =
                    GPUScene::collectLights(model, materialData, emiScale);
                auto& lightTris = lightData.triangles;
                auto& triPrefix = lightData.trianglePrefix;
                auto& objFirst = lightData.objectFirst;
                auto& objArea = lightData.objectArea;
                auto& objPower = lightData.objectPower;
                auto& objCentroid = lightData.objectCentroid;

                // Flatten BVH and upload
                LinearBVHNode* flatBVH = GPUScene::flattenBVH(model);
                GPUModelData gpuData = GPUScene::uploadModel(
                    geometry,
                    flatBVH, model.bvh().numNodesTotal(), hostMat,
                    lightTris.data(), int(lightTris.size()),
                    triPrefix.data(),
                    objFirst.data(), objArea.data(), objPower.data(),
                    objCentroid.data(), int(objArea.size()));
                delete[] flatBVH;

                // Upload skybox to GPU if available
                GPUScene::SkyData skyData = GPUScene::uploadSky(model);
                cudaTextureObject_t gpuSkyTex = skyData.texture;
                if (gpuSkyTex)
                    std::cerr << "Skybox uploaded to GPU (" << skyData.width
                              << "x" << skyData.height << ")\n";
                gpuScene.attach(GPUScene::UploadedResources{
                    gpuData,
                    gpuTexObjs,
                    nTexUploaded,
                    gpuRawTexData,
                    gpuTexWidths,
                    gpuTexHeights,
                    textureUpload.mipRawData,
                    gpuSkyTex,
                    skyData.cdf,
                    skyData.cdfSum,
                    skyData.width,
                    skyData.height
                });
                }
                gpuArgs.skyCDF = gpuScene.skyCDF();
                gpuArgs.skyCDFSum = gpuScene.skyCDFSum();
                gpuArgs.skyWidth = gpuScene.skyWidth();
                gpuArgs.skyHeight = gpuScene.skyHeight();
                gpuArgs.camera = args.camera;
                gpuArgs.width = args.width;
                gpuArgs.height = args.height;
                gpuArgs.spp = args.spp;
                gpuArgs.pDirect = args.pDirect;
                gpuArgs.maxDepth = args.maxDepth;
                gpuArgs.baseSeed = 0x1234;

                // Per-render env/feature assembly — deliberately OUTSIDE the
                // !gpuScene.ready() guard so every render (including repeat
                // renders of an already-loaded scene, e.g. after `preset`
                // installs a rig or `setenv` flips an A/B switch) picks up
                // the current environment. These are cheap: env reads, a few
                // cudaMemcpyToSymbol, args fields.
                // Directional sun (art-directed window light, RENDER_SUN_*)
                // — shared assembly with the render server (applySunFromEnv).
                applySunFromEnv(gpuArgs);
                // NEE samples per rough hit (RENDER_NEE_K, default 4; lower = faster preview)
                if (const char* nk = std::getenv("RENDER_NEE_K"))
                    setNeeK(std::max(1, (int)std::strtof(nk, nullptr)));
                // Foliage shadow transmission (RENDER_LEAF_SHADOW_TINT, default
                // 1.0 = holes fully open). Real canopies leak less light; the
                // official Bistro terrace dapple sits at ~0.6 of open sun.
                if (const char* lt = std::getenv("RENDER_LEAF_SHADOW_TINT"))
                    setLeafShadowTint(fmaxf(0.0f, std::strtof(lt, nullptr)));
                // P4b: exact sRGB EOTF for baseColor textures (default pow2.2)
                if (const char* sr = std::getenv("RENDER_SRGB_EXACT"))
                    setSrgbExact(std::atoi(sr) != 0);
                // GI bounce energy scale + emissive scale
                // (RENDER_INDIRECT_SCALE / RENDER_EMISSION_SCALE) — shared
                // assembly with the render server's non-profile path.
                applyPhysicalEnvFromEnv(gpuArgs);

                // Adaptive per-pixel sampling (自适应逐像素, RENDER_ADAPTIVE_*):
                // spp becomes the budget; pixels stop once all AOV channels'
                // relative standard error < thresh. Off by default (fixed-spp
                // byte-identical path).
                if (const char* ad = std::getenv("RENDER_ADAPTIVE_SPP");
                    ad && ad[0] == '1') {
                    gpuArgs.adaptiveMin = 32;
                    if (const char* m = std::getenv("RENDER_ADAPTIVE_MIN"))
                        gpuArgs.adaptiveMin = std::max(2, (int)std::strtof(m, nullptr));
                    if (const char* t = std::getenv("RENDER_ADAPTIVE_THRESH"))
                        gpuArgs.adaptiveThresh = fmaxf(1e-4f, std::strtof(t, nullptr));
                    if (const char* c = std::getenv("RENDER_ADAPTIVE_CHECK"))
                        gpuArgs.adaptiveCheck = std::max(2, (int)std::strtof(c, nullptr));
                    adaptiveSppOn = true;
                    std::cout << "Adaptive per-pixel sampling: on (min="
                              << gpuArgs.adaptiveMin << " thresh=" << gpuArgs.adaptiveThresh
                              << " check=" << gpuArgs.adaptiveCheck << ")" << std::endl;
                }
                // Wavefront pipeline (RENDER_WAVEFRONT=1): per-bounce stage
                // kernels with optional ray reordering (RENDER_WF_SORT) and
                // material-batched shading (RENDER_WF_MATBATCH), both on by
                // default when the wavefront is active.
                if (const char* wf = std::getenv("RENDER_WAVEFRONT");
                    wf && wf[0] == '1') {
                    useWavefront = true;
                    gpuArgs.wfSortRays = 1;
                    gpuArgs.wfMatBatch = 1;
                    if (const char* s = std::getenv("RENDER_WF_SORT"))
                        gpuArgs.wfSortRays = (s[0] != '0') ? 1 : 0;
                    if (const char* b = std::getenv("RENDER_WF_MATBATCH"))
                        gpuArgs.wfMatBatch = (b[0] != '0') ? 1 : 0;
                    std::cout << "Wavefront pipeline: on (raySort="
                              << gpuArgs.wfSortRays << " matBatch="
                              << gpuArgs.wfMatBatch << ")" << std::endl;
                }
                std::cerr << "Calling renderGPU..." << std::endl;
                if (useWavefront) {
                    auto& wfCtxPtr = wavefrontContexts[modelId];
                    if (!wfCtxPtr)
                        wfCtxPtr = std::make_unique<WavefrontRenderContext>();
                    // CLI semantics: every render is a fresh accumulation.
                    wfCtxPtr->resetAccumulation();
                    renderGPUWavefrontCancelable(gpuArgs, gpuScene.model(), gpuScene.skyTexture(),
                                                 output.data(), pixelCount, *wfCtxPtr,
                                                 &renderJob.cancelFlag());
                }
                else
                    renderGPU(gpuArgs, gpuScene.model(), gpuScene.skyTexture(), output.data(), pixelCount);
                std::cerr << "renderGPU done, freeing..." << std::endl;
                renderJob.complete();
#else
                std::cerr << "GPU backend not compiled (CUDA required)\n";
                useGPU = false;
#endif
            }
            if (!useGPU) {
                renderCPU(model, args, output);
            }

            // Transfer to Photo for post-processing and save
            Photo photo;
            photo.allocate(args.width, args.height);
            const size_t pixelCount = size_t(args.width) * args.height;

            // Build radiance data arrays (for denoising) and G-buffer
            std::vector<RadianceData> rd_Dd(pixelCount), rd_Ds(pixelCount),
                                       rd_Id(pixelCount), rd_Is(pixelCount);
            std::vector<HitInfo>      Gbuffer(pixelCount);
            std::vector<float>        sunMaskVec(pixelCount);   // P1 grade matte

            for (size_t i = 0; i < pixelCount; ++i) {
                const auto& ppd = output[i];
                sunMaskVec[i] = ppd.sunMask;
                float3 finalColor = ppd.radiance_Dd + ppd.radiance_Ds
                                  + ppd.radiance_Id + ppd.radiance_Is
                                  + ppd.emission;
                photo.channels.finalColor[i] = float4(finalColor, 1.0f);
                photo.channels.directDiffuse[i]   = float4(ppd.radiance_Dd, 1.0f);
                photo.channels.directSpecular[i]  = float4(ppd.radiance_Ds, 1.0f);
                photo.channels.indirectDiffuse[i] = float4(ppd.radiance_Id, 1.0f);
                photo.channels.indirectSpecular[i] = float4(ppd.radiance_Is, 1.0f);
                photo.channels.baseColor[i] = float4(ppd.baseColor, 1.0f);
                photo.channels.emission[i]  = float4(ppd.emission, 1.0f);

                // Fill radiance data for denoising
                rd_Dd[i].radiance = ppd.radiance_Dd; rd_Dd[i].Var = ppd.var_Dd;
                rd_Ds[i].radiance = ppd.radiance_Ds; rd_Ds[i].Var = ppd.var_Ds;
                rd_Id[i].radiance = ppd.radiance_Id; rd_Id[i].Var = ppd.var_Id;
                rd_Is[i].radiance = ppd.radiance_Is; rd_Is[i].Var = ppd.var_Is;

                // Fill G-buffer for edge-aware filtering
                Gbuffer[i].surfaceNormal = ppd.normal;
                Gbuffer[i].baseColor     = ppd.baseColor;
                Gbuffer[i].position      = ppd.position;
                Gbuffer[i].shapeNormal   = ppd.shapeNormal;
                Gbuffer[i].roughness     = ppd.roughness;
                Gbuffer[i].metallic      = ppd.metallic;
                Gbuffer[i].specular      = ppd.specular;
                Gbuffer[i].opacity       = ppd.opacity;
            }

            // Bistro interior: linear exposure (no Reinhard) to preserve saturation.
            // Reference uses exposure=512; with our pipeline, 64 gives balanced results.
            // Bistro interior: Reinhard tonemap + moderate exposure + saturation boost.
            // Reinhard compresses HDR to [0,1); saturation boost compensates desaturation.
            photo.exposure = args.exposure;

            // RENDER_DUMPAOV=1: dump raw AOVs + variance + G-buffer (binary,
            // little-endian) before post-processing, for offline denoiser
            // tuning without re-rendering. Files land in <outputPath>/aov/.
            if (const char* dumpEnv = std::getenv("RENDER_DUMPAOV");
                dumpEnv && dumpEnv[0] == '1') {
                std::string aovDir = args.outputPath + "/aov";
                // mkdir() is not recursive: the parent output dir must exist
                // first (photo.save only creates it later, after this dump).
                mkdir(args.outputPath.c_str(), 0755);
                mkdir(aovDir.c_str(), 0755);
                const size_t n = size_t(args.width) * args.height;
                auto dumpF32 = [&](const std::string& name, const float* src, size_t count) {
                    std::ofstream f(aovDir + "/" + name, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(src),
                            std::streamsize(count * sizeof(float)));
                };
                std::vector<float> buf3(n * 3), buf1(n);
                auto dumpRad = [&](const char* name, const RadianceData* rd) {
                    for (size_t i = 0; i < n; ++i) {
                        buf3[i*3+0] = rd[i].radiance.x;
                        buf3[i*3+1] = rd[i].radiance.y;
                        buf3[i*3+2] = rd[i].radiance.z;
                        buf1[i]     = rd[i].Var;
                    }
                    dumpF32(std::string("rad_") + name + ".f32", buf3.data(), n * 3);
                    dumpF32(std::string("var_") + name + ".f32", buf1.data(), n);
                };
                dumpRad("Dd", rd_Dd.data());
                dumpRad("Ds", rd_Ds.data());
                dumpRad("Id", rd_Id.data());
                dumpRad("Is", rd_Is.data());
                // G-buffer: pos(3) shapeNormal(3) surfNormal(3) baseColor(3)
                //           rough metallic specular opacity = 17 floats/px
                std::vector<float> gb(n * 17);
                for (size_t i = 0; i < n; ++i) {
                    const HitInfo& g = Gbuffer[i];
                    float* o = &gb[i * 17];
                    o[0]=g.position.x; o[1]=g.position.y; o[2]=g.position.z;
                    o[3]=g.shapeNormal.x; o[4]=g.shapeNormal.y; o[5]=g.shapeNormal.z;
                    o[6]=g.surfaceNormal.x; o[7]=g.surfaceNormal.y; o[8]=g.surfaceNormal.z;
                    o[9]=g.baseColor.x; o[10]=g.baseColor.y; o[11]=g.baseColor.z;
                    o[12]=g.roughness; o[13]=g.metallic; o[14]=g.specular; o[15]=g.opacity;
                    o[16]=float(g.materialID);
                }
                dumpF32("gbuffer.f32", gb.data(), n * 17);
                // P1: direct-sun grade matte (float per pixel)
                dumpF32("sunmask.f32", sunMaskVec.data(), n);
                std::ofstream meta(aovDir + "/meta.txt");
                meta << args.width << " " << args.height << " " << args.spp << "\n";
                std::cout << "AOV dump written to " << aovDir << "\n";
            }


            // Wire up denoising data to photo
            photo.radiance_Dd = rd_Dd.data();
            photo.radiance_Ds = rd_Ds.data();
            photo.radiance_Id = rd_Id.data();
            photo.radiance_Is = rd_Is.data();
            photo.Gbuffer     = Gbuffer.data();
            // P1: first-hit direct-sun matte for the sun-keyed split-tone
            photo.sunMask     = sunMaskVec.data();

            // RENDER_ALBEDO=1: dump baseColor AOV before post-processing so we can
            // verify texture loading/UVs independently of lighting/denoiser
            if (const char* albedoEnv = std::getenv("RENDER_ALBEDO");
                albedoEnv && albedoEnv[0] == '1') {
                std::string albedoDir = args.outputPath;
                mkdir(albedoDir.c_str(), 0755);
                const size_t n = size_t(photo.width) * photo.height;
                std::vector<float4> tmp(photo.channels.finalColor, photo.channels.finalColor + n);
                for (size_t i = 0; i < n; ++i) {
                    float4 bc = photo.channels.baseColor[i];
                    float3 lc = float3(bc.x, bc.y, bc.z) * args.exposure;
                    lc = float3(powf(fmaxf(lc.x, 0.0f), 1.0f/2.2f),
                                powf(fmaxf(lc.y, 0.0f), 1.0f/2.2f),
                                powf(fmaxf(lc.z, 0.0f), 1.0f/2.2f));
                    photo.channels.finalColor[i] = float4(fminf(lc.x,1.0f), fminf(lc.y,1.0f), fminf(lc.z,1.0f), 1.0f);
                }
                photo.save(albedoDir + "/albedo.png");
                std::copy(tmp.begin(), tmp.end(), photo.channels.finalColor);
                fprintf(stderr, "ALBEDO saved: %s\n", (albedoDir + "/albedo.png").c_str());
            }

            // Post-process and save (with denoising)
            photo.warmTint = args.warmTint;
            photo.saturationBoost = args.saturationBoost;
            // WB / split-tone / grade bands / RENDER_SATURATION env override —
            // shared with the render server (see image.h).
            applyRenderGradeEnv(photo, true);
            // RENDER_DENOISE=0 disables the denoiser for A/B diagnostics
            const char* denoiseEnv = std::getenv("RENDER_DENOISE");
            bool doDenoise = !denoiseEnv || denoiseEnv[0] != '0';
            // RENDER_BLOOM=0 disables bloom (bright daylight scenes: the
            // Clum>=1 threshold fires on the sunlit street itself and the
            // multi-scale blur haze washes the whole frame out)
            bool doBloom = true;
            if (const char* bl = std::getenv("RENDER_BLOOM"))
                doBloom = !(bl[0] == '0');
            int shadeOpt = Photo::Full
                         | (doBloom ? Photo::DoBloom : 0)
                         | (doDenoise ? Photo::DoDenoise : 0);
            // With adaptive per-pixel sampling the denoiser's SPP-adaptive
            // gates key on the EFFECTIVE average sample count, not the budget.
            int effSpp = args.spp;
            if (adaptiveSppOn) {
                effSpp = std::max(1, (int)std::llround(getLastGpuAvgSpp()));
                std::cerr << "Adaptive effective spp for denoiser: " << effSpp << std::endl;
            }
            photo.postProcess(shadeOpt, effSpp);

            // RENDER_DUMPAOV=2: dump the DENOISED AOVs (post postProcess) for
            // offline verification of the denoiser itself.
            if (const char* dumpEnv2 = std::getenv("RENDER_DUMPAOV");
                dumpEnv2 && dumpEnv2[0] == '2') {
                std::string aovDir = args.outputPath + "/aov_dn";
                mkdir(args.outputPath.c_str(), 0755);
                mkdir(aovDir.c_str(), 0755);
                const size_t n = size_t(args.width) * args.height;
                auto dumpF32b = [&](const std::string& name, const float* src, size_t count) {
                    std::ofstream f(aovDir + "/" + name, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(src),
                            std::streamsize(count * sizeof(float)));
                };
                std::vector<float> b3(n * 3), b1(n);
                auto dumpRd = [&](const char* name, const RadianceData* rd) {
                    for (size_t i = 0; i < n; ++i) {
                        b3[i*3+0] = rd[i].radiance.x; b3[i*3+1] = rd[i].radiance.y; b3[i*3+2] = rd[i].radiance.z;
                        b1[i] = rd[i].Var;
                    }
                    dumpF32b(std::string("rad_") + name + ".f32", b3.data(), n * 3);
                    dumpF32b(std::string("var_") + name + ".f32", b1.data(), n);
                };
                dumpRd("Dd", rd_Dd.data()); dumpRd("Ds", rd_Ds.data());
                dumpRd("Id", rd_Id.data()); dumpRd("Is", rd_Is.data());
                for (size_t i = 0; i < n; ++i) {
                    b3[i*3+0] = photo.channels.emission[i].x;
                    b3[i*3+1] = photo.channels.emission[i].y;
                    b3[i*3+2] = photo.channels.emission[i].z;
                }
                dumpF32b("rad_Em.f32", b3.data(), n * 3);
                std::cout << "Denoised AOV dump written to " << aovDir << "\n";
            }

            // Ensure output directory exists
            std::string outDir = args.outputPath;
            mkdir(outDir.c_str(), 0755);
            // Timestamped filename: render_YYYYMMDD_HHMMSS.png (render finish time)
            std::time_t now = std::time(nullptr);
            std::tm tmNow = *std::localtime(&now);
            std::ostringstream tsName;
            tsName << "render_" << std::put_time(&tmNow, "%Y%m%d_%H%M%S") << ".png";
            std::string outPath = outDir + "/" + tsName.str();
            photo.save(outPath);

            double elapsed = timer.elapsed();
            std::cout << "Done in " << std::fixed << std::setprecision(1)
                      << elapsed << "s.\n"
                      << "Output file: " << outPath << "\n"
                      << "Full path: " << outDir << "/" << tsName.str() << "\n"
                      << "(from working dir: " << outDir << "/" << tsName.str() << ")\n"
                      << "Open " << outDir << "/" << tsName.str() << " to view.\n";

        } else if (cmd == "view") {
            std::string type, id;
            iss >> type >> id;
            if (type == "model") {
                auto it = models.find(id);
                if (it == models.end()) {
                    std::cout << "Model '" << id << "' not found.\n";
                } else {
                    const Model& m = *it->second;
                    std::cout << "Model '" << id << "':\n"
                              << "  Faces:  " << m.faces().size() << "\n"
                              << "  Lights: " << m.lightObjects().size() << "\n"
                              << "  BVH:    " << m.bvh().numNodesTotal() << " nodes\n"
                              << "  SkyBox: " << (m.skyMap().empty() ? "none" : "loaded") << "\n";
                }
            } else if (type == "args") {
                auto it = renderArgs.find(id);
                if (it == renderArgs.end()) {
                    std::cout << "Args '" << id << "' not found.\n";
                } else {
                    const RenderArgs& a = it->second;
                    std::cout << "RenderArgs '" << id << "':\n"
                              << "  Resolution: " << a.width << "x" << a.height << "\n"
                              << "  SPP:        " << a.spp << "\n"
                              << "  MaxDepth:   " << a.maxDepth << "\n"
                              << "  FOV:        " << a.camera.fov << "\n"
                              << "  Cam pos:    "
                              << a.camera.position.x << ", "
                              << a.camera.position.y << ", "
                              << a.camera.position.z << "\n"
                              << "  Output:     " << a.outputPath << "\n";
                }
            } else {
                std::cout << "Unknown type: " << type << "\n";
            }

        } else if (cmd == "preset") {
            std::string name, argsId;
            iss >> name >> argsId;
            if (argsId.empty()) {
                std::cout << "Usage: preset <name> <args_id>\n";
                continue;
            }
            RenderArgs args;
            args.spp = 1024;
            args.width = 1920;
            args.height = 1080;
            args.maxDepth = 16;
            args.pDirect = 0.5f;
            args.outputPath = "output";
            args.camera.fov = 60.0f;
            args.camera.focus = 0.0f;
            args.camera.coc = 0.0f;

            ScenePreset preset;
            if (!findScenePreset(name, preset)) {
                std::cout << "Unknown preset '" << name << "'.\n"
                          << "Available preset: interior, interior_quick, bistro, daylight, exterior,"
                          << " counter, wide, front, ball\n";
                continue;
            }
            args.camera = preset.camera;
            args.spp = preset.spp;
            args.width = preset.width;
            args.height = preset.height;
            args.exposure = preset.exposure;
            args.warmTint = preset.warmTint;
            args.saturationBoost = preset.saturationBoost;
            if (preset.maxDepth > 0) args.maxDepth = preset.maxDepth;
            renderArgs[argsId] = args;
            std::cout << "Preset '" << name << "' saved as '" << argsId << "'."
                      << " (" << preset.width << "x" << preset.height << " @ "
                      << preset.spp << " spp, exposure " << preset.exposure;
            if (preset.maxDepth > 0) std::cout << ", maxDepth " << preset.maxDepth;
            if (preset.note) std::cout << " — " << preset.note;
            std::cout << ")\n";
            // Calibrated rig: install the delivery env values as process
            // defaults so `preset` + `render` reproduces the calibrated look
            // with no external env setup. Env vars set before launch (or via
            // setenv) still override — applyPresetRig never touches those.
            std::vector<std::string> installed;
            applyPresetRig(preset, &installed);
            if (!installed.empty()) {
                std::cout << "Rig installed as process defaults"
                          << " (pre-launch env still overrides):\n";
                for (const auto& kv : installed)
                    std::cout << "  " << kv << "\n";
                if (!models.empty())
                    std::cout << "NOTE: sky/emission rig values bake at model"
                                 " load — re-run create model to pick them up.\n";
            }
            if (preset.modelFbx)
                std::cout << "Model: create model <id> models/Bistro_v5_2 "
                          << preset.modelFbx << " "
                          << (preset.skyFile ? preset.skyFile : "null") << "\n";
        } else if (cmd == "findgeom") {
            std::string modelId;
            iss >> modelId;
            auto mit = models.find(modelId);
            if (mit == models.end()) {
                std::cout << "Model '" << modelId << "' not found.\n";
                continue;
            }
            const Model& model = *mit->second;
            // Probe from origin in multiple directions to find where geometry is
            float3 dirs[] = {
                {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1},
                {1,1,0}, {-1,1,0}, {1,-1,0}, {-1,-1,0},
                {1,0,1}, {-1,0,1}, {1,0,-1}, {-1,0,-1},
                {0.577f,0.577f,0.577f}, {-0.577f,0.577f,0.577f},
                {0.577f,-0.577f,0.577f}, {-0.577f,-0.577f,0.577f}
            };
            float3 origins[] = {
                {0,0,0}, {0,2,0}, {0,-2,0},
                {100,2,0}, {-100,2,0}, {0,2,100}, {0,2,-100},
                {575,424,297}, {500,2,300}
            };
            for (auto& o : origins) {
                std::cout << "From (" << o.x << "," << o.y << "," << o.z << "):";
                int hits = 0;
                for (auto& d : dirs) {
                    Ray ray(o, normalize(d));
                    HitRecord hr = model.rayHit(ray);
                    if (hr.hasHit())
                        hits++;
                }
                std::cout << " " << hits << "/" << (int)(sizeof(dirs)/sizeof(dirs[0]))
                          << " directions hit geometry\n";
            }
        } else {
            std::cout << "Unknown command. Type 'help' for available commands.\n";
        }
    }
}
