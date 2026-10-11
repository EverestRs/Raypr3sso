#include "image.h"
#ifdef RENDERER_HAS_OIDN
#include <OpenImageDenoise/oidn.h>
#endif
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>
#include <thread>
#include <atomic>

// ---- Denoising constants ----
static constexpr int FILTER_PASSES = 5;  // Steps {1,2,4,8,16} like reference

static inline float luminance(const float3& c) {
    return c.x * 0.299f + c.y * 0.587f + c.z * 0.114f;
}

// Forward decls (definitions sit after postProcess).
static void filterVar(RadianceData* data, int width, int height);
static void fireflyClamp(RadianceData* data, int width, int height,
                         float gate = 6.0f);
static void despecklePass(Photo& photo, float thresh);
template <typename Fn> static void parallelForRows(int height, Fn&& fn);
static void filterDiffuseDemod(RadianceData* data, const HitInfo* Gbuffer,
                               int width, int height);
static void blendVariance(RadianceData* data, const float3* orig,
                          const float* preVar, int width, int height);

Photo::~Photo() {
    auto freeChannel = [](float4*& ptr) {
        delete[] ptr;
        ptr = nullptr;
    };
    freeChannel(channels.finalColor);
    freeChannel(channels.directDiffuse);
    freeChannel(channels.directSpecular);
    freeChannel(channels.indirectDiffuse);
    freeChannel(channels.indirectSpecular);
    freeChannel(channels.baseColor);
    freeChannel(channels.emission);
}

void Photo::allocate(int w, int h) {
    width = w;
    height = h;
    const size_t size = size_t(w) * h;

    auto allocChannel = [size](float4*& ptr) {
        ptr = new float4[size];
        std::fill(ptr, ptr + size, float4(0.0f));
    };

    allocChannel(channels.finalColor);
    allocChannel(channels.directDiffuse);
    allocChannel(channels.directSpecular);
    allocChannel(channels.indirectDiffuse);
    allocChannel(channels.indirectSpecular);
    allocChannel(channels.baseColor);
    allocChannel(channels.emission);
}

// RENDER_VERBOSE=1 restores the per-chunk full-frame diagnostic scans
// (VARDIAG/GBUF/AOVSTAT...) that the preview pipeline skips by default.
static bool renderVerbose() {
    const char* v = std::getenv("RENDER_VERBOSE");
    return v && v[0] == '1';
}

void Photo::postProcess(int shadeOptions, int spp, int denoiseQuality) {
    // Rig-aware env cache refresh: scene presets install RENDER_* values at
    // runtime (applyPresetRig); re-read the post-processing knobs cached in
    // this file before anything below uses them.
    refreshRenderEnv();
    // RENDER_SPP: offline backdoor — drive the SPP-adaptive gates from dumped
    // AOVs without re-rendering.
    int sppEff = spp;
    if (const char* e = std::getenv("RENDER_SPP")) {
        long v = std::strtol(e, nullptr, 10);
        if (v > 0) sppEff = (int)v;
    }
    // Denoising (before tone mapping, in linear HDR space)
    bool denoised = false;
    if ((shadeOptions & DoDenoise) && radiance_Dd && radiance_Ds &&
        radiance_Id && radiance_Is && Gbuffer) {
        const size_t n = size_t(width) * height;

        // ---- Robust per-image statistics (GENERALITY) ----
        // lumScale: median composite luminance. All AOVs are normalized by it
        // before filtering, so the denoiser's constants operate on a
        // canonical scale — scene brightness and light-unit choices no
        // longer change the denoising behavior.
        RadianceData* aovs[4] = { radiance_Dd, radiance_Ds, radiance_Id, radiance_Is };
        std::vector<float> lumSample;
        lumSample.reserve(n / 7 + 1);
        for (size_t i = 0; i < n; i += 7)
            lumSample.push_back(luminance(radiance_Dd[i].radiance) +
                                luminance(radiance_Ds[i].radiance) +
                                luminance(radiance_Id[i].radiance) +
                                luminance(radiance_Is[i].radiance));
        std::nth_element(lumSample.begin(), lumSample.begin() + lumSample.size() / 2, lumSample.end());
        float lumScale = fmaxf(lumSample[lumSample.size() / 2], 1e-6f);

        // Raw MC mean composite luminance — SPP-stable in the luminance
        // domain (~0.216@4spp ... 0.217@320spp on the bistro interior; the
        // AOVSTAT log prints the channel-SUM domain, ~3x larger for balanced
        // light — keep the two domains straight). The per-pixel Bernoulli
        // misses at low spp leave the MEDIAN degenerate (exactly 0 at
        // 480p@4spp, where >50% of pixels miss all lights in all 4 samples)
        // while the mean stays at the converged level.
        double rawLumSum = 0.0;
        for (size_t i = 0; i < n; ++i)
            rawLumSum += luminance(radiance_Dd[i].radiance) +
                         luminance(radiance_Ds[i].radiance) +
                         luminance(radiance_Id[i].radiance) +
                         luminance(radiance_Is[i].radiance);
        const float rawLumMean = (float)(rawLumSum / (double)n);
        // Degenerate-median fallback: if the raw median is <= rawMean/16 the
        // field is spike-dominated (low-spp regime) and the median-based
        // scale collapses the normalization (all relative gates then operate
        // on a 1e-6-scale field and the denoised output comes out black).
        // Fall back to rawMean/8: keeps the normalized field on O(1-100)
        // values (0.217/8 = 0.027 vs 1e-6), where every relative gate and
        // absolute floor behaves as tuned. At 320spp the median (0.0779) >
        // rawMean/16 (0.0136), so the delivered path is untouched.
        if (lumScale <= rawLumMean / 16.0f)
            lumScale = rawLumMean / 8.0f;

        // Relative firefly safety net: cap at 8x the AOV's own p99.9
        // luminance instead of a hard-coded absolute 100.
        auto relativeClamp = [&](RadianceData* a) {
            std::vector<float> s;
            s.reserve(n / 13 + 1);
            for (size_t i = 0; i < n; i += 13)
                s.push_back(luminance(a[i].radiance));
            std::nth_element(s.begin(), s.begin() + s.size() * 999 / 1000, s.end());
            float p999 = s[size_t(s.size() * 999 / 1000)];
            spatialClamp(a, width, height, fmaxf(20.0f, 8.0f * p999));
        };
        relativeClamp(radiance_Dd);
        relativeClamp(radiance_Ds);
        relativeClamp(radiance_Id);
        relativeClamp(radiance_Is);

        // ---- SPP-adaptive tolerance ----
        // At low spp the per-pixel signal is Bernoulli-spike dominated: most
        // pixels miss the lights entirely and the legitimate energy sits in a
        // few spikes. The 6x local-median firefly gate (tuned at 320spp) then
        // clamps REAL signal, and log-domain averaging additionally
        // underestimates heavy-tailed data (Jensen: geometric mean <<
        // arithmetic mean). Both collapse the output brightness (measured:
        // denoised median 0.467@320 -> 0.207@4, raw radMean constant).
        // (a) widen the firefly gates by k = sqrt(SPP_REF/spp) — a
        //     legitimate spike's excess over the local median grows ~1/spp;
        // (b) an energy-conserving gain at the end restores the 320spp
        //     OUTPUT level (retention-corrected, NOT the raw MC mean — the
        //     delivered 320 calibration includes the denoiser's own ~23%
        //     retention loss). At spp >= 320 the gain is exactly 1.0.
        // The relative varMed step gates and sigmaL floors are left alone:
        // Var/varMed and the normalized-luminance floors are spp-invariant.
        static constexpr float SPP_REF = 320.0f;
        float kSpp = std::sqrt(SPP_REF / fmaxf((float)sppEff, 1.0f));
        kSpp = std::min(8.0f, std::max(1.0f, kSpp));
        const float ffGate = 6.0f * kSpp;

        // (rawLumSum / rawLumMean: computed above with lumScale — it is the
        // energy-conservation reference for the gain at the end of the
        // pipeline; linear filtering preserves the mean, only the nonlinear
        // clamps and log-domain averaging shift it.)
        // Diagnostic: Var distribution before filtering (full-frame scans —
        // RENDER_VERBOSE only; off by default for the per-chunk preview path)
        if (renderVerbose()) {
            double sVar=0,sVar2=0,sRad=0; float mn=1e30f,mx=0;
            for(size_t i=0;i<size_t(width)*height;++i){
                float v=radiance_Dd[i].Var; sVar+=v; sVar2+=double(v)*v;
                float r=luminance(radiance_Dd[i].radiance); sRad+=r;
                mn=fminf(mn,v); mx=fmaxf(mx,v);
            }
            size_t n=size_t(width)*height;
            fprintf(stderr,"VARDIAG Dd: mean=%.6f std=%.6f min=%.6f max=%.6f radMean=%.3f\n",
                    sVar/n,sqrt(sVar2/n-(sVar/n)*(sVar/n)),mn,mx,sRad/n);
            size_t posValid=0;
            for(size_t i=0;i<n;++i) if(std::isfinite(Gbuffer[i].position.x)&&glm::length(Gbuffer[i].position)>1e-6f) posValid++;
            fprintf(stderr,"GBUF: posValid=%zu/%zu (%.1f%%)\n",posValid,n,100.0*posValid/n);
        }
        // ---- P1/P2: sun matte (sunliness) for the sun-keyed split-tone ----
        // Per-pixel direct-sun fraction of the illumination: blurred for
        // stability (the mask itself already carries the soft-shadow ramp),
        // normalized by the pixel's total raw luminance. 1 = sunlit,
        // 0 = shade — the warm/cool split keys on THIS, not on luma.
        if (splitKeySun && sunMask) {
            sunliness.assign(n, 0.0f);
            std::vector<float> mb(n, 0.0f);
            for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float acc = 0.0f; int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = std::min(width - 1, std::max(0, x + dx));
                    int sy = std::min(height - 1, std::max(0, y + dy));
                    acc += sunMask[size_t(sy) * width + sx];
                    ++cnt;
                }
                mb[size_t(y) * width + x] = acc / float(cnt);
            }
            double sSum = 0.0;
            for (size_t i = 0; i < n; ++i) {
                float tot = luminance(radiance_Dd[i].radiance) +
                            luminance(radiance_Ds[i].radiance) +
                            luminance(radiance_Id[i].radiance) +
                            luminance(radiance_Is[i].radiance) +
                            (channels.emission ? luminance(float3(channels.emission[i]))
                                               : 0.0f);
                sunliness[i] = std::min(1.0f, mb[i] / (tot + 1e-4f));
                sSum += sunliness[i];
            }
            fprintf(stderr, "SUNMASK: sun-keyed split-tone (%zu px, mean sunliness %.3f)\n",
                    n, n ? sSum / n : 0.0);
            fflush(stderr);
        }

        // ---- P4c: Intel Open Image Denoise (RENDER_OIDN=1) ----
        // Quality 3 (server final chunk) runs OIDN INSTEAD of the custom
        // NL-means chain (the recomposite prefers oidnColor); quality 2 (CLI
        // delivery) keeps BOTH — the calibrated path is byte-identical.
