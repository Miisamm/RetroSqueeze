# RetroSqueeze icon: an old-school cassette, Win98 ICO (8 bpp + 1-bit mask, 32 + 16 px)
import sys
from PIL import Image, ImageDraw
import struct

def ico_entry(im):
    n = im.size[0]
    a = im.getchannel("A")
    rgb = Image.new("RGB", im.size, (0, 0, 0))
    rgb.paste(im.convert("RGB"), mask=a.point(lambda v: 255 if v >= 128 else 0))
    pal = rgb.quantize(256, method=Image.MEDIANCUT)
    p = pal.getpalette()[:768]; p += [0] * (768 - len(p))
    px = pal.load(); al = a.load()
    xor = b"".join(bytes(px[x, y] for x in range(n)) for y in range(n - 1, -1, -1))
    row = ((n + 31) // 32) * 4
    andm = b""
    for y in range(n - 1, -1, -1):
        bits = 0; r = bytearray(row)
        for x in range(n):
            if al[x, y] < 128: r[x // 8] |= 0x80 >> (x % 8)
        andm += bytes(r)
    hdr = struct.pack("<IiiHHIIiiII", 40, n, 2 * n, 1, 8, 0, len(xor) + len(andm), 0, 0, 256, 0)
    cols = b"".join(bytes((p[i*3+2], p[i*3+1], p[i*3], 0)) for i in range(256))
    return hdr + cols + xor + andm


def cassette(n):
    S = 16; W = n * S; u = W / 32
    im = Image.new("RGBA", (W, W), (0, 0, 0, 0)); d = ImageDraw.Draw(im)
    d.rounded_rectangle([0.5*u, 5*u, 31.5*u, 27*u], radius=2.2*u, fill=(30, 30, 34, 255))
    d.rounded_rectangle([1.5*u, 6*u, 30.5*u, 26*u], radius=1.8*u, fill=(70, 72, 78, 255))
    d.rectangle([3.5*u, 7.5*u, 28.5*u, 18.5*u], fill=(235, 196, 90, 255))           # label
    d.rectangle([3.5*u, 7.5*u, 28.5*u, 9.5*u], fill=(200, 60, 40, 255))             # red stripe
    d.rounded_rectangle([7*u, 11*u, 25*u, 17*u], radius=3*u, fill=(25, 25, 28, 255)) # window
    for cx in (11*u, 21*u):
        d.ellipse([cx-2.6*u, 11.4*u, cx+2.6*u, 16.6*u], fill=(240, 240, 240, 255))
        d.ellipse([cx-1.2*u, 12.8*u, cx+1.2*u, 15.2*u], fill=(40, 40, 44, 255))
    d.polygon([(8*u, 26*u), (10*u, 20.5*u), (22*u, 20.5*u), (24*u, 26*u)], fill=(48, 50, 55, 255))
    for x in (12*u, 20*u):
        d.ellipse([x-1*u, 22.5*u, x+1*u, 24.5*u], fill=(15, 15, 18, 255))
    return im.resize((n, n), Image.LANCZOS)

ims = [cassette(32), cassette(16)]
data = [ico_entry(i) for i in ims]
out = struct.pack("<HHH", 0, 1, len(ims)); off = 6 + 16 * len(ims)
for im, dt in zip(ims, data):
    n = im.size[0]; out += struct.pack("<BBBBHHII", n, n, 0, 0, 1, 8, len(dt), off); off += len(dt)
open("retrosq.ico", "wb").write(out + b"".join(data))
pv = Image.new("RGB", (32*8 + 16 + 16*8, 32*8), (0, 128, 128))
for i, im in enumerate(ims):
    bg = Image.new("RGBA", im.size, (0, 128, 128, 255)); bg.alpha_composite(im)
    pv.paste(bg.convert("RGB").resize((im.size[0]*8,)*2, Image.NEAREST), (i * (32*8 + 16), 0))
pv.save("preview.png")
