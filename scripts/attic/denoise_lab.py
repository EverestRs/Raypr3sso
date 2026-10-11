#!/usr/bin/env python3
"""Offline denoiser lab: load AOV dump, replicate the C++ denoiser, test variants.

Metrics:
  hf_flat  — mean 5x5 high-freq residual on flat-albedo pixels (MC noise; lower=better)
  grain_gain — ratio of final detail to albedo detail on textured pixels (~1 = grain preserved)
  corr     — corr(local detail, albedo local detail) (higher = detail is texture, not noise)
"""
import numpy as np
import sys, os
from numpy.lib.stride_tricks import sliding_window_view

AOV = sys.argv[1] if len(sys.argv) > 1 else 'output_gpu/render_bistro_dump/aov'
W = H = None

def load(name, ch):
    a = np.fromfile(os.path.join(AOV, name), dtype=np.float32)
    return a.reshape(-1, ch)

meta = open(os.path.join(AOV, 'meta.txt')).read().split()
W, H, SPP = int(meta[0]), int(meta[1]), int(meta[2])
N = W * H
Dd = load('rad_Dd.f32', 3).reshape(H, W, 3)
Ds = load('rad_Ds.f32', 3).reshape(H, W, 3)
Id = load('rad_Id.f32', 3).reshape(H, W, 3)
Is = load('rad_Is.f32', 3).reshape(H, W, 3)
vDd = load('var_Dd.f32', 1).reshape(H, W)
vDs = load('var_Ds.f32', 1).reshape(H, W)
vId = load('var_Id.f32', 1).reshape(H, W)
vIs = load('var_Is.f32', 1).reshape(H, W)
gb  = load('gbuffer.f32', 17).reshape(H, W, 17)
pos = gb[..., 0:3]; snrm = gb[..., 3:6]; nrm = gb[..., 6:9]
alb = gb[..., 9:12]; rough = gb[..., 12]; metal = gb[..., 13]
spec = gb[..., 14]; opac = gb[..., 15]

lum = np.array([0.299, 0.587, 0.114])

def lums(a): return a @ lum

# ---------- reference metrics helpers ----------
def hf_residual(a, k=2):
    pad = np.pad(a, ((k, k), (k, k), (0, 0)) if a.ndim == 3 else ((k, k), (k, k)), mode='edge')
    win = sliding_window_view(pad, (2*k+1, 2*k+1, a.shape[2]) if a.ndim == 3 else (2*k+1, 2*k+1))
    mean = win.mean(axis=(-3, -2)) if a.ndim == 3 else win.mean(axis=(-2, -1))
    return np.abs(a - mean)

def local_std(a, k=2):
    pad = np.pad(a, k, mode='edge')
    win = sliding_window_view(pad, (2*k+1, 2*k+1))
    return win.std(axis=(2, 3))

astd = local_std(lums(alb))
flat = astd < 0.005
text = astd > 0.02

def metrics(final_lum, label):
    fhf = hf_residual(final_lum[..., None].repeat(3, -1) if final_lum.ndim == 2 else final_lum)[:, :, 0] \
        if final_lum.ndim == 3 else hf_residual(final_lum)
    fstd = local_std(final_lum)
    c = np.corrcoef(astd[text], fstd[text])[0, 1] if text.sum() > 100 else 0
    print(f'{label:28s} hf_flat={fhf[flat].mean():.4f}  grain_gain={fstd[text].mean()/astd[text].mean():.2f}  corr={c:.3f}')
    return fhf[flat].mean()

