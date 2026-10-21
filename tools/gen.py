"""68000 -> C static recompiler for the cdi_link OS-9 module.

Every discovered function becomes a C function `uint32_t f_XXXXXX(uint32_t pc)`
that starts executing at `pc` (one of its labels) and returns the 68k pc at
which execution must continue when control leaves it (RTS to an unknown caller,
tail jumps, indirect jumps, budget expiry for signal delivery, ...).  A
dispatcher (runtime/cpu.c) maps any pc back to the right host function, so
correctness never depends on the native C call stack.  Direct calls are made
natively for speed:  push return address, call callee; if the callee returns
the expected return address we continue inline, otherwise we propagate.
"""
import os
import sys
import collections

sys.path.insert(0, os.path.dirname(__file__))
from m68k import CC, NBYTES
from analyze import load, Analysis

FX, FN, FZ, FV, FC = 1, 2, 4, 8, 16
BLOCK_END = ('bra', 'b', 'db', 'jmp', 'jsr', 'bsr', 'rts', 'os9', 'trap', 'rtr', 'rte')
ALLF = 31
NZVC = FN | FZ | FV | FC

CC_USES = {0: 0, 1: 0, 2: FC | FZ, 3: FC | FZ, 4: FC, 5: FC, 6: FZ, 7: FZ, 8: FV, 9: FV,
           10: FN, 11: FN, 12: FN | FV, 13: FN | FV, 14: FN | FV | FZ, 15: FN | FV | FZ}

BITS = {'b': 8, 'w': 16, 'l': 32}
MASK = {'b': '0xFFu', 'w': '0xFFFFu', 'l': '0xFFFFFFFFu'}
UT = {'b': 'uint8_t', 'w': 'uint16_t', 'l': 'uint32_t'}
ST = {'b': 'int8_t', 'w': 'int16_t', 'l': 'int32_t'}
RD = {'b': 'RB', 'w': 'RW', 'l': 'RL'}
WR = {'b': 'WB', 'w': 'WW', 'l': 'WL'}


def cond_expr(cc):
    return ['1', '0',
            '(!CF && !ZF)', '(CF || ZF)',
            '(!CF)', '(CF)',
            '(!ZF)', '(ZF)',
            '(!VF)', '(VF)',
            '(!NF)', '(NF)',
            '(NF == VF)', '(NF != VF)',
            '(!ZF && NF == VF)', '(ZF || NF != VF)'][cc]


def flag_info(i):
    """returns (uses, defs) flag masks"""
    op = i.op
    if op in ('b', 's'):
        return CC_USES[i.cc], 0
    if op == 'db':
        return CC_USES[i.cc], 0
    if op in ('addx', 'subx', 'negx', 'abcd', 'sbcd', 'nbcd'):
        return FX | FZ, ALLF
    if op in ('roxl', 'roxr'):
        return FX, ALLF
    if op in ('asl', 'asr', 'lsl', 'lsr'):
        if i.src.kind == 'imm':
            return 0, ALLF
        return FX, ALLF
    if op in ('rol', 'ror'):
        return 0, NZVC
    if op == 'move' and i.src is not None and i.src.kind in ('sr', 'ccr'):
        return ALLF, 0
    if op == 'move' and i.dst is not None and i.dst.kind in ('sr', 'ccr'):
        return 0, ALLF
    if op in ('ori', 'andi', 'eori') and i.dst.kind in ('sr', 'ccr'):
        return ALLF, ALLF
    if op == 'move' and i.dst is not None and i.dst.kind == 'usp':
        return 0, 0
    if op == 'move' and i.src is not None and i.src.kind == 'usp':
        return 0, 0
    if op in ('move', 'moveq', 'tst', 'and', 'or', 'eor', 'not', 'clr', 'ext', 'swap',
              'mulu', 'muls', 'divu', 'divs', 'tas', 'andi', 'ori', 'eori'):
        return 0, NZVC
    if op in ('add', 'sub', 'neg', 'addi', 'subi'):
        return 0, ALLF
    if op in ('addq', 'subq'):
        if i.dst.kind == 'an':
            return 0, 0
        return 0, ALLF
    if op in ('cmp', 'cmpa', 'cmpi', 'cmpm'):
        return 0, NZVC
    if op in ('btst', 'bchg', 'bclr', 'bset'):
        return 0, FZ
    if op == 'chk':
        return 0, FN
    if op in ('os9', 'trap'):
        return 0, ALLF
    if op == 'rtr':
        return 0, ALLF
    return 0, 0


class Func:
    def __init__(self, entry):
        self.entry = entry
        self.addrs = []
        self.set = set()


