"""Find the Haswell-only instructions (MOVBE, LZCNT, BMI1, F16C) and the reciprocal estimates
(vrsqrtps, vrcpps) in the linked image.

scan_segment decodes one executable segment with iced-x86 and classifies each
site: JMP (5+ bytes, patched with a jump to a stub), JMP_RELOCATE (4 bytes,
the jump also covers the plain next instruction, copied into the stub), INT3
(4 bytes, trapped at run time) or UNSUPPORTED (an encoding the stubs do not
emulate: segment override, LOCK, 32-bit addressing or F16C).

Reciprocal estimates give different bits on different CPU makers and under Rosetta 2, so an online
match, where both copies compute the whole fight, drifts apart; their stubs compute the exact value
(src/isa_rewrite.c). They are only taken from code that looks like code (the game's code segment
also holds data), they may move a RIP-relative next instruction into the stub (its disp32 is fixed
there), and one that has no room for the jump is UNSUPPORTED: it keeps its estimate (never an int3).

Usage: isa_rewrite.py out/boot-linked.bin --out out
Reads the G3PROBE5 image, scans its executable segments and writes
<out>/isa-rewrite.bin (format G3ISA1: HEADER, then one RECORD per site).
"""
import argparse
import collections
from dataclasses import dataclass
import hashlib
from pathlib import Path
import struct
import sys
from prepare import span, unpack

# build.sh installs iced-x86 here for a launcher started from the repository (macOS, Linux); one the
# Python already has comes first.
PYDEPS = Path(__file__).resolve().parent.parent / 'out' / 'pydeps'
if PYDEPS.is_dir() and str(PYDEPS) not in sys.path:
    sys.path.append(str(PYDEPS))

try:
    import iced_x86
    from iced_x86 import Code, Decoder, FlowControl, Mnemonic, OpKind, Register, RegisterExt
except ImportError:
    iced_x86 = None

(KIND_MOVBE_LOAD, KIND_MOVBE_STORE, KIND_LZCNT, KIND_TZCNT, KIND_ANDN, KIND_BEXTR,
 KIND_BLSI, KIND_BLSMSK, KIND_BLSR, KIND_VCVTPH2PS, KIND_VCVTPS2PH, KIND_VRSQRTPS, KIND_VRCPPS) = range(1, 14)
MODE_JMP, MODE_JMP_RELOCATE, MODE_INT3, MODE_UNSUPPORTED = range(4)
NO_REG = 0xFF
HEADER = struct.Struct('<8sQ32sII8x')                 # magic, image size, sha256, sites, unsupported
RECORD = struct.Struct('<QBBBBBBBBBBBBB3xq15sB15sx')  # mirrors IsaSite in the C loader
SET_OF_KIND = {KIND_MOVBE_LOAD: 'MOVBE', KIND_MOVBE_STORE: 'MOVBE', KIND_LZCNT: 'LZCNT',
               KIND_TZCNT: 'BMI1', KIND_ANDN: 'BMI1', KIND_BEXTR: 'BMI1', KIND_BLSI: 'BMI1',
               KIND_BLSMSK: 'BMI1', KIND_BLSR: 'BMI1', KIND_VCVTPH2PS: 'F16C', KIND_VCVTPS2PH: 'F16C',
               KIND_VRSQRTPS: 'RECIP', KIND_VRCPPS: 'RECIP'}
RECIP = (KIND_VRSQRTPS, KIND_VRCPPS)
# A reciprocal site counts as code when the CODE_BEFORE instructions before it and the CODE_AFTER after
# it decode as ordinary instructions and one of the USED_WITHIN after it names its destination (in
# GODZILLA's code the farthest first use is 14 instructions on).
CODE_BEFORE, CODE_AFTER, USED_WITHIN = 4, 24, 24


@dataclass
class Site:
    offset: int
    length: int
    kind: int
    width: int
    mode: int
    dst: int
    src1: int
    src2: int
    mem_slot: int   # 0 none, 1 dst, 2 src
    base: int
    index: int
    scale: int
    mem_flags: int  # bit 0: RIP-relative, disp is the absolute target
    disp: int
    original: bytes
    relocated: bytes
    relocated_disp_at: int = 0  # offset of a RIP-relative disp32 in relocated, 0 = none


def kind_of(ins):
    m = ins.mnemonic
    if m == Mnemonic.MOVBE:
        return KIND_MOVBE_LOAD if ins.op0_kind == OpKind.REGISTER else KIND_MOVBE_STORE
    if m in (Mnemonic.VRSQRTPS, Mnemonic.VRCPPS):   # register forms only (the game has no other)
        if ins.op0_kind == OpKind.REGISTER and ins.op1_kind == OpKind.REGISTER:
            return KIND_VRSQRTPS if m == Mnemonic.VRSQRTPS else KIND_VRCPPS
        return None
    return {Mnemonic.LZCNT: KIND_LZCNT, Mnemonic.TZCNT: KIND_TZCNT, Mnemonic.ANDN: KIND_ANDN,
            Mnemonic.BEXTR: KIND_BEXTR, Mnemonic.BLSI: KIND_BLSI, Mnemonic.BLSMSK: KIND_BLSMSK,
            Mnemonic.BLSR: KIND_BLSR, Mnemonic.VCVTPH2PS: KIND_VCVTPH2PS,
            Mnemonic.VCVTPS2PH: KIND_VCVTPS2PH}.get(m)