#ifdef RENDERER_HAS_OIDN
        {
            // Per-call env read (NOT cached): presets install rig env at
            // runtime, and a re-render after `preset`/`setenv` must see it.
            const bool oidnOn = [] {
                const char* e = std::getenv("RENDER_OIDN");
                return e && e[0] == '1';
            }();
            if (oidnOn && denoiseQuality >= 2 && Gbuffer) {
                std::vector<float> color(n * 3), albedo(n * 3), normal(n * 3), out(n * 3);
                for (size_t i = 0; i < n; ++i) {
                    float3 c = radiance_Dd[i].radiance + radiance_Ds[i].radiance +
                               radiance_Id[i].radiance + radiance_Is[i].radiance;
                    c += channels.emission ? float3(channels.emission[i]) : float3(0.0f);
                    color[i*3+0] = c.x; color[i*3+1] = c.y; color[i*3+2] = c.z;
                    float3 a = Gbuffer[i].baseColor;
                    albedo[i*3+0] = a.x; albedo[i*3+1] = a.y; albedo[i*3+2] = a.z;
                    float3 nn = Gbuffer[i].shapeNormal;
                    normal[i*3+0] = nn.x; normal[i*3+1] = nn.y; normal[i*3+2] = nn.z;
                }
                OIDNDevice dev = oidnNewDevice(OIDN_DEVICE_TYPE_CPU);
                oidnCommitDevice(dev);
                OIDNFilter flt = oidnNewFilter(dev, "RT");
                oidnSetSharedFilterImage(flt, "color",  color.data(),  OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
                oidnSetSharedFilterImage(flt, "albedo", albedo.data(), OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
                oidnSetSharedFilterImage(flt, "normal", normal.data(), OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
                oidnSetSharedFilterImage(flt, "output", out.data(),    OIDN_FORMAT_FLOAT3, width, height, 0, 3*sizeof(float), 0);
                oidnSetFilterBool(flt, "hdr", true);
                oidnCommitFilter(flt);
                oidnExecuteFilter(flt);
                const char* err = nullptr;
                if (oidnGetDeviceError(dev, &err) != OIDN_ERROR_NONE)
                    fprintf(stderr, "OIDN error: %s\n", err ? err : "?");
                oidnReleaseFilter(flt);
                oidnReleaseDevice(dev);
                oidnColor = out;
                fprintf(stderr, "OIDN: denoised %dx%d (RT filter, albedo+normal guides)\n", width, height);
                fflush(stderr);
            }
        }
#endif

        // denoiseQuality 3 with a successful OIDN run: the NL-means chain
        // below is redundant (the recomposite prefers oidnColor) — skip its
        // 28 filterRadiance calls and the helper statistics entirely.
        const bool oidnUsed = denoiseQuality >= 3 &&
                              oidnColor.size() == size_t(width) * height * 3;
        const bool nlMeans = !oidnUsed;
        const int nlPasses = !nlMeans ? 0
                           : (denoiseQuality == 1 ? 1 : FILTER_PASSES + 2);

        // Tier 0: snapshot the TRUE MC sample variance (E[X²]−E[X]² over the
        // spp samples) BEFORE filterVar replaces it with spatial variance —
        // the noise-guided blend needs the sampling noise, and spatial
        // variance conflates texture with noise (high on texture at any spp).
        std::vector<float> mcVar[4];
        if (nlMeans) {
            for (int c = 0; c < 4; ++c) {
                mcVar[c].resize(n);
                for (size_t i = 0; i < n; ++i) mcVar[c][i] = aovs[c][i].Var;
            }
        }

        // Tier 1: re-estimate per-pixel variance (reference filterVar)
        if (nlMeans) {
        filterVar(radiance_Dd, width, height);
        filterVar(radiance_Ds, width, height);
        filterVar(radiance_Id, width, height);
        filterVar(radiance_Is, width, height);
        fprintf(stderr, "DENOISE start\n"); fflush(stderr);
        // Firefly pre-clamp on all channels: isolated outliers poison both the
        // weighted mean and the variance estimates used by the filter.
        fireflyClamp(radiance_Dd, width, height, ffGate);
        fireflyClamp(radiance_Ds, width, height, ffGate);
        fireflyClamp(radiance_Id, width, height, ffGate);
        fireflyClamp(radiance_Is, width, height, ffGate);

        // Normalize to median composite luminance 1.0 (scale-invariance).
        const float invScale = 1.0f / lumScale;
        for (int c = 0; c < 4; ++c)
            for (size_t i = 0; i < n; ++i) {
                aovs[c][i].radiance *= invScale;
                aovs[c][i].Var *= invScale * invScale;
                mcVar[c][i] *= invScale * invScale;
            }

        // Per-AOV median Var AFTER filtering-scale normalization — the
        // reference for all relative variance thresholds (spp-invariant).
        float varMed[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        for (int c = 0; c < 4; ++c) {
            std::vector<float> s;
            s.reserve(n / 7 + 1);
            for (size_t i = 0; i < n; i += 7) s.push_back(aovs[c][i].Var);
            std::nth_element(s.begin(), s.begin() + s.size() / 2, s.end());
            varMed[c] = fmaxf(s[s.size() / 2], 1e-12f);
        }

        // Multi-scale edge-aware filtering. The radiance term carries a sigL
        // FLOOR (flat regions must average) and the ALBEDO-SIMILARITY weight
        // (texture must not average) — see getDenoiseWeight.
        // denoiseQuality: full (default) runs the calibrated FILTER_PASSES+2
        // ladder; quick (progressive preview chunks) runs ONE pass — the
        // intermediate frames are transient, and 7 passes × 4 AOVs × 25 taps
        // is the single largest CPU item in the per-chunk pipeline.
        for (int pass = 0; pass < nlPasses; ++pass) {
            filterRadiance(radiance_Dd, Gbuffer, width, height, false, pass, varMed[0], mcVar[0].data());
            filterRadiance(radiance_Ds, Gbuffer, width, height, true,  pass, varMed[1], mcVar[1].data());
            filterRadiance(radiance_Id, Gbuffer, width, height, false, pass, varMed[2], mcVar[2].data());
            filterRadiance(radiance_Is, Gbuffer, width, height, true,  pass, varMed[3], mcVar[3].data());
        }

        // Denormalize back to scene units.
        if (nlMeans)
        for (int c = 0; c < 4; ++c)
            for (size_t i = 0; i < n; ++i) {
                aovs[c][i].radiance *= lumScale;
                aovs[c][i].Var *= lumScale * lumScale;
            }
        } // nlMeans
        // Post-filter firefly cleanup: the edge-stopping weights deliberately
        // keep silhouette pixels un-averaged, so grazing-angle specular
        // fireflies along object edges survive the multi-pass filter. The
        // relative gates (6x local median AND 6x local mean) spare coherent
        // bright structures (lamp cores) and only clamp isolated specks.
        // (quick quality keeps the FIRST fireflyClamp above and skips this
        // second one — the frame is transient.)
        if (nlMeans && denoiseQuality >= 2) {
            fireflyClamp(radiance_Dd, width, height, ffGate);
            fireflyClamp(radiance_Ds, width, height, ffGate);
            fireflyClamp(radiance_Id, width, height, ffGate);
            fireflyClamp(radiance_Is, width, height, ffGate);
        }

        // ---- P4a: position-based ambient occlusion (contact shadows) ----
        // The path tracer captures large-scale occlusion through shadow rays,
        // but small-scale contact darkening (door reveals, crevices, object
        // bases) needs many bounces to converge and is missing at our depths
        // (the official reference shows it). Horizon-style AO from the
        // first-hit world positions: a neighbour OFF the tangent plane on the
        // normal side is an occluder; flat-surface neighbours lie IN the
        // plane and never self-occlude. Applied to the INDIRECT channels only
        // (direct lighting already has traced shadows).
        // RENDER_AO=1 to enable; RADIUS px; STRENGTH 0..1; TAPS per pixel.
        {
            // Per-call env read (see the OIDN note above).
            const bool aoOn = [] {
                const char* e = std::getenv("RENDER_AO");
                return e && e[0] == '1';
            }();
            if (aoOn && Gbuffer) {
                float aoStrength = 0.6f, aoRadius = 12.0f;
                int aoTaps = 12;
                if (const char* e = std::getenv("RENDER_AO_STRENGTH")) aoStrength = std::strtof(e, nullptr);
                if (const char* e = std::getenv("RENDER_AO_RADIUS"))   aoRadius   = std::strtof(e, nullptr);
                if (const char* e = std::getenv("RENDER_AO_TAPS"))     aoTaps     = std::max(4, std::atoi(e));
                std::vector<float> ao(n, 1.0f);
                for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x) {
                    size_t i = size_t(y) * width + x;
                    float3 pp = Gbuffer[i].position;
                    if (!std::isfinite(pp.x) || glm::length(pp) < 1e-6f) continue;
                    const float3 N = Gbuffer[i].shapeNormal;
                    // per-pixel rotation (hash) so the tap pattern does not band
                    uint32_t hsh = uint32_t(x * 73856093) ^ uint32_t(y * 19349663);
                    float rot = (hsh & 1023u) * (2.0f * PI / 1024.0f);
                    float occ = 0.0f, wsum = 0.0f;
                    for (int t = 0; t < aoTaps; ++t) {
                        float ang = rot + t * (2.0f * PI / aoTaps) * 2.4f;   // golden-ish spread
                        float rr  = aoRadius * std::sqrt((t + 0.5f) / aoTaps);
                        int sx = x + int(rr * std::cos(ang));
                        int sy = y + int(rr * std::sin(ang));
                        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
                        float3 pq = Gbuffer[size_t(sy) * width + sx].position;
                        if (!std::isfinite(pq.x) || glm::length(pq) < 1e-6f) continue;
                        float3 diff = pq - pp;
                        float dist = glm::length(diff);
                        if (dist > aoRadius * 2.0f || dist < 1e-5f) continue;
                        // signed height above the tangent plane, normalized
                        float proj = glm::dot(diff, N) / dist;
                        // occluder: neighbour well above the plane (concave
                        // corner / object base), fading with distance
                        float w = std::max(0.0f, proj - 0.15f) / 0.85f;
                        w *= 1.0f - std::min(1.0f, dist / (aoRadius * 2.0f));
                        occ += w;
                        wsum += 1.0f;
                    }
                    if (wsum > 0.0f) occ /= wsum;
                    ao[i] = 1.0f - aoStrength * std::min(1.0f, occ * 2.0f);
                }
                for (int c = 2; c < 4; ++c)   // Id, Is = indirect diffuse/specular
                    for (size_t i = 0; i < n; ++i)
                        aovs[c][i].radiance *= ao[i];
                // OIDN path: its composite replaces the AOV sum downstream,
                // so apply the AO there too — weighted by the pixel's
                // INDIRECT fraction (AO must not darken direct sunlight).
                if (oidnColor.size() == n * 3) {
                    for (size_t i = 0; i < n; ++i) {
                        float idl = luminance(radiance_Id[i].radiance) +
                                    luminance(radiance_Is[i].radiance);
                        float tot = idl + luminance(radiance_Dd[i].radiance) +
                                    luminance(radiance_Ds[i].radiance) +
                                    (channels.emission ? luminance(float3(channels.emission[i])) : 0.0f);
                        float frac = (tot > 1e-6f) ? std::min(1.0f, idl / tot) : 0.0f;
                        float m = 1.0f - (1.0f - ao[i]) * frac;
                        oidnColor[i*3+0] *= m;
                        oidnColor[i*3+1] *= m;
                        oidnColor[i*3+2] *= m;
                    }
                }
                fprintf(stderr, "AO: position-based (radius=%.0fpx strength=%.2f taps=%d)\n",
                        aoRadius, aoStrength, aoTaps);
                fflush(stderr);
            }
        }

        // ---- Albedo high-frequency detail re-injection ----
        // The multi-pass filter smooths low-contrast shading texture
        // (weathering marks, grime) that the albedo-similarity gate cannot
        // protect — it only stops ALBEDO EDGES. The first-hit albedo in the
        // G-buffer is noise-free, so its high-frequency residual is pure
        // texture: transfer it back onto the denoised radiance
        // multiplicatively (per channel, hue-preserving). Linear in the four
        // AOVs, so applying per-AOV equals applying to their sum.
        // RENDER_DENOISE_DETAIL: gain (0 = off, default; 0.5-1.0 restores
        // cobblestone/grain crispness measured at 0.30x of reference).
        float detailGain = -1.0f;
        if (const char* e = std::getenv("RENDER_DENOISE_DETAIL"))
            detailGain = fmaxf(0.0f, std::strtof(e, nullptr));
        if (detailGain > 0.0f && Gbuffer) {
            // 3x3 box blur of the first-hit albedo (wrap-free edges)
            std::vector<float3> alb(n);
            for (size_t i = 0; i < n; ++i) alb[i] = Gbuffer[i].baseColor;
            std::vector<float3> albBlur(n);
            for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float3 acc(0.0f);
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = std::min(width - 1, std::max(0, x + dx));
                    int sy = std::min(height - 1, std::max(0, y + dy));
                    acc += alb[size_t(sy) * width + sx];
                    ++cnt;
                }
                albBlur[size_t(y) * width + x] = acc / float(cnt);
            }
            RadianceData* aovs[4] = {radiance_Dd, radiance_Ds, radiance_Id, radiance_Is};
            for (size_t i = 0; i < n; ++i) {
                float3 hf = alb[i] - albBlur[i];
                float3 fac = float3(1.0f + detailGain * hf.x,
                                    1.0f + detailGain * hf.y,
                                    1.0f + detailGain * hf.z);
                fac = float3(fmaxf(fac.x, 0.0f), fmaxf(fac.y, 0.0f), fmaxf(fac.z, 0.0f));
                for (int c = 0; c < 4; ++c)
                    aovs[c][i].radiance = aovs[c][i].radiance * fac;
            }
        }
        // ---- SPP-adaptive energy gain (bias compensation) ----
        // The calibrated 320spp look already includes the denoiser's own
        // retention loss: log-domain averaging underestimates heavy-tailed
        // data (Jensen) and the clamps remove energy, so the 320spp output
        // keeps only ~77% of the raw MC mean (measured energyGain 1.227-
        // 1.299 on the bistro interior). Targeting the raw MC mean would
        // therefore brighten 320spp by ~25% versus the delivered
        // calibration. The right target for a LOW-spp frame is "what this
        // frame's 320spp output would have been": dnMean_320 ~ r320 x
        // rawMean, where r320 (the 320spp retention) is a property of the
        // scene's converged spikiness, not of the current spp.
        //
        // r320 (the 320spp retention) is a scene/pipeline constant:
        // "how much of the raw MC mean the 320spp denoised output keeps"
        // (measured 0.759 on the bistro interior DELIVERED pipeline, 480p:
        // DENOISE log refGain=1.054 at r320=0.80 -> r = 0.80/1.054).
        // Default 0.80 (within 6%); RENDER_DENOISE_RET320 overrides it per
        // scene. Self-calibration (no domain pitfalls):
        // render the scene at 320spp with RENDER_DENOISE_RET320=0.5 and
        // read the DENOISE log — r320_true = 0.5 / printed refGain
        // (refGain = r320_set / r320_true; it is logged even in the dead
        // zone, where no gain is applied).
        // An earlier per-frame auto-estimate from the denoised output's
        // spikiness (S_dn) proved too noisy (+/-15-20%) and went fully
        // out-of-distribution at 4spp (S_dn=256 -> r320=0.31, wrong by 2x),
        // so it is kept only as a logged diagnostic.
        //
        // spp >= 320: gain is exactly 1.0 (dead zone) — the 320spp output
        // stays bit-identical to the delivered pipeline (zero regression).
        // The energy gain corrects the denoiser's energy retention — it must
        // apply to WHICH EVER denoiser ran: NL-means multiplies the AOVs,
        // OIDN multiplies oidnColor (otherwise the OIDN final frame reads
        // ~2.7x darker than the calibrated NL-means+gain look, measured).
        double dnLumSum = 0.0;
        if (nlMeans)
        for (size_t i = 0; i < n; ++i)
            dnLumSum += luminance(radiance_Dd[i].radiance) +
                        luminance(radiance_Ds[i].radiance) +
                        luminance(radiance_Id[i].radiance) +
                        luminance(radiance_Is[i].radiance);
        const size_t oidnElems = oidnColor.size() / 3;
        if (!nlMeans && oidnElems == n)
        for (size_t i = 0; i < n; ++i)
            dnLumSum += luminance(float3(oidnColor[i*3+0], oidnColor[i*3+1],
                                         oidnColor[i*3+2]));
        float energyGain = 1.0f, refGain = 1.0f, r320 = 0.80f, Sdn = 0.0f;
        if (dnLumSum > 1e-12) {
            std::vector<float> dnLum;
            dnLum.reserve(n / 4 + 1);
            if (nlMeans)
            for (size_t i = 0; i < n; i += 4)
                dnLum.push_back(luminance(radiance_Dd[i].radiance) +
                                luminance(radiance_Ds[i].radiance) +
                                luminance(radiance_Id[i].radiance) +
                                luminance(radiance_Is[i].radiance));
            else
            for (size_t i = 0; i < n; i += 4)
                dnLum.push_back(luminance(float3(oidnColor[i*3+0],
                                                 oidnColor[i*3+1],
                                                 oidnColor[i*3+2])));
            std::nth_element(dnLum.begin(), dnLum.begin() + dnLum.size() / 2, dnLum.end());
            const double dnMean = dnLumSum / (double)n;
            const double dnMedian = dnLum[dnLum.size() / 2];
            Sdn = (float)(dnMean / fmax(dnMedian, 1e-9));   // diagnostic only
            if (const char* e = std::getenv("RENDER_DENOISE_RET320")) {
                double v = std::strtod(e, nullptr);
                if (v > 0.0 && v < 1.0) r320 = (float)v;
            }
            refGain = (float)((r320 * rawLumSum) / dnLumSum);
            refGain = std::min(3.0f, std::max(0.5f, refGain));
            // Dead zone: at spp >= 320 the gain is NOT applied (zero
            // regression), but refGain is logged for r320 calibration.
            if (sppEff < (int)SPP_REF) energyGain = refGain;
        }
        // The gain multiplies the filtered AOVs (NL-means) or oidnColor
        // (OIDN) — the recomposite then picks the matching source.
        if (nlMeans && (energyGain > 1.0001f || energyGain < 0.9999f))
            for (int c = 0; c < 4; ++c)
                for (size_t i = 0; i < n; ++i) {
                    aovs[c][i].radiance *= energyGain;
                    aovs[c][i].Var *= energyGain * energyGain;
                }
        if (!nlMeans && oidnElems == n &&
            (energyGain > 1.0001f || energyGain < 0.9999f))
            for (size_t i = 0; i < n; ++i) {
                oidnColor[i*3+0] *= energyGain;
                oidnColor[i*3+1] *= energyGain;
                oidnColor[i*3+2] *= energyGain;
            }
        fprintf(stderr, "DENOISE done (lumScale=%.4f spp=%d k=%.3f Sdn=%.2f r320=%.3f refGain=%.3f energyGain=%.3f)\n",
                lumScale, sppEff, kSpp, Sdn, r320, refGain, energyGain); fflush(stderr);
        denoised = true;
    }

    // Emission channel denoising (simple spatial blur, edge-aware via G-buffer)
    if ((shadeOptions & DoDenoise) && channels.emission && Gbuffer) {
        const size_t size = size_t(width) * height;
        std::vector<float4> temp(size);
        std::copy(channels.emission, channels.emission + size, temp.data());
        const int rad = 2;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                size_t idx = size_t(y) * width + x;
                const HitInfo& Gp = Gbuffer[idx];
                float4 sum(0.0f);
                float totalW = 0.0f;
                for (int dy = -rad; dy <= rad; ++dy) {
                    for (int dx = -rad; dx <= rad; ++dx) {
                        int sx = x + dx, sy = y + dy;
                        if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
                        size_t sidx = size_t(sy) * width + sx;
                        const HitInfo& Gq = Gbuffer[sidx];
                        float dn = 1.0f - glm::dot(Gp.surfaceNormal, Gq.surfaceNormal);
                        float w = expf(-dn * dn / 0.3f);
                        float spatial = expf(-float(dx*dx + dy*dy) / 8.0f);
                        sum += temp[sidx] * w * spatial;
                        totalW += w * spatial;
                    }
                }
                if (totalW > 1e-8f)
                    channels.emission[idx] = sum / totalW;
            }
        }
    }

    // Recomposite final color from the (denoised) AOVs — without this the
    // filtered radiance arrays were computed but never used, so denoising
    // had no effect on the output image.
    if (denoised) {
        const size_t size = size_t(width) * height;
        const bool useOidn = oidnColor.size() == size * 3;
        for (size_t i = 0; i < size; ++i) {
            float3 c;
            if (useOidn) {
                c = float3(oidnColor[i*3+0], oidnColor[i*3+1], oidnColor[i*3+2]);
            } else {
                c = radiance_Dd[i].radiance + radiance_Ds[i].radiance
                  + radiance_Id[i].radiance + radiance_Is[i].radiance;
                c += float3(channels.emission[i]);
            }
            channels.finalColor[i] = float4(c, 1.0f);
        }
    }

    // Reference pipeline order: exposure-scaled HDR → HDR-domain bloom →
    // soft-clip + gamma. Bloom must run BEFORE gamma so the Clum≥1 threshold
    // selects true light sources in linear units.
    tonemap();
    if (shadeOptions & DoBloom) bloom();
    gammaCorrect();
    // Final chroma-outlier despeckle (display space, after gamma). The
    // path-traced residual at high-contrast boundaries (bulb halos, foliage
    // edges) survives the edge-aware denoiser because edge weights preserve
    // it; a single 3x3-median replacement of isolated chroma outliers targets
    // exactly those. Measured at 480p: noise90 −16%, detail energy kept 88%.
    // RENDER_DESPECKLE=<thresh> (default 0.20; 0 = off).
    {
        float dsp = 0.20f;
        if (const char* e = std::getenv("RENDER_DESPECKLE")) dsp = std::strtof(e, nullptr);
        if (dsp > 0.0f) despecklePass(*this, dsp);
    }

    // Post-gamma effects
    if (shadeOptions & DoFXAA)  fxaa();
    if (shadeOptions & DoDoF)   depthOfField(nullptr, 0.0f, 0.0f);
}

