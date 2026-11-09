# Desenha o ícone do NidraIA (mesmo desenho do public/icone.svg) em vários tamanhos e grava recursos/nidraia.ico
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

function DesenharIcone([int]$t) {
    $bmp = New-Object System.Drawing.Bitmap $t, $t
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.Clear([System.Drawing.Color]::Transparent)
    $e = $t / 32.0  # o SVG original tem 32 × 32

    # Fundo: quadrado arredondado com degradê laranja
    $raio = 8 * $e
    $caminho = New-Object System.Drawing.Drawing2D.GraphicsPath
    $caminho.AddArc(0, 0, 2 * $raio, 2 * $raio, 180, 90)
    $caminho.AddArc($t - 2 * $raio, 0, 2 * $raio, 2 * $raio, 270, 90)
    $caminho.AddArc($t - 2 * $raio, $t - 2 * $raio, 2 * $raio, 2 * $raio, 0, 90)
    $caminho.AddArc(0, $t - 2 * $raio, 2 * $raio, 2 * $raio, 90, 90)
    $caminho.CloseFigure()
    $degrade = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        (New-Object System.Drawing.PointF 0, 0), (New-Object System.Drawing.PointF $t, $t),
        [System.Drawing.Color]::FromArgb(255, 0xff, 0xb3, 0x47), [System.Drawing.Color]::FromArgb(255, 0xff, 0x5e, 0x62))
    $g.FillPath($degrade, $caminho)

    $branco = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    $brancoSuave = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb(230, 255, 255, 255))
    # Sol
    $g.FillEllipse($branco, 7.5 * $e, 7.5 * $e, 7 * $e, 7 * $e)
    # Montanhas
    $montanhas = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF (5 * $e), (25 * $e)), (New-Object System.Drawing.PointF (12 * $e), (17 * $e)),
        (New-Object System.Drawing.PointF (17 * $e), (22 * $e)), (New-Object System.Drawing.PointF (21 * $e), (18 * $e)),
        (New-Object System.Drawing.PointF (27 * $e), (25 * $e)))
    $g.FillPolygon($brancoSuave, $montanhas)
    # Brilho (estrela de 4 pontas)
    $estrela = [System.Drawing.PointF[]]@(
        (New-Object System.Drawing.PointF (24 * $e), (4 * $e)), (New-Object System.Drawing.PointF (25.2 * $e), (6.8 * $e)),
        (New-Object System.Drawing.PointF (28 * $e), (8 * $e)), (New-Object System.Drawing.PointF (25.2 * $e), (9.2 * $e)),
        (New-Object System.Drawing.PointF (24 * $e), (12 * $e)), (New-Object System.Drawing.PointF (22.8 * $e), (9.2 * $e)),
        (New-Object System.Drawing.PointF (20 * $e), (8 * $e)), (New-Object System.Drawing.PointF (22.8 * $e), (6.8 * $e)))
    $g.FillPolygon($branco, $estrela)
    $g.Dispose()
    return $bmp
}

# Arquivo .ico com cada tamanho guardado como PNG (formato aceito desde o Windows Vista)
$tamanhos = 16, 24, 32, 48, 64, 128, 256
$imagens = foreach ($t in $tamanhos) {
    $bmp = DesenharIcone $t
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

$saida = Join-Path $PSScriptRoot "nidraia.ico"
$arquivo = [System.IO.File]::Create($saida)
$w = New-Object System.IO.BinaryWriter $arquivo
$w.Write([uint16]0); $w.Write([uint16]1); $w.Write([uint16]$tamanhos.Count)
$deslocamento = 6 + 16 * $tamanhos.Count
for ($i = 0; $i -lt $tamanhos.Count; $i++) {
    $t = $tamanhos[$i]
    $w.Write([byte]($t % 256)); $w.Write([byte]($t % 256))  # 256 vira 0, como manda o formato
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([uint16]1); $w.Write([uint16]32)
    $w.Write([uint32]$imagens[$i].Length); $w.Write([uint32]$deslocamento)
    $deslocamento += $imagens[$i].Length
}
foreach ($img in $imagens) { $w.Write($img) }
$w.Close()
Write-Host "Ícone gravado em $saida"
