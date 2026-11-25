"""Roda o applet ARM próprio no Unicorn, com apenas as interfaces BREW de teste."""

import os
from pathlib import Path
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / '.ferramentas' / 'emulacao'))

from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_PROT_READ, UC_PROT_EXEC
from unicorn.arm_const import (
    UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
    UC_ARM_REG_PC, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_CPU_ARM_1136,
)

PACKAGE = ROOT / 'build' / 'brew'
MODULE = PACKAGE / 'mod' / 'inventario' / 'inventario.mod'
MIF = PACKAGE / 'mif' / 'inventario.mif'
GAME_ID = os.environ.get('ZEEBO_TEST_GAME_ID', '274754')
GAME = 'fs:/mod/' + GAME_ID
SAMPLE = 'fs:/mif/' + GAME_ID + '.mif'
SAMPLE_BYTES = b'dados de teste'

API = 0x600000
DATA = 0x700000
HEAP = 0x800000
HEAP_SIZE = 0x40000
STACK = 0x980000
REGS = (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)


def class_id(path=MIF, title='Inventario Zeebo'):
    data = path.read_bytes()
    magic, version, _, items, index, index_size, offsets, count, start, total = (
        struct.unpack_from('<4H6I', data))
    assert (magic, version) == (0x11, 1) and items == count
    assert index == 32 and index_size == 8 * items and offsets == index + index_size
    positions = struct.unpack_from(f'<{count + 1}I', data, offsets)
    assert positions[0] == start and positions[-1] == len(data) == start + total
    resources = {}
    for kind, identifier, resource in struct.iter_unpack('<HHI', data[index:offsets]):
        section = resource >> 16
        assert resource & 0xffff == 0
        resources[kind, identifier] = data[positions[section]:positions[section + 1]]
    assert resources[1, 20].decode('utf-16') == title
    applet = resources[0x5000, 2]
    assert len(applet) == 20 and struct.unpack_from('<I', applet, 8)[0] == 20
    return struct.unpack_from('<I', applet)[0]


