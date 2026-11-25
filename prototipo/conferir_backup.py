"""Lê o ZIP do backup, confere o manifesto e localiza arquivos preenchidos com zeros."""

import argparse
from pathlib import Path
from zipfile import BadZipFile, ZipFile
from zlib import crc32

from preparar_memory_copy import latest_attempt

NON_TEXT = bytes(value for value in range(32) if value not in (10, 13)) + b'\x7f'


def checksum(archive, entry):
    crc, size, all_zero = 0, 0, True
    with archive.open(entry) as source:
        while block := source.read(256 * 1024):
            crc = crc32(block, crc)
            size += len(block)
            if all_zero and block.count(0) != len(block):
                all_zero = False
    return (crc, size), bool(size and all_zero)


def manifest_records(archive, entry):
    with archive.open(entry) as stream:
        lines = []
        for line in stream:
            if len(line.translate(None, NON_TEXT)) != len(line):
                raise ValueError(f'Manifesto contém bytes fora do formato de texto: {entry.filename}')
            lines.append(line.decode('utf-8', 'surrogateescape').rstrip('\r\n'))
    lines = latest_attempt(lines)
    records = {}
    for line in lines:
        if not line.startswith('OK '):
            continue
        _, crc, size, source = line.split(' ', 3)
        if not source.startswith('fs:/'):
            raise ValueError(f'Origem inválida no manifesto: {source}')
        path = source[4:]
        if path == 'mcp/lctsys/flixfile.dat':
            path = 'lctsys/flixfile.dat'
        key = path.casefold()
        if key in records:
            raise ValueError(f'Caminho duplicado no manifesto: {path}')
        records[key] = (int(crc, 16), int(size))
    return records, lines


def verify(archive, manifest=None):
    expected, lines = manifest_records(archive, manifest) if manifest else ({}, [])
    prefix = manifest.filename.rpartition('/')[0] if manifest else ''
    prefix = prefix + '/' if prefix else ''
    entries = {}
    failures = []
    for entry in archive.infolist():
        if entry.is_dir() or not entry.filename.startswith(prefix):
            continue
        relative = entry.filename[len(prefix):].casefold()
        if relative in ('manifesto.txt', 'status.txt', 'zb-card.txt'):
            continue
        if relative in entries:
            failures.append(f'Duplicado no ZIP: {entry.filename}')
        entries[relative] = entry
    print(f'Sessão: {prefix or "raiz do ZIP"}')
    if manifest:
        print(f'Resultado do manifesto: {lines[-1] if lines else "vazio"}')
        for line in lines:
            if line.startswith('ERRO '):
                print(line)
        for path in sorted(expected.keys() - entries.keys()):
            failures.append(f'Ausente no ZIP: {path}')
    else:
        print('Sem MANIFESTO.TXT para comparar com os CRCs registrados no Zeebo.')
    zeros = []
    for path, entry in entries.items():
        try:
            actual, all_zero = checksum(archive, entry)
        except (BadZipFile, OSError, RuntimeError) as error:
            failures.append(f'Falha ao ler {entry.filename}: {error}')
            continue
        if all_zero:
            zeros.append(f'Preenchido com zeros ({actual[1]} bytes): {entry.filename}')
        if manifest and path not in expected:
            failures.append(f'Sem registro OK: {entry.filename}')
        elif manifest and actual != expected[path]:
            failures.append(f'CRC32 ou tamanho divergente: {entry.filename}')
    for item in [*failures, *zeros]:
        print(item)
    print(f'{len(entries)} arquivos lidos, {len(failures)} divergências, {len(zeros)} só com zeros.')
    return bool(failures or zeros or (manifest and (not lines or lines[-1] != 'COMPLETO')))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zip', type=Path)
    args = parser.parse_args()
    try:
        with ZipFile(args.zip) as archive:
            manifests = [item for item in archive.infolist()
                         if not item.is_dir() and item.filename.rsplit('/', 1)[-1].casefold() == 'manifesto.txt']
            results = [verify(archive, item) for item in manifests or [None]]
    except (OSError, ValueError, BadZipFile) as error:
        parser.exit(1, f'{error}\n')
    return int(any(results))


if __name__ == '__main__':
    raise SystemExit(main())
