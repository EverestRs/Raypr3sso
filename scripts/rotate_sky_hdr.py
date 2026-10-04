#!/usr/bin/env python3
"""Rotate an equirect HDR sky so its sun disc lands on a target direction
(the renderer's delta-sun direction), so the kernel's sun-cone lighting kill
covers the HDRI sun and the delta sun alone casts shadows.

Equirect convention matches the renderer kernel (see geometry.h sphereToDir):
  dir(u,v) = (-sin(T) sin(phi), cos(T), sin(T) cos(phi)), phi=2pi(u-0.5), T=pi v

Usage: rotate_sky_hdr.py <in.hdr> <out.hdr> <azDeg> <elDeg> [--yaw-only]
  --yaw-only: pure azimuth rotation (column shift) — the sky's vertical
  gradient and horizon glow stay natural; the sun's elevation is unchanged
  (pick a target el within the sun-cone kill radius).
"""
import numpy as np, sys

# --- stb-compatible RGBE IO (same code as scripts/make_nosun_hdr.py) ---
def read_hdr(path):
    with open(path, 'rb') as f:
        while True:
            line = f.readline().decode(errors='ignore')
            if line.startswith('-Y'):
                res = line.split(); H, W = int(res[1]), int(res[3]); break
        data = f.read()
    img = np.zeros((H, W, 4), np.uint8); pos = 0
    for y in range(H):
        assert data[pos] == 2 and data[pos+1] == 2, f'scanline {y} not adaptive RLE'
        pos += 4
        for ch in range(4):
            plane = img[y, :, ch]; i = 0
            while i < W:
                c = data[pos]; pos += 1
                if c > 128:
                    n = c - 128; plane[i:i+n] = data[pos]; pos += 1; i += n
                else:
                    plane[i:i+c] = np.frombuffer(data[pos:pos+c], dtype=np.uint8); pos += c; i += c
    f = img[..., :3].astype(np.float64) / 256.0 * 2.0 ** (img[..., 3].astype(np.float64) - 128.0)[..., None]
    return f