class Gen:
    def __init__(self, an, hooks=None):
        self.an = an
        self.hooks = hooks or {}   # function entry address -> C hook function name
        self.mod = an.mod
        self.insns = an.insns
        self.entries = set(an.funcs) | set(an.labels)
        self.build_funcs()
        self.liveness()

    # ---------------------------------------------------------------- CFG
    def succ(self, i):
        """intra-function successors, plus flag 'exit' if control may leave"""
        a = i.addr
        nxt = a + i.length
        op = i.op
        if op == 'bra':
            return [i.target], False
        if op in ('b', 'db'):
            return [i.target, nxt], False
        if op in ('bsr',):
            return [nxt], True
        if op == 'jsr':
            return [nxt], True
        if op == 'jmp':
            if a in self.an.jumptables:
                return list(self.an.jumptables[a]), False
            if i.src.kind == 'pcdisp':
                return [i.src.addr], False
            if a in self.an.pcrel_calls:
                return [self.an.pcrel_calls[a]], False
            return [], True
        if op in ('rts', 'rte', 'rtr', 'illegal', 'stop', 'reset', 'linea', 'linef'):
            return [], True
        if op == 'os9':
            if i.extra in (0x06, 0x1e):
                return [], True
            return [nxt], True
        if op == 'trap':
            return [nxt], True
        return [nxt], False

    def build_funcs(self):
        self.funcs = {}
        owner = {}
        for e in sorted(self.an.funcs):
            if e not in self.insns:
                continue
            f = Func(e)
            work = [e]
            while work:
                a = work.pop()
                if a in f.set or a not in self.insns:
                    continue
                f.set.add(a)
                i = self.insns[a]
                s, _ = self.succ(i)
                for t in s:
                    # don't walk into other functions' entry points (they get their own)
                    if t in self.an.funcs and t != e:
                        continue
                    work.append(t)
            f.addrs = sorted(f.set)
            self.funcs[e] = f
            for a in f.addrs:
                owner.setdefault(a, e)
        self.owner = owner
        # labels that are not reachable within any function body (e.g. branch
        # target into another function's body): ensure owner exists
        missing = [a for a in self.entries if a in self.insns and a not in owner]
        for a in missing:
            # create a pseudo-function for it
            f = Func(a)
            work = [a]
            while work:
                x = work.pop()
                if x in f.set or x not in self.insns:
                    continue
                f.set.add(x)
                s, _ = self.succ(self.insns[x])
                for t in s:
                    if t in self.an.funcs and t != a:
                        continue
                    work.append(t)
            f.addrs = sorted(f.set)
            self.funcs[a] = f
            for x in f.addrs:
                owner.setdefault(x, a)

    def liveness(self):
        """global (whole-program) backward flag liveness over the intra-procedural CFG."""
        live_in = {a: 0 for a in self.insns}
        info = {a: flag_info(i) for a, i in self.insns.items()}
        succs = {a: self.succ(i) for a, i in self.insns.items()}
        preds = collections.defaultdict(list)
        for a, (s, ex) in succs.items():
            for t in s:
                preds[t].append(a)
        self.live_out = {}
        work = collections.deque(sorted(self.insns, reverse=True))
        inq = set(work)
        while work:
            a = work.popleft()
            inq.discard(a)
            s, ex = succs[a]
            out = ALLF if ex else 0
            for t in s:
                out |= live_in.get(t, ALLF)
            self.live_out[a] = out
            uses, defs = info[a]
            new = uses | (out & ~defs)
            i = self.insns[a]
            # call instructions: the callee may read flags; conservatively live
            if i.op in ('bsr', 'jsr'):
                new = ALLF
            if new != live_in[a]:
                live_in[a] = new
                for p in preds[a]:
                    if p not in inq:
                        inq.add(p)
                        work.append(p)
        self.live_in = live_in

    # ---------------------------------------------------------------- emit
    def gen_files(self, outdir, tag, per_file=40):
        """write the C files for this module; returns (files, entries, names)"""
        os.makedirs(outdir, exist_ok=True)
        fl = sorted(self.funcs)
        files = []
        for k in range(0, len(fl), per_file):
            chunk = fl[k:k + per_file]
            name = 'recomp_%s_%03d.c' % (tag, k // per_file)
            with open(os.path.join(outdir, name), 'w') as o:
                o.write('/* generated by tools/gen.py - do not edit */\n#include "recomp.h"\n\n')
                for e in chunk:
                    o.write(self.gen_func(self.funcs[e]))
                    o.write('\n')
            files.append(name)
        entries = [(a, self.owner[a]) for a in sorted(self.owner)
                   if a in self.entries or a in self.an.funcs]
        return files, entries, dict(self.an.names), fl

    def gen_func(self, f):
        out = []
        name = self.an.names.get(f.entry, '')
        out.append('/* %s */\nuint32_t f_%06x(uint32_t pc) {\n' % (name, f.entry))
        out.append('  uint32_t r_;\n  (void)r_;\n')
        labels = [a for a in f.addrs if a in self.entries or a == f.entry]
        out.append('  switch (pc) {\n')
        for a in labels:
            hook = self.hooks.get(a) if a == f.entry else None
            if hook:
                # runtime hook: returns 0 to run the original code, or the pc to
                # continue at (after doing its own rts)
                out.append('    case 0x%x: { uint32_t h_ = %s(); if (h_) return h_; } goto L_%x;\n' % (a, hook, a))
            else:
                out.append('    case 0x%x: goto L_%x;\n' % (a, a))
        out.append('    default: return recomp_bad_entry(pc, 0x%x);\n  }\n' % f.entry)
        # basic-block leaders for cycle accounting
        need_label = set()
        for a in f.addrs:
            ii = self.insns[a]
            s, _ = self.succ(ii)
            for t in s:
                if t != a + ii.length:
                    need_label.add(t)
        need_label |= set(labels)
        prev_end = None
        prev_term = False
        for a in f.addrs:
            i = self.insns[a]
            if prev_end is not None and prev_end != a:
                # previous insn falls into an address we don't own? (shouldn't happen)
                pi = self.insns[prev_last]
                s, _ = self.succ(pi)
                if prev_end in s:
                    out.append('  return 0x%x;\n' % prev_end)
            if a in need_label:
                out.append('L_%x:\n' % a)
            # count instructions in this block for timing (a block also starts
            # after any control transfer, e.g. a conditional branch not taken)
            if a in need_label or prev_end != a or prev_term:
                n = 0
                b = a
                while b in f.set:
                    n += 1
                    bi = self.insns[b]
                    if bi.op in BLOCK_END:
                        break
                    b += bi.length
                    if b in need_label:
                        break
                out.append('  CYC(%d);\n' % n)
            out.append('  /* %06x: %s */\n' % (a, str(i).replace('*/', '* /')))
            try:
                body = self.gen_insn(i, f)
            except Exception as ex:
                raise Exception('at %x: %s: %s' % (a, i, ex))
            for line in body:
                out.append('  ' + line + '\n')
            prev_end = a + i.length
            prev_last = a
            prev_term = i.op in BLOCK_END
        # fallthrough off end
        last = self.insns[f.addrs[-1]]
        s, _ = self.succ(last)
        nxt = last.addr + last.length
        if nxt in s:
            out.append('  return 0x%x;\n' % nxt)
        out.append('}\n')
        return ''.join(out)

    # -- helpers
    def goto(self, f, t, backward_ok=True):
        # backward transfers are the points where translated code may yield to
        # the scheduler (the interpreter uses the same rule)
        if t in f.set:
            if t <= self._cur.addr:
                return 'BUDGET(0x%x); goto L_%x;' % (t, t)
            return 'goto L_%x;' % t
        if t <= self._cur.addr:
            return 'BUDGET(0x%x); return 0x%x;' % (t, t)
        return 'return 0x%x;' % t

    def gen_insn(self, i, f):
        self._cur = i
        self._f = f
        self.tmp = 0
        live = self.live_out.get(i.addr, ALLF)
        self.live = live
        op = i.op
        m = getattr(self, 'op_' + op, None)
        if m is None:
            raise Exception('no emitter for ' + op)
        lines = m(i)
        if lines and len(lines) > 1:
            return ['{'] + ['  ' + l for l in lines] + ['}']
        return lines

    def t(self):
        self.tmp += 1
        return 't%d' % self.tmp

    def ea_addr(self, L, ea, sz):
        """emit address computation, return C expr (temp var name)"""
        k = ea.kind
        n = NBYTES[sz] if sz else 4
        if k == 'ind':
            return 'A(%d)' % ea.reg
        if k == 'postinc':
            v = self.t()
            inc = 2 if (ea.reg == 7 and n == 1) else n
            L.append('uint32_t %s = A(%d); A(%d) += %d;' % (v, ea.reg, ea.reg, inc))
            return v
        if k == 'predec':
            v = self.t()
            dec = 2 if (ea.reg == 7 and n == 1) else n
            L.append('A(%d) -= %d; uint32_t %s = A(%d);' % (ea.reg, dec, v, ea.reg))
            return v
        if k == 'disp':
            return '(A(%d) + (%d))' % (ea.reg, ea.disp)
        if k == 'index':
            return '(A(%d) + (%d) + %s)' % (ea.reg, ea.disp, self.xidx(ea))
        if k == 'absw' or k == 'absl':
            return '0x%xu' % (ea.value & 0xffffffff)
        if k == 'pcdisp':
            return '0x%xu' % ea.addr
        if k == 'pcindex':
            return '(0x%xu + %s)' % (ea.addr, self.xidx(ea))
        raise Exception('ea_addr ' + k)

    def xidx(self, ea):
        x = ea.xreg
        r = ('D(%d)' % x) if x < 8 else ('A(%d)' % (x - 8))
        if ea.xlong:
            return r
        return '(uint32_t)(int32_t)(int16_t)%s' % r

    def read(self, L, ea, sz, addr_out=None):
        """returns C expr of value (unsigned of size).  If addr_out is a list,
        stores the computed address expression for later write-back."""
        k = ea.kind
        if k == 'dn':
            return '(%s)D(%d)' % (UT[sz], ea.reg) if sz != 'l' else 'D(%d)' % ea.reg
        if k == 'an':
            return '(%s)A(%d)' % (UT[sz], ea.reg) if sz != 'l' else 'A(%d)' % ea.reg
        if k == 'imm':
            v = ea.value & {'b': 0xff, 'w': 0xffff, 'l': 0xffffffff}[sz]
            return '0x%xu' % v
        a = self.ea_addr(L, ea, sz)
        if addr_out is not None:
            if not a.isidentifier():
                v = self.t()
                L.append('uint32_t %s = %s;' % (v, a))
                a = v
            addr_out.append(a)
        v = self.t()
        L.append('%s %s = %s(%s);' % (UT[sz], v, RD[sz], a))
        return v

    def write(self, L, ea, sz, val, addr=None):
        k = ea.kind
        if k == 'dn':
            if sz == 'l':
                L.append('D(%d) = %s;' % (ea.reg, val))
            elif sz == 'w':
                L.append('D(%d) = (D(%d) & 0xFFFF0000u) | (uint16_t)(%s);' % (ea.reg, ea.reg, val))
            else:
                L.append('D(%d) = (D(%d) & 0xFFFFFF00u) | (uint8_t)(%s);' % (ea.reg, ea.reg, val))
            return
        if k == 'an':
            if sz == 'w':
                L.append('A(%d) = (uint32_t)(int32_t)(int16_t)(%s);' % (ea.reg, val))
            else:
                L.append('A(%d) = %s;' % (ea.reg, val))
            return
        if addr is None:
            addr = self.ea_addr(L, ea, sz)
        L.append('%s(%s, %s);' % (WR[sz], addr, val))

    def flags_nz(self, L, sz, r):
        lv = self.live
        b = BITS[sz]
        if lv & FN:
            L.append('NF = ((%s) >> %d) & 1;' % (r, b - 1))
        if lv & FZ:
            L.append('ZF = ((%s) & %s) == 0;' % (r, MASK[sz]))

    def flags_logic(self, L, sz, r):
        self.flags_nz(L, sz, r)
        if self.live & FV:
            L.append('VF = 0;')
        if self.live & FC:
            L.append('CF = 0;')

    # ---------------------------------------------------------------- ops
    def op_move(self, i):
        L = []
        sz = i.size
        if i.dst.kind == 'ccr' or i.dst.kind == 'sr':
            v = self.read(L, i.src, 'w')
            L.append('SET_CCR(%s);' % v)
            if i.dst.kind == 'sr':
                L.append('SET_SRH((%s) >> 8);' % v)
            return L
        if i.src.kind in ('sr', 'ccr'):
            self.write(L, i.dst, 'w', 'GET_SR()')
            return L
        if i.src.kind == 'usp' or i.dst.kind == 'usp':
            L.append('/* move usp ignored */')
            return L
        v = self.read(L, i.src, sz)
        if i.dst.kind not in ('dn', 'an'):
            # evaluate value into temp before dst side-effects
            if not v.startswith('t') and not v.startswith('0x'):
                tv = self.t()
                L.append('%s %s = %s;' % (UT[sz], tv, v))
                v = tv
        self.write(L, i.dst, sz, v)
        self.flags_logic(L, sz, v)
        return L

    def op_movea(self, i):
        L = []
        v = self.read(L, i.src, i.size)
        self.write(L, i.dst, i.size, v)
        return L

    def op_moveq(self, i):
        L = ['D(%d) = 0x%xu;' % (i.dst.reg, i.src.value)]
        v = i.src.value
        if self.live & FN: L.append('NF = %d;' % (v >> 31))
        if self.live & FZ: L.append('ZF = %d;' % (v == 0))
        if self.live & FV: L.append('VF = 0;')
        if self.live & FC: L.append('CF = 0;')
        return L

    def op_lea(self, i):
        L = []
        a = self.ea_addr(L, i.src, None)
        L.append('A(%d) = %s;' % (i.dst.reg, a))
        return L

    def op_pea(self, i):
        L = []
        a = self.ea_addr(L, i.src, None)
        v = self.t()
        L.append('uint32_t %s = %s;' % (v, a))
        L.append('A(7) -= 4; WL(A(7), %s);' % v)
        return L

    def op_clr(self, i):
        L = []
        self.write(L, i.dst, i.size, '0')
        if self.live & FN: L.append('NF = 0;')
        if self.live & FZ: L.append('ZF = 1;')
        if self.live & FV: L.append('VF = 0;')
        if self.live & FC: L.append('CF = 0;')
        return L

    def op_tst(self, i):
        L = []
        v = self.read(L, i.dst, i.size)
        self.flags_logic(L, i.size, v)
        return L

    def _arith(self, i, kind, src_val=None):
        """kind: add/sub/cmp; i.dst is destination"""
        L = []
        sz = i.size
        s = src_val if src_val is not None else self.read(L, i.src, sz)
        if not (s.startswith('t') or s.startswith('0x')):
            ts = self.t()
            L.append('%s %s = %s;' % (UT[sz], ts, s))
            s = ts
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        if not (d.startswith('t')):
            td = self.t()
            L.append('%s %s = %s;' % (UT[sz], td, d))
            d = td
        r = self.t()
        op = '+' if kind == 'add' else '-'
        L.append('%s %s = (%s)(%s %s %s);' % (UT[sz], r, UT[sz], d, op, s))
        if kind != 'cmp':
            self.write(L, i.dst, sz, r, addr[0] if addr else None)
        self.arith_flags(L, kind, sz, s, d, r)
        return L

    def arith_flags(self, L, kind, sz, s, d, r, xflag=True):
        lv = self.live
        b = BITS[sz] - 1
        if kind == 'add':
            cexpr = '((((%s) & (%s)) | (~(%s) & ((%s) | (%s)))) >> %d) & 1' % (s, d, r, s, d, b)
            vexpr = '((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1' % (s, r, d, r, b)
        else:
            cexpr = '((((%s) & ~(%s)) | ((%s) & ~(%s)) | ((%s) & (%s))) >> %d) & 1' % (s, d, r, d, s, r, b)
            vexpr = '((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1' % (s, d, r, d, b)
        if kind == 'cmp':
            xflag = False
        if lv & FC or (xflag and lv & FX):
            L.append('CF = %s;' % cexpr)
            if xflag and lv & FX:
                L.append('XF = CF;')
        if lv & FV:
            L.append('VF = %s;' % vexpr)
        self.flags_nz(L, sz, r)

    def op_add(self, i): return self._arith(i, 'add')
    def op_sub(self, i): return self._arith(i, 'sub')
    def op_cmp(self, i): return self._arith(i, 'cmp')
    def op_addi(self, i): return self._arith(i, 'add')
    def op_subi(self, i): return self._arith(i, 'sub')
    def op_cmpi(self, i): return self._arith(i, 'cmp')
    def op_cmpm(self, i): return self._arith(i, 'cmp')

    def op_addq(self, i):
        if i.dst.kind == 'an':
            return ['A(%d) += %d;' % (i.dst.reg, i.src.value)]
        return self._arith(i, 'add')

    def op_subq(self, i):
        if i.dst.kind == 'an':
            return ['A(%d) -= %d;' % (i.dst.reg, i.src.value)]
        return self._arith(i, 'sub')

    def op_adda(self, i):
        L = []
        s = self.read(L, i.src, i.size)
        if i.size == 'w':
            s = '(uint32_t)(int32_t)(int16_t)(%s)' % s
        L.append('A(%d) += %s;' % (i.dst.reg, s))
        return L

    def op_suba(self, i):
        L = []
        s = self.read(L, i.src, i.size)
        if i.size == 'w':
            s = '(uint32_t)(int32_t)(int16_t)(%s)' % s
        L.append('A(%d) -= %s;' % (i.dst.reg, s))
        return L

    def op_cmpa(self, i):
        L = []
        s = self.read(L, i.src, i.size)
        ts = self.t()
        if i.size == 'w':
            L.append('uint32_t %s = (uint32_t)(int32_t)(int16_t)(%s);' % (ts, s))
        else:
            L.append('uint32_t %s = %s;' % (ts, s))
        d = self.t()
        L.append('uint32_t %s = A(%d);' % (d, i.dst.reg))
        r = self.t()
        L.append('uint32_t %s = %s - %s;' % (r, d, ts))
        self.arith_flags(L, 'cmp', 'l', ts, d, r)
        return L

    def op_neg(self, i):
        L = []
        sz = i.size
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        td = self.t()
        L.append('%s %s = %s;' % (UT[sz], td, d))
        r = self.t()
        L.append('%s %s = (%s)(0 - %s);' % (UT[sz], r, UT[sz], td))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        self.arith_flags(L, 'sub', sz, td, '0', r)
        return L

    def _addx(self, i, kind):
        L = []
        sz = i.size
        s = self.read(L, i.src, sz)
        ts = self.t()
        L.append('%s %s = %s;' % (UT[sz], ts, s))
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        td = self.t()
        L.append('%s %s = %s;' % (UT[sz], td, d))
        r = self.t()
        op = '+' if kind == 'add' else '-'
        L.append('%s %s = (%s)(%s %s %s %s XF);' % (UT[sz], r, UT[sz], td, op, ts, op))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        save = self.live
        self.live = ALLF
        b = BITS[sz] - 1
        if kind == 'add':
            L.append('CF = XF = ((((%s) & (%s)) | (~(%s) & ((%s) | (%s)))) >> %d) & 1;' % (ts, td, r, ts, td, b))
            L.append('VF = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1;' % (ts, r, td, r, b))
        else:
            L.append('CF = XF = ((((%s) & ~(%s)) | ((%s) & ~(%s)) | ((%s) & (%s))) >> %d) & 1;' % (ts, td, r, td, ts, r, b))
            L.append('VF = ((((%s) ^ (%s)) & ((%s) ^ (%s))) >> %d) & 1;' % (ts, td, r, td, b))
        L.append('NF = ((%s) >> %d) & 1;' % (r, b))
        L.append('if (%s) ZF = 0;' % r)
        self.live = save
        return L

    def op_addx(self, i): return self._addx(i, 'add')
    def op_subx(self, i): return self._addx(i, 'sub')

    def op_negx(self, i):
        L = []
        sz = i.size
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        td = self.t()
        L.append('%s %s = %s;' % (UT[sz], td, d))
        r = self.t()
        L.append('%s %s = (%s)(0 - %s - XF);' % (UT[sz], r, UT[sz], td))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        b = BITS[sz] - 1
        L.append('CF = XF = (((%s) | (%s)) >> %d) & 1;' % (td, r, b))
        L.append('VF = (((%s) & (%s)) >> %d) & 1;' % (td, r, b))
        L.append('NF = ((%s) >> %d) & 1;' % (r, b))
        L.append('if (%s) ZF = 0;' % r)
        return L

    def _logic(self, i, cop):
        L = []
        sz = i.size
        if i.dst.kind in ('ccr', 'sr'):
            v = i.src.value
            if i.dst.kind == 'ccr':
                L.append('SET_CCR(GET_CCR() %s 0x%x);' % (cop, v & 0xff))
            else:
                L.append('{ uint16_t sr_ = GET_SR() %s 0x%x; SET_CCR(sr_); SET_SRH(sr_ >> 8); }' % (cop, v))
            return L
        s = self.read(L, i.src, sz)
        if not (s.startswith('t') or s.startswith('0x')):
            ts = self.t()
            L.append('%s %s = %s;' % (UT[sz], ts, s))
            s = ts
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        r = self.t()
        L.append('%s %s = %s %s %s;' % (UT[sz], r, d, cop, s))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        self.flags_logic(L, sz, r)
        return L

    def op_and(self, i): return self._logic(i, '&')
    def op_or(self, i): return self._logic(i, '|')
    def op_eor(self, i): return self._logic(i, '^')
    def op_andi(self, i): return self._logic(i, '&')
    def op_ori(self, i): return self._logic(i, '|')
    def op_eori(self, i): return self._logic(i, '^')

    def op_not(self, i):
        L = []
        sz = i.size
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        r = self.t()
        L.append('%s %s = (%s)~%s;' % (UT[sz], r, UT[sz], d))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        self.flags_logic(L, sz, r)
        return L

    def op_ext(self, i):
        L = []
        r = i.dst.reg
        if i.size == 'w':
            L.append('D(%d) = (D(%d) & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)D(%d);' % (r, r, r))
            self.flags_logic(L, 'w', 'D(%d)' % r)
        else:
            L.append('D(%d) = (uint32_t)(int32_t)(int16_t)D(%d);' % (r, r))
            self.flags_logic(L, 'l', 'D(%d)' % r)
        return L

    def op_swap(self, i):
        r = i.dst.reg
        L = ['D(%d) = (D(%d) >> 16) | (D(%d) << 16);' % (r, r, r)]
        self.flags_logic(L, 'l', 'D(%d)' % r)
        return L

    def op_exg(self, i):
        def reg(e):
            return ('D(%d)' if e.kind == 'dn' else 'A(%d)') % e.reg
        return ['{ uint32_t x_ = %s; %s = %s; %s = x_; }' % (reg(i.src), reg(i.src), reg(i.dst), reg(i.dst))]

    def op_mulu(self, i):
        L = []
        s = self.read(L, i.src, 'w')
        r = i.dst.reg
        L.append('D(%d) = (uint32_t)(uint16_t)D(%d) * (uint32_t)(uint16_t)(%s);' % (r, r, s))
        self.flags_logic(L, 'l', 'D(%d)' % r)
        return L

    def op_muls(self, i):
        L = []
        s = self.read(L, i.src, 'w')
        r = i.dst.reg
        L.append('D(%d) = (uint32_t)((int32_t)(int16_t)D(%d) * (int32_t)(int16_t)(%s));' % (r, r, s))
        self.flags_logic(L, 'l', 'D(%d)' % r)
        return L

    def op_divu(self, i):
        L = []
        s = self.read(L, i.src, 'w')
        r = i.dst.reg
        L.append('m68k_divu(%d, %s, 0x%x);' % (r, s, i.addr))
        return L

    def op_divs(self, i):
        L = []
        s = self.read(L, i.src, 'w')
        r = i.dst.reg
        L.append('m68k_divs(%d, %s, 0x%x);' % (r, s, i.addr))
        return L

    def _shift(self, i):
        L = []
        sz = i.size
        op = i.op
        if i.src.kind == 'imm':
            cnt = '%d' % i.src.value
        else:
            cnt = '(D(%d) & 63)' % i.src.reg
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        r = self.t()
        # fast paths when flags are dead and count is immediate
        simple = (self.live & ALLF) == 0 and i.src.kind == 'imm' and op in ('lsl', 'lsr', 'asl', 'asr')
        if simple:
            n = i.src.value
            if op in ('lsl', 'asl'):
                L.append('%s %s = (%s)(%s << %d);' % (UT[sz], r, UT[sz], d, n))
            elif op == 'lsr':
                L.append('%s %s = (%s)(%s >> %d);' % (UT[sz], r, UT[sz], d, n))
            else:
                L.append('%s %s = (%s)((%s)%s >> %d);' % (UT[sz], r, UT[sz], ST[sz], d, n))
        else:
            L.append('%s %s = (%s)SH_%s_%s(%s, %s);' % (UT[sz], r, UT[sz], op.upper(), sz.upper(), d, cnt))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        return L

    op_asl = op_asr = op_lsl = op_lsr = op_rol = op_ror = op_roxl = op_roxr = _shift

    def _bit(self, i):
        L = []
        if i.dst.kind == 'dn':
            sz = 'l'
            bm = 31
        else:
            sz = 'b'
            bm = 7
        if i.src.kind == 'imm':
            bit = '%d' % (i.src.value & bm)
        else:
            bit = '(D(%d) & %d)' % (i.src.reg, bm)
        addr = []
        d = self.read(L, i.dst, sz, addr_out=addr)
        if self.live & FZ:
            L.append('ZF = !((%s >> %s) & 1);' % (d, bit))
        if i.op == 'btst':
            return L
        r = self.t()
        if i.op == 'bset':
            L.append('%s %s = %s | (1u << %s);' % (UT[sz], r, d, bit))
        elif i.op == 'bclr':
            L.append('%s %s = %s & ~(1u << %s);' % (UT[sz], r, d, bit))
        else:
            L.append('%s %s = %s ^ (1u << %s);' % (UT[sz], r, d, bit))
        self.write(L, i.dst, sz, r, addr[0] if addr else None)
        return L

    op_btst = op_bset = op_bclr = op_bchg = _bit

    def op_s(self, i):
        L = []
        self.write(L, i.dst, 'b', '(%s ? 0xFF : 0)' % cond_expr(i.cc))
        return L

    def op_tas(self, i):
        L = []
        addr = []
        d = self.read(L, i.dst, 'b', addr_out=addr)
        self.flags_logic(L, 'b', d)
        self.write(L, i.dst, 'b', '(uint8_t)(%s | 0x80)' % d, addr[0] if addr else None)
        return L

    def op_chk(self, i):
        L = []
        s = self.read(L, i.src, 'w')
        L.append('if ((int16_t)D(%d) < 0 || (int16_t)D(%d) > (int16_t)(%s)) return m68k_exception(6, 0x%x);' %
                 (i.dst.reg, i.dst.reg, s, i.addr))
        return L

    def op_link(self, i):
        r = i.src.reg
        return ['A(7) -= 4; WL(A(7), A(%d)); A(%d) = A(7); A(7) += 0x%xu;' % (r, r, i.dst.value)]

    def op_unlk(self, i):
        r = i.src.reg
        return ['A(7) = A(%d); A(%d) = RL(A(7)); A(7) += 4;' % (r, r)]

    def op_movem(self, i):
        L = []
        sz = i.size
        n = NBYTES[sz]
        mask = i.extra
        regs = [k for k in range(16) if mask & (1 << k)]

        def rname(k):
            return ('D(%d)' % k) if k < 8 else ('A(%d)' % (k - 8))
        if i.src is None:
            # regs -> memory
            ea = i.dst
            if ea.kind == 'predec':
                # store in reverse order (a7..d0), decrementing
                L.append('uint32_t ma = A(%d);' % ea.reg)
                for k in reversed(regs):
                    L.append('ma -= %d; %s(ma, %s);' % (n, WR[sz], rname(k) if sz == 'l' else '(uint16_t)' + rname(k)))
                L.append('A(%d) = ma;' % ea.reg)
            else:
                a = self.ea_addr(L, ea, sz)
                L.append('uint32_t ma = %s;' % a)
                for k in regs:
                    L.append('%s(ma, %s); ma += %d;' % (WR[sz], rname(k) if sz == 'l' else '(uint16_t)' + rname(k), n))
        else:
            ea = i.src
            if ea.kind == 'postinc':
                L.append('uint32_t ma = A(%d);' % ea.reg)
            else:
                a = self.ea_addr(L, ea, sz)
                L.append('uint32_t ma = %s;' % a)
            for k in regs:
                if sz == 'l':
                    L.append('%s = RL(ma); ma += 4;' % rname(k))
                else:
                    L.append('%s = (uint32_t)(int32_t)(int16_t)RW(ma); ma += 2;' % rname(k))
            if ea.kind == 'postinc':
                L.append('A(%d) = ma;' % ea.reg)
        return L

    def op_nop(self, i):
        return []

    # -- control flow
    def op_bra(self, i):
        return [self.goto(self._f, i.target)]

    def op_b(self, i):
        return ['if %s { %s }' % (cond_expr(i.cc), self.goto(self._f, i.target))]

    def op_db(self, i):
        r = i.src.reg
        L = []
        nxt = i.addr + i.length
        body = 'uint16_t c_ = (uint16_t)D(%d) - 1; D(%d) = (D(%d) & 0xFFFF0000u) | c_; if (c_ != 0xFFFF) { %s }' % (
            r, r, r, self.goto(self._f, i.target))
        if i.cc == 1:
            L.append('{ %s }' % body)
        elif i.cc == 0:
            pass  # dbt: never loops
        else:
            L.append('if (!%s) { %s }' % (cond_expr(i.cc), body))
        return L

    def call(self, target_expr, ret, direct=None):
        """emit a 68k subroutine call"""
        L = ['A(7) -= 4; WL(A(7), 0x%xu);' % ret]
        if direct is not None and direct in self.funcs:
            L.append('r_ = f_%06x(0x%xu);' % (direct, direct))
        else:
            L.append('r_ = m68k_call(%s);' % target_expr)
        L.append('if (r_ != 0x%xu) return r_;' % ret)
        return L

    def op_bsr(self, i):
        ret = i.addr + i.length
        return self.call('0x%xu' % i.target, ret, i.target)

    def op_jsr(self, i):
        ret = i.addr + i.length
        ea = i.src
        if ea.kind == 'pcdisp':
            return self.call('0x%xu' % ea.addr, ret, ea.addr)
        if i.addr in self.an.pcrel_calls:
            t = self.an.pcrel_calls[i.addr]
            return self.call('0x%xu' % t, ret, t)
        if ea.kind == 'disp' and ea.reg == 5 and ea.addr is not None:
            # a5 jump table entry, statically resolved
            # the real call also executes the 'jmp abs.l' stub in the jump table
            L = ['A5CHECK(0x%x);' % i.addr, 'CYC(1);']
            return L + self.call('0x%xu' % ea.addr, ret, ea.addr)
        L = []
        a = self.ea_addr(L, ea, None)
        v = self.t()
        L.append('uint32_t %s = %s;' % (v, a))
        return L + self.call(v, ret)

    def op_jmp(self, i):
        ea = i.src
        if ea.kind == 'pcdisp':
            return [self.goto(self._f, ea.addr)]
        if i.addr in self.an.pcrel_calls:
            return [self.goto(self._f, self.an.pcrel_calls[i.addr])]
        if i.addr in self.an.jumptables:
            L = []
            a = self.ea_addr(L, ea, None)
            v = self.t()
            L.append('uint32_t %s = %s;' % (v, a))
            L.append('switch (%s) {' % v)
            for t in sorted(set(self.an.jumptables[i.addr])):
                L.append('  case 0x%x: %s' % (t, self.goto(self._f, t)))
            L.append('}')
            L.append('return %s;' % v)
            return L
        if ea.kind == 'disp' and ea.reg == 5 and ea.addr is not None:
            return ['CYC(1);', 'return 0x%xu;' % ea.addr]
        L = []
        a = self.ea_addr(L, ea, None)
        L.append('return %s;' % a)
        return L

    def op_rts(self, i):
        return ['{ uint32_t ra_ = RL(A(7)); A(7) += 4; return ra_; }']

    def op_rtr(self, i):
        return ['{ SET_CCR(RW(A(7))); uint32_t ra_ = RL(A(7) + 2); A(7) += 6; return ra_; }']

    def op_rte(self, i):
        return ['return m68k_exception(8, 0x%x);' % i.addr]

    def op_os9(self, i):
        nxt = i.addr + i.length
        if i.extra in (0x06, 0x1e):
            return ['return os9_syscall(0x%x, 0x%xu);' % (i.extra, nxt)]
        return ['r_ = os9_syscall(0x%x, 0x%xu);' % (i.extra, nxt),
                'if (r_ != 0x%xu) return r_;' % nxt]

    def op_trap(self, i):
        nxt = i.addr + i.length
        return ['return m68k_trap(%d, 0x%x, 0x%xu);' % (i.src.value, i.extra or 0, nxt)]

    def op_illegal(self, i):
        return ['return m68k_exception(4, 0x%x);' % i.addr]

    op_linea = op_linef = op_stop = op_reset = op_illegal

    def op_trapv(self, i):
        return ['if (VF) return m68k_exception(7, 0x%x);' % i.addr]

    def _bcd(self, i):
        L = []
        s = self.read(L, i.src, 'b') if i.src is not None else '0'
        ts = self.t()
        L.append('uint8_t %s = %s;' % (ts, s))
        addr = []
        d = self.read(L, i.dst, 'b', addr_out=addr)
        r = self.t()
        fn = {'abcd': 'BCD_ADD', 'sbcd': 'BCD_SUB', 'nbcd': 'BCD_SUB'}[i.op]
        if i.op == 'nbcd':
            L.append('uint8_t %s = %s(%s, 0);' % (r, fn, d))
            # nbcd: 0 - d - X
            L[-1] = 'uint8_t %s = BCD_SUB(0, %s);' % (r, d)
        else:
            L.append('uint8_t %s = %s(%s, %s);' % (r, fn, d, ts))
        self.write(L, i.dst, 'b', r, addr[0] if addr else None)
        return L

    op_abcd = op_sbcd = op_nbcd = _bcd

    def op_movep(self, i):
        L = []
        if i.src.kind == 'dn':
            a = self.ea_addr(L, i.dst, None)
            r = i.src.reg
            if i.size == 'w':
                L.append('WB(%s, D(%d) >> 8); WB(%s + 2, D(%d));' % (a, r, a, r))
            else:
                L.append('WB(%s, D(%d) >> 24); WB(%s + 2, D(%d) >> 16); WB(%s + 4, D(%d) >> 8); WB(%s + 6, D(%d));' % (a, r, a, r, a, r, a, r))
        else:
            a = self.ea_addr(L, i.src, None)
            r = i.dst.reg
            if i.size == 'w':
                L.append('D(%d) = (D(%d) & 0xFFFF0000u) | (RB(%s) << 8) | RB(%s + 2);' % (r, r, a, a))
            else:
                L.append('D(%d) = (RB(%s) << 24) | (RB(%s + 2) << 16) | (RB(%s + 4) << 8) | RB(%s + 6);' % (r, a, a, a, a))
        return L


def write_tables(outdir, mods):
    """mods: list of (gen, entries, names, funcs) for all modules"""
    lo = min(g.an.code_lo for g, _, _, _ in mods)
    hi = max(g.an.code_hi for g, _, _, _ in mods)
    with open(os.path.join(outdir, 'recomp_funcs.h'), 'w') as o:
        o.write('/* generated */\n#pragma once\n#include <stdint.h>\n')
        for _, _, _, fl in mods:
            for e in fl:
                o.write('uint32_t f_%06x(uint32_t pc);\n' % e)
    with open(os.path.join(outdir, 'recomp_table.c'), 'w') as o:
        o.write('/* generated */\n#include "recomp.h"\n\n')
        o.write('const uint32_t recomp_code_lo = 0x%x, recomp_code_hi = 0x%x;\n' % (lo, hi))
        o.write('const RecompEntry recomp_entries[] = {\n')
        for _, entries, _, _ in mods:
            for a, f in entries:
                o.write('  {0x%x, f_%06x},\n' % (a, f))
        o.write('};\nconst int recomp_num_entries = sizeof(recomp_entries)/sizeof(recomp_entries[0]);\n')
        o.write('const RecompName recomp_names[] = {\n')
        for _, _, names, _ in mods:
            for a, n in sorted(names.items()):
                o.write('  {0x%x, "%s"},\n' % (a, n))
        o.write('};\nconst int recomp_num_names = sizeof(recomp_names)/sizeof(recomp_names[0]);\n')


BUMPER_BASE = 0x000E0000


def ensure_extracted():
    """extract the program modules from the disc image in rom/ if needed"""
    if os.path.exists('extracted/cdi_link') and os.path.exists('extracted/cdi_bumper'):
        return
    import glob
    import subprocess
    imgs = [p for p in glob.glob('rom/*.bin')]
    if not imgs:
        sys.exit('no disc image found in rom/ (expected the .bin of Link: The Faces of Evil)')
    subprocess.check_call([sys.executable, os.path.join(os.path.dirname(__file__), 'cdi_extract.py'),
                           imgs[0], 'extracted'])


HOOKS = {
    'hero_hit': 'hook_hero_hit',          # damage to Link (invincibility cheat)
    'kill_sprite': 'hook_kill_sprite',    # sprite deaths; sprite 0 is Link
}


def main():
    import glob
    ensure_extracted()
    for f in glob.glob('gen/recomp_*.c'):
        os.remove(f)
    mods = []
    total = 0
    # main program
    mod = load('extracted/cdi_link')
    an = Analysis(mod, extra_entries=[0x11f2c6])
    # game functions intercepted by the runtime (runtime/cheats.c), by MacsBug name
    by_name = {n: a for a, n in an.names.items()}
    hooks = {by_name[n]: h for n, h in HOOKS.items() if n in by_name}
    missing = [n for n in HOOKS if n not in by_name]
    if missing:
        sys.exit('hook targets not found: %s' % ', '.join(missing))
    g = Gen(an, hooks)
    files, entries, names, fl = g.gen_files('gen', 'link')
    mods.append((g, entries, names, fl))
    total += len(fl)
    # Philips CD-i "bumper" (logo animation) forked by the game
    from os9mod import Module
    bmod = Module(open('extracted/cdi_bumper', 'rb').read(), BUMPER_BASE)
    ban = Analysis(bmod, a5=False)
    bg = Gen(ban)
    bfiles, bentries, bnames, bfl = bg.gen_files('gen', 'bumper')
    mods.append((bg, bentries, bnames, bfl))
    total += len(bfl)
    write_tables('gen', mods)
    print('generated', total, 'functions in', len(files) + len(bfiles), 'files')


if __name__ == '__main__':
    main()
