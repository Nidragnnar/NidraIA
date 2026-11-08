# Gerador de Imagens

Gerador de imagens com IA que roda **no seu próprio computador**, de graça e sem internet (depois de baixar o modelo). Usa o motor aberto [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp) com **Vulkan**, que funciona em placas **AMD, NVIDIA e Intel**, e um programa próprio em **C++** com interface em português.

## Recursos

- Escreva o que quer ver e gere de 1 a 4 imagens por vez
- Prévia da imagem enquanto ela é gerada, com barra de progresso e botão de cancelar
- Formatos quadrado, retrato, paisagem e grande
- Semente (seed) salva em cada imagem: use **Reutilizar configurações** para repetir ou variar uma imagem
- Galeria com todas as imagens, salvas na pasta `galeria/` junto de um arquivo `.json` com o prompt e as configurações
- Visualizador com download e opção de apagar
- Log do motor na tela (clique no indicador de status no topo)

## Como usar

1. Dê dois cliques em **`GeradorImagens.exe`**.
2. O navegador abre em http://localhost:3333. Espere o indicador ficar verde (**Pronto**) e gere.

Para fechar, feche a janela preta. O motor fecha junto.

Não precisa instalar nada além do driver da placa de vídeo atualizado.

## Estrutura

```
GeradorImagens/
├── GeradorImagens.exe    # o programa (gerado pela compilação)
├── config.json           # portas, modelo e configurações padrão
├── src/gerador.cpp       # servidor: liga o motor, serve a interface e salva a galeria
├── public/               # interface (HTML, CSS e JavaScript)
├── third_party/          # cpp-httplib e nlohmann/json (só cabeçalhos)
├── CMakeLists.txt        # receita de compilação
├── compilar.bat          # compila com o Visual Studio
├── motor/                # stable-diffusion.cpp (não vai para o Git)
├── modelos/              # arquivos .safetensors (não vão para o Git)
└── galeria/              # imagens geradas (não vão para o Git)
```

## Compilar

Precisa do **Visual Studio 2026** com a carga de trabalho **Desenvolvimento para desktop com C++** (já inclui o CMake).

Dê dois cliques em `compilar.bat`, ou rode:

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

O `GeradorImagens.exe` é copiado para a raiz do projeto. A biblioteca do Visual C++ vai embutida no `.exe`, então ele roda em outros computadores sem instalar nada.

Para testes, `GeradorImagens.exe --sem-navegador` não abre o navegador sozinho.

## Instalar o motor e o modelo

As pastas `motor/` e `modelos/` não estão no repositório porque são grandes.

**Motor:** baixe o arquivo `...-bin-win-vulkan-x64.zip` em [releases do stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp/releases) e extraia o conteúdo dentro de `motor/`. Testado com a versão `master-1062-b43af40`.

**Modelo:** baixe [`sd_xl_turbo_1.0_fp16.safetensors`](https://huggingface.co/stabilityai/sdxl-turbo) (6,6 GB) e coloque em `modelos/`.

Para usar outro modelo, coloque o arquivo em `modelos/` e troque o nome em `config.json` (campo `modelo`), ajustando também os `padroes` (passos e CFG) recomendados para ele.

## Configurações do SDXL Turbo

| Configuração | Valor recomendado |
| --- | --- |
| Tamanho | 512 × 512 (o modelo foi treinado nesse tamanho) |
| Passos | 1 a 4 |
| CFG | 1 (desativado) |

O SDXL Turbo tem licença da Stability AI para **uso não comercial**.

## Desempenho medido (AMD Radeon RX 7600, 8 GB)

| Situação | Tempo |
| --- | --- |
| 1 imagem 512 × 512, 4 passos | cerca de 5,5 s |
| 2 imagens 512 × 512 (primeira geração depois de abrir) | cerca de 15 a 18 s |

O modelo ocupa cerca de 4,9 GB de memória de vídeo; o codificador de texto e o VAE ficam na memória RAM.

## Como funciona

1. O `GeradorImagens.exe` liga o `sd-server.exe` do stable-diffusion.cpp em segundo plano, só para este computador (`127.0.0.1`).
2. A interface envia o pedido para o `GeradorImagens.exe`, que repassa ao motor e consulta o andamento a cada 0,4 s.
3. Quando a imagem fica pronta, o programa salva o PNG e as configurações na pasta `galeria/` do dia.

Tudo acontece no seu computador: nenhum prompt ou imagem é enviado para a internet.

## Licenças de terceiros

- [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp): MIT
- [cpp-httplib](https://github.com/yhirose/cpp-httplib): MIT
- [nlohmann/json](https://github.com/nlohmann/json): MIT
- SDXL Turbo: licença da Stability AI (uso não comercial)