class FakeBrew:
    instruction_limit = 200000

    def __init__(self, base, restricted=False, module_path=MODULE):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_1136)
        for addr, length in ((base - 0x1000, 0x10000), (API, 0x1000), (DATA, 0x1000),
                             (HEAP, HEAP_SIZE), (STACK, 0x10000)):
            self.uc.mem_map(addr, length)
        image = module_path.read_bytes()
        self.uc.mem_write(base, image)
        self.methods = {}
        self.uc.hook_add(UC_HOOK_CODE, self.dispatch, begin=API, end=API + 0xfff)
        self.base = base
        self.restricted = restricted
        self.allocations = set()
        self.next_heap = HEAP
        self.timer = None
        self.cursor = iter(())
        self.file_error = 0
        self.opened = []
        self.probed = []
        self.released = []
        self.text = []
        self.frame = []

        self.shell = DATA + 0x100
        self.files = DATA + 0x200
        self.display = DATA + 0x300
        self.file = DATA + 0x400
        self.object(self.shell, {0: lambda: 2, 1: lambda: self.release(self.shell),
                                 2: self.create, 11: self.set_timer, 12: self.cancel_timer})
        self.object(self.files, {1: lambda: self.release(self.files), 2: self.open,
                                 9: lambda: self.file_error, 10: self.enum_init, 11: self.enum_next})
        self.object(self.file, {1: lambda: self.release(self.file), 3: self.read})
        self.object(self.display, {1: lambda: self.release(self.display), 4: self.draw_text,
                                   5: self.clear, 7: self.update})
        helpers = DATA + 0x500
        self.store(helpers + 26 * 4, self.stub(self.allocate))
        self.store(helpers + 27 * 4, self.stub(self.free))
        self.store(base - 4, helpers)
        self.uc.mem_protect(base, 0x1000 * ((len(image) + 4095) // 4096),
                            UC_PROT_READ | UC_PROT_EXEC)

    def reg(self, i):
        return self.uc.reg_read(REGS[i])

    def store(self, address, value):
        self.uc.mem_write(address, struct.pack('<I', value))

    def word(self, address):
        return struct.unpack('<I', self.uc.mem_read(address, 4))[0]

    def c_string(self, address):
        data = self.uc.mem_read(address, 128)
        return data.split(b'\0', 1)[0].decode('ascii')

    def stub(self, method):
        address = API + 4 * len(self.methods)
        self.methods[address] = method
        self.store(address, 0xE12FFF1E)  # BX LR
        return address

    def object(self, address, slots):
        self.store(address, address + 4)
        for position, method in slots.items():
            self.store(address + 4 + position * 4, self.stub(method))

    def dispatch(self, uc, address, size, userdata):
        del size, userdata
        uc.reg_write(UC_ARM_REG_R0, self.methods[address]() & 0xffffffff)

    def invoke(self, address, *args):
        stop = API + 0xffc
        for i, register in enumerate(REGS):
            self.uc.reg_write(register, args[i] if i < len(args) else 0)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0xff00)
        self.uc.reg_write(UC_ARM_REG_LR, stop)
        self.uc.emu_start(address, stop, count=self.instruction_limit)
        if self.uc.reg_read(UC_ARM_REG_PC) != stop:
            raise AssertionError('chamada ARM não retornou no orçamento de instruções')
        return self.reg(0)

    def invoke_slot(self, obj, slot, *args):
        return self.invoke(self.word(self.word(obj) + 4 * slot), obj, *args)

    def allocate(self):
        address = self.next_heap
        self.next_heap += (self.reg(0) + 7) & ~7
        assert self.next_heap < HEAP + HEAP_SIZE
        self.allocations.add(address)
        return address

    def free(self):
        self.allocations.remove(self.reg(0))
        return 0

    def create(self):
        objects = {0x01001001: self.display, 0x01001003: self.files}
        self.store(self.reg(2), objects[self.reg(1)])
        return 0

    def release(self, address):
        self.released.append(address)
        return 0

    def set_timer(self):
        assert self.timer is None and self.reg(1) == 10
        self.timer = (self.reg(2), self.reg(3))
        return 0

    def cancel_timer(self):
        assert self.timer == (self.reg(1), self.reg(2))
        self.timer = None
        return 0

    def drain_timers(self):
        for _ in range(64):
            if not self.timer:
                return
            callback, context = self.timer
            self.timer = None
            self.invoke(callback, context)
        raise AssertionError('app não encerrou a enumeração')

    def enum_init(self):
        path, dirs = self.c_string(self.reg(1)), self.reg(2)
        self.probed.append((path, dirs))
        if self.restricted and path == 'fs:/card0/':
            return 20
        entries = {
            'fs:/mod/': [(GAME, 2, 0)],
            'fs:/mif/': [(SAMPLE, 0, len(SAMPLE_BYTES))],
            'fs:/card0/': [],
            GAME: [('udata', 2, 0), ('assets', 2, 0)],
            GAME + '/udata': [('slot1', 2, 0)],
            GAME + '/udata/slot1': [(GAME + '/udata/slot1/save.dat', 0, 30)],
            GAME + '/assets': [],
        }
        self.cursor = iter((name, attribute, size) for name, attribute, size in entries[path]
                           if bool(attribute & 2) == bool(dirs))
        self.file_error = 0
        return 0

    def enum_next(self):
        entry = next(self.cursor, None)
        if entry is None:
            self.file_error = 1
            return 0
        name, attribute, size = entry
        self.uc.mem_write(self.reg(1), struct.pack('<B3xII64s', attribute, 0, size, name.encode()))
        return 1

    def open(self):
        path, mode = self.c_string(self.reg(1)), self.reg(2)
        assert mode == 1 and path == SAMPLE
        self.opened.append((path, mode))
        if self.restricted:
            self.file_error = 5
            return 0
        return self.file

    def read(self):
        assert self.reg(0) == self.file and self.reg(2) == 256
        self.uc.mem_write(self.reg(1), SAMPLE_BYTES)
        return len(SAMPLE_BYTES)

    def draw_text(self):
        data = self.uc.mem_read(self.reg(2), self.reg(3) * 2)
        self.frame.append(data.decode('utf-16le'))
        return 0

    def clear(self):
        self.frame = []
        return 0

    def update(self):
        self.text = list(self.frame)
        return 0

    def start(self):
        assert self.invoke(self.base, self.shell, 0, DATA) == 0
        module = self.word(DATA)
        assert self.invoke_slot(module, 2, self.shell, class_id(), DATA + 4) == 0
        app = self.word(DATA + 4)
        assert self.invoke_slot(app, 2, 0, 0, 0) == 1
        return module, app

    def stop(self, module, app):
        assert self.invoke_slot(app, 2, 1, 0, 0) == 1
        assert self.invoke_slot(app, 1) == 0
        assert self.invoke_slot(module, 1) == 0
        assert not self.allocations and self.timer is None
        assert self.display in self.released and self.files in self.released


class BrewTest(unittest.TestCase):
    def test_roda_em_duas_bases_e_le_so_o_mif_ficticio(self):
        for base in (0x100000, 0x350000):
            with self.subTest(base=hex(base)):
                brew = FakeBrew(base)
                module, app = brew.start()
                brew.drain_timers()
                self.assertIn((GAME + '/udata/slot1', 0), brew.probed)
                self.assertIn((GAME + '/udata/slot1', 1), brew.probed)
                self.assertEqual(brew.opened, [(SAMPLE, 1)])
                self.assertIn(f'LEITURA 0x00000000 14 bytes {SAMPLE}', brew.text)
                self.assertIn(brew.file, brew.released)
                self.assertFalse(any(SAMPLE_BYTES.decode() in text for text in brew.text))
                brew.stop(module, app)

    def test_preserva_retornos_do_brew_e_continua(self):
        brew = FakeBrew(0x100000, restricted=True)
        module, app = brew.start()
        brew.drain_timers()
        self.assertIn('ERRO 0x00000014 fs:/card0/', brew.text)
        self.assertIn(f'ERRO 0x00000005 {SAMPLE}', brew.text)
        self.assertIn((GAME + '/udata', 1), brew.probed)
        brew.stop(module, app)

    def test_suspende_retoma_e_cancela_timer_no_stop(self):
        brew = FakeBrew(0x100000)
        module, app = brew.start()
        self.assertIsNotNone(brew.timer)
        self.assertEqual(brew.invoke_slot(app, 2, 2, 0, 0), 1)
        self.assertIsNone(brew.timer)
        self.assertEqual(brew.invoke_slot(app, 2, 3, 0, 0), 1)
        self.assertIsNotNone(brew.timer)
        brew.stop(module, app)
        self.assertEqual(brew.opened, [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
