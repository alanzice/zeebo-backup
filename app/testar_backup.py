"""Executa o applet de backup ARM em um BREW com armazenamento fictício."""

from dataclasses import dataclass
from contextlib import redirect_stdout
from io import StringIO
import json
from pathlib import Path
import struct
import sys
from tempfile import TemporaryDirectory
import unittest
from zipfile import ZipFile
from zlib import crc32

from brew.empacotar import mif_payload
from conferir_backup import verify
from preparar_memory_copy import export_session
from testar_brew import FakeBrew, ROOT, DATA, class_id
from unicorn.arm_const import UC_ARM_REG_SP


PACKAGE = ROOT / 'build' / 'backup'
MODULE = PACKAGE / 'mod' / 'zeeback' / 'zeeback.mod'
MIF = PACKAGE / 'mif' / 'zeeback.mif'
UP, DOWN, RIGHT, OK, BACK = 0xE031, 0xE032, 0xE034, 0xE064, 0xE04A
HOME = 0xE030
DEVICE_OBJECTS = 0xA00000
SD = 'fs:/card0/'
ROOT_BACKUP = SD + 'ZB/'
SD_MARKER = ROOT_BACKUP + 'ZB-CARD.TXT'
GAME = 'fs:/mod/274754'
QUAKE = 'fs:/mod/274802'
FIFA = 'fs:/mod/274803'
GAME_DATA = bytes(range(256)) * 193
SETTINGS = {
    'fs:/mod/274755/tectoy.cfg': b'LANG=pt\n',
    'fs:/mod/274755/tt_prefs.db': b'prefs ficticios',
    'fs:/sys/hid_devices.cfg': b'controle ficticio',
    'fs:/mcp/tvenc_status.dat': b'tv ficticia',
}


@dataclass
class OpenFile:
    path: str
    writing: bool
    position: int = 0


