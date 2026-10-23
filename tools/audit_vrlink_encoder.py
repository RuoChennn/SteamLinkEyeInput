"""Read-only audit of the verified SteamVR 2.18.4 Steam Link encoder.

Requires pefile and capstone (pass their existing parent with --vendor).
Never loads the DLL, touches a running process, or changes SteamVR settings.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--vendor', type=Path)
parser.add_argument('--driver', type=Path, default=Path(r'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\drivers\vrlink\bin\win64\driver_vrlink.dll'))
parser.add_argument('--out', type=Path, default=Path('build/encoder-audit-verified'))
args = parser.parse_args()
if args.vendor:
    sys.path.insert(0, str(args.vendor.resolve()))
import pefile
import capstone

expected = '8e7100b6b1e88d3ad6dd05f30f030dc9a69b092b64a60d35347cebb5c516b55d'
digest = hashlib.sha256(args.driver.read_bytes()).hexdigest()
if digest != expected:
    raise SystemExit('Unsupported DLL hash; offsets are version specific.')
pe = pefile.PE(str(args.driver))
base = pe.OPTIONAL_HEADER.ImageBase
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True
# These starts are instruction boundaries, checked against .pdata and known entry points.
ranges = {
    'native-receive': (0x9e9880, 0x9ead81),
    'present-entry': (0x9ed800, 0x9ed920),
    'present-mode': (0x9edbea, 0x9ee2db),
    'present-gaze-crop': (0x9ee2db, 0x9eed45),
    'settings': (0x9ebbc0, 0x9ec753),
    'clock': (0xa29980, 0xa299bd),
    'constructor': (0x9e31e0, 0x9e34a4),
    'packet-dispatch': (0xa03230, 0xa03376),
    'model-parser': (0x9e8d20, 0x9e8e9b),
    'unknown-eye-capability': (0x9e6ae0, 0x9e6c40),
    'known-eye-capability': (0x9e85d9, 0x9e86a5),
    'spoof-display-size': (0x9f21a0, 0x9f22d5),
}
args.out.mkdir(parents=True, exist_ok=True)
instructions = {}
for name, (start, end) in ranges.items():
    lines = []
    for ins in md.disasm(pe.get_data(start, end-start), base+start):
        rva = ins.address-base
        instructions[rva] = ins
        notes = []
        for op in ins.operands:
            if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
                target = ins.address+ins.size+op.mem.disp-base
                try:
                    raw = pe.get_data(target, 200).split(b'\0')[0]
                    if len(raw)>3 and all(32 <= c < 127 for c in raw):
                        notes.append(repr(raw.decode('ascii')))
                    if ins.mnemonic in ('comiss','movss','ucomiss'):
                        notes.append('float='+str(struct.unpack('<f',pe.get_data(target,4))[0]))
                except (ValueError, struct.error, pefile.PEFormatError):
                    pass
        lines.append(f'{rva:08x}: {ins.mnemonic:9} {ins.op_str} '+ ' '.join(notes))
    (args.out/(name+'.asm')).write_text('\n'.join(lines)+'\n', encoding='utf-8')
checks = {
    0x9e98cc: ('cmp', '[rsi + 0x15]'),
    0x9e9914: ('mov', '[r14 + 0x188]'),
    0x9e9920: ('movss', '[r14 + 0x178]'),
    0x9e997b: ('cmp', '[r14 + 0x25a]'),
    0x9e9a75: ('call', '[r9 + 0x50]'),
    0x9ed821: ('mov', 'r15, rcx'),
    0x9ed878: ('mov', '[r15 - 0x10]'),
    0x9ee268: ('cmp', '[r15 + 0x300], 2'),
    0x9ee31b: ('sub', '[r15 + 0x178]'),
    0x9ee322: ('cmp', '[r15 + 0xf1]'),
    0x9ee32c: ('cmp', '0x3d090'),
    0x9ee33c: ('comiss', '[r15 + 0x170]'),
    0x9ee52b: ('movss', '[r15 + 0x180]'),
    0x9eea0b: ('cmp', '[r15 + 0xf3]'),
    0x9ec72d: ('mov', '[r14 + 0x25a]'),
    0xa299a6: ('imul', '0xf4240'),
    0xa0336c: ('call', '0x1809e9880'),
    0x9e8e10: ('mov', 'eax, 3'),
    0x9e8e8b: ('mov', 'eax, 4'),
    0x9e6afb: ('test', 'al, al'),
    0x9e6b6a: ('mov', '0x1779'),
    0x9e6bd5: ('call', '[r10 + 0x48]'),
    0x9e85e7: ('mov', 'eax, 0x1368'),
    0x9e8604: ('mov', '[rdi + 0x20], 1'),
    0x9e86a1: ('call', '[r10 + 0x48]'),
    0x9f222a: ('jne', '0x1809f22a9'),
}
for rva, (mnemonic, operand) in checks.items():
    ins = instructions.get(rva)
    if ins is None or ins.mnemonic != mnemonic or operand not in ins.op_str:
        raise SystemExit(f'Audit check failed at RVA {rva:x}')
manifest = {
    'driver': str(args.driver), 'sha256': digest,
    'structural_checks_passed':len(checks),
    'scope':'Static on-disk disassembly; no live session gate values verified.',
    'hmd_fields':{'gaze':'0x178..0x180','timestamp_us':'0x188','uv':'0x190..0x19c','eye_enabled':'0x101','filter':'0x102','marker':'0x103','spatial_mode':'0x310','share_eye':'0x25a','crop':'0x3f8..0x40b'},
    'present_this':'HMD + 0x10', 'timeout_us':250000,
    'model_enums':{'Quest Pro':3,'Quest 3':4},
    'known_eye_model_mask':'0x1368 (model 3 selected, model 4 not selected)',
    'hasEyeTracking_literal_present': b'hasEyeTracking\0' in args.driver.read_bytes(),
}
(args.out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n',encoding='utf-8')
print(f'PASS: {len(checks)} structural checks; hash verified; evidence: {args.out}')
