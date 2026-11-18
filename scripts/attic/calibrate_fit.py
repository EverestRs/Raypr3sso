#!/usr/bin/env python3
"""P3: automated daylight grade fitting (坐标下降，分区对官方拟合).

Drives ONE Raypr3sso console session (model loaded once), renders
480x270@64 per evaluation, and coordinate-descends the RENDER-TIME env knobs
(sun tint, WB, split-tone, indirect scale) to minimise a zone score against
the official reference. Load-time knobs (sky scale/tint) are fixed to the
current best values — reloading the model per sky change costs 3-5 min.

Usage: calibrate_fit.py [max_evals] [out_params.json]
"""
import subprocess, sys, os, json, re, time
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'build', 'Raypr3sso')
REF = os.path.join(ROOT, 'reference', 'official_bistro_daylight.png')
WORK = os.path.join(ROOT, 'output_gpu', 'calib_fit')
os.makedirs(WORK, exist_ok=True)

# Fixed (non-searched) environment — load-time sky rig + geometry + grading
FIXED = dict(
    RENDER_SKY_SCALE='2.3', RENDER_SKY_TINT='0.80 0.92 1.22',
    RENDER_SKY_HORIZON='0.5', RENDER_SKY_HTRANS='0.6',
    RENDER_SKY_FOCUS_DIM='0.999', RENDER_SKY_FOCUS_CONE='12',
    RENDER_EMISSION_SCALE='0.008',
    RENDER_SUN_AZ='-50', RENDER_SUN_EL='55', RENDER_SUN_INT='14',
    RENDER_TONEMAP='aces', RENDER_DENOISE='1',
    RENDER_SATURATION='1.8', RENDER_INDIRECT_SCALE='0.6',
    RENDER_DENOISE_DETAIL='1.2', RENDER_DENOISE_NOISE_TARGET='0.12',
    RENDER_SPLIT_KEY='sun',
    RENDER_CURVE='0,0 0.1,0.06 0.2,0.13 0.3,0.20 0.4,0.27 0.55,0.50 0.66,0.74 0.85,0.88 1,0.96',
)
SKY = 'models/Bistro_v5_2/qwantani_azm55_4k.hdr'

# Searched parameters: name -> [init, step, lo, hi]; SUN_TINT is 3 scalars
P0 = {
    'sun_r': 1.00, 'sun_g': 0.85, 'sun_b': 0.70,
    'wb': 0.30, 'split_w': 0.08, 'split_c': 0.12, 'ind': 0.60,
}
STEP0 = {'sun_r': 0.03, 'sun_g': 0.03, 'sun_b': 0.04, 'wb': 0.10,
         'split_w': 0.04, 'split_c': 0.05, 'ind': 0.12}
LIMITS = {'sun_r': (0.9, 1.05), 'sun_g': (0.75, 1.0), 'sun_b': (0.6, 1.0),
          'wb': (0.0, 0.6), 'split_w': (0.0, 0.25), 'split_c': (0.0, 0.30),
          'ind': (0.0, 1.0)}

# Reference zone table (normalized boxes), computed from the official image
ZONES = {
    'facade': (0.30, 0.38, 0.30, 0.42), 'foliage': (0.345, 0.39, 0.42, 0.50),
    'door': (0.42, 0.47, 0.30, 0.45), 'sign': (0.35, 0.52, 0.115, 0.16),
    'vespa': (0.485, 0.53, 0.53, 0.60), 'street': (0.30, 0.70, 0.85, 1.00),
    'terrace': (0.40, 0.65, 0.60, 0.68), 'awning': (0.36, 0.52, 0.235, 0.27),
    'cobbleFg': (0.05, 0.45, 0.80, 1.00),
}
W = np.array([0.299, 0.587, 0.114])

def zones_of(img):
    a = np.asarray(img.convert('RGB'), dtype=np.float64) / 255.0
    h, w = a.shape[:2]
    out = {}
    for n, (x0, x1, y0, y1) in ZONES.items():
        z = a[int(y0*h):int(y1*h), int(x0*w):int(x1*w)].reshape(-1, 3)
        out[n] = z.mean(axis=0)
    return out

OFF = zones_of(Image.open(REF))

