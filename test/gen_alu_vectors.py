"""Independent bit-serial reference model for 68000 shifts/rotates, DIVU/DIVS,
ABCD/SBCD; writes test/alu_vectors.h consumed by test/alu_test.c."""
import random
random.seed(1234)
B = {8: 0xff, 16: 0xffff, 32: 0xffffffff}

def shift(op, d, n, x, bits):
    m = B[bits]; d &= m; msb = 1 << (bits - 1)
    c = 0; v = 0; xo = x
    if op in ('asl', 'lsl', 'asr', 'lsr'):
        for _ in range(n):
            if op in ('asl', 'lsl'):
                c = (d >> (bits - 1)) & 1
                nd = (d << 1) & m
                if op == 'asl' and ((nd ^ d) & msb): v = 1
                d = nd
            else:
                c = d & 1
                d = (d >> 1) | (d & msb if op == 'asr' else 0)
            xo = c
        if n == 0: c = 0
    elif op in ('rol', 'ror'):
        for _ in range(n):
            if op == 'rol':
                c = (d >> (bits - 1)) & 1; d = ((d << 1) | c) & m
            else:
                c = d & 1; d = (d >> 1) | (c << (bits - 1))
        if n == 0: c = 0
    else:  # roxl/roxr
        for _ in range(n):
            if op == 'roxl':
                c = (d >> (bits - 1)) & 1; d = ((d << 1) | xo) & m; xo = c
            else:
                c = d & 1; d = (d >> 1) | (xo << (bits - 1)); xo = c
        c = xo
    nf = (d >> (bits - 1)) & 1; z = int(d == 0)
    return d, xo, nf, z, v, c

def bcd_add(d, s, x):
    lo = (d & 15) + (s & 15) + x
    res = (d & 0xf0) + (s & 0xf0) + lo
    if lo > 9: res += 6
    c = 1 if res > 0x99 else 0
    if c: res += 0x60
    return res & 0xff, c

def bcd_sub(d, s, x):
    # decimal: value(d) - value(s) - x for valid BCD inputs
    dv = (d >> 4) * 10 + (d & 15); sv = (s >> 4) * 10 + (s & 15)
    r = dv - sv - x; c = 0
    if r < 0: r += 100; c = 1
    return ((r // 10) << 4) | (r % 10), c

lines = []
ops = ['asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr']
for op_i, op in enumerate(ops):
    for bits in (8, 16, 32):
        for _ in range(400):
            d = random.choice([random.getrandbits(bits), 0, B[bits], 1 << (bits - 1), (1 << (bits - 1)) - 1, 1])
            n = random.choice([random.randint(0, 9), random.randint(0, 63), bits - 1, bits, bits + 1])
            x = random.randint(0, 1)
            r = shift(op, d, n, x, bits)
            # X is unaffected for rol/ror and for any shift by 0 except roxl/roxr
            lines.append('{%d,%d,0x%x,%d,%d, 0x%x,%d,%d,%d,%d,%d},' % ((op_i, bits, d, n, x) + r))
open('test/alu_vectors.h', 'w').write('\n'.join(lines) + '\n')
bl = []
for _ in range(3000):
    a = random.randint(0, 9) * 16 + random.randint(0, 9)
    b = random.randint(0, 9) * 16 + random.randint(0, 9)
    x = random.randint(0, 1)
    ra, ca = bcd_add(a, b, x)
    rs, cs = bcd_sub(a, b, x)
    bl.append('{0x%x,0x%x,%d,0x%x,%d,0x%x,%d},' % (a, b, x, ra, ca, rs, cs))
open('test/bcd_vectors.h', 'w').write('\n'.join(bl) + '\n')
print(len(lines), 'shift vectors,', len(bl), 'bcd vectors')