// One-pass chroma-outlier despeckle: replace pixels whose max channel deviates
// from their 3x3 median by more than thresh with that median. Thread-local
// buffers only (the parallelForRows stack-canary lesson).
static void despecklePass(Photo& photo, float thresh) {
    const int w = photo.width, h = photo.height;
    if (!photo.channels.finalColor || w <= 0 || h <= 0) return;
    float4* px = photo.channels.finalColor;
    std::vector<float4> src(px, px + size_t(w) * h);
    auto clampiY = [h](int v) { return v < 0 ? 0 : (v > h - 1 ? h - 1 : v); };
    auto clampiX = [w](int v) { return v < 0 ? 0 : (v > w - 1 ? w - 1 : v); };
    auto med9 = [](float* v) {
        for (int i = 1; i < 9; ++i) {          // insertion sort of 9
            float key = v[i]; int j = i - 1;
            while (j >= 0 && v[j] > key) { v[j+1] = v[j]; --j; }
            v[j+1] = key;
        }
        return v[4];
    };
    parallelForRows(h, [&](int y0, int y1) {
      for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < w; ++x) {
            float ch[3];
            for (int c = 0; c < 3; ++c) {
                float nb[9]; int k = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        const float4& s = src[size_t(clampiY(y+dy)) * w + clampiX(x+dx)];
                        nb[k++] = (c == 0) ? s.x : (c == 1) ? s.y : s.z;
                    }
                ch[c] = med9(nb);
            }
            const float3 c0 = src[size_t(y) * w + x];
            float dev = fmaxf(fmaxf(fabsf(c0.x - ch[0]), fabsf(c0.y - ch[1])), fabsf(c0.z - ch[2]));
            if (dev > thresh)
                px[size_t(y) * w + x] = float4(ch[0], ch[1], ch[2], 1.0f);
        }
      }
    });
}

