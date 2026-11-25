[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]{6}$')]
    [string]$GameId = '274754',
    [switch]$TestarAcessoSd
)

$ErrorActionPreference = 'Stop'
$toolRoot = Join-Path $PSScriptRoot '.ferramentas'
$zig = Join-Path $toolRoot 'zig-x86_64-windows-0.15.2/zig.exe'
$output = Join-Path $PSScriptRoot 'build'

if (-not (Test-Path -LiteralPath $zig -PathType Leaf)) {
    throw 'Zig portátil ausente. Consulte README.md.'
}

New-Item -ItemType Directory -Path $output -Force | Out-Null
$previousGlobalCache = $env:ZIG_GLOBAL_CACHE_DIR
$previousLocalCache = $env:ZIG_LOCAL_CACHE_DIR

try {
    $env:ZIG_GLOBAL_CACHE_DIR = Join-Path $toolRoot 'cache-global'
    $env:ZIG_LOCAL_CACHE_DIR = Join-Path $output 'cache-local'
    $inventory = Join-Path $PSScriptRoot 'inventario.c'
    $pc = Join-Path $PSScriptRoot 'pc.c'
    $pcOutput = Join-Path $output 'inventario-pc.exe'
    $armOutput = Join-Path $output 'inventario-arm.o'
    $common = @('-std=c99', '-Wall', '-Wextra', '-Werror', '-O2')

    & $zig cc @common $inventory $pc -o $pcOutput
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar para Windows.' }

    $brew = Join-Path $PSScriptRoot 'brew'
    $classId = '0x01f05a42'
    $brewElf = Join-Path $output 'inventario-brew.elf'
    $applet = Join-Path $output 'applet-brew.o'
    $entry = Join-Path $output 'entrada-brew.o'
    $target = @('-target', 'arm-freestanding-eabi', '-mcpu=arm1136jf_s', '-marm',
        '-ffreestanding', '-fropi', '-fvisibility=hidden', '-fno-unwind-tables',
        '-fno-asynchronous-unwind-tables')
    & $zig cc @common @target -c $inventory -o $armOutput
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar o objeto ARM.' }

    & $zig cc @common @target "-DINVENTORY_CLASS=$classId" "-DGAME_ID=$GameId" `
        -c (Join-Path $brew 'applet.c') -o $applet
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar o applet BREW.' }

    & $zig cc -target arm-freestanding-eabi -mcpu=arm1136jf_s -marm `
        -c (Join-Path $brew 'entrada.S') -o $entry
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao compilar a entrada ARM do applet.' }

    & $zig ld.lld --emit-relocs -T (Join-Path $brew 'modulo.ld') $entry $applet $armOutput -o $brewElf
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o applet BREW.' }

    & python (Join-Path $brew 'empacotar.py') $brewElf (Join-Path $output 'brew') $classId
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao empacotar o applet BREW.' }

    $backup = Join-Path $PSScriptRoot 'backup'
    $backupClassId = '0x01f05a43'
    $backupObjects = @($entry)
    $backupElf = Join-Path $output 'zeebobackup.elf'
    foreach ($baseName in @('applet', 'backup', 'texto', 'tela')) {
        $object = Join-Path $output "zb-$baseName.o"
        & $zig cc @common @target -fno-jump-tables "-DINVENTORY_CLASS=$backupClassId" `
            -c (Join-Path $backup "$baseName.c") -o $object
        if ($LASTEXITCODE -ne 0) { throw "Falha ao compilar $baseName para o backup." }
        $backupObjects += $object
    }
    & $zig ld.lld --emit-relocs -T (Join-Path $brew 'modulo.ld') @backupObjects -o $backupElf
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao ligar o applet de backup.' }

    & python (Join-Path $brew 'empacotar.py') $backupElf (Join-Path $output 'backup') $backupClassId `
        --name zeeback --title 'Zeebo Backup' --homebrew-package --sd-marker --sd-access `
        --zip (Join-Path $output 'zeebo-backup-v0.12.zip')
    if ($LASTEXITCODE -ne 0) { throw 'Falha ao empacotar o applet de backup.' }

    if ($TestarAcessoSd) {
        & python (Join-Path $brew 'empacotar.py') $backupElf (Join-Path $output 'backup-system') $backupClassId `
            --name zeeback --title 'Zeebo Backup' --homebrew-package --sd-marker --sd-access --system-privilege `
            --zip (Join-Path $output 'zeebo-backup-v0.12-teste-sd.zip')
        if ($LASTEXITCODE -ne 0) { throw 'Falha ao empacotar o teste de acesso ao SD.' }
    }

    Write-Output $pcOutput, $armOutput
}
finally {
    $env:ZIG_GLOBAL_CACHE_DIR = $previousGlobalCache
    $env:ZIG_LOCAL_CACHE_DIR = $previousLocalCache
}
