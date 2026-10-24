#!/usr/bin/env python3
"""Adds the imports of a prepared game to src/import_names.inc (NID#library -> symbol name).

The runtime binds imports by symbol name (runtime_lookup), so every NID the game, its libc and
its Fios2 import must be named here. Names come from shadPS4's aerolib table
(src/core/aerolib/aerolib.inl, GPL-2.0-or-later), passed with --aerolib. Existing entries are
kept; new ones are appended in library order.

    python tools/gen_import_names.py out/link.json --aerolib path/to/aerolib.inl
"""
import argparse
import json
import re
from pathlib import Path

INC = Path(__file__).resolve().parent.parent / 'src' / 'import_names.inc'


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('link_json', type=Path, help='out/link.json written by scripts/link_modules.py')
    p.add_argument('--aerolib', type=Path, required=True)
    a = p.parse_args()
    aero = {m.group(1): m.group(2) for m in
            re.finditer(r'STUB\(\s*"([^"]+)"\s*,\s*([^)\s]+)\s*\)', a.aerolib.read_text(encoding='utf-8', errors='replace'))}
    text = INC.read_text(encoding='utf-8')
    known = set(re.findall(r'\{"([^"]+)","[^"]+"\}', text))
    imports = json.loads(a.link_json.read_text())['unresolved_imports']
    added, unnamed = [], []
    for name in sorted(set(imports), key=lambda n: (n.split('#')[1], n)):
        if name in known:
            continue
        symbol = aero.get(name.split('#')[0])
        if symbol:
            added.append(f'{{"{name}","{symbol}"}},')
        else:
            unnamed.append(name)
    if added:
        INC.write_text(text.rstrip('\n') + '\n' + '\n'.join(added) + '\n', encoding='utf-8', newline='\n')
    print(f'{len(added)} names added, {len(unnamed)} without a known name: {" ".join(unnamed)}')


if __name__ == '__main__':
    main()
