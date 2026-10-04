#!/usr/bin/env python3
"""Compare our Bistro exterior render with the official reference image.

Metrics (display domain, both PNGs):
  global median/mean luminance, blown-out fraction, R/B ratio
  zone medians: cobblestone foreground, green facade, red awning, top strip
  hf_flat: 5x5 high-frequency residual on locally-flat pixels (visible noise)
Also writes a side-by-side composite for eyeballing.

Usage: compare_official.py <official.png> <render.png> [sbs_out.png]
"""
import sys
import numpy as np
from PIL import Image

LUM = np.array([0.299, 0.587, 0.114])

def load(p):
    return np.asarray(Image.open(p).convert('RGB'), dtype=np.float64) / 255.0

def lum(a): return a @ LUM

def zone(a, x0, x1, y0, y1):
    h, w = a.shape[:2]
    return a[int(y0*h):int(y1*h), int(x0*w):int(x1*w)]

def med(a): return float(np.median(lum(a.reshape(-1, 3))))

def hf_flat(a):
    """Mean |x - 5x5 mean| on pixels whose 5x5 local luminance std is small
    (flat regions — where MC noise is visible), normalized by local level."""
    l = lum(a)
    from numpy.lib.stride_tricks import sliding_window_view
    win = sliding_window_view(np.pad(l, 2, mode='edge'), (5, 5))
    mu = win.mean(axis=(-2, -1))
    sd = win.std(axis=(-2, -1))
    flat = sd < 0.02
    if flat.sum() < 100:
        return float('nan')
    res = np.abs(l - mu)
    return float((res[flat] / (mu[flat] + 1e-3)).mean())

def stats(name, a):
    l = lum(a)
    print(f"[{name}] median={np.median(l):.3f} mean={l.mean():.3f} "
          f"blown={100*(l > 0.98).mean():.2f}% "
          f"R/B={(a[...,0].mean()/max(a[...,2].mean(),1e-9)):.3f} "
          f"hf_flat={hf_flat(a):.4f}")
    return l

def main():
    off = load(sys.argv[1])
    ren = load(sys.argv[2])
    # same size assumed (1920x1061); resize render if needed
    if off.shape != ren.shape:
        ren = np.asarray(Image.fromarray((ren*255).astype(np.uint8))
                         .resize((off.shape[1], off.shape[0])), dtype=np.float64) / 255.0
    stats("official", off)
    stats("render  ", ren)
    zones = [
        ("cobbleFg", 0.30, 0.70, 0.85, 1.00),
        ("facadeGreen", 0.20, 0.45, 0.15, 0.45),
        ("awning", 0.33, 0.55, 0.17, 0.28),
        ("topStrip", 0.30, 0.70, 0.00, 0.08),
        ("vespa", 0.46, 0.60, 0.52, 0.68),
    ]
    for name, x0, x1, y0, y1 in zones:
        mo, mr = med(zone(off, x0, x1, y0, y1)), med(zone(ren, x0, x1, y0, y1))
        print(f"  {name:12s} official={mo:.3f} render={mr:.3f} ratio={mr/max(mo,1e-9):.3f}")
    if len(sys.argv) > 3:
        out = sys.argv[3]
        # Safety: never overwrite an input render — a glob expansion bug once
        # destroyed a 35-minute final render this way.
        if out == sys.argv[2]:
            out = out.rsplit('.', 1)[0] + '_sbs.png'
            print(f"(sbs output renamed to avoid clobbering input: {out})")
        sbs = np.concatenate([off, ren], axis=1)
        Image.fromarray((np.clip(sbs, 0, 1)*255).astype(np.uint8)).save(out)
        print(f"side-by-side: {out}")

if __name__ == "__main__":
    main()