class FakeStorage(FakeBrew):
    instruction_limit = 1500000

    def __init__(self, base=0x100000, *, card=True, corrupt=False, short_write=False,
                 fail_write=False, previous=False, extra_games=False, root_test_fails=False,
                 controller=False, prepared_sd=False, hidden_zb_info=False, deny_mkdir=False,
                 sd_error=0, sd_capacity=64_000_000):
        super().__init__(base, module_path=MODULE)
        self.uc.mem_map(DEVICE_OBJECTS, 0x10000)
        self.next_handle = DEVICE_OBJECTS
        self.clock_ms = 0
        self.step_ms = 15
        helpers = self.word(base - 4)
        self.store(helpers + 44 * 4, self.stub(lambda: self.clock_ms))
        self.catalog_ids = ['274754', '274802', '274803']
        if extra_games:
            self.catalog_ids += ['276121', '276152', '276154', '277455']
        self.card = card
        self.root_test_fails = root_test_fails
        self.controller = controller
        self.hidden_zb_info = hidden_zb_info
        self.deny_mkdir = deny_mkdir
        self.sd_error = sd_error
        self.sd_capacity = sd_capacity
        self.pad_state = 0
        self.shoulder_state = 0
        self.extra_timers = {}
        self.close_requests = []
        self.corrupt = corrupt
        self.short_write = short_write
        self.fail_write = fail_write
        self.source_paths = {
            GAME + '/double.mod': GAME_DATA,
            GAME + '/udata/slot1/save.dat': b'save ficticio',
            GAME + '/udata/vazio.dat': b'',
            GAME + '/udata/nao-copiar.KEY': b'material da fixture',
            QUAKE + '/quake.mod': b'quake ficticio',
            FIFA + '/fifa.mod': b'fifa ficticio',
            'fs:/mif/274754.mif': b'descritor ficticio A',
            'fs:/mif/274802.mif': b'descritor ficticio B',
            'fs:/mif/274803.mif': b'descritor ficticio C',
            **SETTINGS,
        }
        self.dirs = {'fs:/mod', 'fs:/mif', 'fs:/sys', 'fs:/mcp', 'fs:/mod/274755', GAME,
                     GAME + '/udata', GAME + '/udata/slot1', GAME + '/empty', QUAKE, FIFA}
        self.dirs.update('fs:/mod/' + item for item in self.catalog_ids)
        if self.card:
            self.dirs.add(SD.rstrip('/'))
        self.sd_files = {}
        if prepared_sd:
            assert card
            self.dirs.add(ROOT_BACKUP.rstrip('/'))
            self.sd_files[SD_MARKER.lower()] = b'ZB04\r\n'
        if previous:
            self.dirs.update({ROOT_BACKUP.rstrip('/'), ROOT_BACKUP + 'B000001'})
            self.sd_files[(ROOT_BACKUP + 'B000001/old.dat').lower()] = b'anterior'
        self.mutations = []
        self.opens = []
        self.blocked_sources = set()
        self.files_open = {}
        self.cursors = {}
        self.errors = {}
        self.frames = []
        self.commands = []
        self.colors = {1: 0, 2: 0xffffff00}
        self.manager_created = False

        self.object(self.shell, {0: lambda: 2, 1: lambda: self.release(self.shell),
                                 2: self.create, 6: self.close_applet,
                                 11: self.set_timer, 12: self.cancel_timer})
        self.object(self.display, {1: lambda: self.release(self.display), 4: self.draw_text,
                                   5: self.draw_rect, 7: self.update, 10: self.set_color})

    def new_handle(self):
        address = self.next_handle
        self.next_handle += 0x80
        assert self.next_handle < DEVICE_OBJECTS + 0x10000
        return address

    def create(self):
        cls = self.reg(1)
        if cls == 0x01001001:
            address = self.display
        elif cls == 0x0106c411:
            if not self.controller:
                return 1
            address = self.new_handle()
            self.object(address, {1: lambda: self.release(address), 3: self.create_pad,
                                  7: self.list_pads})
        else:
            assert cls == 0x01001003
            address = self.new_handle() if self.manager_created else self.files
            self.manager_created = True
            self.object(address, {1: self.release_manager, 2: self.open, 3: self.info,
                                  4: self.remove, 5: self.mkdir, 7: self.exists,
                                  9: self.last_error, 10: self.enum_init, 11: self.enum_next,
                                  12: self.rename, 13: self.enum_next_ex, 20: self.free_space_ex})
        self.store(self.reg(2), address)
        return 0

    def list_pads(self):
        assert self.reg(1) == 0x0106c3fd and self.reg(3) == 2
        self.store(self.reg(2), 42)
        self.store(self.word(self.uc.reg_read(UC_ARM_REG_SP)), 1)
        return 0

    def create_pad(self):
        assert self.reg(1) == 42
        address = self.new_handle()
        self.object(address, {1: lambda: self.release(address), 6: self.pad_info, 7: self.pad_count})
        self.store(self.reg(2), address)
        return 0

    def pad_info(self):
        button = self.reg(1)
        assert 0 <= button < 8
        state, uid = {1: (self.pad_state, 0x0106c40a),
                      5: (self.shoulder_state, 0x0106c408)}.get(button, (0, 0))
        self.uc.mem_write(self.reg(2), struct.pack('<5i', button, state, uid, 0, 1))
        return 0

    def pad_count(self):
        self.store(self.reg(1), 8)
        return 0

    def release_manager(self):
        address = self.reg(0)
        self.cursors.pop(address, None)
        return self.release(address)

    def file_data(self, path):
        return self.sd_files.get(path.lower()) if path.startswith(SD) else self.source_paths.get(path)

    def directory(self, path):
        path = path.rstrip('/')
        return any(path.lower() == item.lower() for item in self.dirs) if path.startswith('fs:/card0') else path in self.dirs

    def last_error(self):
        return self.errors.get(self.reg(0), 0)

    def free_space_ex(self):
        assert self.c_string(self.reg(1)) == SD
        if not self.card or self.sd_capacity is None:
            return 20
        self.store(self.reg(2), 128_000_000)
        used = sum(map(len, self.sd_files.values()))
        self.store(self.reg(3), max(0, self.sd_capacity - used))
        return 0

    def info(self):
        path = self.c_string(self.reg(1))
        if self.sd_error and path.startswith(SD):
            self.errors[self.reg(0)] = self.sd_error
            return 1
        if self.hidden_zb_info and path == ROOT_BACKUP.rstrip('/'):
            return 1
        directory = self.directory(path)
        content = self.file_data(path)
        if not directory and content is None:
            return 5
        self.write_info(self.reg(2), path, 2 if directory else 0, len(content or b''))
        return 0

    def exists(self):
        path = self.c_string(self.reg(1))
        if self.root_test_fails and path == SD:
            return 1
        return 0 if self.directory(path) or self.file_data(path) is not None else 257

    def enum_init(self):
        path = self.c_string(self.reg(1)).rstrip('/')
        if not self.directory(path):
            return 5
        directory = bool(self.reg(2))
        candidates = self.dirs if directory else self.source_paths.keys()
        self.cursors[self.reg(0)] = iter(sorted(child for child in candidates
                                                if child.rpartition('/')[0] == path))
        self.errors[self.reg(0)] = 0
        return 0

    def next_path(self):
        manager = self.reg(0)
        path = next(self.cursors[manager], None)
        if path is None:
            self.errors[manager] = 1
        return path

    def enum_next(self):
        path = self.next_path()
        if path is None:
            return 0
        directory = self.directory(path)
        self.write_info(self.reg(1), path, 2 if directory else 0, len(self.file_data(path) or b''))
        return 1

    def enum_next_ex(self):
        path = self.next_path()
        if path is None:
            return 0
        entry = self.reg(1)
        assert self.word(entry) == 40
        name, capacity = self.word(entry + 16), self.word(entry + 20)
        raw = path.encode('ascii') + b'\x00'
        assert len(raw) <= capacity
        self.uc.mem_write(name, raw)
        self.store(entry + 4, 2 if self.directory(path) else 0)
        self.store(entry + 12, len(self.file_data(path) or b''))
        return 1

    def write_info(self, address, path, attribute, size):
        self.uc.mem_write(address, struct.pack('<B3xII64s', attribute, 0, size, path.encode('ascii')))

    def changed(self, operation, path):
        assert path == ROOT_BACKUP.rstrip('/') or path.startswith(ROOT_BACKUP), (operation, path)
        assert not any(part in ('', '.', '..') for part in path[len(SD):].split('/'))
        self.mutations.append((operation, path))

    def mkdir(self):
        path = self.c_string(self.reg(1))
        if self.sd_error and path.startswith(SD):
            self.errors[self.reg(0)] = self.sd_error
            return 1
        if self.deny_mkdir:
            self.errors[self.reg(0)] = 13
            return 1
        if self.directory(path) or not self.directory(path.rpartition('/')[0]):
            return 5
        self.changed('mkdir', path)
        self.dirs.add(path)
        return 0

    def open(self):
        path, mode = self.c_string(self.reg(1)), self.reg(2)
        self.opens.append((path, mode))
        if mode == 1 and path in self.blocked_sources:
            self.errors[self.reg(0)] = 257
            return 0
        if self.sd_error and path.startswith(SD):
            self.errors[self.reg(0)] = self.sd_error
            return 0
        if mode == 4:
            self.changed('open create', path)
            assert self.file_data(path) is None and self.directory(path.rpartition('/')[0])
            self.sd_files[path.lower()] = b''
        elif mode == 8:
            assert path == ROOT_BACKUP + 'STATUS.TXT' or path.endswith('/MANIFESTO.TXT')
            if self.file_data(path) is None:
                self.errors[self.reg(0)] = 5
                return 0
            self.changed('open append', path)
        else:
            assert mode == 1
            if self.file_data(path) is None:
                self.errors[self.reg(0)] = 5
                return 0
        address = self.new_handle()
        self.object(address, {1: self.release_file, 3: self.read, 5: self.write, 7: self.seek})
        self.files_open[address] = OpenFile(path, mode in (4, 8))
        return address

    def release_file(self):
        address = self.reg(0)
        del self.files_open[address]
        return self.release(address)

    def read(self):
        file = self.files_open[self.reg(0)]
        assert not file.writing
        contents = self.file_data(file.path)
        data = contents[file.position:file.position + self.reg(2)]
        if self.corrupt and file.path.endswith('/TRANSFER.PRT') and data:
            data = bytes((data[0] ^ 0x80,)) + data[1:]
        self.uc.mem_write(self.reg(1), data)
        file.position += len(data)
        return len(data)

    def seek(self):
        file = self.files_open[self.reg(0)]
        assert not file.writing and self.reg(1) == 0
        file.position = self.reg(2)
        return 0

    def write(self):
        file = self.files_open[self.reg(0)]
        assert file.writing
        self.changed('write', file.path)
        if self.fail_write and file.path.endswith('/TRANSFER.PRT'):
            if self.fail_write == 262:
                self.errors[self.files] = 262
            return 0
        size = self.reg(2)
        if self.short_write and file.path.endswith('/TRANSFER.PRT'):
            size = min(2000, size)
        self.sd_files[file.path.lower()] += bytes(self.uc.mem_read(self.reg(1), size))
        return size

    def remove(self):
        path = self.c_string(self.reg(1))
        self.changed('remove', path)
        assert path.endswith(('/TRANSFER.PRT', '/TROCA.PRT'))
        del self.sd_files[path.lower()]
        return 0

    def rename(self):
        source, dest = self.c_string(self.reg(1)), self.c_string(self.reg(2))
        self.changed('rename src', source)
        self.changed('rename dst', dest)
        assert source.lower() in self.sd_files and dest.lower() not in self.sd_files
        self.sd_files[dest.lower()] = self.sd_files.pop(source.lower())
        return 0

    def set_timer(self):
        delay = self.reg(1)
        task = self.reg(2), self.reg(3)
        if delay == 15:
            assert self.timer is None
            self.timer = task
        else:
            assert delay in (30, 1600) and delay not in self.extra_timers
            self.extra_timers[delay] = task
        return 0

    def cancel_timer(self):
        task = self.reg(1), self.reg(2)
        for delay, timer in self.extra_timers.items():
            if task == timer:
                del self.extra_timers[delay]
                return 0
        return super().cancel_timer()

    def fire_extra_timer(self, delay):
        self.invoke(*self.extra_timers.pop(delay))

    def close_applet(self):
        assert self.reg(1) == 0
        self.close_requests.append(self.reg(1))
        return 0

    @staticmethod
    def rgb(rgb):
        return '#{:02x}{:02x}{:02x}'.format((rgb >> 8) & 255, (rgb >> 16) & 255, (rgb >> 24) & 255)

    def set_color(self):
        item = self.reg(1)
        previous = self.colors[item]
        self.colors[item] = self.reg(2)
        return previous

    def draw_rect(self):
        rectangle = list(struct.unpack('<4h', self.uc.mem_read(self.reg(1), 8)))
        if rectangle == [0, 0, 640, 480]:
            self.commands = []
        self.commands.append({'kind': 'rect', 'bounds': rectangle, 'color': self.rgb(self.reg(3))})
        return 0

    def draw_text(self):
        point = self.uc.reg_read(UC_ARM_REG_SP)
        x, y, _, flags = struct.unpack('<4I', self.uc.mem_read(point, 16))
        assert flags == 0x8000
        value = self.uc.mem_read(self.reg(2), self.reg(3) * 2).decode('utf-16le')
        for command in reversed(self.commands):
            if command['kind'] == 'rect':
                left, top, width, height = command['bounds']
                if left <= x < left + width and top <= y < top + height:
                    background = command['color']
                    break
        else:
            raise AssertionError(f'Sem fundo pintado para {value!r}')
        self.commands.append({'kind': 'text', 'x': x, 'y': y, 'color': self.rgb(self.colors[1]),
                              'background': background,
                              'bold': bool(self.reg(1)), 'value': value})
        return 0

    def update(self):
        self.text = [cmd['value'] for cmd in self.commands if cmd['kind'] == 'text']
        self.frames.append(list(self.commands))
        return 0

    def start(self):
        assert self.invoke(self.base, self.shell, 0, DATA) == 0
        module = self.word(DATA)
        assert self.invoke_slot(module, 2, self.shell, class_id(MIF, 'Zeebo Backup'), DATA + 4) == 0
        app = self.word(DATA + 4)
        assert self.invoke_slot(app, 2, 0, 0, 0) == 1
        return module, app

    def press(self, app, key):
        assert self.invoke_slot(app, 2, 0x100, key, 0) == 1

    def choose_game(self, app):
        for key in (DOWN, OK, OK, RIGHT, OK):
            self.press(app, key)

    def choose_all(self, app):
        for key in (OK, RIGHT, OK):
            self.press(app, key)

    def choose_resume(self, app):
        for key in (DOWN, DOWN, DOWN, OK):
            self.press(app, key)

    def step_timer(self):
        callback, context = self.timer
        self.timer = None
        self.clock_ms = (self.clock_ms + self.step_ms) & 0xffffffff
        self.invoke(callback, context)

    def drain_timers(self):
        for _ in range(120):
            if not self.timer:
                return
            self.step_timer()
        raise AssertionError('backup did not finish')

    def stop(self, module, app):
        super().stop(module, app)
        assert not self.files_open and not self.cursors
        assert not self.extra_timers