// ---- stb_image_write for PNG output ----
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

void Photo::save(const std::string& filename) {
    if (!channels.finalColor) return;
    const std::vector<uint8_t> pngData = rgbBytes();
    stbi_write_png(filename.c_str(), width, height, 3,
                   pngData.data(), width * 3);
}

std::vector<uint8_t> Photo::rgbBytes() const {
    const size_t size = size_t(width) * height;
    std::vector<uint8_t> pngData(size * 3);
    if (!channels.finalColor) return pngData;
    for (size_t i = 0; i < size; ++i) {
        const float3 c = float3(channels.finalColor[i]);
        pngData[i * 3 + 0] = static_cast<uint8_t>(
            std::min(255.0f, std::max(0.0f, c.x * 255.0f)));
        pngData[i * 3 + 1] = static_cast<uint8_t>(
            std::min(255.0f, std::max(0.0f, c.y * 255.0f)));
        pngData[i * 3 + 2] = static_cast<uint8_t>(
            std::min(255.0f, std::max(0.0f, c.z * 255.0f)));
    }
    return pngData;
}

void Photo::tonemap() {
    if (!channels.finalColor) return;
    const size_t size = size_t(width) * height;
    const float exp = fmaxf(exposure, 1e-6f);
    // Optional auto white balance (von Kries, gray-world). Default OFF so the
    // tungsten atmosphere of the reference look is preserved.
    if (wbStrength > 0.0f) {
        double mr = 0, mg = 0, mb = 0;
        for (size_t i = 0; i < size; ++i) {
            float3 c = float3(channels.finalColor[i]);
            mr += c.x; mg += c.y; mb += c.z;
        }
        mr /= size; mg /= size; mb /= size;
        float mLum = float(0.299 * mr + 0.587 * mg + 0.114 * mb);
        auto wbGain = [&](double m) -> float {
            if (m < 1e-8) return 1.0f;
            float g = float(mLum / m);
            return 1.0f + wbStrength * (g - 1.0f);
        };
        float gR = wbGain(mr), gG = wbGain(mg), gB = wbGain(mb);
        fprintf(stderr, "WB: mean=(%.2f,%.2f,%.2f) gains=(%.3f,%.3f,%.3f)\n",
                mr, mg, mb, gR, gG, gB);
        for (size_t i = 0; i < size; ++i) {
            float3 c = float3(channels.finalColor[i]);
            c.x *= gR; c.y *= gG; c.z *= gB;
            channels.finalColor[i] = float4(c, 1.0f);
        }
    }
    // Exposure scaling only — tone curve lives in gammaCorrect() (tanh
    // soft-clip, matching the reference pipeline)
    for (size_t i = 0; i < size; ++i) {
        float3 c = float3(channels.finalColor[i]);
        c = c * exp;
        channels.finalColor[i] = float4(c, 1.0f);
    }
}

