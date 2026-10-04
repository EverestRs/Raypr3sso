#!/usr/bin/env python3
"""Material parameter audit from gbuffer dump + improved denoiser variants."""
import numpy as np
import os, sys
from numpy.lib.stride_tricks import sliding_window_view

AOV = sys.argv[1] if len(sys.argv) > 1 else '/tmp/aov_bistro/aov'
W = H = None

def load(name, ch):
    a = np.fromfile(os.path.join(AOV, name), dtype=np.float32)
    return a.reshape(-1, ch)

meta = open(os.path.join(AOV, 'meta.txt')).read().split()
W, H, SPP = int(meta[0]), int(meta[1]), int(meta[2])
gb  = load('gbuffer.f32', 17).reshape(H, W, 17)
alb = gb[..., 9:12]; rough = gb[..., 12]; metal = gb[..., 13]
opac = gb[..., 15]; matid = gb[..., 16]
Dd = load('rad_Dd.f32', 3).reshape(H, W, 3)
Ds = load('rad_Ds.f32', 3).reshape(H, W, 3)
Id = load('rad_Id.f32', 3).reshape(H, W, 3)
Is = load('rad_Is.f32', 3).reshape(H, W, 3)
vDd = load('var_Dd.f32', 1).reshape(H, W)
vDs = load('var_Ds.f32', 1).reshape(H, W)
vId = load('var_Id.f32', 1).reshape(H, W)
vIs = load('var_Is.f32', 1).reshape(H, W)
pos = gb[..., 0:3]; snrm = gb[..., 3:6]; nrm = gb[..., 6:9]; spec = gb[..., 14]
lum = np.array([0.299, 0.587, 0.114])
lums = lambda a: a @ lum

# --- downsample 2x for lab speed (denoiser behavior is spp-driven, not res-driven) ---
DS = 2
H0, W0 = H, W
H, W = H // DS, W // DS
def ds(a):
    if a.ndim == 3: return a[:H*DS, :W*DS].reshape(H, DS, W, DS, -1).mean(axis=(1, 3))
    return a[:H*DS, :W*DS].reshape(H, DS, W, DS).mean(axis=(1, 3))
Dd, Ds, Id, Is = map(ds, (Dd, Ds, Id, Is))
vDd, vDs, vId, vIs = map(ds, (vDd, vDs, vId, vIs))
pos, snrm, nrm, alb = map(ds, (pos, snrm, nrm, alb))
rough, metal, spec, opac = map(ds, (rough, metal, spec, opac))

# ================= material audit =================
print('=== MATERIAL PARAMETER AUDIT (problem 1) ===')
m = metal.ravel(); r = rough.ravel()
print(f'metallic: mean={m.mean():.3f}  %>0.5={100*(m>0.5).mean():.1f}%  %>0.8={100*(m>0.8).mean():.1f}%  ==0: {100*(m<0.01).mean():.1f}%')
print(f'roughness: mean={r.mean():.3f}  <0.1: {100*(r<0.1).mean():.1f}%  0.1-0.4: {100*((r>=0.1)&(r<0.4)).mean():.1f}%  >0.6: {100*(r>0.6).mean():.1f}%')
# distinct materials and their params (matID is unfilled in the dump; use
# a coarse position grid as a pseudo-material grouping instead)
ids = (pos[..., 0].astype(np.int32) * 7 + pos[..., 1].astype(np.int32) * 13
       + pos[..., 2].astype(np.int32) * 3).ravel()
rough_f = rough.ravel(); metal_f = metal.ravel(); opac_f = opac.ravel(); alb_f = alb.reshape(-1, 3)
uniq, counts = np.unique(ids, return_counts=True)
print(f'distinct position cells: {len(uniq)}')
rows = []
for u, c in zip(uniq, counts):
    if c < 2000: continue
    sel = ids == u
    rows.append((c, u, rough_f[sel].mean(), metal_f[sel].mean(),
                 alb_f[sel].mean(0), opac_f[sel].mean()))
rows.sort(reverse=True)
print(f'{"cell":>8} {"pixels":>8} {"rough":>6} {"metal":>6} {"albedo RGB":>18} {"opac":>5}')
for c, u, rm, mm, am, om in rows[:12]:
    print(f'{u:8d} {c:8d} {rm:6.2f} {mm:6.2f}   ({am[0]:.2f},{am[1]:.2f},{am[2]:.2f}) {om:5.2f}')

# ================= denoiser lab =================
H_, W_ = H, W
def local_std(a, k=2):
    pad = np.pad(a, k, mode='edge')
    win = sliding_window_view(pad, (2*k+1, 2*k+1))
    return win.std(axis=(2, 3))
def hf_residual(a):
    k = 2
    pad = np.pad(a, k, mode='edge')
    win = sliding_window_view(pad, (5, 5))
    return np.abs(a - win.mean(axis=(-2, -1)))

astd = local_std(lums(alb))
flat = astd < 0.005
text = astd > 0.02

