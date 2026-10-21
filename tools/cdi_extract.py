"""Extract files from a CD-i (Green Book) BIN image (2352-byte raw Mode 2 sectors).

Usage: python cdi_extract.py <image.bin> <outdir> [--all]

By default only ordinary (non real-time) files are written; --all also dumps
the large real-time .rtr/.rtf streams.

Each file is written in two forms:
  <name>          - concatenated user data (2048 bytes for Form 1, 2324 for Form 2)
  <name>.sectors  - raw 2352-byte sectors (for real-time files with interleaved
                    audio/video channels, whose subheaders matter)
"""
import os, struct, sys

RAW = 2352

class Disc:
    def __init__(self, path):
        self.f = open(path, "rb")

    def raw(self, lsn):
        self.f.seek(lsn * RAW)
        return self.f.read(RAW)

    def data(self, lsn):
        s = self.raw(lsn)
        sub = s[16:24]
        form2 = sub[2] & 0x20
        return s[24:24 + (2324 if form2 else 2048)], sub

def parse_dir(disc, lsn, size):
    out = []
    blocks = (size + 2047) // 2048
    for b in range(blocks):
        d, _ = disc.data(lsn + b)
        p = 0
        while p < 2048:
            ln = d[p]
            if ln == 0:
                break
            rec = d[p:p + ln]
            start = struct.unpack(">I", rec[6:10])[0]
            fsize = struct.unpack(">I", rec[14:18])[0]
            nlen = rec[32]
            name = rec[33:33 + nlen]
            q = 33 + nlen
            if q & 1:
                q += 1
            owner = struct.unpack(">I", rec[q:q + 4])[0] if q + 4 <= ln else 0
            attr = struct.unpack(">H", rec[q + 4:q + 6])[0] if q + 6 <= ln else 0
            fnum = rec[q + 8] if q + 9 <= ln else 0
            out.append((name, start, fsize, attr, fnum, rec[25]))
            p += ln
    return out

def walk(disc, lsn, size, path, outdir, listing, all_files=False):
    for name, start, fsize, attr, fnum, flags in parse_dir(disc, lsn, size):
        if name in (b"\x00", b"\x01"):
            continue
        nm = name.decode("latin-1")
        full = path + "/" + nm
        if attr & 0x8000:  # directory
            listing.append(f"DIR  {full:40s} lsn={start}")
            os.makedirs(os.path.join(outdir, full.strip("/")), exist_ok=True)
            walk(disc, start, fsize, full, outdir, listing, all_files)
            continue
        listing.append(f"FILE {full:40s} lsn={start:6d} size={fsize:10d} attr={attr:04x} fnum={fnum}")
        if fnum and not all_files:
            continue  # real-time stream: not needed for recompilation
        dst = os.path.join(outdir, full.strip("/"))
        # Real-time files: follow sectors until EOF bit in submode; data files: by size.
        nsec = (fsize + 2047) // 2048
        with open(dst, "wb") as o, open(dst + ".sectors", "wb") as r:
            for i in range(nsec):
                raw = disc.raw(start + i)
                d, sub = disc.data(start + i)
                r.write(raw)
                o.write(d)
        if nsec and all(not (disc.data(start + i)[1][2] & 0x20) for i in range(min(nsec, 4))):
            # plain Form 1 data file: trim to exact size, drop raw copy
            with open(dst, "r+b") as o:
                o.truncate(fsize)
            os.remove(dst + ".sectors")

def main():
    img, outdir = sys.argv[1], sys.argv[2]
    all_files = '--all' in sys.argv[3:]
    disc = Disc(img)
    label, _ = disc.data(16)
    pt_size = struct.unpack(">I", label[136:140])[0]
    pt_lsn = struct.unpack(">I", label[148:152])[0]
    pt, _ = disc.data(pt_lsn)
    # first path table entry = root
    nlen = pt[0]
    root_lsn = struct.unpack(">I", pt[2:6])[0]
    root_d, _ = disc.data(root_lsn)
    root_size = struct.unpack(">I", root_d[14:18])[0]
    os.makedirs(outdir, exist_ok=True)
    listing = []
    walk(disc, root_lsn, root_size, "", outdir, listing, all_files)
    with open(os.path.join(outdir, "_listing.txt"), "w") as o:
        o.write("\n".join(listing) + "\n")
    print("\n".join(listing))

if __name__ == "__main__":
    main()
