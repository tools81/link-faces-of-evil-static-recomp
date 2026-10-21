"""MC68000 instruction decoder (the 68070 in the CD-i is ISA compatible).

decode(mem, base, addr) -> Insn
"""
import struct

SZ = {0: 'b', 1: 'w', 2: 'l'}
NBYTES = {'b': 1, 'w': 2, 'l': 4}
CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']


class DecodeError(Exception):
    pass


class EA:
    """Effective address.
    kind: dn an ind postinc predec disp index absw absl pcdisp pcindex imm
          sr ccr usp
    """
    __slots__ = ('kind', 'reg', 'disp', 'xreg', 'xlong', 'value', 'addr')

    def __init__(self, kind, reg=0, disp=0, xreg=None, xlong=False, value=0, addr=None):
        self.kind = kind
        self.reg = reg
        self.disp = disp
        self.xreg = xreg      # index register: 0-7 = D0-7, 8-15 = A0-7
        self.xlong = xlong
        self.value = value
        self.addr = addr      # resolved absolute target for pc-relative

    def __str__(self):
        k = self.kind
        if k == 'dn': return 'd%d' % self.reg
        if k == 'an': return 'sp' if self.reg == 7 else 'a%d' % self.reg
        r = 'sp' if self.reg == 7 else 'a%d' % self.reg
        if k == 'ind': return '(%s)' % r
        if k == 'postinc': return '(%s)+' % r
        if k == 'predec': return '-(%s)' % r
        if k == 'disp': return '%d(%s)' % (self.disp, r)
        if k == 'index':
            return '%d(%s,%s.%s)' % (self.disp, r, xname(self.xreg), 'l' if self.xlong else 'w')
        if k == 'absw': return '($%x).w' % (self.value & 0xffffffff)
        if k == 'absl': return '($%x).l' % self.value
        if k == 'pcdisp': return '$%x(pc)' % self.addr
        if k == 'pcindex':
            return '$%x(pc,%s.%s)' % (self.addr, xname(self.xreg), 'l' if self.xlong else 'w')
        if k == 'imm': return '#$%x' % self.value
        if k in ('sr', 'ccr', 'usp'): return k
        return '?'


def xname(x):
    return ('d%d' % x) if x < 8 else ('a%d' % (x - 8))


class Insn:
    __slots__ = ('addr', 'length', 'op', 'size', 'src', 'dst', 'cc', 'target', 'extra', 'raw')

    def __init__(self, addr, op, size=None, src=None, dst=None):
        self.addr = addr
        self.op = op
        self.size = size
        self.src = src
        self.dst = dst
        self.cc = None
        self.target = None
        self.extra = None
        self.length = 0

    def __str__(self):
        m = self.op
        if self.cc is not None:
            m = m.replace('cc', CC[self.cc]) if 'cc' in m else m + CC[self.cc]
        if self.size:
            m += '.' + self.size
        ops = []
        if self.op == 'movem':
            regs = reglist_str(self.extra)
            if self.extra_dir_tomem():
                ops = [regs, str(self.dst)]
            else:
                ops = [str(self.src), regs]
        else:
            if self.src is not None: ops.append(str(self.src))
            if self.dst is not None: ops.append(str(self.dst))
            if self.target is not None: ops.append('$%x' % self.target)
            if self.op in ('trap', 'os9') and self.extra is not None:
                ops.append('$%x' % self.extra)
        return '%-8s %s' % (m, ','.join(ops))

    def extra_dir_tomem(self):
        return self.dst is not None and self.dst.kind not in ('dn', 'an') and self.src is None


def reglist_str(mask):
    names = ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)]
    return '/'.join(names[i] for i in range(16) if mask & (1 << i)) or '<none>'


class Reader:
    def __init__(self, mem, base, addr):
        self.mem = mem
        self.base = base
        self.pc = addr

    def w(self):
        o = self.pc - self.base
        if o < 0 or o + 2 > len(self.mem):
            raise DecodeError('out of range')
        v = (self.mem[o] << 8) | self.mem[o + 1]
        self.pc += 2
        return v

    def sw(self):
        v = self.w()
        return v - 0x10000 if v & 0x8000 else v

    def l(self):
        return (self.w() << 16) | self.w()