def register_number(reg):
    return NO_REG if reg == Register.NONE else RegisterExt.number(reg)


def site_of(ins, kind, original):
    """Operands in order fill dst, src1, src2; the memory operand leaves its slot NO_REG. The width is the
    operand size in bits, except for a reciprocal: the vector's size in bytes (16 or 32, a byte holds it)."""
    regs, mem_slot, width = [NO_REG] * 3, 0, 0
    for i in range(ins.op_count):
        if ins.op_kind(i) == OpKind.REGISTER:
            regs[i] = register_number(ins.op_register(i))
            width = width or RegisterExt.size(ins.op_register(i)) * (1 if kind in RECIP else 8)
        elif ins.op_kind(i) == OpKind.MEMORY:
            mem_slot = 1 if i == 0 else 2
    if SET_OF_KIND[kind] == 'F16C':
        width = 0
    base = index = NO_REG
    scale = mem_flags = disp = 0
    unsupported = SET_OF_KIND[kind] == 'F16C' or ins.segment_prefix != Register.NONE or ins.has_lock_prefix
    if mem_slot:
        unsupported |= any(r != Register.NONE and RegisterExt.size(r) != 8 for r in (ins.memory_base, ins.memory_index))
        scale = ins.memory_index_scale
        if ins.is_ip_rel_memory_operand:
            mem_flags, disp = 1, ins.ip_rel_memory_address
        else:
            base, index = register_number(ins.memory_base), register_number(ins.memory_index)
            disp = ins.memory_displacement  # iced reports it as u64; the table stores int64
            disp -= (1 << 64) if disp >= 1 << 63 else 0
    mode = MODE_UNSUPPORTED if unsupported else MODE_JMP if ins.len >= 5 else MODE_INT3
    return Site(ins.ip, ins.len, kind, width, mode, *regs, mem_slot, base, index, scale, mem_flags, disp, original, b'')


def is_plain(ins, end, rip_relative=False):
    """Candidate to be moved into a 4-byte site's stub (branch targets are checked later). A reciprocal
    site's stub also takes a RIP-relative one (rip_relative): the loader aims its disp32 from the stub."""
    return (ins.code != Code.INVALID and ins.flow_control == FlowControl.NEXT
            and (rip_relative or not ins.is_ip_rel_memory_operand)
            and not ins.has_rep_prefix and not ins.has_repne_prefix and kind_of(ins) is None and ins.next_ip <= end)


# Instructions compiled game code does not contain: data decoded as code soon runs into them.
UNLIKELY = {Mnemonic.INT, Mnemonic.INT1, Mnemonic.INTO, Mnemonic.HLT, Mnemonic.IN, Mnemonic.OUT, Mnemonic.INSB,
            Mnemonic.INSW, Mnemonic.INSD, Mnemonic.OUTSB, Mnemonic.OUTSW, Mnemonic.OUTSD, Mnemonic.RETF,
            Mnemonic.IRET, Mnemonic.IRETD, Mnemonic.IRETQ, Mnemonic.CLI, Mnemonic.STI}


def looks_like_code(ins):
    return ins.code != Code.INVALID and ins.mnemonic not in UNLIKELY


def names_register(ins, number):
    """ins has a vector register operand number `number` (xmm or ymm)."""
    return any(ins.op_kind(i) == OpKind.REGISTER and RegisterExt.is_vector_register(ins.op_register(i))
               and RegisterExt.number(ins.op_register(i)) == number for i in range(ins.op_count))


