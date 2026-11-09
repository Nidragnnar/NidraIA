# Baixa o motor e os modelos do NidraIA e confere cada arquivo pelo SHA-256.
# Pode rodar de novo quantas vezes quiser: o que já está certo é pulado.
$ErrorActionPreference = "Stop"
$raiz = Split-Path -Parent $PSScriptRoot
Set-Location $raiz

$componentes = @(
    @{ grupo = "motor"; nome = "Motor stable-diffusion.cpp (Vulkan)"; mb = 29
       url = "https://github.com/leejet/stable-diffusion.cpp/releases/download/master-1062-b43af40/sd-master-b43af40-bin-win-vulkan-x64.zip"
       destino = "motor\motor.zip"; sha = "4ecc7e5f0b9b5ba9ad0ec6d6a10ed9c6693d0838b3123824fc5db7a9b06a0171"; extrair = "motor" },
    @{ grupo = "sdxl"; nome = "Modelo SDXL Turbo"; mb = 6617
       url = "https://huggingface.co/stabilityai/sdxl-turbo/resolve/main/sd_xl_turbo_1.0_fp16.safetensors"
       destino = "modelos\sd_xl_turbo_1.0_fp16.safetensors"; bytes = 6938081905; sha = "e869ac7d6942cb327d68d5ed83a40447aadf20e0c3358d98b2cc9e270db0da26" },
    @{ grupo = "upscale"; nome = "Aumentar resolução (fotos)"; mb = 64
       url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.1.0/RealESRGAN_x4plus.pth"
       destino = "modelos\upscale\RealESRGAN_x4plus.pth"; sha = "4fa0d38905f75ac06eb49a7951b426670021be3018265fd191d2125df9d682f1" },
    @{ grupo = "upscale"; nome = "Aumentar resolução (anime)"; mb = 17
       url = "https://github.com/xinntao/Real-ESRGAN/releases/download/v0.2.2.4/RealESRGAN_x4plus_anime_6B.pth"
       destino = "modelos\upscale\RealESRGAN_x4plus_anime_6B.pth"; sha = "f872d837d3c90ed2e05227bed711af5671a6fd1c9f7d7e91c911a61f155e99da" },
    @{ grupo = "flux"; nome = "FLUX.1 schnell (modelo)"; mb = 6566
       url = "https://huggingface.co/leejet/FLUX.1-schnell-gguf/resolve/main/flux1-schnell-q4_0.gguf"
       destino = "modelos\flux-schnell\flux1-schnell-q4_0.gguf"; bytes = 6884606880; sha = "4f30741d2bfc786c92934ce925fcb0a43df3441e76504b797c3d5d5f0878fa6f" },
    @{ grupo = "flux"; nome = "FLUX.1 schnell (texto T5)"; mb = 2762
       url = "https://huggingface.co/city96/t5-v1_1-xxl-encoder-gguf/resolve/main/t5-v1_1-xxl-encoder-Q4_K_M.gguf"
       destino = "modelos\flux-schnell\t5-v1_1-xxl-encoder-Q4_K_M.gguf"; bytes = 2896123072; sha = "6be2b0b7e2de7cf2919340c88cb802a103a997ce46c53131cec91958c1db1af4" },
    @{ grupo = "flux"; nome = "FLUX.1 schnell (texto CLIP)"; mb = 235
       url = "https://huggingface.co/comfyanonymous/flux_text_encoders/resolve/main/clip_l.safetensors"
       destino = "modelos\flux-schnell\clip_l.safetensors"; bytes = 246144152; sha = "660c6f5b1abae9dc498ac2d21e1347d2abdb0cf6c0c0c8576cd796491d9a6cdd" },
    @{ grupo = "flux"; nome = "FLUX.1 schnell (VAE)"; mb = 320
       url = "https://huggingface.co/ffxvs/vae-flux/resolve/main/ae.safetensors"
       destino = "modelos\flux-schnell\ae.safetensors"; bytes = 335304388; sha = "afc8e28272cd15db3919bacdb6918ce9c1ed22e96cb12c4d5ed0fba823529e38" },
    @{ grupo = "tradutor"; nome = "Tradutor llama.cpp"; mb = 36
       url = "https://github.com/ggml-org/llama.cpp/releases/download/b12208/llama-b12208-bin-win-vulkan-x64.zip"
       destino = "tradutor\llama.zip"; sha = "377f3daa288624535b992a0b88ef93b1689c3445547b6666c411561586b6c477"; extrair = "tradutor" },
    @{ grupo = "tradutor"; nome = "Tradutor Qwen3 4B"; mb = 2382
       url = "https://huggingface.co/unsloth/Qwen3-4B-Instruct-2507-GGUF/resolve/main/Qwen3-4B-Instruct-2507-Q4_K_M.gguf"
       destino = "tradutor\Qwen3-4B-Instruct-2507-Q4_K_M.gguf"; bytes = 2497281120; sha = "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597" }
)

function Escrever($texto, $cor = "Gray") { Write-Host $texto -ForegroundColor $cor }

function Pronto($c) {
    # Zips já extraídos contam como prontos se o programa principal existir
    if ($c.extrair -eq "motor") { return Test-Path "motor\sd-server.exe" }
    if ($c.extrair -eq "tradutor") { return Test-Path "tradutor\llama-server.exe" }
    if (-not (Test-Path $c.destino)) { return $false }
    return (Get-FileHash $c.destino -Algorithm SHA256).Hash.ToLower() -eq $c.sha
}

