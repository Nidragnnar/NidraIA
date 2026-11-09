# NidraIA

Gerador de imagens com IA que roda **no seu próprio computador**, de graça e sem internet (depois de baixar os modelos). Usa o motor aberto [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) com **Vulkan**, que funciona em placas **AMD, NVIDIA e Intel**, e um programa próprio em **C++** com interface em português.

## Recursos

- Dois modelos, com troca pela própria interface:
  - **SDXL Turbo**: o mais rápido (cerca de 5 s por imagem)
  - **FLUX.1 schnell**: mais qualidade e entende melhor o prompt, mais lento
- **Tradução automática do prompt para inglês**, feita no próprio PC pelo [llama.cpp](https://github.com/ggml-org/llama.cpp) (os modelos entendem inglês muito melhor). Por padrão a tradução aparece numa caixa para você **conferir e corrigir** antes de gerar
- **Estilos prontos**: Foto realista, Cinema, Anime, Aquarela, Pintura a óleo, Pixel art, Animação 3D e Fantasia sombria. Eles acrescentam termos em inglês ao prompt (lista editável no `config.json`)
- **Partir de uma imagem sua**: envie uma foto ou desenho e escolha o quanto a IA pode mudar
- **Aumentar resolução 4×** com [Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN) (512 → 2048 em poucos segundos), com um modelo para fotos e outro para anime e ilustração
- **Gerar 4 variações** de uma imagem da galeria: mesma cena, detalhes diferentes (imagem para imagem com força 0,45)
- 1 a 4 imagens por vez, com prévia, barra de progresso e botão de cancelar
- Formatos quadrado, retrato, paisagem e grande
- Semente (seed) salva em cada imagem: **Reutilizar configurações** repete ou varia uma imagem
- Galeria salva na pasta `galeria/`, com um `.json` ao lado de cada imagem (prompt original, prompt em inglês e configurações)
- Log do motor e do tradutor na tela (clique no indicador de status no topo)

## Para usar (sem programar)

Baixe o `.zip` da [última Release](https://github.com/Nidragnnar/NidraIA/releases), extraia e siga o `LEIA-ME.txt`:

1. Dois cliques em **`Instalar componentes.bat`**: confere se há espaço no disco, baixa o motor e os modelos conferindo cada arquivo pelo SHA-256 e mostra se a placa de vídeo foi encontrada.
2. Dois cliques em **`NidraIA.exe`**: o navegador abre em http://localhost:3333.

## Estrutura

```
NidraIA/
├── NidraIA.exe                # o programa (gerado pela compilação)
├── config.json                # portas, modelos e configurações padrão
├── Instalar componentes.bat   # instalador dos componentes
├── instalador/                # script do instalador e LEIA-ME para a Release
├── src/nidraia.cpp            # servidor: liga motor e tradutor, serve a interface e salva a galeria
├── public/                    # interface (HTML, CSS e JavaScript)
├── third_party/               # cpp-httplib e nlohmann/json (só cabeçalhos)
├── CMakeLists.txt             # receita de compilação
├── compilar.bat               # compila com o Visual Studio
├── empacotar.ps1              # monta o .zip da Release em dist/
├── motor/                     # stable-diffusion.cpp (não vai para o Git)
├── modelos/                   # modelos de imagem (não vão para o Git)
├── tradutor/                  # llama.cpp + modelo de tradução (não vão para o Git)
└── galeria/                   # imagens geradas (não vão para o Git)
```

## Compilar

Precisa do **Visual Studio 2026** com a carga de trabalho **Desenvolvimento para desktop com C++** (já inclui o CMake).

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

Ou dois cliques em `compilar.bat`. O `NidraIA.exe` é copiado para a raiz do projeto, com a biblioteca do Visual C++ embutida, o ícone e a versão.

A **versão** fica só no `CMakeLists.txt` (`project(NidraIA VERSION ...)`): ela vai para o programa, para as Propriedades do `.exe` e para o nome do zip da Release. O ícone é gerado por `recursos/gerar-icone.ps1`.

Para testes, `NidraIA.exe --sem-navegador` não abre o navegador sozinho.

Para montar a Release: `powershell -ExecutionPolicy Bypass -File empacotar.ps1` (gera `dist/NidraIA-v<versão>.zip`).

## Componentes

O `instalador/instalar.ps1` tem a lista completa, com links e hashes. Resumo:

| Componente | Tamanho | Licença |
| --- | --- | --- |
| stable-diffusion.cpp `master-1062-b43af40` (Vulkan) | 29 MB | MIT |
| SDXL Turbo fp16 | 6,6 GB | Stability AI, **uso não comercial** |
| FLUX.1 schnell Q4_0 + T5 Q4_K_M + CLIP-L + VAE | 9,9 GB | Apache 2.0 |
| Real-ESRGAN x4plus + x4plus anime 6B | 81 MB | BSD-3 |
| llama.cpp `b12208` (Vulkan) | 36 MB | MIT |
| Qwen3-4B-Instruct-2507 Q4_K_M (tradutor) | 2,4 GB | Apache 2.0 |

Para adicionar outro modelo, coloque os arquivos em `modelos/` e acrescente uma entrada em `modelos` no `config.json`. Cada chave de `arquivos` vira uma opção do motor (`model` → `-m`, `vae` → `--vae`, `diffusion-model` → `--diffusion-model`...).

## Desempenho medido (AMD Radeon RX 7600, 8 GB, Ryzen 5 5600G, 16 GB de RAM)

| Situação | Tempo |
| --- | --- |
| SDXL Turbo, 1 imagem 512 × 512, 4 passos | cerca de 5,5 s |
| SDXL Turbo, 1 imagem 1024 × 1024 | cerca de 11 s |
| SDXL Turbo, 4 imagens 512 × 512 | cerca de 21 s |
| FLUX.1 schnell, 1 imagem 512 × 512, 4 passos | 35 a 42 s |
| FLUX.1 schnell, 1 imagem 1024 × 1024 | cerca de 61 s |
| Tradução do prompt (Qwen3 4B, no processador) | 1 a 2,5 s (a primeira, que liga o tradutor, 3 a 4 s) |
| SDXL Turbo, 4 variações de uma imagem 512 × 512 | 72 a 78 s com um jogo aberto usando 3,7 GB da placa (sem o jogo não medido) |
| Aumentar 4× (512 → 2048), modelo de fotos | cerca de 7 s |
| Aumentar 4× (512 → 2048), modelo de anime | cerca de 3 s |
| Trocar de modelo | o motor reinicia em cerca de 1 s; o primeiro uso carrega o modelo |

O tradutor ocupa cerca de 2,5 GB de RAM e se desliga sozinho depois de 5 minutos sem uso (`desligarAposMinutos` no `config.json`).

## Como funciona

1. O `NidraIA.exe` liga o `sd-server.exe` (motor) com o modelo escolhido, só para este computador (`127.0.0.1`). Trocar de modelo reinicia o motor com os arquivos do outro modelo.
2. Com a tradução ligada, o prompt passa antes pelo `llama-server.exe`, que roda no processador para não disputar a placa de vídeo com o motor.
3. A interface acompanha a geração e, no fim, o programa salva o PNG e as configurações na pasta `galeria/` do dia.
4. Se o `NidraIA.exe` fechar, o Windows encerra o motor e o tradutor junto.

Nenhum prompt ou imagem é enviado para a internet.
