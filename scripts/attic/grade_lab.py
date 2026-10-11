#!/usr/bin/env python3
"""Offline grade tuning: apply saturation + luma contrast curve (same math as
the pipeline's gammaCorrect tail) to a base render, score zones vs official.
Usage: grade_lab.py <base.png> [sat] ["x0,y0 x1,y1 ..."]
"""
import sys, numpy as np
from PIL import Image

W = np.array([0.299, 0.587, 0.114])

def monotone_cubic(x, y):
    n = len(x)
    h = np.diff(x); d = np.diff(y) / h
    m = np.zeros(n); m[0] = d[0]; m[-1] = d[-1]
    for i in range(1, n - 1):
        if d[i-1] * d[i] <= 0: m[i] = 0
        else:
            w1 = 2*h[i-1] + h[i]; w2 = h[i-1] + 2*h[i]
            m[i] = (w1 + w2) / (w1/d[i-1] + w2/d[i])
    def f(t):
        t = np.clip(t, 0, 1)
        idx = np.clip(np.searchsorted(x, t) - 1, 0, n - 2)
        hh = h[idx]; s = (t - x[idx]) / hh
        s2 = s*s; s3 = s2*s
        return ((2*s3-3*s2+1)*y[idx] + (s3-2*s2+s)*hh*m[idx] +
                (-2*s3+3*s2)*y[idx+1] + (s3-s2)*hh*m[idx+1])
    return f

ZONES = {  # name: (box, official RGB)
  'vespa_body':   ((0.485, 0.53, 0.53, 0.60), (0.246, 0.390, 0.520)),
  'vespa_rear':   ((0.52, 0.565, 0.55, 0.62), (0.232, 0.374, 0.516)),
  'awning_top':   ((0.36, 0.52, 0.19, 0.235), (0.412, 0.110, 0.145)),
  'awning_valance':((0.36, 0.52, 0.235, 0.27), (0.466, 0.181, 0.230)),
  'facade_green': ((0.30, 0.38, 0.30, 0.42), (0.143, 0.169, 0.137)),
  'sign_panel':   ((0.35, 0.52, 0.115, 0.16), (0.153, 0.177, 0.152)),
  'door_frame':   ((0.42, 0.47, 0.30, 0.45), (0.096, 0.105, 0.093)),
  'foliage':      ((0.345, 0.39, 0.42, 0.50), (0.111, 0.139, 0.086)),
  'street_fg':    ((0.30, 0.70, 0.85, 1.00), (0.757, 0.740, 0.704)),
}

def zone_mean(a, box):
    x0, x1, y0, y1 = box
    h, w = a.shape[:2]
    z = a[int(y0*h):int(y1*h), int(x0*w):int(x1*w)].reshape(-1, 3)
    return z.mean(axis=0)

def grade(a, sat, f):
    l = a @ W
    c = l[..., None] + (a - l[..., None]) * sat
    L = np.clip(c @ W, 1e-6, None)
    c = c * (f(L) / L)[..., None]
    return np.clip(c, 0, 1)

def main():
    a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.float64) / 255
    sat = float(sys.argv[2]) if len(sys.argv) > 2 else 1.3
    pts = sys.argv[3] if len(sys.argv) > 3 else "0,0 0.1,0.05 0.2,0.11 0.3,0.18 0.4,0.29 0.55,0.55 0.66,0.80 0.85,0.91 1,0.97"
    x = np.array([float(p.split(',')[0]) for p in pts.split()])
    y = np.array([float(p.split(',')[1]) for p in pts.split()])
    f = monotone_cubic(x, y)
    g = grade(a, sat, f)
    err = 0.0
    print(f"sat={sat} curve=[{pts}]")
    for name, (box, tgt) in ZONES.items():
        m = zone_mean(g, box)
        e = float(np.abs(m - np.array(tgt)).sum())
        err += e
        print(f"  {name:15s} got ({m[0]:.3f},{m[1]:.3f},{m[2]:.3f})  tgt ({tgt[0]:.3f},{tgt[1]:.3f},{tgt[2]:.3f})  err {e:.3f}")
    print(f"  TOTAL ERR {err:.3f}")
    Image.fromarray((g * 255).astype(np.uint8)).save('/tmp/grade_lab_out.png')

if __name__ == '__main__':
    main()