def expected_path(run, source):
    return (ROOT_BACKUP + run + '/' + source[4:]).lower()


def preview():
    brew = FakeStorage(extra_games=True, controller=True)
    brew.dirs.add('fs:/mod/277380')
    brew.source_paths['fs:/mif/277380.mif'] = mif_payload(0x0108fab8, 'GOF')
    module, app = brew.start()
    screens = {'menu': brew.frames[-1]}
    brew.shoulder_state = 1
    brew.fire_extra_timer(30)
    screens['menu-escuro'] = brew.frames[-1]
    brew.press(app, DOWN)
    brew.press(app, OK)
    for _ in range(7):
        brew.press(app, DOWN)
        if 'Galaxy on Fire (GOF)' in brew.text:
            break
    else:
        raise AssertionError('Galaxy on Fire (GOF) ausente da prévia')
    screens['jogos'] = brew.frames[-1]
    for _ in range(8):
        brew.press(app, UP)
    for key in (OK, RIGHT):
        brew.press(app, key)
    screens['confirmar'] = brew.frames[-1]
    brew.press(app, OK)
    brew.drain_timers()
    screens['resultado'] = brew.frames[-1]
    for key in (OK, DOWN, OK):
        brew.press(app, key)
    screens['instrucoes'] = brew.frames[-1]
    brew.stop(module, app)
    full = FakeStorage()
    full.step_ms = 750
    full.source_paths[GAME + '/video.dat'] = bytes(196_608)
    module, app = full.start()
    full.choose_all(app)
    screens['calculando'] = full.frames[-1]
    while full.timer:
        full.step_timer()
        if any('% dos bytes' in line and not line.startswith('0%') for line in full.text):
            screens['progresso'] = full.frames[-1]
            break
    full.press(app, OK)
    screens['pausado'] = full.frames[-1]
    full.press(app, BACK)
    full.press(app, OK)
    full.choose_resume(app)
    screens['retomar'] = full.frames[-1]
    full.stop(module, app)
    no_card = FakeStorage(card=False)
    module, app = no_card.start()
    no_card.choose_game(app)
    screens['falha-sd'] = no_card.frames[-1]
    no_card.press(app, RIGHT)
    screens['diagnostico-sd'] = no_card.frames[-1]
    no_card.stop(module, app)
    output = ROOT / 'build' / 'backup-preview.json'
    output.write_text(json.dumps(screens), encoding='utf-8')
    print(output)