def write_hdr(path, img):
    H, W = img.shape[:2]
    m = img.max(axis=-1)
    e = np.ceil(np.log2(np.maximum(m, 1e-12))).astype(int)
    f2 = img / (2.0 ** np.maximum(e, -20))[..., None]
    rgb = np.clip(f2 * 256.0, 0, 255).astype(np.uint8)
    E = np.clip(e + 128, 0, 255).astype(np.uint8)
    rgbe = np.zeros((H, W, 4), np.uint8); rgbe[..., :3] = rgb; rgbe[..., 3] = E
    def rle_plane(plane):
        out = bytearray(); i = 0; n = len(plane)
        while i < n:
            j = i
            while j < n and plane[j] == plane[i] and j - i < 127: j += 1
            run = j - i
            if run >= 3:
                out.append(run + 128); out.append(int(plane[i])); i = j
            else:
                j2 = i
                while j2 < n and (j2 + 1 >= n or plane[j2+1] != plane[j2]) and j2 - i < 127: j2 += 1
                lit = max(1, j2 - i)
                out.append(lit); out.extend(int(b) for b in plane[i:i+lit]); i = max(j2, i + 1)
        return bytes(out)
    with open(path, 'wb') as fo:
        fo.write(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y %d +X %d\n' % (H, W))
        for r in range(H):
            fo.write(bytes([2, 2, (W >> 8) & 255, W & 255]))
            for ch in range(4):
                fo.write(rle_plane(rgbe[r, :, ch]))

def uv_to_dir(u, v):
    phi = 2.0 * np.pi * (u - 0.5)
    T = np.pi * v
    return np.array([-np.sin(T) * np.sin(phi), np.cos(T), np.sin(T) * np.cos(phi)])

def dir_to_uv(d):
    d = d / np.linalg.norm(d)
    u = np.arctan2(-d[0], d[2]) / (2 * np.pi) + 0.5
    v = np.arccos(np.clip(d[1], -1, 1)) / np.pi
    return u % 1.0, v

def rot_matrix(a, b):
    """Rotation taking unit vector a to unit vector b (Rodrigues)."""
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9:
        return np.eye(3)
    vx = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + vx + vx @ vx * (1.0 / (1.0 + c))

def main():
    src, dst = sys.argv[1], sys.argv[2]
    az, el = float(sys.argv[3]), float(sys.argv[4])
    azR, elR = np.deg2rad(az), np.deg2rad(el)
    target = np.array([np.cos(elR) * np.cos(azR), np.sin(elR), np.cos(elR) * np.sin(azR)])

    img = read_hdr(src)
    H, W = img.shape[:2]
    lum = img @ [0.299, 0.587, 0.114]
    # Sun disc centroid over the bright region (robust vs single hot texel)
    thr = np.percentile(lum, 99.99)
    ys, xs = np.nonzero(lum >= thr)
    # unwrap x around the brightest texel to handle seam-crossing discs
    ix0, iy0 = np.unravel_index(np.argmax(lum), lum.shape)[1], np.unravel_index(np.argmax(lum), lum.shape)[0]
    xsu = (xs - ix0 + W / 2) % W + ix0 - W / 2
    cx, cy = float(xsu.mean()), float(ys.mean())
    u_s, v_s = cx / W, cy / H
    sun_dir = uv_to_dir(u_s, v_s)
    tgt_u, tgt_v = dir_to_uv(target)
    print(f'sun disc centroid: texel ({cx:.0f},{cy:.0f}) uv=({u_s:.4f},{v_s:.4f}) '
          f'dir=({sun_dir[0]:.3f},{sun_dir[1]:.3f},{sun_dir[2]:.3f}) lum={lum.max():.0f}')
    print(f'target dir az={az} el={el} -> uv=({tgt_u:.4f},{tgt_v:.4f}) dir=({target[0]:.3f},{target[1]:.3f},{target[2]:.3f})')

    yaw_only = '--yaw-only' in sys.argv
    if yaw_only:
        # Pure azimuth rotation: shift u so the sun's kernel-azimuth matches
        # the target direction's. Elevation/horizon content untouched.
        az_t = np.arctan2(-target[0], target[2]) / (2 * np.pi) + 0.5  # target u
        az_s = np.arctan2(-sun_dir[0], sun_dir[2]) / (2 * np.pi) + 0.5
        du = (az_s - az_t) % 1.0   # target texel u samples source at u + du
        yy, xx = np.mgrid[0:H, 0:W]
        su = ((xx + 0.5) / W + du) % 1.0
        sv = (yy + 0.5) / H
        print(f'yaw-only: du={du:.4f} ({du*360:.1f}deg)')
    else:
        R = rot_matrix(sun_dir, target)
        Rt = R.T  # inverse: target -> source

        yy, xx = np.mgrid[0:H, 0:W]
        u = (xx + 0.5) / W
        v = (yy + 0.5) / H
        phi = 2 * np.pi * (u - 0.5)
        T = np.pi * v
        dx = -np.sin(T) * np.sin(phi)
        dy = np.cos(T)
        dz = np.sin(T) * np.cos(phi)
        sx = Rt[0, 0] * dx + Rt[0, 1] * dy + Rt[0, 2] * dz
        sy = Rt[1, 0] * dx + Rt[1, 1] * dy + Rt[1, 2] * dz
        sz = Rt[2, 0] * dx + Rt[2, 1] * dy + Rt[2, 2] * dz
        su = (np.arctan2(-sx, sz) / (2 * np.pi) + 0.5) % 1.0
        sv = np.arccos(np.clip(sy, -1, 1)) / np.pi
    # bilinear sample (u wraps, v clamps)
    fx = su * W - 0.5
    fy = sv * H - 0.5
    x0 = np.floor(fx).astype(int) % W
    x1 = (x0 + 1) % W
    y0 = np.clip(np.floor(fy).astype(int), 0, H - 1)
    y1 = np.clip(y0 + 1, 0, H - 1)
    tx = (fx - np.floor(fx))[..., None]
    ty = (fy - np.floor(fy))[..., None]
    out = (img[y0, x0] * (1 - tx) + img[y0, x1] * tx) * (1 - ty) + \
          (img[y1, x0] * (1 - tx) + img[y1, x1] * tx) * ty
    # report where the sun ended up
    lum2 = out @ [0.299, 0.587, 0.114]
    iy2, ix2 = np.unravel_index(np.argmax(lum2), lum2.shape)
    u2, v2 = (ix2 + 0.5) / W, (iy2 + 0.5) / H
    print(f'after rotation: sun at uv=({u2:.4f},{v2:.4f}) (target ({tgt_u:.4f},{tgt_v:.4f}))')
    write_hdr(dst, out)
    print(f'written: {dst}')

if __name__ == '__main__':
    main()
