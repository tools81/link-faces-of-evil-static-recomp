"""OS-9/68K module parsing."""
import struct


class Module:
    def __init__(self, data, base):
        self.data = data
        self.base = base
        assert data[0:2] == b'\x4a\xfc'
        (self.size, self.owner, self.nameoff) = struct.unpack('>III', data[4:16])
        (self.access, self.typelang, self.attrrev, self.edition) = struct.unpack('>HBBH', data[16:22])
        (self.execoff, self.excptoff, self.memsize, self.stacksize,
         self.idataoff, self.irefsoff) = struct.unpack('>IIIIII', data[0x30:0x48])
        e = data.index(b'\0', self.nameoff)
        self.name = data[self.nameoff:e].decode('latin-1')
        self.data = data[:self.size]

    def w(self, off):
        return struct.unpack('>H', self.data[off:off + 2])[0]

    def l(self, off):
        return struct.unpack('>I', self.data[off:off + 4])[0]

    def idata(self):
        """returns (data_offset, bytes) for initialized data"""
        o = self.idataoff
        doff, cnt = struct.unpack('>II', self.data[o:o + 8])
        return doff, self.data[o + 8:o + 8 + cnt]

    def irefs(self):
        """Microware irefs: groups of (hi word, count, offsets...) terminated by 0,0.
        Two tables: code-relative refs then data-relative refs.
        returns (code_refs, data_refs) as lists of data-area offsets"""
        o = self.irefsoff
        tables = []
        for _ in range(2):
            refs = []
            while True:
                hi, cnt = struct.unpack('>HH', self.data[o:o + 4])
                o += 4
                if cnt == 0:
                    break
                for _ in range(cnt):
                    lo = struct.unpack('>H', self.data[o:o + 2])[0]
                    o += 2
                    refs.append((hi << 16) | lo)
            tables.append(refs)
        return tables[0], tables[1]