# ---------- current C++ denoiser (faithful port) ----------
def denoise_current(A, V, G, specular, n_pass=5):
    """Port of image.cpp filterRadiance chain. A: HxWx3 radiance, V: HxW var.
    G: dict with nrm, snrm, pos, rough, metal, spec, opac."""
    sig_n, sig_l, sig_m = 1024.0, 1.0, 1.0
    eps, eps_r, eps_zero = 1e-2, 4e-2, 1e-6
    h5 = np.array([0.0625, 0.25, 0.375, 0.25, 0.0625])
    A = A.copy(); V = V.copy()
    # log domain
    L = np.log1p(A)
    for p in range(n_pass):
        step = 1 << p
        step = np.where(V > 0.0036, np.minimum(step*4, 64), np.where(V > 0.0007, np.minimum(step*2, 64), step))
        step = step.astype(int)
        Lnew = np.empty_like(L); Vnew = np.empty_like(V)
        # shift-based accumulation (5x5 kernel)
        acc = np.zeros_like(L); accw = np.zeros_like(V); accv = np.zeros_like(V)
        varcap = np.minimum(np.maximum(V, 0), 0.018)
        sigL = sig_l * np.sqrt(varcap) + eps
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                # neighbor arrays shifted by dy*step, dx*step (per-pixel step)
                sh = np.zeros_like(L); shv = np.zeros_like(V); valid = np.zeros((H, W), bool)
                if dx == 0 and dy == 0:
                    sh, shv, valid = L, V, np.ones((H, W), bool)
                else:
                    ys, xs = np.indices((H, W))
                    ny = np.clip(ys + dy * step, 0, H-1); nx = np.clip(xs + dx * step, 0, W-1)
                    ok = (ys + dy*step >= 0) & (ys + dy*step < H) & (xs + dx*step >= 0) & (xs + dx*step < W)
                    sh, shv, valid = L[ny, nx], V[ny, nx], ok
                w = h5[dx+2] * h5[dy+2]
                if dx == 0 and dy == 0:
                    acc += sh * w; accw += w; accv += shv * w * w
                    continue
                dotN = np.maximum((nrm * nrm[ny, nx]).sum(-1), 0)
                wt = np.power(np.maximum(dotN, 0), sig_n)
                direction = pos[ny, nx] - pos
                dirlen = np.linalg.norm(direction, axis=-1) + 1e-12
                dirn = direction / dirlen[..., None]
                sinTheta = np.abs((snrm * dirn).sum(-1))
                cosTheta = np.sqrt(np.maximum(1 - sinTheta**2, 0))
                tanTheta = sinTheta / (cosTheta + eps_zero)
                k = -tanTheta / 1.0
                dl = np.linalg.norm(L - L[ny, nx], axis=-1)
                k += -dl / sigL
                mdiff = np.sqrt((metal - metal[ny, nx])**2 + (spec - spec[ny, nx])**2 + (opac - opac[ny, nx])**2)
                k += -mdiff / sig_m
                wq = np.where(k < -7.5, 0.0, wt * np.exp(np.maximum(k, -20)))
                if specular:
                    wq = wq * np.maximum(rough, eps_r)
                wq = np.where(valid, wq, 0.0)
                acc += sh * wq[..., None]; accw += wq; accv += shv * wq * wq
        okm = accw > 1e-8
        Lnew[okm] = (acc[okm] / accw[okm, None])
        Vnew[okm] = accv[okm] / (accw[okm]**2)
        Lnew[~okm] = L[~okm]; Vnew[~okm] = V[~okm]
        L, V = Lnew, Vnew
    return np.expm1(L), V

print('=== baselines ===')
comp = Dd + Ds + Id + Is
metrics(lums(comp), 'raw (no denoise)')
print()
print('=== current C++ denoiser (port) ===')
for name, (A, V, sp) in [('Dd', (Dd, vDd, False)), ('Ds', (Ds, vDs, True)),
                          ('Id', (Id, vId, False)), ('Is', (Is, vIs, True))]:
    Af, Vf = denoise_current(A, V, None, sp)
    if name == 'Dd': fDd = Af
    if name == 'Ds': fDs = Af
    if name == 'Id': fId = Af
    if name == 'Is': fIs = Af
comp_f = fDd + fDs + fId + fIs
metrics(lums(comp_f), 'current denoiser')
np.save('/tmp/dn_current.npy', comp_f)
