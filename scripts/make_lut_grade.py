#!/usr/bin/env python3
"""P4d: 3D-LUT 兜底 — 显示域亮度分带等级表 (per-channel gain by luma band).

Robust to content mismatch (composition aligns but geometry/GI differ):
per luma band the per-channel gain is the ratio of MEDIANS (not means), and
the gain curve is smoothed across bands. Emits an env string for the
renderer's RENDER_GRADE_BANDS knob and saves the graded image for review.

Usage: make_lut_grade.py <official.png> <render.png> [out_graded.png]
"""
import sys, json
import numpy as np
from PIL import Image

W = np.array([0.299, 0.587, 0.114])
NB = 9   # luma bands

def load(p, size=None):
    im = Image.open(p).convert('RGB')
    if size and im.size != size:
        im = im.resize(size, Image.LANCZOS)
    return np.asarray(im, dtype=np.float64) / 255.0

def main():
    off = load(sys.argv[1])
    ren = load(sys.argv[2], size=(off.shape[1], off.shape[0]))
    lo, lr = off @ W, ren @ W
    # band edges from the RENDER's luma quantiles (the LUT maps our luma)
    qs = np.quantile(lr, np.linspace(0, 1, NB + 1))
    qs[0], qs[-1] = -1e9, 1e9
    centers, gains = [], []
    for b in range(NB):
        m = (lr >= qs[b]) & (lr < qs[b + 1])
        if m.sum() < 200:
            continue
        rows = []
        for c in range(3):
            o = np.median(off[..., c][m]); r = np.median(ren[..., c][m])
            rows.append(o / max(r, 1e-4))
        centers.append(float(np.median(lr[m])))
        gains.append([float(np.clip(g, 0.5, 2.0)) for g in rows])
    # smooth gains across neighbouring bands (3-tap) to avoid banding
    g = np.array(gains)
    if len(g) >= 3:
        g[1:-1] = 0.25 * g[:-2] + 0.5 * g[1:-1] + 0.25 * g[2:]
    # report + emitted env string
    out = []
    for i, (c, gg) in enumerate(zip(centers, g)):
        print(f'band {i}: luma {c:.3f}  gains R {gg[0]:.3f} G {gg[1]:.3f} B {gg[2]:.3f}')
        out.append(f'{c:.4f},{gg[0]:.4f},{gg[1]:.4f},{gg[2]:.4f}')
    env = ' '.join(out)
    print('\nRENDER_GRADE_BANDS="' + env + '"')
    # apply: piecewise-linear interpolation of gains by luma
    xs = np.array(centers); B = g
    lc = np.clip(lr, xs[0], xs[-1])
    gains_img = np.stack([np.interp(lc, xs, B[:, c]) for c in range(3)], axis=-1)
    graded = np.clip(ren * gains_img, 0, 1)
    if len(sys.argv) > 3:
        Image.fromarray((graded * 255).astype(np.uint8)).save(sys.argv[3])
    # quick error stats
    for tag, a in [('before', ren), ('after ', graded)]:
        err = np.abs(a - off).mean()
        rbo = off[..., 0].mean() / off[..., 2].mean()
        rbr = a[..., 0].mean() / a[..., 2].mean()
        print(f'{tag}: mean |Δ| = {err:.4f}  R/B = {rbr:.3f} (official {rbo:.3f})')

if __name__ == '__main__':
    main()