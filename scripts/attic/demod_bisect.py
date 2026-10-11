#!/usr/bin/env python3
"""Bisect C++ demod vs lab demod using cached npz."""
import numpy as np
from numpy.lib.stride_tricks import sliding_window_view

d = np.load('/tmp/lab_data.npz')
Dd, Ds, Id, Is = d['Dd'], d['Ds'], d['Id'], d['Is']
vDd, vDs, vId, vIs = d['vDd'], d['vDs'], d['vId'], d['vIs']
pos, snrm, nrm, alb = d['pos'], d['snrm'], d['nrm'], d['alb']
rough, metal, spec, opac = d['rough'], d['metal'], d['spec'], d['opac']
H, W = alb.shape[0], alb.shape[1]
lumw = np.array([0.299, 0.587, 0.114])
lums = lambda a: a @ lumw

def local_std(a, k=2):
    pad = np.pad(a, k, mode='edge')
    win = sliding_window_view(pad, (2*k+1, 2*k+1))
    return win.std(axis=(2, 3))
def hf(a, k=2):
    pad = np.pad(a, k, mode='edge')
    win = sliding_window_view(pad, (5, 5))
    return np.abs(a - win.mean(axis=(-2, -1)))
astd = local_std(lums(alb))
flat = astd < 0.005
text = astd > 0.02
def metrics(L, label):
    fstd = local_std(L)
    c = np.corrcoef(astd[text], fstd[text])[0, 1]
    print(f'{label:44s} hf_flat={hf(L)[flat].mean():.4f} gain={fstd[text].mean()/astd[text].mean():.2f} corr={c:.3f}')

def firefly_clamp(A, k=6.0):
    L = lums(A)
    pad = np.pad(L, 1, mode='edge')
    win = sliding_window_view(pad, (3, 3))
    med = np.median(win, axis=(-2, -1))
    scale = np.minimum(1.0, (k * np.maximum(med, 1e-3)) / np.maximum(L, 1e-6))
    return A * scale[..., None]

def smooth_filter(X, V, passes, sig_floor, norm_pow=64.0, var_cap=0.05):
    eps = 1e-2
    h5 = np.array([0.0625, 0.25, 0.375, 0.25, 0.0625])
    L = np.log1p(np.maximum(X, 0))
    V = V.astype(np.float32).copy()
    ys, xs = np.indices((H, W))
    for base in passes:
        step = np.where(V > 0.0036, np.minimum(base*4, 64),
               np.where(V > 0.0007, np.minimum(base*2, 64), base)).astype(int)
        acc = np.zeros_like(L); accw = np.zeros((H, W), np.float32)
        sigL = np.sqrt(np.minimum(np.maximum(V, 0), var_cap)) * 0.5 + sig_floor
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ny = np.clip(ys+dy*step, 0, H-1); nx = np.clip(xs+dx*step, 0, W-1)
                ok = (ys+dy*step >= 0)&(ys+dy*step < H)&(xs+dx*step >= 0)&(xs+dx*step < W)
                w = h5[dx+2]*h5[dy+2]
                if dx == 0 and dy == 0:
                    acc += L*w; accw += w; continue
                dotN = np.maximum((nrm*nrm[ny, nx]).sum(-1), 0)
                wt = np.power(np.maximum(dotN, 0), norm_pow)
                direction = pos[ny, nx] - pos
                dirn = direction/(np.linalg.norm(direction, axis=-1)[..., None]+1e-12)
                sinT = np.abs((snrm*dirn).sum(-1))
                cosT = np.sqrt(np.maximum(1-sinT**2, 0))
                k = -(sinT/(cosT+1e-6))
                dl = np.linalg.norm(L-L[ny, nx], axis=-1)
                k += -dl/sigL
                wq = np.where(k < -7.5, 0, wt*np.exp(np.maximum(k, -20)))
                acc += L[ny, nx]*wq[..., None]*ok[..., None]; accw += wq*ok
        okm = accw > 1e-8
        L = np.where(okm[..., None], acc/np.maximum(accw, 1e-8)[..., None], L)
    return np.expm1(L)

