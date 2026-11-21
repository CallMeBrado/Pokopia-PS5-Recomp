"""Append newly translated shaders to the Pokopia shader pack.

Usage:
  python -I boot/extend-shader-cache.py prepare <translated dir> [--base boot/shader-cache-655360] [--out work/shader-pack-extension]
  python -I boot/extend-shader-cache.py upload [--base ...] [--out ...]

prepare: reads <translated dir>/results.tsv and <hash>.spv from spirv_harness --batch, skips keys the base pack
already has (a duplicate key makes the loader reject the whole cache) and anything not "ok", and writes
<out>/append.spvpack (the new SPIR-V, in order) and <out>/results.tsv (base index plus new rows whose
spv_offset points past the current end of the base pack).

upload: checks that /data/pokopia/shaders/shaders.spvpack on the PS4 is exactly the base pack size, appends
append.spvpack to it (FTP APPE), verifies the new size, then replaces results.tsv by way of a temporary name.
Finally it appends the same bytes to the local base pack and installs the new index locally, so the two stay
identical. Never run upload while the game is running.
"""
import argparse
import ftplib
import io
import os
import pathlib
import re
import sys

HOST, PORT = '172.30.0.99', 2121
REMOTE = '/data/pokopia/shaders'
SPIRV_MAGIC = b'\x03\x02\x23\x07'


def read_index(path):
    lines = path.read_text(encoding='utf-8').splitlines()
    return lines[0], lines[1:]


def prepare(args):
    base, out, src = pathlib.Path(args.base), pathlib.Path(args.out), pathlib.Path(args.translated)
    header, rows = read_index(base / 'results.tsv')
    columns = header.split('\t')
    if columns[-1] != 'spv_offset':
        sys.exit('base results.tsv has no spv_offset column')
    known = {row.split('\t', 1)[0] for row in rows}
    pack_size = (base / 'shaders.spvpack').stat().st_size

    new_header, new_rows = read_index(src / 'results.tsv')
    if new_header.split('\t') != columns[:-1]:
        sys.exit('translated results.tsv schema differs from the base pack')
    status_col, size_col = columns.index('status'), columns.index('spv_bytes')

    out.mkdir(parents=True, exist_ok=True)
    added, skipped_known, skipped_bad = [], 0, 0
    offset = pack_size
    with open(out / 'append.spvpack', 'wb') as pack:
        for row in new_rows:
            fields = row.split('\t')
            key = fields[0]
            if not re.fullmatch(r'[0-9a-f]{16}', key):
                sys.exit(f'invalid key {key!r}')
            if key in known:
                skipped_known += 1
                continue
            data = (src / f'{key}.spv').read_bytes() if fields[status_col].startswith('ok') else b''
            if (not data or data[:4] != SPIRV_MAGIC or len(data) % 4 or len(data) != int(fields[size_col])):
                skipped_bad += 1
                continue
            pack.write(data)
            added.append(row + f'\t{offset}')
            known.add(key)
            offset += len(data)
    with open(out / 'results.tsv', 'w', encoding='utf-8', newline='\n') as index:
        index.write(header + '\n')
        index.writelines(r + '\n' for r in rows)
        index.writelines(r + '\n' for r in added)
    (out / 'base_size.txt').write_text(str(pack_size), encoding='ascii')
    print(f'prepared {len(added)} new shaders ({offset - pack_size:,} bytes); '
          f'skipped {skipped_known} already in the pack, {skipped_bad} not ok; '
          f'index {len(rows) + len(added):,} rows; pack {pack_size:,} -> {offset:,} bytes')


def upload(args):
    base, out = pathlib.Path(args.base), pathlib.Path(args.out)
    expected = int((out / 'base_size.txt').read_text(encoding='ascii'))
    append = (out / 'append.spvpack').read_bytes()
    index = (out / 'results.tsv').read_bytes()
    if (base / 'shaders.spvpack').stat().st_size != expected:
        sys.exit('local base pack changed since prepare; run prepare again')
    ftp = ftplib.FTP()
    ftp.connect(HOST, PORT, timeout=60)
    ftp.login()
    ftp.voidcmd('TYPE I')
    remote_size = ftp.size(f'{REMOTE}/shaders.spvpack')
    if remote_size != expected:
        sys.exit(f'remote pack is {remote_size} bytes, expected {expected}; refusing to append')
    if append:
        ftp.storbinary(f'APPE {REMOTE}/shaders.spvpack', io.BytesIO(append))
    after = ftp.size(f'{REMOTE}/shaders.spvpack')
    if after != expected + len(append):
        sys.exit(f'remote pack is {after} bytes after append, expected {expected + len(append)}')
    ftp.storbinary(f'STOR {REMOTE}/results.tsv.new', io.BytesIO(index))
    if ftp.size(f'{REMOTE}/results.tsv.new') != len(index):
        sys.exit('uploaded index size mismatch')
    try:
        ftp.delete(f'{REMOTE}/results.tsv')
    except ftplib.error_perm:
        pass
    ftp.rename(f'{REMOTE}/results.tsv.new', f'{REMOTE}/results.tsv')
    if ftp.size(f'{REMOTE}/results.tsv') != len(index):
        sys.exit('remote index size mismatch after rename')
    ftp.quit()
    with open(base / 'shaders.spvpack', 'ab') as pack:
        pack.write(append)
    (base / 'results.tsv').write_bytes(index)
    print(f'uploaded: pack {expected:,} -> {expected + len(append):,} bytes, index {len(index):,} bytes; local copy updated')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('command', choices=('prepare', 'upload'))
    parser.add_argument('translated', nargs='?')
    parser.add_argument('--base', default='boot/shader-cache-655360')
    parser.add_argument('--out', default='work/shader-pack-extension')
    args = parser.parse_args()
    os.chdir(pathlib.Path(__file__).resolve().parents[1])
    if args.command == 'prepare':
        if not args.translated:
            parser.error('prepare needs the translated directory')
        prepare(args)
    else:
        upload(args)


if __name__ == '__main__':
    main()