def metrics(final_lum, label):
    fhf = hf_residual(final_lum)
    fstd = local_std(final_lum)
    c = np.corrcoef(astd[text], fstd[text])[0, 1] if text.sum() > 100 else 0
    print(f'{label:34s} hf_flat={fhf[flat].mean():.4f}  grain_gain={fstd[text].mean()/astd[text].mean():.2f}  corr={c:.3f}')
    return fhf[flat].mean()

def firefly_clamp(A, k=6.0):
    """Local outlier clamp: luminance > k x 3x3-median of neighbors -> clamp."""
    L = lums(A)
    pad = np.pad(L, 1, mode='edge')
    win = sliding_window_view(pad, (3,3))
    med = np.median(win, axis=(-2,-1))
    scale = np.minimum(1.0, (k * np.maximum(med, 1e-3)) / np.maximum(L, 1e-6))
    return A * scale[..., None]

def denoise(A, V, specular, sig_a=0.0, passes=(1,2,4,8,16), var_cap=0.018,
            use_albedo_w=True, preclamp=True):
    """Improved filter: current C++ port + optional albedo-similarity weight +
    optional firefly pre-clamp. passes: list of step sizes."""
    eps, eps_r, eps_zero = 1e-2, 4e-2, 1e-6
    h5 = np.array([0.0625, 0.25, 0.375, 0.25, 0.0625])
    A = firefly_clamp(A).astype(np.float32) if preclamp else A.astype(np.float32)
    V = V.astype(np.float32).copy()
    L = np.log1p(A)
    for p, base_step in enumerate(passes):
        step = np.where(V > 0.0036, np.minimum(base_step*4, 64),
               np.where(V > 0.0007, np.minimum(base_step*2, 64), base_step)).astype(int)
        acc = np.zeros_like(L); accw = np.zeros((H_, W_), np.float32); accv = np.zeros_like(V)
        sigL = np.sqrt(np.minimum(np.maximum(V,0), var_cap)) + eps
        ys, xs = np.indices((H_, W_))
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ny = np.clip(ys + dy*step, 0, H_-1); nx = np.clip(xs + dx*step, 0, W_-1)
                ok = (ys+dy*step>=0)&(ys+dy*step<H_)&(xs+dx*step>=0)&(xs+dx*step<W_)
                w = h5[dx+2]*h5[dy+2]
                if dx==0 and dy==0:
                    acc += L*w; accw += w; accv += V*w*w; continue
                sh, shv = L[ny,nx], V[ny,nx]
                dotN = np.maximum((nrm*nrm[ny,nx]).sum(-1), 0)
                wt = np.power(np.maximum(dotN,0), 1024.0)
                direction = pos[ny,nx] - pos
                dirlen = np.linalg.norm(direction, axis=-1)+1e-12
                dirn = direction/dirlen[...,None]
                sinTheta = np.abs((snrm*dirn).sum(-1))
                cosTheta = np.sqrt(np.maximum(1-sinTheta**2,0))
                k = -(sinTheta/(cosTheta+eps_zero))
                dl = np.linalg.norm(L-sh, axis=-1)
                k += -dl/sigL
                if use_albedo_w and sig_a > 0:
                    da = np.linalg.norm(alb - alb[ny,nx], axis=-1)/(np.linalg.norm(alb,axis=-1)+0.02)
                    k += -da*da/(2*sig_a*sig_a)
                mdiff = np.sqrt((metal-metal[ny,nx])**2+(spec-spec[ny,nx])**2+(opac-opac[ny,nx])**2)
                k += -mdiff
                wq = np.where(k < -7.5, 0.0, wt*np.exp(np.maximum(k,-20)))
                if specular: wq = wq*np.maximum(rough, eps_r)
                wq = np.where(ok, wq, 0.0)
                acc += sh*wq[...,None]; accw += wq; accv += shv*wq*wq
        okm = accw > 1e-8
        Lnew = np.where(okm[...,None], acc/np.maximum(accw,1e-8)[...,None], L)
        Vnew = np.where(okm, accv/np.maximum(accw,1e-8)**2, V)
        L, V = Lnew, Vnew
    return np.expm1(L)

print()
print('=== DENOISER VARIANTS (problem 3+4) ===')
comp = Dd+Ds+Id+Is
metrics(lums(comp), 'raw (no denoise)')

chans = [(Dd,vDd,False),(Ds,vDs,True),(Id,vId,False),(Is,vIs,True)]
def run(variant, **kw):
    outs = [denoise(A,V,sp,**kw) for A,V,sp in chans]
    return outs[0]+outs[1]+outs[2]+outs[3]

metrics(lums(run('cur', sig_a=0, preclamp=False, passes=(1,2,4,8,16))), 'C++ port (no albedo w)')
for sa in (0.10, 0.15, 0.25):
    metrics(lums(run('a', sig_a=sa, preclamp=True)), f'albedo w sig_a={sa} + preclamp')