def scan_segment(code, address):
    """Sites of one executable segment loaded at address, in address order."""
    if iced_x86 is None:
        raise RuntimeError('iced_x86 is not installed')
    end = address + len(code)
    sites, targets, candidates = [], set(), []
    pending = None  # 4-byte site waiting for the instruction after it
    before = collections.deque(maxlen=CODE_BEFORE)  # looks_like_code of the last instructions
    checks, data = [], set()                         # reciprocal sites still being checked; offsets found in data
    decoder = Decoder(64, code, ip=address)
    for ins in decoder:
        if ins.flow_control in (FlowControl.UNCONDITIONAL_BRANCH, FlowControl.CONDITIONAL_BRANCH, FlowControl.CALL) \
                and ins.op0_kind == OpKind.NEAR_BRANCH64:
            targets.add(ins.near_branch_target)
        code_like = looks_like_code(ins)
        for check in checks:   # [site, instructions seen after it, destination named]
            check[1] += 1
            check[2] = check[2] or (check[1] <= USED_WITHIN and names_register(ins, check[0].dst))
            if not code_like or (check[1] == USED_WITHIN and not check[2]):
                data.add(check[0].offset)
        checks = [c for c in checks if c[1] < CODE_AFTER and c[0].offset not in data]
        if pending is not None and is_plain(ins, end, pending.kind in RECIP):
            disp_at = decoder.get_constant_offsets(ins).displacement_offset if ins.is_ip_rel_memory_operand else 0
            candidates.append((pending, ins.ip, code[ins.ip - address:ins.next_ip - address], disp_at))
        pending = None
        kind = kind_of(ins)
        if kind is not None and (kind not in RECIP or all(before)):
            site = site_of(ins, kind, code[ins.ip - address:ins.next_ip - address])
            sites.append(site)
            if kind in RECIP:
                checks.append([site, 0, False])
            if site.mode == MODE_INT3 and ins.len == 4:
                pending = site
        before.append(code_like)
    data.update(c[0].offset for c in checks)   # the segment ended before CODE_AFTER instructions
    sites = [s for s in sites if s.offset not in data]
    for site, ip, relocated, disp_at in candidates:
        if ip not in targets:
            site.mode, site.relocated, site.relocated_disp_at = MODE_JMP_RELOCATE, relocated, disp_at
    for site in sites:
        if site.kind in RECIP and site.mode == MODE_INT3:
            site.mode = MODE_UNSUPPORTED
    return sites


def read_boot_linked(data):
    """Segments (address, size, flags) and image of a G3PROBE5 file, in link_modules.py's order."""
    magic, size, entry, ns, nr, ni, flags = unpack('<8s6Q', data, 0)
    if magic != b'G3PROBE5':
        raise ValueError('not a G3PROBE5 image')
    pos = 56 + 8 + 32   # header, procparam, main TLS
    nmodules, = unpack('<Q', data, pos)
    pos += 8 + nmodules * 56
    nbindings, = unpack('<Q', data, pos)
    pos += 8 + nbindings * 24
    segments = [unpack('<3Q', data, pos + i * 24) for i in range(ns)]
    pos += ns * 24 + ni * 128 + nr * 32
    return segments, span(data, pos, size)


def encode_table(image, sites):
    unsupported = sum(s.mode == MODE_UNSUPPORTED for s in sites)
    table = [HEADER.pack(b'G3ISA1\0\0', len(image), hashlib.sha256(image).digest(), len(sites), unsupported)]
    for s in sorted(sites, key=lambda s: s.offset):   # the C loader rejects records out of order
        table.append(RECORD.pack(s.offset, s.length, s.kind, s.width, s.mode, s.dst, s.src1, s.src2, s.mem_slot,
                                 s.base, s.index, s.scale, s.mem_flags, s.relocated_disp_at, s.disp, s.original,
                                 len(s.relocated), s.relocated))
    return b''.join(table)


def summary(sites):
    sets = [SET_OF_KIND[s.kind] for s in sites]
    modes = [s.mode for s in sites]
    per_set = ', '.join(f'{name} {sets.count(name)}' for name in ('MOVBE', 'BMI1', 'LZCNT', 'F16C', 'RECIP'))
    return (f'ISA: {len(sites)} sites ({per_set}); {modes.count(MODE_JMP_RELOCATE)} relocated, '
            f'{modes.count(MODE_INT3)} int3, {modes.count(MODE_UNSUPPORTED)} unsupported')


def fixture_code():
    """Code behind tests/data/isa_fixture.bin, loaded by the C loader test."""
    return b''.join(bytes.fromhex(h) for h in (
        'c4e270f3c9',              # blsr ecx, ecx                 JMP
        '0f38f108', '90',          # movbe [rax], ecx; nop         JMP_RELOCATE
        'c4e270f7050c000000',      # bextr eax, [rip+0xc], ecx     JMP
        '66f30fbdc1',              # lzcnt ax, cx                  JMP
        'f3480fbcc1',              # tzcnt rax, rcx                JMP
        '0f38f108', 'ebfe',        # movbe [rax], ecx; jmp $       INT3
        'c3'))                     # ret


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('boot', type=Path)
    p.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'out')
    a = p.parse_args(argv)
    try:
        if iced_x86 is None:
            raise RuntimeError('iced_x86 is not installed')
        segments, image = read_boot_linked(a.boot.read_bytes())
        sites = [site for address, size, flags in segments if flags & 1
                 for site in scan_segment(span(image, address, size), address)]
        (a.out / 'isa-rewrite.bin').write_bytes(encode_table(image, sites))
    except (OSError, ValueError, RuntimeError) as e:
        print(f'ISA: rewrite table unavailable ({e})')
        return 1
    print(summary(sites))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