// RENDER_DENOISE_NOISE_TARGET: relative-noise threshold guiding a per-pixel
// blend between the filtered and the pre-pass value inside every
// filterRadiance pass. A pixel whose MC noise (sqrt(Var)/luminance) is well
// below the target is nearly clean — averaging it only erodes low-contrast
// shading texture (weathering marks) that the albedo gate cannot protect
// (the gate stops ALBEDO EDGES, not subtle albedo-matched shading detail).
// Fireflies keep a huge Var, so they are still filtered at any spp.
// 0 = old behaviour (always full filter). Default 0.05 (~5% relative noise).
//
// These three caches are MUTABLE and re-read by refreshRenderEnv() at each
// postProcess: scene presets install their rig env values at runtime
// (applyPresetRig), which a program-start cache would never see.
static float g_denoiseNoiseTarget = 0.05f;

// RENDER_TONEMAP=aces selects the ACES fit (Narkowicz 2015) as the tone
// curve — a filmic shoulder that collapses highlights toward white (the
// official Bistro renders have ~0.02% blown pixels; the warm-preserving
// tanh shoulder saturates several %). Default: the reference tanh clip.
static bool g_useAcesTonemap = false;

// RENDER_CURVE="x0,y0 x1,y1 ...": optional display-domain contrast curve on
// LUMA (Fritsch–Carlson monotone cubic through the control points, 1024-entry
// LUT). The official daylight grade crushes midtones while keeping sunlit
// highlights — a lighting-rig contrast no global sun/sky knob reproduces
// (see README 13.10). Empty = off (byte-identical historical behaviour).
struct ContrastCurve {
    bool on = false;
    float lut[1024];
};
static ContrastCurve g_contrast;

static ContrastCurve parseContrastCurve() {
    ContrastCurve cc;
    const char* e = std::getenv("RENDER_CURVE");
    if (!e || !*e) return cc;
    std::vector<double> xs, ys;
    {
        std::istringstream ss(e);
        std::string tok;
        while (ss >> tok) {
            double x, y;
            if (sscanf(tok.c_str(), "%lf,%lf", &x, &y) == 2) { xs.push_back(x); ys.push_back(y); }
        }
    }
    if (xs.size() < 2) return cc;
    const int n = (int)xs.size();
    // Fritsch–Carlson monotone cubic
    std::vector<double> h(n - 1), d(n - 1), m(n);
    for (int i = 0; i < n - 1; ++i) { h[i] = xs[i + 1] - xs[i]; d[i] = (ys[i + 1] - ys[i]) / h[i]; }
    m[0] = d[0]; m[n - 1] = d[n - 2];
    for (int i = 1; i < n - 1; ++i) {
        if (d[i - 1] * d[i] <= 0) m[i] = 0;
        else {
            double w1 = 2 * h[i - 1] + h[i], w2 = h[i - 1] + 2 * h[i];
            m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
        }
    }
    for (int i = 0; i < 1024; ++i) {
        double t = i / 1023.0;
        int seg = 0;
        while (seg < n - 2 && t > xs[seg + 1]) ++seg;
        double hh = h[seg], s = (t - xs[seg]) / hh;
        double s2 = s * s, s3 = s2 * s;
        double v = (2 * s3 - 3 * s2 + 1) * ys[seg] + (s3 - 2 * s2 + s) * hh * m[seg]
                 + (-2 * s3 + 3 * s2) * ys[seg + 1] + (s3 - s2) * hh * m[seg + 1];
        cc.lut[i] = float(v);
    }
    cc.on = true;
    fprintf(stderr, "Contrast curve on (%zu control points)\n", xs.size());
    return cc;
}

// Re-read the env knobs cached in the file-statics above. Called at the top
// of Photo::postProcess (single-threaded, before any parallel pixel work) —
// scene presets install rig env values at runtime via applyPresetRig, which
// a program-start cache would never see.
void refreshRenderEnv() {
    const char* nt = std::getenv("RENDER_DENOISE_NOISE_TARGET");
    if (!nt || !*nt) g_denoiseNoiseTarget = 0.05f;
    else {
        const float v = std::strtof(nt, nullptr);
        g_denoiseNoiseTarget = (v >= 0.0f && v < 10.0f) ? v : 0.05f;
    }
    const char* t = std::getenv("RENDER_TONEMAP");
    g_useAcesTonemap = t && std::string(t) == "aces";
    // The 1024-entry LUT rebuild is the only non-trivial work here — cache it
    // keyed on the raw env value (re-parse ONLY when the operator changes it;
    // same value => byte-identical curve, so behaviour is unchanged).
    static std::string cachedCurveEnv;
    const char* ce = std::getenv("RENDER_CURVE");
    const std::string curveEnv = ce ? ce : "";
    if (curveEnv != cachedCurveEnv) {
        g_contrast = parseContrastCurve();
        cachedCurveEnv = curveEnv;
    }
}

