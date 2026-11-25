"""Empacota apenas a imagem ARM independente de posição deste applet."""

import argparse
from pathlib import Path
import re
from struct import unpack_from, iter_unpack, pack
from zipfile import ZIP_DEFLATED, ZipFile

MIF_PRIVILEGE_FILE = 0x0001
MIF_PRIVILEGE_SYSTEM = 0xffff
CARD0_ACCESS_CLASS = 0x0101414e


def module_payload(elf):
    if elf[:6] != b'\x7fELF\x01\x01':
        raise ValueError('esperado ELF32 little-endian')
    kind, machine, _, entry, _, sections_at = unpack_from('<HH4I', elf, 16)
    if (kind, machine, entry) != (2, 40, 0):
        raise ValueError('esperado ELF executável ARM com entrada em zero')
    section_size, section_count, names_index = unpack_from('<3H', elf, 46)
    if section_size != 40:
        raise ValueError('tabela de seções ELF inesperada')
    sections = [unpack_from('<10I', elf, sections_at + i * section_size)
                for i in range(section_count)]
    _, _, _, _, start, length, *_ = sections[names_index]
    names = elf[start:start + length]

    image = None
    for i, section in enumerate(sections):
        name_at, kind, flags, address, start, length, _, target, _, _ = section
        name = names[name_at:].split(b'\0', 1)[0]
        if length and flags & 2:
            if name != b'.image' or kind != 1 or address != 0 or flags & 5 != 4:
                raise ValueError(f'seção carregável inesperada: {name!r}')
            image = elf[start:start + length]
        if kind == 9 and sections[target][2] & 2:
            for _, info in iter_unpack('<II', elf[start:start + length]):
                # REL32, CALL, JUMP24 e ALU_PC_G0 são relativos ao PC.
                if info & 255 not in (3, 28, 29, 58):
                    raise ValueError(f'relocação incompatível {info & 255} em seção {i}')
        if kind == 4 and sections[target][2] & 2:
            raise ValueError('seção RELA inesperada')
    if not image or len(image) < 12:
        raise ValueError('imagem ARM ausente ou truncada')
    return image + bytes(-len(image) % 4)


def backup_icon(size):
    """Ícone original da tela do app, em BMP de 24 bits encapsulado para BREW."""
    rows = bytearray()
    for y in reversed(range(size)):
        for x in range(size):
            u, v = x * 16 // size, y * 16 // size
            color = (11, 21, 32) if u in (0, 15) or v in (0, 15) else (111, 232, 181)
            if (4 <= u < 12 and 4 <= v < 10) or (5 <= u < 11 and v == 11):
                color = (23, 58, 57)
            rows.extend(reversed(color))
        rows.extend(bytes((-size * 3) % 4))
    bitmap = (pack('<2sIHHI', b'BM', 54 + len(rows), 0, 0, 54)
              + pack('<IiiHH6I', 40, size, size, 1, 24, 0, len(rows), 0, 0, 0, 0)
              + rows)
    return b'\x0c\x00image/bmp\0' + bitmap


def mif_payload(class_id, title, with_icons=False, privileges=MIF_PRIVILEGE_FILE, dependencies=()):
    title_payload = b'\xff\xfe' + title.encode('utf-16le')
    # Estrutura de módulo e applet do conftest. Os IDs 21/23 vêm do MIF do ZeeUtils.
    module = pack('<6I4H2I', 0x1002, 0, 0, 0, 0, 0, privileges, 0, 1, len(dependencies), 0, 0)
    applet = pack('<5I', class_id, 0, 20, 0, 0)
    records = []
    if with_icons:
        records.extend([(6, 21, backup_icon(64)), (6, 23, backup_icon(16))])
    records.extend([(1, 20, title_payload), (0x5000, 0, module), (0x5000, 2, applet)])
    if dependencies:
        records.append((0x5000, 3, pack(f'<{len(dependencies) + 1}I', *dependencies, 0)))
    index = b''.join(pack('<HHI', kind, resource, i << 16)
                     for i, (kind, resource, _) in enumerate(records))
    table_at = 32 + len(index)
    offsets = [table_at + 4 * (len(records) + 1)]
    for _, _, payload in records:
        offsets.append(offsets[-1] + len(payload))
    header = pack('<4H6I', 0x11, 1, 1, len(records), 32, len(index), table_at,
                  len(records), offsets[0], offsets[-1] - offsets[0])
    return header + index + pack(f'<{len(offsets)}I', *offsets) + b''.join(row[2] for row in records)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('elf', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('class_id', type=lambda value: int(value, 0))
    parser.add_argument('--name', default='inventario')
    parser.add_argument('--title', default='Inventario Zeebo')
    parser.add_argument('--zip', type=Path)
    parser.add_argument('--homebrew-package', action='store_true',
                        help='inclui ícones e o arquivo .sig vazio para Zeebo preparado para homebrew')
    parser.add_argument('--sd-marker', action='store_true',
                        help='inclui a pasta ZB e um arquivo de diagnóstico para o SD')
    parser.add_argument('--system-privilege', action='store_true',
                        help='solicita o privilégio System no MIF de teste')
    parser.add_argument('--sd-access', action='store_true',
                        help='declara a dependência CARD0_ACCESS no MIF')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-z0-9]{1,20}', args.name):
        parser.error('use um nome de módulo alfanumérico minúsculo')
    image = module_payload(args.elf.read_bytes())
    privileges = MIF_PRIVILEGE_SYSTEM if args.system_privilege else MIF_PRIVILEGE_FILE
    dependencies = (CARD0_ACCESS_CLASS,) if args.sd_access else ()
    mif = mif_payload(args.class_id, args.title, with_icons=args.homebrew_package,
                      privileges=privileges, dependencies=dependencies)
    module_path = args.output / 'mod' / args.name / (args.name + '.mod')
    mif_path = args.output / 'mif' / (args.name + '.mif')
    files = [(mif_path, mif), (module_path, image)]
    if args.homebrew_package:
        files.append((module_path.with_suffix('.sig'), b''))
    for path, data in files:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
        print(path)
    if args.zip:
        with ZipFile(args.zip, 'w', ZIP_DEFLATED) as archive:
            for folder in (mif_path.parent, module_path.parent.parent, module_path.parent):
                archive.write(folder, folder.relative_to(args.output).as_posix())
            for path, _ in files:
                archive.write(path, path.relative_to(args.output).as_posix())
            if args.sd_marker:
                archive.writestr('ZB/', b'')
                archive.writestr('ZB/ZB-CARD.TXT', b'ZB04\r\n')
        print(args.zip)


if __name__ == '__main__':
    main()
