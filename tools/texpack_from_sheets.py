#!/usr/bin/env python3
"""A texture pack from sprite sheets: each dumped texture (textures/dump/<key>.png) whose shape is a
frame of one of the sheets (mirrored or not) gets that frame, with the sheet's colours, as
<out>/<key>.png. For sheets of the same art with more colours (the game reduced its people to 16
colours a sheet; the sheet of a wife in the house lost her white).

usage: python tools/texpack_from_sheets.py SHEETS_DIR DUMP_DIR OUT_DIR [--scale N]

A frame is an island of opaque pixels in a sheet (pixels up to 2 apart join). A dump matches a frame
of its exact size (both cropped to their opaque pixels) when at most 2.5% of the pixels differ in
being opaque and the opaque ones are within 20 per channel on average (so a lookalike in other
colours is not taken). --scale N writes the images N times larger (nearest). Needs Pillow and numpy.
"""
import os
import sys

import numpy as np
from PIL import Image

MASK_TOL, COL_TOL = 0.025, 20.0


def frames_of(path):
    a = np.asarray(Image.open(path).convert('RGBA'))
    op = a[:, :, 3] >= 128
    h, w = op.shape
    seen = np.zeros_like(op)
    out = []
    for y, x in zip(*np.nonzero(op)):
        if seen[y, x]:
            continue
        seen[y, x] = True
        st, x0, x1, y0, y1 = [(y, x)], x, x, y, y
        while st:
            cy, cx = st.pop()
            x0, x1, y0, y1 = min(x0, cx), max(x1, cx), min(y0, cy), max(y1, cy)
            for ny in range(max(cy - 2, 0), min(cy + 3, h)):
                for nx in range(max(cx - 2, 0), min(cx + 3, w)):
                    if op[ny, nx] and not seen[ny, nx]:
                        seen[ny, nx] = True
                        st.append((ny, nx))
        if x1 - x0 > 8 and y1 - y0 > 8:
            f = a[y0:y1 + 1, x0:x1 + 1].copy()
            f[:, :, 3] = np.where(f[:, :, 3] >= 128, 255, 0)
            out.append(f)
    return out


def main():
    args = sys.argv[1:]
    scale = 1
    if '--scale' in args:
        i = args.index('--scale')
        scale = int(args[i + 1])
        del args[i:i + 2]
    if len(args) != 3:
        sys.exit(__doc__)
    sheets, dumps, out = args
    by_size = {}
    for name in sorted(os.listdir(sheets)):
        if not name.lower().endswith('.png'):
            continue
        fr = frames_of(os.path.join(sheets, name))
        print('%s: %d frames' % (name, len(fr)))
        for f in fr:
            for g in (f, f[:, ::-1]):
                by_size.setdefault(g.shape[:2], []).append((name, g))
    os.makedirs(out, exist_ok=True)
    made = 0
    for name in sorted(os.listdir(dumps)):
        if not name.endswith('.png'):
            continue
        d = np.asarray(Image.open(os.path.join(dumps, name)).convert('RGBA'))
        op = d[:, :, 3] > 0
        if not op.any():
            continue
        ys, xs = np.nonzero(op)
        y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
        c, cop = d[y0:y1, x0:x1, :3].astype(int), op[y0:y1, x0:x1]
        best = None
        for sheet, g in by_size.get(cop.shape, ()):
            gop = g[:, :, 3] > 0
            mis = np.count_nonzero(gop != cop) / cop.size
            both = gop & cop
            if mis > MASK_TOL or not both.any():
                continue
            col = np.abs(g[:, :, :3].astype(int) - c)[both].mean()
            if col <= COL_TOL and (best is None or (mis, col) < best[0]):
                best = ((mis, col), sheet, g)
        if not best:
            continue
        img = np.zeros_like(d)
        img[y0:y1, x0:x1] = best[2]
        im = Image.fromarray(img, 'RGBA')
        if scale > 1:
            im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
        im.save(os.path.join(out, name))
        made += 1
        print('%s <- %s (%.1f%% shape, %.1f colour)' % (name, best[1], best[0][0] * 100, best[0][1]))
    print('%d images in %s' % (made, out))


if __name__ == '__main__':
    main()