void Photo::gammaCorrect() {
    if (!channels.finalColor) return;
    const float invGamma = 1.0f / 2.2f;
    const size_t size = size_t(width) * height;
    // Saturation boost (display domain, luma-preserving): c' = L + (c-L)*s.
    // The field existed in Photo for ages but was never applied — the
    // daylight exterior needs it (official awning R:G ≈ 2.3:1). 1.0 = off,
    // so every historical render is byte-identical.
    const float sat = saturationBoost;
    for (size_t i = 0; i < size; ++i) {
        float3 c = float3(channels.finalColor[i]);
        if (g_useAcesTonemap) {
            float3 n = (c * (2.51f * c + 0.03f)) / (c * (2.43f * c + 0.59f) + 0.14f);
            c = float3(fminf(fmaxf(n.x, 0.0f), 1.0f),
                       fminf(fmaxf(n.y, 0.0f), 1.0f),
                       fminf(fmaxf(n.z, 0.0f), 1.0f));
            c = float3(powf(c.x, invGamma), powf(c.y, invGamma), powf(c.z, invGamma));
        } else {
        // Reference soft-clip: above 0.75 luminance, compress brightness along
        // the original hue with a tanh shoulder — highlights stay saturated
        // warm instead of collapsing to white
        float Clum = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
        if (Clum > 0.75f) {
            float lumBound = tanhf(3.0f * (Clum - 0.75f)) / 3.0f + 0.75f;
            c = c / Clum * lumBound;
        }
        c = float3(fmaxf(c.x, 0.0f), fmaxf(c.y, 0.0f), fmaxf(c.z, 0.0f));
        c = float3(fminf(c.x, 1.0f), fminf(c.y, 1.0f), fminf(c.z, 1.0f));
        c = float3(powf(c.x, invGamma), powf(c.y, invGamma), powf(c.z, invGamma));
        }
        if (sat != 1.0f) {
            float l = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
            c = float3(l + (c.x - l) * sat, l + (c.y - l) * sat, l + (c.z - l) * sat);
            c = float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                       fminf(fmaxf(c.y, 0.0f), 1.0f),
                       fminf(fmaxf(c.z, 0.0f), 1.0f));
        }
        // Display-domain luma contrast (RENDER_CURVE) — hue-preserving scale
        // along the pixel's own luminance, so texture hue is untouched.
        if (g_contrast.on) {
            float l = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
            float lc = g_contrast.lut[(int)(fminf(fmaxf(l, 0.0f), 1.0f) * 1023.0f + 0.5f)];
            float s = lc / fmaxf(l, 1e-6f);
            c = c * s;
            c = float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                       fminf(fmaxf(c.y, 0.0f), 1.0f),
                       fminf(fmaxf(c.z, 0.0f), 1.0f));
        }
        // Split-tone (RENDER_SPLIT_WARM/COOL): warm the sunlit, cool the shade.
        // Key: "sun" = the P1 direct-sun matte (物理正确的分区), "luma" = the
        // legacy display-luma smoothstep.
        float sw;
        if (splitKeySun && sunliness.size() == size) {
            float sf = sunliness[i];
            sw = fminf(fmaxf(sf, 0.0f), 1.0f);
            sw = sw * sw * (3.0f - 2.0f * sw);
        } else {
            float sl = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
            sw = fminf(fmaxf((sl - 0.15f) / 0.60f, 0.0f), 1.0f);
            sw = sw * sw * (3.0f - 2.0f * sw);   // smoothstep 0.15..0.75
        }
        if (splitWarm != 0.0f) {
            c.x *= 1.0f + splitWarm * sw;
            c.z *= 1.0f - splitWarm * 0.6f * sw;
        }
        if (splitCool != 0.0f) {
            float shadow = 1.0f - sw;
            c.z *= 1.0f + splitCool * shadow;
            c.x *= 1.0f - splitCool * 0.6f * shadow;
        }
        // P4d luma-band grade (RENDER_GRADE_BANDS): piecewise-linear
        // per-channel gain by luma — the fitted "finishing LUT" that pulls
        // each tonal band's colour toward the official plate.
        if (gradeBands.size() >= 8) {
            float l = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
            const int nb = int(gradeBands.size() / 4);
            int hi = 1;
            while (hi < nb - 1 && gradeBands[hi*4] < l) ++hi;
            const int lo = hi - 1;
            float l0 = gradeBands[lo*4], l1 = gradeBands[hi*4];
            float t = (l1 > l0) ? fminf(fmaxf((l - l0) / (l1 - l0), 0.0f), 1.0f) : 0.0f;
            float g0 = gradeBands[lo*4+1], g1 = gradeBands[hi*4+1];
            float g2 = gradeBands[lo*4+2], g3 = gradeBands[hi*4+2];
            float g4 = gradeBands[lo*4+3], g5 = gradeBands[hi*4+3];
            c.x *= g0 + (g1 - g0) * t;
            c.y *= g2 + (g3 - g2) * t;
            c.z *= g4 + (g5 - g4) * t;
        }
        c = float3(fminf(fmaxf(c.x, 0.0f), 1.0f),
                   fminf(fmaxf(c.y, 0.0f), 1.0f),
                   fminf(fmaxf(c.z, 0.0f), 1.0f));
        channels.finalColor[i] = float4(c, 1.0f);
    }
}

// ---- Bloom: reference HDR-domain multi-scale glow ----
// Threshold Clum>=1 in linear units (only true light sources), extract
// pixel/pow(Clum,0.65)-1, then accumulate 5-tap gaussian blurs at doubling
// step sizes 1..128 — the wide footprint is what makes lamps "glow".
void Photo::bloom() {
    if (!channels.finalColor || width < 4 || height < 4) return;

    const size_t size = size_t(width) * height;
    const int filterRadius = 2;
    const float h[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    const float omega_bloom = 0.65f;

    std::vector<float3> bloomColor(size, float3(0.0f));
    for (size_t id = 0; id < size; ++id) {
        float3 c = float3(channels.finalColor[id]);
        float Clum = c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
        if (Clum < 1.0f) continue;
        float3 b = c / powf(Clum, omega_bloom);
        bloomColor[id] = float3(fmaxf(b.x - 1.0f, 0.0f),
                                fmaxf(b.y - 1.0f, 0.0f),
                                fmaxf(b.z - 1.0f, 0.0f));
    }

    std::vector<float3> temp(size);
    // step<=8 (was 16): at 480-1080p the ±63px footprint made the lamp glow
    // CRAWL up the pendant rod and into the ceiling (the rod pixels read
    // ~0.7 in the final image while every raw AOV there is black). ±15px
    // keeps the official tight halo.
    for (int step = 1; step <= 8; step *= 2) {
        temp = bloomColor;
        for (int y = 0; y < height; ++y)
            for (int x = 0; x < width; ++x) {
                float3 col(0.0f);
                for (int ky = -filterRadius; ky <= filterRadius; ++ky)
                    for (int kx = -filterRadius; kx <= filterRadius; ++kx) {
                        int nx = x + kx * step, ny = y + ky * step;
                        if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                        float3 t = temp[size_t(ny) * width + nx];
                        float w = h[ky + filterRadius] * h[kx + filterRadius];
                        col = col + t * w;
                    }
                bloomColor[size_t(y) * width + x] = col;
            }
        // Every scale's glow accumulates into the frame
        for (size_t id = 0; id < size; ++id) {
            float3 c = float3(channels.finalColor[id]);
            c = c + bloomColor[id] / 6.0f;
            channels.finalColor[id] = float4(c, 1.0f);
        }
    }
}

// ---- FXAA: simplified edge-directed blur ----
void Photo::fxaa() {
    if (!channels.finalColor || width < 4 || height < 4) return;

    const size_t size = size_t(width) * height;
    std::vector<float4> original(size);
    std::copy(channels.finalColor, channels.finalColor + size, original.data());

    const float edgeThreshold = 0.1f;
    const int spanMax = 8;

    for (int y = 2; y < height - 2; ++y) {
        for (int x = 2; x < width - 2; ++x) {
            const size_t idx = size_t(y) * width + x;

            // Luminance at center and neighbors
            auto lum = [&](int dx, int dy) -> float {
                const float3 c = float3(original[(y + dy) * width + (x + dx)]);
                return c.x * RGB_WEIGHT[0] + c.y * RGB_WEIGHT[1] + c.z * RGB_WEIGHT[2];
            };

            float lC = lum(0, 0);
            float lN = lum(0, -1), lS = lum(0, 1);
            float lW = lum(-1, 0), lE = lum(1, 0);

            // Horizontal and vertical contrast
            float hContrast = fabsf(lW + lE - 2.0f * lC);
            float vContrast = fabsf(lN + lS - 2.0f * lC);

            if (hContrast < edgeThreshold && vContrast < edgeThreshold) continue;

            // Simple blur along the dominant edge direction
            float4 avg(0.0f);
            float total = 0.0f;
            float kernel[3] = {0.25f, 0.50f, 0.25f};

            if (hContrast > vContrast) {
                for (int k = -2; k <= 2; ++k) {
                    avg += original[size_t(y) * width + (x + k)] * kernel[abs(k)];
                    total += kernel[abs(k)];
                }
            } else {
                for (int k = -2; k <= 2; ++k) {
                    avg += original[size_t(y + k) * width + x] * kernel[abs(k)];
                    total += kernel[abs(k)];
                }
            }
            if (total > 0.0f) channels.finalColor[idx] = avg / total;
        }
    }
}

// ---- Depth of Field: simple bokeh blur ----
void Photo::depthOfField(const float* depthBuffer, float focus, float coc) {
    if (!channels.finalColor || !depthBuffer || width < 4 || height < 4) return;
    (void)coc;

    const size_t size = size_t(width) * height;
    std::vector<float4> original(size);
    std::copy(channels.finalColor, channels.finalColor + size, original.data());

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t idx = size_t(y) * width + x;
            float depth = depthBuffer[idx];
            float blurRadius = fabsf(depth - focus) * 0.5f;
            int r = std::min(8, std::max(0, static_cast<int>(blurRadius)));

            if (r < 1) continue;

            float4 sum(0.0f);
            int count = 0;
            for (int dy = -r; dy <= r; ++dy) {
                for (int dx = -r; dx <= r; ++dx) {
                    int sx = x + dx, sy = y + dy;
                    if (sx < 0 || sx >= width || sy < 0 || sy >= height) continue;
                    // Simple disk bokeh
                    if (dx * dx + dy * dy <= r * r) {
                        sum += original[size_t(sy) * width + sx];
                        ++count;
                    }
                }
            }
            if (count > 0) channels.finalColor[idx] = sum / float(count);
        }
    }
}