def score(img):
    ours = zones_of(img)
    s = 0.0
    for n in ZONES:
        o, r = OFF[n], ours[n]
        lo, lr = o @ W, r @ W
        # luminance term (log, capped) + hue term (R/B ratio, weighted 2x)
        lr_ratio = np.log2(max(lr, 1e-4) / max(lo, 1e-4))
        s += min(abs(lr_ratio), 0.6)
        rb_o = o[0] / max(o[2], 1e-6)
        rb_r = r[0] / max(r[2], 1e-6)
        s += 2.0 * min(abs(np.log2(max(rb_r, 1e-3) / max(rb_o, 1e-3))), 1.2)
    return s

class Session:
    def __init__(self):
        env = dict(os.environ); env.update(FIXED)
        self.p = subprocess.Popen([BIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, env=env, cwd=ROOT)
        self._cmd(f'create model m models/Bistro_v5_2 BistroExterior.fbx {SKY}')
        self.eval_count = 0

    def _cmd(self, line):
        self.p.stdin.write(line + '\n'); self.p.stdin.flush()

    def render(self, params, tag):
        outdir = os.path.join(WORK, tag)
        os.makedirs(outdir, exist_ok=True)
        self._cmd(f'setenv RENDER_SUN_TINT {params["sun_r"]:.3f} {params["sun_g"]:.3f} {params["sun_b"]:.3f}')
        self._cmd(f'setenv RENDER_WB_STRENGTH {params["wb"]:.3f}')
        self._cmd(f'setenv RENDER_SPLIT_WARM {params["split_w"]:.3f}')
        self._cmd(f'setenv RENDER_SPLIT_COOL {params["split_c"]:.3f}')
        self._cmd(f'setenv RENDER_INDIRECT_SCALE {params["ind"]:.3f}')
        self._cmd(f'create args {tag}')
        for v in ['480', '270', '64', '12', '0.70', '-17 3.2 0.7', '1 -0.11 -0.09', '50', outdir]:
            self._cmd(v)
        self._cmd(f'render m {tag} gpu')
        # wait for the render to finish
        while True:
            line = self.p.stdout.readline()
            if not line:
                raise RuntimeError('console died')
            if line.startswith('Output file:'):
                png = line.split(':', 1)[1].strip()
                break
        self.eval_count += 1
        return Image.open(os.path.join(ROOT, png))

    def close(self):
        try:
            self._cmd('exit'); self.p.stdin.close(); self.p.wait(timeout=30)
        except Exception:
            self.p.kill()

def main():
    max_evals = int(sys.argv[1]) if len(sys.argv) > 1 else 80
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(WORK, 'best_params.json')
    sess = Session()
    try:
        cur = dict(P0)
        best_img = sess.render(cur, f'e{sess.eval_count:03d}_init')
        best = score(best_img)
        print(f'[init] score={best:.4f} params={cur}', flush=True)
        step = dict(STEP0)
        improved = True
        while improved and sess.eval_count < max_evals:
            improved = False
            for key in list(cur.keys()):
                if sess.eval_count >= max_evals: break
                for sign in (+1, -1):
                    trial = dict(cur)
                    trial[key] = float(np.clip(cur[key] + sign*step[key], *LIMITS[key]))
                    if trial[key] == cur[key]: continue
                    img = sess.render(trial, f'e{sess.eval_count:03d}_{key}{"p" if sign>0 else "m"}')
                    s = score(img)
                    print(f'[eval {sess.eval_count}] {key}{"+" if sign>0 else "-"} -> score={s:.4f} (best {best:.4f}) params={trial}', flush=True)
                    if s < best - 1e-4:
                        best, cur, improved = s, trial, True
                        break
            if not improved:
                step = {k: v*0.5 for k, v in step.items()}
                print(f'[sweep] no improvement, halving steps -> {step}', flush=True)
                if max(step.values()) < 0.005: break
        print(f'BEST score={best:.4f} params={cur}', flush=True)
        with open(out_path, 'w') as f:
            json.dump({'score': best, 'params': cur, 'evals': sess.eval_count}, f, indent=2)
        print('written:', out_path)
    finally:
        sess.close()

if __name__ == '__main__':
    main()