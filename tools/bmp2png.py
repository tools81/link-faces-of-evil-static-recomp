"""Convert 32-bit top-down BMP dumps to PNG (no dependencies). Optionally crop/scale."""
import struct, sys, zlib

def bmp_to_png(src, dst, xscale=1, yscale=2):
    b = open(src, 'rb').read()
    off = struct.unpack('<I', b[10:14])[0]
    w, h = struct.unpack('<ii', b[18:26])
    h = abs(h)
    px = b[off:]
    rows = []
    for y in range(h):
        row = bytearray()
        line = px[y * w * 4:(y + 1) * w * 4]
        for x in range(0, w, xscale):
            bb, g, r = line[x * 4], line[x * 4 + 1], line[x * 4 + 2]
            row += bytes((r, g, bb))
        for _ in range(yscale):
            rows.append(b'\x00' + bytes(row))
    ow = (w + xscale - 1) // xscale
    raw = b''.join(rows)
    def chunk(t, d):
        c = struct.pack('>I', len(d)) + t + d
        return c + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', ow, h * yscale, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)

if __name__ == '__main__':
    for s in sys.argv[1:]:
        bmp_to_png(s, s[:-4] + '.png', 2, 2)