# Arquivos grandes: baixa 8 partes ao mesmo tempo (bem mais rápido quando o servidor limita cada conexão).
# As partes ficam em <destino>.partes\ e, se a internet cair, as que já terminaram são aproveitadas.
function BaixarEmPartes($c) {
    $bytes = [int64]$c.bytes
    $partes = 8
    $tamanho = [math]::Ceiling($bytes / $partes)
    $pasta = "$($c.destino).partes"
    New-Item -ItemType Directory -Force $pasta | Out-Null

    $processos = @()
    for ($i = 0; $i -lt $partes; $i++) {
        $inicio = [int64]$i * $tamanho
        $fim = [math]::Min($inicio + $tamanho, $bytes) - 1
        $arquivo = Join-Path $pasta "parte$i"
        if ((Test-Path $arquivo) -and (Get-Item $arquivo).Length -eq ($fim - $inicio + 1)) { continue }
        $processos += Start-Process curl.exe -NoNewWindow -PassThru -ArgumentList @(
            "-sL", "--retry", "8", "--retry-delay", "3", "-r", "$inicio-$fim", "-o", "`"$arquivo`"", "`"$($c.url)`"")
    }

    # Mostra o progresso somando o tamanho das partes
    while ($processos | Where-Object { -not $_.HasExited }) {
        $baixado = (Get-ChildItem $pasta -File | Measure-Object Length -Sum).Sum
        $pct = [math]::Floor(100 * $baixado / $bytes)
        Write-Progress -Activity "Baixando $($c.nome)" -Status "$pct% ($([math]::Round($baixado / 1MB)) de $([math]::Round($bytes / 1MB)) MB)" -PercentComplete $pct
        Start-Sleep -Milliseconds 700
    }
    Write-Progress -Activity "Baixando $($c.nome)" -Completed

    # Confere se todas as partes vieram inteiras e junta tudo num arquivo só
    $saida = [System.IO.File]::Create((Join-Path (Get-Location) $c.destino))
    try {
        for ($i = 0; $i -lt $partes; $i++) {
            $inicio = [int64]$i * $tamanho
            $esperado = [math]::Min($inicio + $tamanho, $bytes) - $inicio
            $arquivo = Join-Path $pasta "parte$i"
            if (-not (Test-Path $arquivo) -or (Get-Item $arquivo).Length -ne $esperado) {
                throw "O download de $($c.nome) ficou incompleto. Rode o instalador de novo para continuar."
            }
            $entrada = [System.IO.File]::OpenRead((Join-Path (Get-Location) $arquivo))
            try { $entrada.CopyTo($saida) } finally { $entrada.Dispose() }
        }
    } finally {
        $saida.Dispose()
    }
    Remove-Item -Recurse -Force $pasta
}

function Baixar($c) {
    New-Item -ItemType Directory -Force (Split-Path $c.destino) | Out-Null
    Escrever "Baixando $($c.nome) ($([math]::Round($c.mb / 1024, 1)) GB)..." "Cyan"
    # curl.exe já vem no Windows 10 e 11; -C - continua de onde parou se a internet cair
    if ($c.mb -ge 200) {
        BaixarEmPartes $c
    } else {
        & curl.exe -L --retry 5 --retry-delay 3 -C - --progress-bar -o $c.destino $c.url
        if ($LASTEXITCODE -ne 0) { throw "O download de $($c.nome) falhou. Rode o instalador de novo para continuar." }
    }
    Escrever "Conferindo o arquivo..."
    $hash = (Get-FileHash $c.destino -Algorithm SHA256).Hash.ToLower()
    if ($hash -ne $c.sha) {
        Remove-Item $c.destino -Force
        throw "O arquivo $($c.nome) veio corrompido e foi apagado. Rode o instalador de novo."
    }
    if ($c.extrair) {
        Expand-Archive $c.destino -DestinationPath $c.extrair -Force
        Remove-Item $c.destino -Force
    }
    Escrever "OK: $($c.nome)" "Green"
}

Clear-Host
Escrever "=====================================" "Yellow"
Escrever "   Instalador de componentes do NidraIA" "Yellow"
Escrever "=====================================" "Yellow"
Escrever ""
Escrever "O motor, o SDXL Turbo e o aumento de resolucao sao obrigatorios (cerca de 6,7 GB)."
Escrever ""
$flux = (Read-Host "Baixar tambem o FLUX.1 schnell? Mais qualidade, mais lento (cerca de 9,9 GB) [s/N]") -match "^[sS]"
$tradutor = (Read-Host "Baixar o tradutor de prompt para ingles? (cerca de 2,4 GB) [S/n]") -notmatch "^[nN]"

$escolhidos = $componentes | Where-Object {
    $_.grupo -in @("motor", "sdxl", "upscale") -or ($flux -and $_.grupo -eq "flux") -or ($tradutor -and $_.grupo -eq "tradutor")
}

try {
    foreach ($c in $escolhidos) {
        if (Pronto $c) { Escrever "Ja instalado: $($c.nome)" "Green"; continue }
        Baixar $c
    }
    Escrever ""
    Escrever "Tudo pronto! Abra o NidraIA.exe." "Green"
} catch {
    Escrever ""
    Escrever $_.Exception.Message "Red"
}
Escrever ""
Read-Host "Aperte Enter para fechar"
