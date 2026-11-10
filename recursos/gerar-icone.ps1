# Gera o ícone do NidraIA a partir de recursos/icone-original.png (cabeça de dragão feita pelo próprio NidraIA).
# Saídas: public/icone.png (página) e recursos/nidraia.ico (programa), com cantos arredondados.
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$original = [System.Drawing.Image]::FromFile((Join-Path $PSScriptRoot "icone-original.png"))

# Desenha a imagem num quadrado de lado $t com cantos arredondados (fora deles fica transparente)
function Desenhar([int]$t) {
    $bmp = New-Object System.Drawing.Bitmap $t, $t
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = "AntiAlias"
    $g.InterpolationMode = "HighQualityBicubic"
    $g.PixelOffsetMode = "HighQuality"
    $g.Clear([System.Drawing.Color]::Transparent)
    $raio = [math]::Max(2, $t * 0.2)
    $caminho = New-Object System.Drawing.Drawing2D.GraphicsPath
    $caminho.AddArc(0, 0, 2 * $raio, 2 * $raio, 180, 90)
    $caminho.AddArc($t - 2 * $raio, 0, 2 * $raio, 2 * $raio, 270, 90)
    $caminho.AddArc($t - 2 * $raio, $t - 2 * $raio, 2 * $raio, 2 * $raio, 0, 90)
    $caminho.AddArc(0, $t - 2 * $raio, 2 * $raio, 2 * $raio, 90, 90)
    $caminho.CloseFigure()
    $pincel = New-Object System.Drawing.TextureBrush $original
    $pincel.ScaleTransform($t / $original.Width, $t / $original.Height)
    $g.FillPath($pincel, $caminho)
    $pincel.Dispose()
    $g.Dispose()
    return $bmp
}

function EmPng($bmp) {
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    return , $ms.ToArray()
}

# Ícone da página
$pagina = Desenhar 256
$pagina.Save((Join-Path (Split-Path $PSScriptRoot) "public\icone.png"), [System.Drawing.Imaging.ImageFormat]::Png)
$pagina.Dispose()

# Arquivo .ico com cada tamanho guardado como PNG (formato aceito desde o Windows Vista)
$tamanhos = 16, 24, 32, 48, 64, 128, 256
$imagens = foreach ($t in $tamanhos) {
    $bmp = Desenhar $t
    EmPng $bmp
    $bmp.Dispose()
}
$original.Dispose()

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
Write-Host "Ícone gravado em public\icone.png e $saida"
