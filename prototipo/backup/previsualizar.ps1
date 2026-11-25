[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
& python -B (Join-Path $project 'testar_backup.py') --preview
if ($LASTEXITCODE -ne 0) { throw 'Falha ao capturar a interface emulada.' }

Add-Type -AssemblyName System.Drawing
$build = Join-Path $project 'build'
$frames = Get-Content -Raw -LiteralPath (Join-Path $build 'backup-preview.json') | ConvertFrom-Json -AsHashtable
$out = Join-Path $build 'preview'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$normal = [Drawing.Font]::new('Segoe UI', 15, [Drawing.FontStyle]::Regular, [Drawing.GraphicsUnit]::Pixel)
$bold = [Drawing.Font]::new('Segoe UI', 21, [Drawing.FontStyle]::Bold, [Drawing.GraphicsUnit]::Pixel)

try {
    foreach ($screen in $frames.Keys) {
        $bitmap = [Drawing.Bitmap]::new(640, 480)
        $canvas = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $canvas.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
            foreach ($command in $frames[$screen]) {
                $color = [Drawing.ColorTranslator]::FromHtml($command.color)
                $brush = [Drawing.SolidBrush]::new($color)
                try {
                    if ($command.kind -eq 'rect') {
                        $x, $y, $width, $height = $command.bounds
                        $canvas.FillRectangle($brush, [int]$x, [int]$y, [int]$width, [int]$height)
                    } else {
                        $font = if ($command.bold) { $bold } else { $normal }
                        $canvas.DrawString($command.value, $font, $brush, [single]$command.x, [single]$command.y)
                    }
                } finally {
                    $brush.Dispose()
                }
            }
            $path = Join-Path $out "$screen.png"
            $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
            Write-Output $path
        } finally {
            $canvas.Dispose()
            $bitmap.Dispose()
        }
    }
} finally {
    $normal.Dispose()
    $bold.Dispose()
}
