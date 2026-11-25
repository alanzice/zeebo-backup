"""Verifica o executável Windows com as quatro árvores fictícias."""

import re
import subprocess
import unittest
from pathlib import Path


EXE = Path(__file__).resolve().parent / 'build' / 'inventario-pc.exe'


def run(*args):
    return subprocess.run([EXE, *args], check=True, capture_output=True, text=True).stdout


class InventarioTest(unittest.TestCase):
    def test_lista_raizes_e_uma_arvore_e_le_amostra(self):
        output = run()
        self.assertRegex(output, r'listar\s+fs:/mif/\s+fim \(codigo 0\), 1 entradas')
        self.assertRegex(output, r'listar\s+fs:/card0/\s+fim \(codigo 0\), 0 entradas')
        self.assertRegex(output, r'arquivo\s+fs:/mod/123456/udata/slot2/save.dat\s+ok')
        self.assertRegex(output, r'ler\s+fs:/mod/123456/udata/amostra.dat\s+ok \(codigo 0\), 14 bytes')
        self.assertRegex(output, r'arquivo\s+fs:/mod/123456/udata/slot2\s+entrada ignorada')
        self.assertNotIn('save ficticio\n', output)
        self.assertNotRegex(output, r'listar\s+fs:/mod/654321')
        self.assertEqual(len(re.findall(r'listar\s+fs:/mod/123456/udata\s+ok', output)), 1)

    def test_continua_apos_falhas_independentes(self):
        output = run('--restrito')
        self.assertRegex(output, r'fs:/mif/\s+acesso negado \(codigo 5\)')
        self.assertRegex(output, r'fs:/card0/\s+midia indisponivel \(codigo 21\)')
        self.assertRegex(output, r'fs:/mod/123456/assets\s+ausente \(codigo 3\)')
        self.assertRegex(output, r'fs:/mod/123456/udata/slot2\s+erro \(codigo 23\)')
        self.assertRegex(output, r'ler\s+fs:/mod/123456/udata/amostra.dat\s+ok')

    def test_limita_entradas_e_continua(self):
        output = run('--limite')
        self.assertEqual(len(re.findall(r'pasta\s+fs:/mod/123456\s+ok', output)), 128)
        self.assertRegex(output, r'fs:/mod/\s+limite')
        self.assertRegex(output, r'ler\s+fs:/mod/123456/udata/amostra.dat\s+ok')

    def test_cancelamento_encerra_o_cursor(self):
        output = run('--cancelar')
        self.assertIn('Inventario cancelado', output)
        self.assertNotIn('fs:/mif/', output)
        self.assertNotRegex(output, r'(?m)^ler\s')


if __name__ == '__main__':
    unittest.main(verbosity=2)