// =========================================================================
// Spatial Denoising (NL-Means Joint Bilateral Filter)
// =========================================================================

void spatialClamp(RadianceData* data, int width, int height, float maxLuma) {
    const int size = width * height;
    for (int i = 0; i < size; ++i) {
        float lum = data[i].radiance.x * 0.299f +
                    data[i].radiance.y * 0.587f +
                    data[i].radiance.z * 0.114f;
        if (lum > maxLuma) {
            float scale = maxLuma / lum;
            data[i].radiance *= scale;
            data[i].Var *= scale * scale;
        }
    }
}

// Row-banded parallel for over [0,height): fn(y0, y1) processes rows y0..y1-1.
// Pixel work is independent per band (separate output buffers), so splitting
// rows across hardware threads is deterministic and race-free.
template <typename Fn>
static void parallelForRows(int height, Fn&& fn) {
    const unsigned hw = std::thread::hardware_concurrency();
    const int nThread = (int)std::max(1u, hw == 0 ? 8u : hw);
    if (nThread <= 1 || height < nThread * 2) { fn(0, height); return; }
    std::vector<std::thread> threads;
    threads.reserve(nThread);
    int rows = 0;
    for (int t = 0; t < nThread; ++t) {
        int r0 = rows, r1 = rows + height / nThread + (t < height % nThread ? 1 : 0);
        rows = r1;
        if (r1 <= r0) continue;
        threads.emplace_back([&fn, r0, r1] { fn(r0, r1); });
    }
    for (auto& th : threads) th.join();
}

// Local outlier (firefly) clamp: an isolated sample far above its 3x3-median
// neighborhood is MC noise, not structure — pull it down to gate x the
// median. `gate` is SPP-adaptive (6 at 320spp, up to 48 at 4spp — see
// postProcess): at low spp the legitimate per-pixel energy spike routinely
// exceeds 6x the local median and a fixed gate would clamp real signal.
// Spatially coherent brightness (lamp cores, their halos) has a high local
// median and is untouched.
static void fireflyClamp(RadianceData* data, int width, int height, float gate) {
    const size_t size = size_t(width) * height;
    std::vector<float> lum(size);
    for (size_t i = 0; i < size; ++i)
        lum[i] = luminance(data[i].radiance);
    parallelForRows(height, [&](int y0, int y1) {
    float nb[9];  // per-thread stack: shared-across-threads nb raced and
                  // smashed the canary under -O3 vectorization
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < width; ++x) {
            size_t idx = size_t(y) * width + x;
            if (lum[idx] <= 0.0f) continue;
            int n = 0;
            float sum = 0.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int sx = std::min(width - 1, std::max(0, x + dx));
                    int sy = std::min(height - 1, std::max(0, y + dy));
                    float l = lum[size_t(sy) * width + sx];
                    nb[n++] = l;
                    sum += l;
                }
            std::nth_element(nb, nb + 4, nb + 9);
            // Double gate: gate x median AND gate x local mean. The median
            // test alone destroys legitimately lit pixels in dark regions
            // (median ~0, so any real signal exceeds "gate x median"), which
            // silently dimmed the indirect channel by ~19% in A/B tests.
            float cap = gate * fmaxf(nb[4], 1e-3f);
            cap = fmaxf(cap, gate * sum / 9.0f);
            if (lum[idx] > cap) {
                float scale = cap / lum[idx];
                data[idx].radiance *= scale;
                data[idx].Var *= scale * scale;
            }
        }
    });
}

// Tier 1: reference filterVar — re-estimate variance from a 3x3 window. The
// E2-dot(E,E) term (spatial radiance variance) provides the variance scale the
// radiance-diff weight needs to tolerate texture/normal-map detail.
static void filterVar(RadianceData* data, int width, int height) {
    static const int radius = 1;
    static const float h[3] = {0.25f, 0.5f, 0.25f};
    const size_t size = size_t(width) * height;
    std::vector<float> varEst(size, 0.0f);
    parallelForRows(height, [&](int y0, int y1) {
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < width; ++x) {
            float3 E(0.0f);
            float E2 = 0.0f, Var = 0.0f;
            for (int dy = -radius; dy <= radius; ++dy)
                for (int dx = -radius; dx <= radius; ++dx) {
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                    const RadianceData& Lq = data[size_t(ny) * width + nx];
                    float w = h[dx + radius] * h[dy + radius];
                    E += Lq.radiance * w;
                    E2 += glm::dot(Lq.radiance, Lq.radiance) * w;
                    Var += Lq.Var * w;
                }
            varEst[size_t(y) * width + x] = Var + E2 - glm::dot(E, E);
        }
    });
    for (size_t i = 0; i < size; ++i) data[i].Var = varEst[i];
}

// Tier 1: variance-confidence blend — adopt the filtered estimate where
// PRE-filter Var is above ~median; converged pixels (Var << median) stay sharp.
// Median (not mean) is used because heavy-tailed MC Var skews the mean.
static void blendVariance(RadianceData* data, const float3* orig,
                          const float* preVar, int width, int height) {
    const size_t size = size_t(width) * height;
    std::vector<float> tmp(preVar, preVar + size);
    size_t mid = size / 2;
    std::nth_element(tmp.begin(), tmp.begin() + mid, tmp.end());
    const float median = tmp[mid] + 1e-6f;
    const float sigma0 = 0.15f * median;
    for (size_t i = 0; i < size; ++i) {
        const float v = preVar[i];
        const float g = v / (v + sigma0);
        data[i].radiance = orig[i] + g * (data[i].radiance - orig[i]);
    }
}