def spec_filt(A, V, sig_a=0.15, preclamp=True, passes=(1,2,4,8,16,18,20)):
    eps, eps_r, eps_zero = 1e-2, 4e-2, 1e-6
    h5 = np.array([0.0625, 0.25, 0.375, 0.25, 0.0625])
    A = firefly_clamp(A) if preclamp else A.astype(np.float32)
    V = V.astype(np.float32).copy()
    L = np.log1p(A)
    ys, xs = np.indices((H, W))
    for base in passes:
        step = np.where(V > 0.0036, np.minimum(base*4, 64),
               np.where(V > 0.0007, np.minimum(base*2, 64), base)).astype(int)
        acc = np.zeros_like(L); accw = np.zeros((H, W), np.float32)
        sigL = np.sqrt(np.minimum(np.maximum(V, 0), 0.018)) + eps
        for dy in range(-2, 3):
            for dx in range(-2, 3):
                ny = np.clip(ys+dy*step, 0, H-1); nx = np.clip(xs+dx*step, 0, W-1)
                ok = (ys+dy*step>=0)&(ys+dy*step<H)&(xs+dx*step>=0)&(xs+dx*step<W)
                w = h5[dx+2]*h5[dy+2]
                if dx == 0 and dy == 0:
                    acc += L*w; accw += w; continue
                dotN = np.maximum((nrm*nrm[ny, nx]).sum(-1), 0)
                wt = np.power(np.maximum(dotN, 0), 1024.0)
                direction = pos[ny, nx]-pos
                dirn = direction/(np.linalg.norm(direction, axis=-1)[..., None]+1e-12)
                sinT = np.abs((snrm*dirn).sum(-1))
                cosT = np.sqrt(np.maximum(1-sinT**2, 0))
                k = -(sinT/(cosT+eps_zero))
                dl = np.linalg.norm(L-L[ny, nx], axis=-1)
                k += -dl/sigL
                da = np.linalg.norm(alb-alb[ny, nx], axis=-1)/(np.linalg.norm(alb, axis=-1)+0.02)
                k += -da*da/(2*sig_a*sig_a)
                md = np.sqrt((metal-metal[ny, nx])**2+(spec-spec[ny, nx])**2+(opac-opac[ny, nx])**2)
                k += -md
                wq = np.where(k < -7.5, 0, wt*np.exp(np.maximum(k, -20)))
                wq = wq*np.maximum(rough, eps_r)
                wq = np.where(ok, wq, 0.0)
                acc += L[ny, nx]*wq[..., None]; accw += wq
        okm = accw > 1e-8
        L = np.where(okm[..., None], acc/np.maximum(accw, 1e-8)[..., None], L)
    return np.expm1(L)

def demod(D, alb, V, scale_var):
    a = np.maximum(alb, 0.08)
    irr = D / a
    Vs = V/np.maximum((a**2).mean(-1), 1e-4) if scale_var else V
    irr_f = smooth_filter(irr, Vs, passes=(2, 4, 8, 16, 32, 64), sig_floor=0.35)
    return a * irr_f

metrics(lums(Dd+Ds+Id+Is), 'raw')
metrics(lums(Dd), 'raw Dd only')
D1 = demod(firefly_clamp(Dd), alb, vDd, False)
metrics(lums(D1), 'demod Dd (lab, V unscaled)')
D2 = demod(firefly_clamp(Dd), alb, vDd, True)
metrics(lums(D2), 'demod Dd (cpp, V scaled 1/a^2)')
I1 = demod(firefly_clamp(Id), alb, vId, False)
metrics(lums(I1), 'demod Id (lab)')
S1 = spec_filt(Ds, vDs)
P1 = spec_filt(Is, vIs)
metrics(lums(D1+S1+I1+P1), 'lab full composite')
metrics(lums(D2+S1+I2+P1), 'cpp-style full composite')