def s8(v):
    return v - 0x100 if v & 0x80 else v


def read_ea(r, mode, reg, size):
    if mode == 0: return EA('dn', reg)
    if mode == 1: return EA('an', reg)
    if mode == 2: return EA('ind', reg)
    if mode == 3: return EA('postinc', reg)
    if mode == 4: return EA('predec', reg)
    if mode == 5: return EA('disp', reg, disp=r.sw())
    if mode == 6:
        ext = r.w()
        if ext & 0x0700:
            raise DecodeError('full extension word')
        return EA('index', reg, disp=s8(ext & 0xff), xreg=(ext >> 12) & 15, xlong=bool(ext & 0x800))
    if mode == 7:
        if reg == 0:
            v = r.sw()
            return EA('absw', value=v & 0xffffffff)
        if reg == 1: return EA('absl', value=r.l())
        if reg == 2:
            p = r.pc
            d = r.sw()
            return EA('pcdisp', disp=d, addr=(p + d) & 0xffffffff)
        if reg == 3:
            p = r.pc
            ext = r.w()
            if ext & 0x0700:
                raise DecodeError('full extension word')
            d = s8(ext & 0xff)
            return EA('pcindex', disp=d, xreg=(ext >> 12) & 15, xlong=bool(ext & 0x800), addr=(p + d) & 0xffffffff)
        if reg == 4:
            if size is None:
                raise DecodeError('imm without size')
            if size == 'b': v = r.w() & 0xff
            elif size == 'w': v = r.w()
            else: v = r.l()
            return EA('imm', value=v)
    raise DecodeError('bad ea %d/%d' % (mode, reg))


# EA category checks
def is_data(ea): return ea.kind != 'an'
def is_mem(ea): return ea.kind not in ('dn', 'an')
def is_alterable(ea): return ea.kind not in ('pcdisp', 'pcindex', 'imm')
def is_control(ea): return ea.kind in ('ind', 'disp', 'index', 'absw', 'absl', 'pcdisp', 'pcindex')


def need(cond):
    if not cond:
        raise DecodeError('invalid ea for op')


def decode(mem, base, addr):
    r = Reader(mem, base, addr)
    op = r.w()
    i = _decode(r, op, addr)
    i.length = r.pc - addr
    i.raw = op
    return i


