#!/usr/bin/env python3
"""从 san_giuseppe_bridge_4k.hdr 剔除太阳光斑，生成 san_giuseppe_bridge_4k_nosun.hdr。

白天天光照方案：HDRI 只提供蓝天环境光（光斑区域用周围光环填充+模糊），
直射阳光由 RENDER_SUN_* delta 太阳提供（方向/强度独立可控）。
依赖：/tmp/extmatch/sg4096.raw（由 tools dumpfull 生成）——本脚本自带
stb 等价的 HDR 解码（自适应 RLE），可直接读原始 HDR。
"""
import numpy as np, sys

SRC = 'models/Bistro_v5_2/san_giuseppe_bridge_4k.hdr'
DST = 'models/Bistro_v5_2/san_giuseppe_bridge_4k_nosun.hdr'

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
                    plane[i:i+c] = data[pos:pos+c]; pos += c; i += c
    f = img[..., :3].astype(np.float64) / 256.0 * 2.0 ** (img[..., 3].astype(np.float64) - 128.0)
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

img = read_hdr(sys.argv[1] if len(sys.argv) > 1 else SRC)
H, W = img.shape[:2]
lum = img @ [0.299, 0.587, 0.114]
iy, ix = np.unravel_index(np.argmax(lum), lum.shape)
print(f'sun core at texel ({ix},{iy}) lum={lum[iy,ix]:.0f}')
yy, xx = np.mgrid[0:H, 0:W]
dx = np.abs(xx - ix); dx = np.minimum(dx, W - dx)
dist2 = (xx - ix - dx + dx)**2  # placeholder replaced below
dist2 = dx*dx + (yy-iy)**2
mask = (lum > 10) & (dist2 < 90*90)
ring = (dist2 >= 150*150) & (dist2 <= 250*250) & (lum < 5)
ref = np.median(img[ring], axis=0)
print('masked texels:', int(mask.sum()), 'fill:', ref.round(3))
img[mask] = ref
def box1d(a, k, axis):
    ap = np.pad(a, [(k//2, k//2) if ax == axis else (0, 0) for ax in range(3)], mode='wrap')
    c = np.cumsum(ap, axis=axis)
    c = np.concatenate([np.zeros_like(np.take(c, [0], axis=axis)), c], axis=axis)
    hi = np.take(c, range(k, a.shape[axis]+k), axis=axis)
    lo = np.take(c, range(0, a.shape[axis]), axis=axis)
    return (hi - lo) / k
blurred = box1d(box1d(img, 31, 0), 31, 1)
img[mask] = blurred[mask]
write_hdr(sys.argv[2] if len(sys.argv) > 2 else DST, np.clip(img, 0, None))
print('written', DST)