// Edge-aware weight (reference getWeight equivalent).
float getDenoiseWeight(const HitInfo& Gp, const HitInfo& Gq,
                       const RadianceData& Lp, const RadianceData& Lq,
                       const float3& LmP, const float3& LmQ,
                       float lumLP, float lumLQ, float varMed,
                       bool specular) {
    static constexpr float sigma_z  = 1.0f;
    static constexpr float sigma_n  = 1024.0f;
    static constexpr float sigma_l  = 1.0f;
    static constexpr float sigma_m  = 1.0f;
    static constexpr float eps      = 1e-2f;
    static constexpr float eps_r    = 4e-2f;
    static constexpr float eps_zero = 1e-6f;

    if (!isfinite(Gq.position.x)) return 0.0f;

    // Normal power term: the reference gates on the smooth vertex normal.
    // For DIFFUSE channels use the GEOMETRY (shape) normal — the shading
    // normal carries normal-map perturbation, and with sigma_n=1024 a 5
    // degree texel-to-texel wobble zeroes the weight, which silently
    // reduced the indirect-diffuse pass to a near-passthrough. Specular
    // keeps the shading normal (glossy detail must not smear).
    const float3& Np = specular ? Gp.surfaceNormal : Gp.shapeNormal;
    const float3& Nq = specular ? Gq.surfaceNormal : Gq.shapeNormal;
    float dotN = fmaxf(glm::dot(Np, Nq), 0.0f);
    float weight = powf(dotN, sigma_n);
    if (weight < 1e-6f) return 0.0f;

    float k = 0.0f;

    // Position/plane rejection via shapeNormal — stops cross-geometry bleeding.
    float3 dir = Gq.position - Gp.position;
    if (glm::length(dir) > 1e-8f) {
        dir = glm::normalize(dir);
        float sinTheta = fabsf(glm::dot(Gp.shapeNormal, dir));
        float cosTheta = sqrtf(fmaxf(1.0f - sinTheta * sinTheta, 0.0f));
        float tanTheta = sinTheta / (cosTheta + eps_zero);
        k += -tanTheta / sigma_z;
    }

    // Variance-aware radiance difference (reference): normalized by sqrt(Var).
    // CRITICAL floor: at 320spp the MC noise sigma (~sqrt(Var)) matches
    // sigL, so the noisy CENTER pixel rejects its equally-noisy neighbors
    // and the filter degenerates to a passthrough. The floor lets flat
    // regions average hard; texture survival is handled by the albedo
    // term below, not by tight radiance gating.
    // Radiance difference on the 3x3 LOCAL MEAN (Lm), not the noisy center.
    // Lm is ~3x less noisy, so MC grain does not self-reject its neighbors,
    // while a shadow boundary still shows a large Lm difference on both
    // sides and keeps edge stopping (contact shadows survive the filter).
    // GENERAL FORM: all thresholds are RELATIVE — to the local noise
    // (sqrt(Var)/3), the local brightness (luminance of the center) and the
    // per-image noise scale (median sqrt(Var), in g_varMed). The AOVs are
    // additionally normalized to median luminance 1.0 before filtering, so
    // the same constants apply to any scene brightness and any spp.
    const float varCapped = fminf(fmaxf(Lp.Var, 0.0f), 512.0f * varMed);
    float sigmaL = fmaxf(sigma_l * sqrtf(varCapped) / 3.0f,
                  fmaxf(0.05f * lumLP, 0.10f * sqrtf(varMed))) + eps;
    k += -glm::length(LmP - LmQ) / sigmaL;

    // Material difference (metallic / specular / opacity).
    float materialDiff = glm::length(float3(Gp.metallic - Gq.metallic,
                                             Gp.specular  - Gq.specular,
                                             Gp.opacity   - Gq.opacity));
    k += -materialDiff / sigma_m;

    // Albedo similarity: wood grain / fabric weave ARE albedo variation. The
    // radiance-difference term cannot tell MC grain from texture (at 320spp
    // their magnitudes match), but the first-hit albedo can: refuse to average
    // across albedo edges so texture survives while flat-albedo MC noise is
    // averaged away. (Offline AOV-lab: corr(detail, albedo) 0.09 -> 0.54.)
    float albedoDiff = glm::length(Gp.baseColor - Gq.baseColor) /
                       (glm::length(Gp.baseColor) + 0.02f);
    k += -albedoDiff * albedoDiff / (2.0f * 0.15f * 0.15f);

    if (k < -7.5f) return 0.0f;
    weight *= expf(k);

    // Specular lobes: gate by roughness so glossy reflections are not smeared.
    if (specular) weight *= fmaxf(Gp.roughness, eps_r);

    if (!isfinite(weight)) return 0.0f;
    return weight;
}

void filterRadiance(RadianceData* data, const HitInfo* Gbuffer,
                    int width, int height, bool specular, int pass,
                    float varMed, const float* mcVar) {
    if (!data || !Gbuffer) return;

    // Reference 5x5 Gaussian kernel; multi-scale reach via the sampling step.
    constexpr int filterRadius = 2;
    static const float h[5] = {0.0625f, 0.25f, 0.375f, 0.25f, 0.0625f};
    // D: filter in log domain — radiance noise is closer to Gaussian there,
    // so bright specular/highlight regions denoise without smearing them.
    static constexpr float LOG_KEEP = 0.01f;  // AOV sum must stay unchanged
    const size_t size = size_t(width) * height;

    std::vector<RadianceData> temp(size);
    for (size_t i = 0; i < size; ++i) {
        temp[i].Var = data[i].Var;
        float3 r = data[i].radiance;
        temp[i].radiance = float3(glm::log(1.0f + r.x),
                                       glm::log(1.0f + r.y),
                                       glm::log(1.0f + r.z));
    }
    std::vector<float3> radBuf(size, float3(0.0f));
    std::vector<float>  varBuf(size, 0.0f);

    // Per-pixel linear luminance (for the brightness-relative sigmaL floor).
    std::vector<float> lumLin(size);
    parallelForRows(height, [&](int y0, int y1) {
        for (size_t i = size_t(y0) * width; i < size_t(y1) * width; ++i)
            lumLin[i] = luminance(data[i].radiance);
    });

    // 3x3 box mean of the log radiance — a noise-stable reference for the
    // radiance-difference weight (edge stopping without noise self-rejection).
    std::vector<float3> Lm(size);
    parallelForRows(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
            for (int x = 0; x < width; ++x) {
                float3 acc(0.0f);
                int cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        int sx = std::min(width - 1, std::max(0, x + dx));
                        int sy = std::min(height - 1, std::max(0, y + dy));
                        acc += temp[size_t(sy) * width + sx].radiance;
                        ++cnt;
                    }
                Lm[size_t(y) * width + x] = acc / float(cnt);
            }
    });

    parallelForRows(height, [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        for (int x = 0; x < width; ++x) {
            size_t idx = size_t(y) * width + x;
            const HitInfo& Gp = Gbuffer[idx];
            const RadianceData& Lp = temp[idx];
            if (!isfinite(Gp.position.x)) { radBuf[idx] = Lp.radiance; varBuf[idx] = Lp.Var; continue; }

            // E: adaptive footprint — noisier pixels average over a wider
            // reach so flat walls/ceilings clean up while texture keeps its
            // base step. Var is per-pass updated, so noise shrinks naturally.
            int step = 1 << pass;
            // Cap the reach: step up to 64 samples +/-256px away, and small
            // contact shadows get averaged away from every side ("floating
            // chairs"). 16 caps the reach at +/-64px. The variance thresholds
            // are RELATIVE to this AOV's median Var — scale- and spp-invariant.
            if (Lp.Var > 12.0f * varMed)     step = glm::min(step * 4, 16);
            else if (Lp.Var > 3.0f * varMed) step = glm::min(step * 2, 16);

            float3 sumRad(0.0f);
            float sumW = 0.0f, sumVar = 0.0f;
            for (int dy = -filterRadius; dy <= filterRadius; ++dy)
                for (int dx = -filterRadius; dx <= filterRadius; ++dx) {
                    int nx = x + dx * step, ny = y + dy * step;
                    if (nx < 0 || nx >= width || ny < 0 || ny >= height) continue;
                    size_t nidx = size_t(ny) * width + nx;
                    const RadianceData& Lq = temp[nidx];
                    float w = h[dx + filterRadius] * h[dy + filterRadius];
                    if (dx != 0 || dy != 0)
                        w *= getDenoiseWeight(Gp, Gbuffer[nidx], Lp, Lq,
                                              Lm[idx], Lm[nidx],
                                              lumLin[idx], lumLin[nidx], varMed,
                                              specular);
                    sumRad += Lq.radiance * w;
                    // NL-means variance propagation: weight then /sumW^2.
                    sumVar += Lq.Var * w * w;
                    sumW += w;
                }
            if (sumW > 1e-8f) {
                radBuf[idx] = sumRad / sumW;
                varBuf[idx] = sumVar / (sumW * sumW);
            } else {
                radBuf[idx] = Lp.radiance;
                varBuf[idx] = Lp.Var;
            }
        }
    });
    for (size_t i = 0; i < size; ++i) {
        // Restore from log-domain mean (AOV sum stays unchanged in expectation)
        float3 filtered = float3(::expf(radBuf[i].x) - 1.0f,
                                 ::expf(radBuf[i].y) - 1.0f,
                                 ::expf(radBuf[i].z) - 1.0f);
        // Variance-guided blend: a pixel whose pre-pass relative MC noise is
        // well under the target is already clean — full filtering here only
        // erodes low-contrast texture. Fireflies (huge Var) still blend to 1.
        // g_denoiseNoiseTarget <= 0 disables the blend (legacy behaviour).
        // IMPORTANT: use the TRUE MC sample variance (mcVar snapshot taken
        // before filterVar) — filterVar replaces Var with SPATIAL variance,
        // which on textured regions stays high at any spp (texture is
        // signal, not noise) and would keep the blend pinned at 1.
        float3 orig = data[i].radiance;
        if (g_denoiseNoiseTarget > 0.0f) {
            float vMC = mcVar ? mcVar[i] : temp[i].Var;
            float noiseFrac = sqrtf(fmaxf(vMC, 0.0f)) / (lumLin[i] + 1e-4f);
            float blend = (noiseFrac - 0.4f * g_denoiseNoiseTarget) /
                          (1.2f * g_denoiseNoiseTarget);
            blend = fminf(fmaxf(blend, 0.0f), 1.0f);
            filtered = orig + (filtered - orig) * blend;
        }
        data[i].radiance = filtered;
        data[i].Var = varBuf[i];
    }
}
