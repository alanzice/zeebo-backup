"""Exporta os jogos e saves de uma sessão concluída como um ZIP para Memory Copy."""

import argparse
from pathlib import Path
import stat
from zipfile import ZIP_DEFLATED, ZipFile


def check_regular(path):
    metadata = path.lstat()
    if path.is_symlink() or getattr(metadata, 'st_file_attributes', 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT:
        raise ValueError(f'Link ou ponto de junção na sessão: {path}')
    if not (stat.S_ISDIR(metadata.st_mode) or stat.S_ISREG(metadata.st_mode)):
        raise ValueError(f'Tipo de arquivo incompatível: {path}')


def archive_tree(archive, session, path):
    check_regular(path)
    archive.write(path, path.relative_to(session).as_posix())
    if path.is_dir():
        for child in sorted(path.iterdir()):
            archive_tree(archive, session, child)


def game_manifest(lines):
    checksums = {}
    for line in lines:
        if not line.startswith('OK '):
            continue
        _, checksum, size, source = line.split(' ', 3)
        if source.startswith(('fs:/mif/', 'fs:/mod/')):
            checksums[source[4:]] = (int(checksum, 16), int(size))
    return checksums


def latest_attempt(lines):
    start = 0
    for at, line in enumerate(lines):
        if line == 'RETOMADA':
            start = at + 1
    return lines[start:]


def export_session(session, output):
    check_regular(session)
    session = session.resolve()
    output = output.resolve()
    if output.is_relative_to(session):
        raise ValueError('Escolha um ZIP fora da pasta da sessão.')
    log = session / 'MANIFESTO.TXT'
    if not log.exists():
        log = session / 'STATUS.TXT'
    check_regular(log)
    lines = latest_attempt(log.read_text(encoding='utf-8', errors='surrogateescape').splitlines())
    if (not lines or lines[-1].strip() != 'COMPLETO'
            or any((session / name).exists() for name in ('TRANSFER.PRT', 'TROCA.PRT'))):
        raise ValueError('A sessão precisa estar concluída e sem arquivo temporário pendente.')
    trees = [session / name for name in ('mif', 'mod')]
    if any(not directory.is_dir() for directory in trees):
        raise ValueError('A sessão precisa conter as pastas mif e mod.')
    expected = game_manifest(lines)
    if any(line.startswith('OK ') for line in lines) and not expected:
        raise ValueError('O relatório não registra arquivos de jogos.')

    with output.open('x+b') as stream:
        try:
            with ZipFile(stream, 'w', ZIP_DEFLATED) as archive:
                for directory in trees:
                    archive_tree(archive, session, directory)
                files = {entry.filename: (entry.CRC, entry.file_size)
                         for entry in archive.infolist() if not entry.is_dir()}
                if expected and files != expected:
                    raise ValueError('Os arquivos do cartão divergem dos caminhos, CRCs ou tamanhos do relatório.')
                if any(not any(name.startswith(f'{root}/') for name in files) for root in ('mif', 'mod')):
                    raise ValueError('O ZIP precisa conter arquivos em mif e em mod.')
            stream.seek(0)
            with ZipFile(stream) as archive:
                damaged = archive.testzip()
                if damaged:
                    raise ValueError(f'Falha na validação do ZIP: {damaged}')
        except BaseException:
            stream.close()
            output.unlink()
            raise
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('sessao', type=Path, help='pasta Bxxxxxx do backup')
    parser.add_argument('--saida', type=Path, help='caminho de um ZIP novo')
    args = parser.parse_args()
    output = args.saida or Path(__file__).resolve().parent / 'build' / f'memory-copy-{args.sessao.name}.zip'
    try:
        print(export_session(args.sessao, output))
    except (OSError, ValueError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