def _decode(r, op, addr):
    hi = op >> 12
    mode = (op >> 3) & 7
    reg = op & 7
    reg9 = (op >> 9) & 7
    ss = (op >> 6) & 3

    if hi == 0:
        if op & 0x100:
            if mode == 1:
                opm = (op >> 6) & 3
                sz = 'w' if opm in (0, 2) else 'l'
                d = r.sw()
                i = Insn(addr, 'movep', sz)
                if opm < 2:
                    i.src = EA('disp', reg, disp=d); i.dst = EA('dn', reg9)
                else:
                    i.src = EA('dn', reg9); i.dst = EA('disp', reg, disp=d)
                return i
            name = ['btst', 'bchg', 'bclr', 'bset'][ss]
            dst = read_ea(r, mode, reg, 'b')
            need(is_data(dst))
            if name != 'btst': need(is_alterable(dst))
            return Insn(addr, name, None, EA('dn', reg9), dst)
        t = (op >> 9) & 7
        if t == 4:
            name = ['btst', 'bchg', 'bclr', 'bset'][ss]
            bit = r.w() & 0xff
            dst = read_ea(r, mode, reg, 'b')
            need(is_data(dst))
            if name == 'btst':
                need(dst.kind != 'imm')
            else:
                need(is_alterable(dst))
            return Insn(addr, name, None, EA('imm', value=bit), dst)
        names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
        if t not in names or ss == 3:
            raise DecodeError('bad op0')
        name = names[t]
        sz = SZ[ss]
        if mode == 7 and reg == 4 and name in ('ori', 'andi', 'eori'):
            v = r.w()
            if sz == 'b': return Insn(addr, name, 'b', EA('imm', value=v & 0xff), EA('ccr'))
            if sz == 'w': return Insn(addr, name, 'w', EA('imm', value=v), EA('sr'))
            raise DecodeError('bad')
        src = read_ea(r, 7, 4, sz)
        dst = read_ea(r, mode, reg, sz)
        need(is_data(dst) and is_alterable(dst))
        return Insn(addr, name, sz, src, dst)

    if hi in (1, 2, 3):
        sz = {1: 'b', 3: 'w', 2: 'l'}[hi]
        src = read_ea(r, mode, reg, sz)
        dmode = (op >> 6) & 7
        if dmode == 1:
            need(sz != 'b')
            return Insn(addr, 'movea', sz, src, EA('an', reg9))
        if sz == 'b': need(src.kind != 'an')
        dst = read_ea(r, dmode, reg9, sz)
        need(is_data(dst) and is_alterable(dst))
        return Insn(addr, 'move', sz, src, dst)

    if hi == 4:
        if op == 0x4afc: return Insn(addr, 'illegal')
        if op == 0x4e70: return Insn(addr, 'reset')
        if op == 0x4e71: return Insn(addr, 'nop')
        if op == 0x4e72:
            return Insn(addr, 'stop', None, EA('imm', value=r.w()))
        if op == 0x4e73: return Insn(addr, 'rte')
        if op == 0x4e75: return Insn(addr, 'rts')
        if op == 0x4e76: return Insn(addr, 'trapv')
        if op == 0x4e77: return Insn(addr, 'rtr')
        if (op & 0xfff0) == 0x4e40:
            i = Insn(addr, 'trap', None, EA('imm', value=op & 15))
            if (op & 15) == 0:
                i.op = 'os9'
                i.extra = r.w()
            elif (op & 15) in (13, 15):
                # OS-9 trap handler calls (cio/math): trap #n, dc.w func
                i.extra = r.w()
            return i
        if (op & 0xfff8) == 0x4e50:
            return Insn(addr, 'link', None, EA('an', reg), EA('imm', value=r.sw() & 0xffffffff))
        if (op & 0xfff8) == 0x4e58: return Insn(addr, 'unlk', None, EA('an', reg))
        if (op & 0xfff8) == 0x4e60: return Insn(addr, 'move', 'l', EA('an', reg), EA('usp'))
        if (op & 0xfff8) == 0x4e68: return Insn(addr, 'move', 'l', EA('usp'), EA('an', reg))
        if (op & 0xffc0) == 0x4e80:
            ea = read_ea(r, mode, reg, None); need(is_control(ea))
            return Insn(addr, 'jsr', None, ea)
        if (op & 0xffc0) == 0x4ec0:
            ea = read_ea(r, mode, reg, None); need(is_control(ea))
            return Insn(addr, 'jmp', None, ea)
        if op & 0x100:
            if ss == 3:
                ea = read_ea(r, mode, reg, None); need(is_control(ea))
                return Insn(addr, 'lea', 'l', ea, EA('an', reg9))
            if ss == 2:
                ea = read_ea(r, mode, reg, 'w'); need(is_data(ea))
                return Insn(addr, 'chk', 'w', ea, EA('dn', reg9))
            raise DecodeError('bad 4')
        sub = (op >> 8) & 15
        if sub == 0:
            if ss == 3:
                ea = read_ea(r, mode, reg, 'w'); need(is_data(ea) and is_alterable(ea))
                return Insn(addr, 'move', 'w', EA('sr'), ea)
            ea = read_ea(r, mode, reg, SZ[ss]); need(is_data(ea) and is_alterable(ea))
            return Insn(addr, 'negx', SZ[ss], None, ea)
        if sub == 2:
            if ss == 3: raise DecodeError('move from ccr (68010)')
            ea = read_ea(r, mode, reg, SZ[ss]); need(is_data(ea) and is_alterable(ea))
            return Insn(addr, 'clr', SZ[ss], None, ea)
        if sub == 4:
            if ss == 3:
                ea = read_ea(r, mode, reg, 'w'); need(is_data(ea))
                return Insn(addr, 'move', 'w', ea, EA('ccr'))
            ea = read_ea(r, mode, reg, SZ[ss]); need(is_data(ea) and is_alterable(ea))
            return Insn(addr, 'neg', SZ[ss], None, ea)
        if sub == 6:
            if ss == 3:
                ea = read_ea(r, mode, reg, 'w'); need(is_data(ea))
                return Insn(addr, 'move', 'w', ea, EA('sr'))
            ea = read_ea(r, mode, reg, SZ[ss]); need(is_data(ea) and is_alterable(ea))
            return Insn(addr, 'not', SZ[ss], None, ea)
        if sub == 8:
            if ss == 0:
                ea = read_ea(r, mode, reg, 'b'); need(is_data(ea) and is_alterable(ea))
                return Insn(addr, 'nbcd', 'b', None, ea)
            if ss == 1:
                if mode == 0: return Insn(addr, 'swap', 'w', None, EA('dn', reg))
                ea = read_ea(r, mode, reg, None); need(is_control(ea))
                return Insn(addr, 'pea', 'l', ea)
            if mode == 0:
                return Insn(addr, 'ext', 'w' if ss == 2 else 'l', None, EA('dn', reg))
            mask = r.w()
            ea = read_ea(r, mode, reg, None)
            need(ea.kind in ('ind', 'predec', 'disp', 'index', 'absw', 'absl'))
            i = Insn(addr, 'movem', 'w' if ss == 2 else 'l', None, ea)
            if ea.kind == 'predec':  # reversed mask
                mask = int('{:016b}'.format(mask)[::-1], 2)
            i.extra = mask
            return i
        if sub == 0xa:
            if ss == 3:
                ea = read_ea(r, mode, reg, 'b'); need(is_data(ea) and is_alterable(ea))
                return Insn(addr, 'tas', 'b', None, ea)
            ea = read_ea(r, mode, reg, SZ[ss])
            if ss == 0: need(ea.kind != 'an')
            return Insn(addr, 'tst', SZ[ss], None, ea)
        if sub == 0xc:
            if ss in (2, 3):
                mask = r.w()
                ea = read_ea(r, mode, reg, None)
                need(ea.kind in ('ind', 'postinc', 'disp', 'index', 'absw', 'absl', 'pcdisp', 'pcindex'))
                i = Insn(addr, 'movem', 'w' if ss == 2 else 'l', ea, None)
                i.extra = mask
                return i
        raise DecodeError('bad 4xxx')

    if hi == 5:
        if ss == 3:
            cc = (op >> 8) & 15
            if mode == 1:
                p = r.pc
                d = r.sw()
                i = Insn(addr, 'db', None, EA('dn', reg))
                i.cc = cc
                i.target = (p + d) & 0xffffffff
                return i
            ea = read_ea(r, mode, reg, 'b'); need(is_data(ea) and is_alterable(ea))
            i = Insn(addr, 's', 'b', None, ea)
            i.cc = cc
            return i
        q = reg9 or 8
        ea = read_ea(r, mode, reg, SZ[ss]); need(is_alterable(ea))
        if ss == 0: need(ea.kind != 'an')
        return Insn(addr, 'subq' if op & 0x100 else 'addq', SZ[ss], EA('imm', value=q), ea)

    if hi == 6:
        cc = (op >> 8) & 15
        d = op & 0xff
        p = r.pc
        if d == 0:
            d = r.sw()
        elif d == 0xff:
            raise DecodeError('long branch (68020)')
        else:
            d = s8(d)
        name = {0: 'bra', 1: 'bsr'}.get(cc, 'b')
        i = Insn(addr, name)
        if name == 'b': i.cc = cc
        i.target = (p + d) & 0xffffffff
        i.size = None
        return i

    if hi == 7:
        if op & 0x100: raise DecodeError('bad moveq')
        return Insn(addr, 'moveq', 'l', EA('imm', value=s8(op & 0xff) & 0xffffffff), EA('dn', reg9))

    if hi in (8, 0xc):
        if ss == 3:
            name = ('divu' if hi == 8 else 'mulu') if not (op & 0x100) else ('divs' if hi == 8 else 'muls')
            ea = read_ea(r, mode, reg, 'w'); need(is_data(ea))
            return Insn(addr, name, 'w', ea, EA('dn', reg9))
        if (op & 0x1f0) == 0x100:
            name = 'sbcd' if hi == 8 else 'abcd'
            if op & 8:
                return Insn(addr, name, 'b', EA('predec', reg), EA('predec', reg9))
            return Insn(addr, name, 'b', EA('dn', reg), EA('dn', reg9))
        if hi == 0xc and (op & 0x1f8) in (0x140, 0x148, 0x188):
            m = op & 0x1f8
            if m == 0x140: return Insn(addr, 'exg', 'l', EA('dn', reg9), EA('dn', reg))
            if m == 0x148: return Insn(addr, 'exg', 'l', EA('an', reg9), EA('an', reg))
            return Insn(addr, 'exg', 'l', EA('dn', reg9), EA('an', reg))
        name = 'or' if hi == 8 else 'and'
        sz = SZ[ss]
        ea = read_ea(r, mode, reg, sz)
        if op & 0x100:
            need(is_mem(ea) and is_alterable(ea))
            return Insn(addr, name, sz, EA('dn', reg9), ea)
        need(is_data(ea))
        return Insn(addr, name, sz, ea, EA('dn', reg9))

    if hi in (9, 0xd):
        base = 'sub' if hi == 9 else 'add'
        if ss == 3:
            sz = 'l' if op & 0x100 else 'w'
            ea = read_ea(r, mode, reg, sz)
            return Insn(addr, base + 'a', sz, ea, EA('an', reg9))
        sz = SZ[ss]
        if (op & 0x130) == 0x100:
            if op & 8:
                return Insn(addr, base + 'x', sz, EA('predec', reg), EA('predec', reg9))
            return Insn(addr, base + 'x', sz, EA('dn', reg), EA('dn', reg9))
        ea = read_ea(r, mode, reg, sz)
        if op & 0x100:
            need(is_mem(ea) and is_alterable(ea))
            return Insn(addr, base, sz, EA('dn', reg9), ea)
        if sz == 'b': need(ea.kind != 'an')
        return Insn(addr, base, sz, ea, EA('dn', reg9))

    if hi == 0xb:
        if ss == 3:
            sz = 'l' if op & 0x100 else 'w'
            ea = read_ea(r, mode, reg, sz)
            return Insn(addr, 'cmpa', sz, ea, EA('an', reg9))
        sz = SZ[ss]
        if op & 0x100:
            if mode == 1:
                return Insn(addr, 'cmpm', sz, EA('postinc', reg), EA('postinc', reg9))
            ea = read_ea(r, mode, reg, sz); need(is_data(ea) and is_alterable(ea))
            return Insn(addr, 'eor', sz, EA('dn', reg9), ea)
        ea = read_ea(r, mode, reg, sz)
        if sz == 'b': need(ea.kind != 'an')
        return Insn(addr, 'cmp', sz, ea, EA('dn', reg9))

    if hi == 0xe:
        if ss == 3:
            t = (op >> 9) & 7
            if t > 3: raise DecodeError('bitfield (020)')
            name = ['as', 'ls', 'rox', 'ro'][t] + ('l' if op & 0x100 else 'r')
            ea = read_ea(r, mode, reg, 'w'); need(is_mem(ea) and is_alterable(ea))
            return Insn(addr, name, 'w', EA('imm', value=1), ea)
        t = (op >> 3) & 3
        name = ['as', 'ls', 'rox', 'ro'][t] + ('l' if op & 0x100 else 'r')
        if op & 0x20:
            src = EA('dn', reg9)
        else:
            src = EA('imm', value=reg9 or 8)
        return Insn(addr, name, SZ[ss], src, EA('dn', reg))

    if hi == 0xa:
        return Insn(addr, 'linea', None, EA('imm', value=op))
    if hi == 0xf:
        return Insn(addr, 'linef', None, EA('imm', value=op))
    raise DecodeError('unhandled')
