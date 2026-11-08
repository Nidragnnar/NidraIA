# Monta o pacote da Release em dist\NidraIA-v<versão>.zip (sem motor nem modelos)
$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

$versao = (Select-String -Path src\nidraia.cpp -Pattern 'VERSAO = "([^"]+)"').Matches[0].Groups[1].Value
if (-not (Test-Path NidraIA.exe)) { throw "Compile antes: rode compilar.bat" }

$pasta = "dist\NidraIA"
Remove-Item -Recurse -Force dist -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $pasta, "$pasta\instalador", "$pasta\motor", "$pasta\modelos", "$pasta\tradutor" | Out-Null

Copy-Item NidraIA.exe, config.json, "Instalar componentes.bat" $pasta
Copy-Item -Recurse public $pasta
Copy-Item instalador\instalar.ps1 "$pasta\instalador"
Copy-Item instalador\LEIA-ME.txt $pasta

$zip = "dist\NidraIA-v$versao.zip"
Compress-Archive -Path $pasta -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
Write-Host "Pacote: $zip ($([math]::Round((Get-Item $zip).Length / 1MB, 1)) MB)"
Write-Host "SHA-256: $hash"
