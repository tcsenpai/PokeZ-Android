# PNG pack textures -> BC7 DDS with every mip level, on any OS (no texconv): the layout runtime/nvn/vulkan/textures.c
# hd_load_dds reads (DX10 header, DXGI 98 BC7_UNORM; the runtime samples it sRGB or not as the game's own texture).
# A <hash>.dds is preferred over <hash>.png when both are in textures/hd: ~1/4 to 1/5 of the PNG's GPU memory, no PNG
# decode on the render thread. Mip levels: Lanczos, colour and alpha each on its own (an RGBA resize weights by alpha).
# Needs Pillow, numpy and etcpak (pip install etcpak: 2.6 MB, its BC7 encoder).
#
# usage: python tools/za/png_to_dds.py <src dir or .png files> --dst <dir> [--keep-png]
#        e.g. python tools/za/png_to_dds.py textures/hd --dst textures/hd   (then remove the .png, or keep them as a fallback)
import argparse, glob, os, struct, sys
import numpy as np
from PIL import Image
import etcpak

ap = argparse.ArgumentParser()
ap.add_argument('src', nargs='+')
ap.add_argument('--dst', required=True)
args = ap.parse_args()
os.makedirs(args.dst, exist_ok=True)
files = []
for s in args.src:
    files += sorted(glob.glob(os.path.join(s, '*.png'))) if os.path.isdir(s) else [s]


def level_images(im):
    w, h = im.size
    rgb, a = im.convert('RGB'), im.getchannel('A')
    out = [im]
    while w > 1 or h > 1:
        w, h = max(1, w // 2), max(1, h // 2)
        lv = rgb.resize((w, h), Image.LANCZOS).convert('RGBA')
        lv.putalpha(a.resize((w, h), Image.LANCZOS))
        out.append(lv)
    return out


def bc7(im):
    w, h = im.size
    pw, ph = (w + 3) // 4 * 4, (h + 3) // 4 * 4
    px = np.asarray(im, dtype=np.uint8)
    if (pw, ph) != (w, h):  # (levels under 4x4: the block padded with the edge texels)
        px = np.pad(px, ((0, ph - h), (0, pw - w), (0, 0)), mode='edge')
    return etcpak.compress_bc7(np.ascontiguousarray(px).tobytes(), pw, ph, None)


for f in files:
    im = Image.open(f).convert('RGBA')
    lvs = level_images(im)
    data = b''.join(bc7(l) for l in lvs)
    w, h = im.size
    # DDS_HEADER: size 124, flags CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE, pitch = level 0 bytes
    hdr = struct.pack('<4sIIIIIII44x', b'DDS ', 124, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000, h, w,
                      ((w + 3) // 4) * ((h + 3) // 4) * 16, 0, len(lvs))
    hdr += struct.pack('<II4s20x', 32, 0x4, b'DX10')  # DDS_PIXELFORMAT: FOURCC DX10
    hdr += struct.pack('<IIIII', 0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)  # caps: TEXTURE|MIPMAP|COMPLEX
    hdr += struct.pack('<IIIII', 98, 3, 0, 1, 0)  # DX10: DXGI_FORMAT_BC7_UNORM, TEXTURE2D, misc 0, array 1, flags 0
    assert len(hdr) == 148
    out = os.path.join(args.dst, os.path.splitext(os.path.basename(f))[0] + '.dds')
    with open(out, 'wb') as fo:
        fo.write(hdr + data)
    print(f'{os.path.basename(out)}: {w}x{h}, {len(lvs)} levels, {len(hdr) + len(data) >> 10} KB')