class BackupTest(unittest.TestCase):
    def test_pacote_do_memory_copy_inclui_sig_e_icones(self):
        with ZipFile(ROOT / 'build' / 'zeebo-backup-v0.12.zip') as package:
            self.assertIsNone(package.testzip())
            self.assertEqual(set(package.namelist()), {
                'mif/', 'mod/', 'mod/zeeback/', 'mif/zeeback.mif',
                'mod/zeeback/zeeback.mod', 'mod/zeeback/zeeback.sig',
                'ZB/', 'ZB/ZB-CARD.TXT',
            })
            self.assertEqual(package.read('mod/zeeback/zeeback.sig'), b'')
            self.assertEqual(package.read('mod/zeeback/zeeback.mod'), MODULE.read_bytes())
            self.assertEqual(package.read('ZB/ZB-CARD.TXT'), b'ZB04\r\n')
            data = package.read('mif/zeeback.mif')
            self.assertEqual(data, MIF.read_bytes())
        table, count = struct.unpack_from('<II', data, 16)
        offsets = struct.unpack_from(f'<{count + 1}I', data, table)
        sizes = {21: 64, 23: 16}
        prefix = b'\x0c\x00image/bmp\0'
        for kind, identifier, ref in struct.iter_unpack('<HHI', data[32:table]):
            if kind != 6:
                continue
            section = ref >> 16
            icon = data[offsets[section]:offsets[section + 1]]
            self.assertTrue(icon.startswith(prefix + b'BM'))
            size = sizes.pop(identifier)
            self.assertEqual(struct.unpack_from('<iiHH', icon, len(prefix) + 18), (size, size, 1, 24))
        self.assertEqual(sizes, {})
        self.assertEqual(class_id(MIF, 'Zeebo Backup'), 0x01F05A43)
        module_section = next(ref >> 16 for kind, identifier, ref
                              in struct.iter_unpack('<HHI', data[32:table])
                              if (kind, identifier) == (0x5000, 0))
        privilege_at = offsets[module_section] + 24
        self.assertEqual(data[privilege_at:privilege_at + 2], b'\x01\x00')
        self.assertEqual(struct.unpack_from('<H', data, offsets[module_section] + 30), (1,))
        dependencies = next(ref >> 16 for kind, identifier, ref
                            in struct.iter_unpack('<HHI', data[32:table])
                            if (kind, identifier) == (0x5000, 3))
        self.assertEqual(data[offsets[dependencies]:offsets[dependencies + 1]],
                         struct.pack('<II', 0x0101414e, 0))
        system_path = ROOT / 'build' / 'zeebo-backup-v0.12-teste-sd.zip'
        if system_path.is_file():
            with ZipFile(system_path) as system_package:
                self.assertIsNone(system_package.testzip())
                self.assertEqual(system_package.read('mod/zeeback/zeeback.mod'), MODULE.read_bytes())
                self.assertEqual(system_package.read('mif/zeeback.mif'),
                                 data[:privilege_at] + b'\xff\xff' + data[privilege_at + 2:])

    def test_backup_um_jogo_preserva_copia_anterior_e_aceita_escrita_curta(self):
        brew = FakeStorage(base=0x350000, short_write=True, previous=True)
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        run = 'B000002'
        for path in (GAME + '/double.mod', GAME + '/udata/slot1/save.dat',
                     GAME + '/udata/vazio.dat', 'fs:/mif/274754.mif'):
            self.assertEqual(brew.sd_files[expected_path(run, path)], brew.source_paths[path])
        self.assertTrue(brew.directory(ROOT_BACKUP + run + '/mod/274754/empty'))
        self.assertNotIn(expected_path(run, QUAKE + '/quake.mod'), brew.sd_files)
        self.assertEqual(brew.sd_files[(ROOT_BACKUP + 'B000001/old.dat').lower()], b'anterior')
        self.assertIn('Backup concluído', brew.text)
        self.assertNotIn((GAME + '/udata/nao-copiar.KEY', 1), brew.opens)
        status = brew.sd_files[(ROOT_BACKUP + run + '/MANIFESTO.TXT').lower()]
        self.assertTrue(status.endswith(b'COMPLETO\r\n'))
        self.assertIn(f'OK {crc32(GAME_DATA):08x} {len(GAME_DATA)} {GAME}/double.mod'.encode(), status)
        self.assertIn(f'OK 00000000 0 {GAME}/udata/vazio.dat'.encode(), status)
        brew.stop(module, app)

    def test_catalogo_le_titulo_do_mif_e_usa_os_ids_de_referencia(self):
        brew = FakeStorage()
        brew.dirs.update('fs:/mod/' + item for item in ('12875', '276675', '277285', '888888', '999999'))
        brew.source_paths['fs:/mif/12875.mif'] = mif_payload(0x010292c3, 'Módulo local')
        brew.source_paths['fs:/mif/888888.mif'] = mif_payload(0x01012233, 'Título local')
        brew.source_paths['fs:/mif/999999.mif'] = b'\x11\x00' + bytes(30)
        module, app = brew.start()
        for key in (DOWN, OK):
            brew.press(app, key)
        self.assertIn('IMICRO3D (Módulo local)', brew.text)
        self.assertIn('Resident Evil 4', brew.text)
        self.assertIn('Zeebo Extreme Corrida Aérea', brew.text)
        for _ in range(7):
            brew.press(app, DOWN)
        self.assertIn('Título local', brew.text)
        self.assertIn('Título indisponível', brew.text)
        brew.stop(module, app)

    def test_nome_de_divulgacao_com_apelido_e_sem_duplicar_o_titulo(self):
        brew = FakeStorage()
        brew.dirs.add('fs:/mod/277380')
        brew.source_paths['fs:/mif/277380.mif'] = mif_payload(0x0108fab8, 'GOF')
        brew.source_paths['fs:/mif/274754.mif'] = mif_payload(0x0102f789, 'DOUBLE DRAGON')
        module, app = brew.start()
        for key in (DOWN, OK):
            brew.press(app, key)
        self.assertIn('Galaxy on Fire (GOF)', brew.text)
        self.assertIn('Double Dragon', brew.text)
        self.assertNotIn('Double Dragon (DOUBLE DRAGON)', brew.text)
        for key in (DOWN, DOWN, DOWN, OK):
            brew.press(app, key)
        self.assertIn('Galaxy on Fire (GOF)', brew.text)
        brew.stop(module, app)

    def test_r_alterna_tema_e_instrucoes_nao_escrevem_no_sd(self):
        brew = FakeStorage(controller=True)
        module, app = brew.start()
        light = brew.frames[-1][0]['color']
        brew.shoulder_state = 1
        brew.fire_extra_timer(30)
        dark = brew.frames[-1][0]['color']
        self.assertNotEqual(light, dark)
        brew.fire_extra_timer(30)
        self.assertEqual(brew.frames[-1][0]['color'], dark)
        for key in (DOWN, DOWN, OK):
            brew.press(app, key)
        self.assertIn('O Memory Copy grava do SD para o console.', brew.text)
        brew.shoulder_state = 0
        brew.fire_extra_timer(30)
        brew.shoulder_state = 1
        brew.fire_extra_timer(30)
        self.assertEqual(brew.frames[-1][0]['color'], light)
        brew.pad_state = 1
        brew.fire_extra_timer(30)
        self.assertIn('Backup completo', brew.text)
        self.assertEqual(brew.mutations, [])
        brew.stop(module, app)

    def test_exporta_sessao_antiga_e_confere_manifesto_novo_sem_renomear(self):
        with TemporaryDirectory() as root:
            root = Path(root)
            session = root / 'B000001'
            files = {'mod/274754/udata/Slot 1.DAT': b'save', 'mif/274754.mif': b'mif',
                     'sys/hid_devices.cfg': b'config'}
            for name, content in files.items():
                path = session / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(content)
            (session / 'mod/274754/empty').mkdir()
            log = session / 'STATUS.TXT'
            log.write_text('COMPLETO\n', encoding='utf-8')
            old_zip = export_session(session, root / 'antigo.zip')
            entries = ''.join(f'OK {crc32(content):08x} {len(content)} fs:/{name}\n'
                              for name, content in files.items())
            log = session / 'MANIFESTO.TXT'
            log.write_text(entries + 'COMPLETO\n', encoding='utf-8')
            new_zip = export_session(session, root / 'novo.zip')
            for package in (old_zip, new_zip):
                with ZipFile(package) as archive:
                    self.assertIn('mod/274754/empty/', archive.namelist())
                    self.assertEqual(archive.read('mod/274754/udata/Slot 1.DAT'), b'save')
                    self.assertEqual(archive.read('mif/274754.mif'), b'mif')
                    self.assertTrue(all(name.startswith(('mif/', 'mod/')) for name in archive.namelist()))
            (session / 'mod/274754/udata/Slot 1.DAT').write_bytes(b'save alterado')
            bad_zip = root / 'divergente.zip'
            with self.assertRaises(ValueError):
                export_session(session, bad_zip)
            self.assertFalse(bad_zip.exists())

    def test_backup_completo_inclui_todos_os_jogos_e_configuracoes(self):
        brew = FakeStorage()
        resource_dir = 'fs:/mod/nfsresources/fe_components'
        resource = resource_dir + '/arquivo_de_recurso_com_nome_longo_original.dat'
        private = 'fs:/mod/sys/download'
        brew.dirs.update({'fs:/mod/nfsresources', resource_dir, 'fs:/mod/sys', private})
        brew.source_paths[resource] = b'recurso complementar'
        brew.source_paths[private + '/arquivo.dat'] = b'dado excluido'
        brew.source_paths['fs:/mcp/lctsys/flixfile.dat'] = b'configuracao de teste'
        module, app = brew.start()
        brew.choose_all(app)
        brew.drain_timers()
        run = 'B000001'
        for path in brew.source_paths:
            if not path.endswith('.KEY') and not path.startswith(private) and not path.endswith('flixfile.dat'):
                self.assertEqual(brew.sd_files[expected_path(run, path)], brew.source_paths[path])
        self.assertEqual(brew.sd_files[(ROOT_BACKUP + run + '/lctsys/flixfile.dat').lower()],
                         b'configuracao de teste')
        self.assertNotIn((private + '/arquivo.dat', 1), brew.opens)
        self.assertIn('Backup concluído', brew.text)
        self.assertIn('Itens excluídos: 2', brew.text)
        brew.stop(module, app)

    def test_historico_na_raiz_preserva_a_sessao_anterior_e_o_id_original(self):
        brew = FakeStorage()
        brew.dirs.add('fs:/mod/zeetris')
        brew.source_paths['fs:/mod/zeetris/zeetris.mod'] = b'app ficticio'
        brew.source_paths['fs:/mif/zeetris.mif'] = b'mif ficticio'
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        brew.press(app, OK)
        brew.press(app, OK)
        for _ in range(3):
            brew.press(app, DOWN)
        self.assertIn('Zeetris', brew.text)
        self.assertIn('zeetris', brew.text)
        for key in (OK, RIGHT, OK):
            brew.press(app, key)
        brew.drain_timers()
        history = brew.sd_files[(ROOT_BACKUP + 'STATUS.TXT').lower()]
        self.assertIn(b'B000001 | Jogo/app 274754 | COMPLETO', history)
        self.assertIn(b'B000002 | Jogo/app zeetris | COMPLETO', history)
        self.assertNotIn((ROOT_BACKUP + 'B000002/STATUS.TXT').lower(), brew.sd_files)
        self.assertIn(expected_path('B000002', 'fs:/mod/zeetris/zeetris.mod'), brew.sd_files)
        brew.stop(module, app)

    def test_sd_ausente_e_confirmacao_sem_mutacao(self):
        brew = FakeStorage(card=False)
        module, app = brew.start()
        brew.press(app, OK)
        brew.press(app, OK)  # Voltar é a ação inicialmente selecionada.
        self.assertEqual(brew.mutations, [])
        brew.choose_all(app)
        self.assertIn('Backup parcial', brew.text)
        self.assertIn('Preparar a pasta ZB no SD', brew.text)
        self.assertIn('fs:/card0/ZB', brew.text)
        self.assertEqual(brew.mutations, [])
        brew.press(app, RIGHT)
        self.assertIn('Diagnóstico do cartão SD', brew.text)
        self.assertIn('Leitura de ZB-CARD.TXT: 5', brew.text)
        self.assertIn('Consulta ao volume SD: 20', brew.text)
        brew.stop(module, app)

    def test_espaco_livre_zero_impede_abertura_da_sessao(self):
        brew = FakeStorage(sd_capacity=0)
        module, app = brew.start()
        brew.press(app, OK)
        self.assertIn('Espaço livre no SD: 0 bytes', brew.text)
        for key in (RIGHT, OK):
            brew.press(app, key)
        self.assertIn('Espaço insuficiente no cartão SD', brew.text)
        self.assertEqual(brew.mutations, [])
        brew.stop(module, app)

    def test_backup_completo_mostra_progresso_dos_bytes_verificados(self):
        brew = FakeStorage()
        brew.source_paths[GAME + '/video.dat'] = bytes(196_608)
        brew.source_paths[GAME + '/grande.KEY'] = bytes(196_608)
        module, app = brew.start()
        brew.choose_all(app)
        self.assertIn('Calculando tamanho do backup', brew.text)
        while 'Calculando tamanho do backup' in brew.text:
            brew.step_timer()
        self.assertFalse(any(path.endswith('/TRANSFER.PRT') for _, path in brew.mutations))
        self.assertIn('0% dos bytes estimados verificados', brew.text)
        percentages = set()
        while brew.timer:
            brew.step_timer()
            for line in brew.text:
                if '% dos bytes' in line:
                    percent = int(line.split('%', 1)[0])
                    self.assertLess(percent, 100)
                    if percent:
                        self.assertIn(expected_path('B000001', GAME + '/double.mod'), brew.sd_files)
                        percentages.add(percent)
        self.assertIn('100% do backup', brew.text)
        self.assertIn('Itens excluídos: 2', brew.text)
        estimated = sum(len(content) for name, content in brew.source_paths.items()
                        if not name.endswith('.KEY'))
        self.assertIn(len(GAME_DATA) * 100 // estimated, percentages)
        brew.stop(module, app)

    def test_cancelar_estimativa_libera_enum_e_preserva_estado_cancelado(self):
        brew = FakeStorage()
        module, app = brew.start()
        catalog_cursors = set(brew.cursors)
        brew.choose_all(app)
        brew.step_timer()
        self.assertIn('Calculando tamanho do backup', brew.text)
        self.assertGreater(len(brew.cursors), len(catalog_cursors))
        brew.press(app, BACK)
        self.assertIn('Backup cancelado', brew.text)
        self.assertEqual(set(brew.cursors), catalog_cursors)
        self.assertFalse(any('%' in line for line in brew.text))
        brew.stop(module, app)

    def test_pausa_copia_sem_perder_o_arquivo_em_andamento(self):
        brew = FakeStorage()
        module, app = brew.start()
        brew.choose_all(app)
        while not any(path.endswith('/transfer.prt') for path in brew.sd_files):
            brew.step_timer()
        before = brew.sd_files.copy()
        brew.press(app, OK)
        self.assertIn('Backup pausado', brew.text)
        self.assertIsNone(brew.timer)
        self.assertEqual(brew.sd_files, before)
        brew.press(app, OK)
        brew.drain_timers()
        self.assertIn('Backup concluído', brew.text)
        brew.stop(module, app)

    def test_animacao_na_estimativa_e_tempo_restante_exclui_pausa(self):
        brew = FakeStorage()
        brew.step_ms = 750
        brew.source_paths.update({GAME + f'/oculto{i:03}.KEY': b'x' for i in range(190)})
        brew.source_paths[GAME + '/video.dat'] = bytes(196_608)
        module, app = brew.start()
        brew.choose_all(app)

        def dots():
            return [command['color'] for command in brew.frames[-1]
                    if command.get('bounds', [0, 0])[1] == 66]

        first = dots()
        for _ in range(8):
            brew.step_timer()
        self.assertIn('Calculando tamanho do backup', brew.text)
        self.assertEqual(len(first), 4)
        self.assertNotEqual(first, dots())

        def estimate():
            return next((line for line in brew.text if line.startswith('Restam ~ ')), None)

        while 'Calculando tamanho do backup' in brew.text:
            brew.step_timer()
        self.assertIn('Medindo velocidade...', brew.text)
        while brew.timer and estimate() is None:
            brew.step_timer()
        expected = estimate()
        self.assertIsNotNone(expected)
        rows = {command['value']: command['y'] for command in brew.frames[-1]
                if command['kind'] == 'text'}
        percent = next(line for line in rows if '% dos bytes estimados' in line)
        self.assertGreaterEqual(rows[expected] - rows[percent], 30)
        brew.press(app, OK)
        self.assertIn('Backup pausado', brew.text)
        self.assertIsNone(estimate())
        brew.clock_ms += 60_000
        brew.press(app, OK)
        self.assertEqual(estimate(), expected)
        brew.drain_timers()
        brew.stop(module, app)

    def test_retomada_apos_reabrir_reaproveita_e_corrige_arquivos(self):
        boot = FakeStorage()
        unchanged = GAME + '/a.dat'
        changed = GAME + '/b.dat'
        corrupted = GAME + '/double.mod'
        boot.source_paths.update({unchanged: b'original', changed: b'antes'})
        module, app = boot.start()
        boot.choose_all(app)
        session = 'B000001'
        while expected_path(session, corrupted) not in boot.sd_files:
            boot.step_timer()
        boot.press(app, BACK)
        boot.stop(module, app)

        reboot = FakeStorage()
        reboot.dirs.update(boot.dirs)
        reboot.sd_files.update(boot.sd_files)
        reboot.sd_files[expected_path(session, corrupted)] = bytes(len(GAME_DATA))
        # Simula falta de energia entre guardar um arquivo anterior e concluir a troca.
        disk_session = (ROOT_BACKUP + session).lower()
        reboot.sd_files[disk_session + '/troca.prt'] = reboot.sd_files.pop(expected_path(session, changed))
        reboot.sd_files[disk_session + '/transfer.prt'] = b'dados temporarios'
        reboot.sd_files[disk_session + '/manifesto.txt'] += f'TROCA {changed}\r\n'.encode()
        reboot.source_paths.update({unchanged: b'original', changed: b'depois'})
        module, app = reboot.start()
        reboot.choose_resume(app)
        self.assertIn(ROOT_BACKUP + session, reboot.text)
        reboot.press(app, OK)
        self.assertIn('Lendo o manifesto no SD', reboot.text)
        reboot.drain_timers()
        self.assertIn('Backup concluído', reboot.text)
        self.assertIn('Reaproveitados do SD: 1', reboot.text)
        self.assertEqual(reboot.sd_files[expected_path(session, corrupted)], GAME_DATA)
        self.assertEqual(reboot.sd_files[expected_path(session, changed)], b'depois')
        for operation, path in reboot.mutations:
            self.assertFalse(operation.startswith('rename') and path.lower() == expected_path(session, unchanged))
        manifest = reboot.sd_files[(ROOT_BACKUP + session + '/MANIFESTO.TXT').lower()]
        self.assertIn(b'RETOMADA\r\n', manifest)
        self.assertTrue(manifest.endswith(b'COMPLETO\r\n'))
        self.assertFalse(any(path.endswith(('/transfer.prt', '/troca.prt')) for path in reboot.sd_files))
        reboot.stop(module, app)

    def test_relata_recursos_ausentes_e_arquivos_zerados_sem_excluir(self):
        brew = FakeStorage()
        system = 'fs:/mod/wms/imelody0.imy'
        save = GAME + '/udata/slot1/save.dat'
        zero = GAME + '/zeros.dat'
        brew.source_paths.update({system: b'apenas visivel na enumeracao', zero: bytes(30)})
        brew.dirs.add('fs:/mod/wms')
        brew.blocked_sources.update({system, save})
        module, app = brew.start()
        brew.choose_all(app)
        brew.drain_timers()
        manifest = brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()]
        self.assertIn(b'ERRO 257 ' + system.encode(), manifest)
        self.assertIn(b'NOTA Possivel recurso de sistema ausente: ' + system.encode(), manifest)
        self.assertIn(b'ERRO 257 ' + save.encode(), manifest)
        self.assertNotIn(b'NOTA Possivel recurso de sistema ausente: ' + save.encode(), manifest)
        self.assertIn(b'ZERO ' + zero.encode(), manifest)
        self.assertEqual(brew.sd_files[expected_path('B000001', zero)], bytes(30))
        self.assertIn('Só zeros: 1', brew.text)
        self.assertIn('Possíveis recursos de sistema: 1', brew.text)
        self.assertIn(save, brew.text)
        self.assertIn('Backup parcial', brew.text)
        brew.stop(module, app)

    def test_retomada_recusa_manifesto_com_bytes_binarios_sem_alterar_sd(self):
        brew = FakeStorage()
        manifest = ROOT_BACKUP + 'B000001/MANIFESTO.TXT'
        brew.dirs.update({ROOT_BACKUP.rstrip('/'), (ROOT_BACKUP + 'B000001')})
        brew.sd_files[manifest.lower()] = (
            b'Zeebo Backup\r\nESCOPO COMPLETO\r\nDIR fs:/mod\r\n'
            b'\x00\x02\x00dados binarios\r\nPARCIAL\r\n'
        )
        before = brew.sd_files.copy()
        module, app = brew.start()
        brew.choose_resume(app)
        self.assertIn(ROOT_BACKUP + 'B000001', brew.text)
        brew.press(app, OK)
        brew.drain_timers()
        self.assertIn('Manifesto ilegível no SD', brew.text)
        self.assertIn(manifest, brew.text)
        self.assertEqual(brew.sd_files, before)
        self.assertEqual(brew.mutations, [])
        brew.stop(module, app)

    def test_conferencia_zip_detecta_zeros_e_divergencia_de_mesmo_tamanho(self):
        with TemporaryDirectory() as directory:
            path = Path(directory) / 'teste.zip'
            with ZipFile(path, 'w') as archive:
                archive.writestr('B000001/MANIFESTO.TXT',
                                 f'OK {crc32(b"abc"):08x} 3 fs:/mod/game/zero.dat\nCOMPLETO\n')
                archive.writestr('B000001/mod/game/zero.dat', bytes(3))
            output = StringIO()
            with ZipFile(path) as archive, redirect_stdout(output):
                self.assertTrue(verify(archive, archive.getinfo('B000001/MANIFESTO.TXT')))
            self.assertIn('Preenchido com zeros (3 bytes)', output.getvalue())
            self.assertIn('CRC32 ou tamanho divergente', output.getvalue())

    def test_espaco_insuficiente_preserva_arquivos_ja_verificados(self):
        brew = FakeStorage(sd_capacity=len(GAME_DATA) + 320)
        extra = GAME + '/extra.dat'
        brew.source_paths[extra] = bytes(1024)
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        self.assertIn('Espaço insuficiente no cartão SD', brew.text)
        self.assertIn('1 arquivos verificados', brew.text)
        self.assertEqual(brew.sd_files[expected_path('B000001', GAME + '/double.mod')], GAME_DATA)
        self.assertNotIn(expected_path('B000001', extra), brew.sd_files)
        self.assertFalse(any(path.endswith('/transfer.prt') for path in brew.sd_files))
        manifest = brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()]
        self.assertIn(b'ERRO -8 ' + extra.encode(), manifest)
        self.assertTrue(manifest.endswith(b'PARCIAL\r\n'))
        brew.stop(module, app)

    def test_erro_de_cartao_cheio_quando_consulta_indisponivel(self):
        brew = FakeStorage(sd_capacity=None, fail_write=262)
        module, app = brew.start()
        for key in (DOWN, OK, OK):
            brew.press(app, key)
        self.assertIn('Espaço livre no SD indisponível', brew.text)
        for key in (RIGHT, OK):
            brew.press(app, key)
        brew.drain_timers()
        self.assertIn('Espaço insuficiente no cartão SD', brew.text)
        self.assertIn('Código BREW: 262', brew.text)
        self.assertFalse(any(path.endswith('/transfer.prt') for path in brew.sd_files))
        brew.stop(module, app)

    def test_pasta_preparada_funciona_quando_getinfo_falha(self):
        brew = FakeStorage(prepared_sd=True, hidden_zb_info=True)
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        self.assertIn('Backup concluído', brew.text)
        self.assertIn((SD_MARKER, 1), brew.opens)
        self.assertEqual(brew.sd_files[SD_MARKER.lower()], b'ZB04\r\n')
        self.assertNotIn(('mkdir', ROOT_BACKUP.rstrip('/')), brew.mutations)
        self.assertEqual(brew.sd_files[expected_path('B000001', GAME + '/double.mod')], GAME_DATA)
        brew.stop(module, app)

    def test_diagnostico_distingue_leitura_do_cartao_e_falha_ao_criar_sessao(self):
        brew = FakeStorage(prepared_sd=True, hidden_zb_info=True, deny_mkdir=True)
        module, app = brew.start()
        brew.choose_game(app)
        self.assertIn('fs:/card0/ZB/B000001', brew.text)
        brew.press(app, RIGHT)
        self.assertIn('Código da operação: 1', brew.text)
        self.assertIn('GetLastError: 13', brew.text)
        self.assertIn('GetInfo de ZB: 1', brew.text)
        self.assertIn('Leitura de ZB-CARD.TXT: 0', brew.text)
        self.assertEqual(brew.mutations, [])
        self.assertNotIn((GAME + '/double.mod', 1), brew.opens)
        brew.stop(module, app)

    def test_identifica_o_codigo_21_do_zeebo_mesmo_com_a_pasta_pronta(self):
        brew = FakeStorage(prepared_sd=True, sd_error=21)
        module, app = brew.start()
        brew.choose_game(app)
        brew.press(app, RIGHT)
        self.assertIn('Código da operação: 1', brew.text)
        self.assertIn('GetLastError: 21', brew.text)
        self.assertIn('Leitura de ZB-CARD.TXT: 21', brew.text)
        self.assertIn('BREW negou acesso ao SD (AEE_EPRIVLEVEL).', brew.text)
        self.assertEqual(brew.mutations, [])
        brew.stop(module, app)

    def test_mostra_operacao_invalida_quando_volume_responde(self):
        brew = FakeStorage(prepared_sd=True, sd_error=266)
        module, app = brew.start()
        brew.choose_game(app)
        brew.press(app, RIGHT)
        self.assertIn('GetLastError: 266', brew.text)
        self.assertIn('Leitura de ZB-CARD.TXT: 266', brew.text)
        self.assertIn('Consulta ao volume SD: 0', brew.text)
        self.assertIn('BREW recusou a operação no SD (266).', brew.text)
        self.assertEqual(brew.mutations, [])
        brew.stop(module, app)

    def test_raiz_do_sd_rejeitada_pelo_test_mas_pasta_funciona(self):
        brew = FakeStorage(root_test_fails=True)
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        self.assertIn('Backup concluído', brew.text)
        self.assertEqual(brew.sd_files[expected_path('B000001', GAME + '/double.mod')], GAME_DATA)
        brew.stop(module, app)

    def test_botao_dois_hid_volta_e_cancela_sem_repetir_ao_segurar(self):
        brew = FakeStorage(controller=True)
        module, app = brew.start()
        for key in (DOWN, OK, OK):
            brew.press(app, key)
        brew.pad_state = 1
        brew.fire_extra_timer(30)
        self.assertIn('1 de 3', brew.text)
        brew.fire_extra_timer(30)
        self.assertIn('1 de 3', brew.text)
        brew.pad_state = 0
        brew.fire_extra_timer(30)
        for key in (OK, RIGHT, OK):
            brew.press(app, key)
        brew.step_timer()
        brew.pad_state = 1
        brew.fire_extra_timer(30)
        self.assertIn('Backup cancelado', brew.text)
        brew.stop(module, app)

    def test_home_exige_tres_solturas_e_cancela_backup_antes_de_fechar(self):
        brew = FakeStorage(controller=True)
        module, app = brew.start()
        brew.choose_game(app)
        brew.step_timer()
        for _ in range(4):
            brew.press(app, HOME)
        self.assertEqual(brew.close_requests, [])
        brew.invoke_slot(app, 2, 0x102, HOME, 0)
        brew.fire_extra_timer(1600)
        for count in (1, 2):
            brew.invoke_slot(app, 2, 0x102, HOME, 0)
            self.assertIn(f'Home {count}/3. Repita para sair.', brew.text)
            self.assertEqual(brew.close_requests, [])
        brew.invoke_slot(app, 2, 0x102, HOME, 0)
        self.assertEqual(brew.close_requests, [0])
        self.assertIsNone(brew.timer)
        self.assertEqual(brew.extra_timers, {})
        self.assertTrue(brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()].endswith(b'CANCELADO\r\n'))
        brew.stop(module, app)

    def test_cancelamento_interrompe_e_limpa_temporario(self):
        brew = FakeStorage()
        module, app = brew.start()
        brew.choose_game(app)
        brew.step_timer()
        self.assertTrue(any(path.endswith('/transfer.prt') for path in brew.sd_files))
        brew.press(app, BACK)
        self.assertIn('Backup cancelado', brew.text)
        self.assertFalse(any(path.endswith('/transfer.prt') for path in brew.sd_files))
        self.assertTrue(brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()].endswith(b'CANCELADO\r\n'))
        brew.stop(module, app)

    def test_detecta_corrupcao_e_falha_de_escrita_sem_publicar_copia(self):
        for option in ('corrupt', 'fail_write'):
            with self.subTest(option=option):
                brew = FakeStorage(**{option: True})
                module, app = brew.start()
                brew.choose_game(app)
                brew.drain_timers()
                self.assertIn('Backup parcial', brew.text)
                self.assertNotIn(expected_path('B000001', GAME + '/double.mod'), brew.sd_files)
                self.assertFalse(any(path.endswith('/transfer.prt') for path in brew.sd_files))
                self.assertTrue(brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()].endswith(b'PARCIAL\r\n'))
                brew.stop(module, app)

    def test_colisao_de_pasta_no_sd_preserva_a_primeira_arvore(self):
        brew = FakeStorage()
        lower = GAME + '/assets'
        upper = GAME + '/ASSETS'
        brew.dirs.update((lower, upper))
        brew.source_paths[lower + '/a.bar'] = b'minusculo'
        brew.source_paths[upper + '/a.bar'] = b'maiusculo'
        module, app = brew.start()
        brew.choose_game(app)
        brew.drain_timers()
        self.assertIn('Backup parcial', brew.text)
        self.assertIn('Conflito de caminho no SD', brew.text)
        self.assertIn('Código do app: -4', brew.text)
        self.assertNotIn('Criar uma pasta de backup no SD', brew.text)
        self.assertEqual(brew.sd_files[expected_path('B000001', lower + '/a.bar')], b'maiusculo')
        manifest = brew.sd_files[(ROOT_BACKUP + 'B000001/MANIFESTO.TXT').lower()]
        self.assertIn(f'ERRO -4 {lower}'.encode(), manifest)
        self.assertIn(f'NOTA Destino em conflito no SD: {ROOT_BACKUP}B000001/{lower[4:]}'.encode(), manifest)
        self.assertNotIn((lower + '/a.bar', 1), brew.opens)
        brew.stop(module, app)


if __name__ == '__main__':
    if sys.argv[1:] == ['--preview']:
        preview()
    else:
        unittest.main(verbosity=2)
