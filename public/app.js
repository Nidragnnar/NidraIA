// Interface do NidraIA: envia o pedido, mostra a prévia e cuida da galeria
const $ = (seletor) => document.querySelector(seletor);

const el = {
    estado: $("#estado"),
    estadoTexto: $("#estado-texto"),
    liberar: $("#liberar"),
    promptRotulo: $("#prompt-rotulo"),
    modos: $("#modos"),
    modoDica: $("#modo-dica"),
    forcaCampo: $("#forca-campo"),
    log: $("#log"),
    versao: $("#versao"),
    form: $("#form"),
    modelo: $("#modelo"),
    modeloDesc: $("#modelo-desc"),
    traduzir: $("#traduzir"),
    traduzirOpcao: $("#traduzir-opcao"),
    conferir: $("#conferir"),
    conferirOpcao: $("#conferir-opcao"),
    conferencia: $("#conferencia"),
    conferenciaFechar: $("#conferencia-fechar"),
    promptEn: $("#prompt-en"),
    negativoEn: $("#negativo-en"),
    negativoEnCampo: $("#negativo-en-campo"),
    estilos: $("#estilos"),
    arquivo: $("#arquivo"),
    envio: $("#envio"),
    envioTexto: $("#envio-texto"),
    envioPrevia: $("#envio-previa"),
    envioOpcoes: $("#envio-opcoes"),
    envioRemover: $("#envio-remover"),
    forca: $("#forca"),
    forcaValor: $("#forca-valor"),
    visAumentarGrupo: $("#vis-aumentar-grupo"),
    visAumentar: $("#vis-aumentar"),
    visUpscaler: $("#vis-upscaler"),
    atualTraducao: $("#atual-traducao"),
    visOriginal: $("#vis-original"),
    prompt: $("#prompt"),
    negativo: $("#negativo"),
    quantidade: $("#quantidade"),
    seed: $("#seed"),
    passos: $("#passos"),
    passosValor: $("#passos-valor"),
    cfg: $("#cfg"),
    cfgValor: $("#cfg-valor"),
    erroForm: $("#erro-form"),
    gerar: $("#gerar"),
    cancelar: $("#cancelar"),
    atual: $("#atual"),
    atualStatus: $("#atual-status"),
    atualImagens: $("#atual-imagens"),
    barra: $("#barra"),
    galeria: $("#galeria"),
    galeriaTotal: $("#galeria-total"),
    galeriaVazia: $("#galeria-vazia"),
    vis: $("#visualizador"),
    visImagem: $("#vis-imagem"),
    visPrompt: $("#vis-prompt"),
    visDados: $("#vis-dados"),
    visBaixar: $("#vis-baixar"),
    toast: $("#toast"),
};

let motorPronto = false;
let trocandoModelo = false;
let modelos = [];
let modeloAtivo = null;
let trabalhoAtual = null;
let galeria = [];
let abertoNoVisualizador = null;

// ========== Utilidades ==========
async function api(caminho, opcoes = {}) {
    const resposta = await fetch(caminho, {
        ...opcoes,
        headers: opcoes.body ? { "Content-Type": "application/json" } : undefined,
    });
    const dados = resposta.status === 204 ? null : await resposta.json().catch(() => null);
    if (!resposta.ok) throw new Error(dados?.erro || `Erro ${resposta.status}`);
    return dados;
}

let temporizadorToast;
function avisar(texto) {
    el.toast.textContent = texto;
    el.toast.classList.add("visivel");
    clearTimeout(temporizadorToast);
    temporizadorToast = setTimeout(() => el.toast.classList.remove("visivel"), 2800);
}

const esperar = (ms) => new Promise((r) => setTimeout(r, ms));

function formatoEscolhido() {
    const [largura, altura] = el.form.formato.value.split("x").map(Number);
    return { largura, altura };
}

// ========== Estado do motor ==========
const TEXTOS_ESTADO = {
    carregando: "Carregando o modelo...",
    pronto: "Pronto",
    erro: "Erro no motor (clique para ver)",
    parado: "Motor parado",
    dormindo: "Placa liberada",
};

// Consulta o estado do motor o tempo todo: rápido enquanto carrega, devagar depois
// (assim a tela nota quando a placa de vídeo é liberada sozinha por falta de uso)
let rodadaMotor = 0;
async function acompanharMotor() {
    const rodada = ++rodadaMotor;  // uma chamada nova substitui a anterior
    for (;;) {
        let carregando = true;
        try {
            const estado = await api("/api/estado");
            if (rodada !== rodadaMotor) return;
            el.versao.textContent = `v${estado.versao}`;
            el.traduzirOpcao.hidden = !estado.tradutor;
            atualizarRotuloGerar();
            if (!upscalers.length) montarUpscalers(estado.upscalers || []);
            if (!el.estilos.children.length) montarEstilos(estado.estilos || []);
            if (estado.modelo !== modeloAtivo || !modelos.length) {
                modelos = estado.modelos;
                modeloAtivo = estado.modelo;
                montarSeletorModelos();
                aplicarPadroes(modeloAtual());
                atualizarModo();
            }
            const nome = modeloAtual()?.nome || "";
            el.estado.dataset.estado = estado.estado;
            el.estadoTexto.textContent =
                estado.estado === "pronto"
                    ? `Pronto · ${nome}`
                    : estado.estado === "dormindo"
                      ? `Placa liberada · ${nome}`
                      : estado.estado === "carregando"
                        ? `Carregando ${nome}...`
                        : TEXTOS_ESTADO[estado.estado] || estado.estado;
            el.estado.title = estado.estado === "dormindo" ? "O modelo foi desligado para liberar a placa de vídeo. Ele religa sozinho quando você gerar." : "";
            const log = estado.log.join("\n");
            if (el.log.textContent !== log) el.log.textContent = log;
            // "dormindo" ou "erro": o próximo pedido tenta religar o motor, então dá para gerar
            motorPronto = ["pronto", "dormindo", "erro"].includes(estado.estado);
            if (estado.estado !== "carregando") trocandoModelo = false;
            atualizarBotoes();
            if (estado.estado === "erro" && carregandoAntes) el.log.hidden = false;
            carregando = estado.estado === "carregando";
            carregandoAntes = carregando;
        } catch {
            if (rodada !== rodadaMotor) return;
            el.estado.dataset.estado = "erro";
            el.estadoTexto.textContent = "Servidor desligado";
        }
        await esperar(carregando ? 1500 : 5000);
        if (rodada !== rodadaMotor) return;
    }
}
let carregandoAntes = true;  // o log só abre sozinho quando o erro aparece ao carregar

const modeloAtual = () => modelos.find((m) => m.id === modeloAtivo);

function montarSeletorModelos() {
    el.modelo.replaceChildren(
        ...modelos.map((m) => {
            const opcao = document.createElement("option");
            opcao.value = m.id;
            opcao.textContent = m.disponivel ? m.nome : `${m.nome} (não baixado)`;
            opcao.disabled = !m.disponivel;
            return opcao;
        })
    );
    el.modelo.value = modeloAtivo;
    const m = modeloAtual();
    el.modeloDesc.textContent = m ? `${m.descricao} Licença: ${m.licenca}.` : "";
}

function aplicarPadroes(modelo) {
    if (!modelo) return;
    const p = modelo.padroes;
    el.passos.max = modelo.maxPassos;
    el.passos.value = p.passos;
    el.cfg.value = p.cfg;
    const formato = el.form.querySelector(`input[name=formato][value="${p.largura}x${p.altura}"]`);
    if (formato) formato.checked = true;
    mostrarValores();
}

el.modelo.addEventListener("change", async () => {
    const escolhido = el.modelo.value;
    if (escolhido === modeloAtivo) return;
    trocandoModelo = true;
    motorPronto = false;
    atualizarBotoes();
    try {
        await api("/api/modelo", { method: "POST", body: JSON.stringify({ id: escolhido }) });
        avisar("Trocando de modelo. Isso leva alguns segundos.");
    } catch (erro) {
        avisar(erro.message);
        el.modelo.value = modeloAtivo;
    }
    acompanharMotor();
});

function mostrarValores() {
    el.passosValor.textContent = el.passos.value;
    el.cfgValor.textContent = Number(el.cfg.value).toFixed(1);
}

function atualizarBotoes() {
    el.gerar.disabled = !motorPronto || Boolean(trabalhoAtual);
    el.cancelar.hidden = !trabalhoAtual;
    // Não deixa trocar de modelo no meio de uma geração
    el.modelo.disabled = Boolean(trabalhoAtual) || trocandoModelo;
    el.liberar.hidden = el.estado.dataset.estado !== "pronto" || Boolean(trabalhoAtual);
}

// Desliga o modelo para devolver a memória da placa de vídeo; ele religa no próximo "Gerar"
el.liberar.addEventListener("click", async () => {
    try {
        await api("/api/liberar", { method: "POST" });
        avisar("Placa de vídeo liberada. O modelo religa sozinho quando você gerar.");
    } catch (erro) {
        avisar(erro.message);
    }
    acompanharMotor();
});

el.estado.addEventListener("click", () => {
    el.log.hidden = !el.log.hidden;
    el.estado.setAttribute("aria-expanded", String(!el.log.hidden));
});

// ========== Estilos prontos ==========
const CHAVE_ESTILO = "nidraia.estilo";

function lerPreferencia(chave) {
    try {
        return localStorage.getItem(chave);
    } catch {
        return null;
    }
}

function montarEstilos(lista) {
    const salvo = lerPreferencia(CHAVE_ESTILO) || "nenhum";
    el.estilos.replaceChildren(
        ...lista.map((estilo) => {
            const rotulo = document.createElement("label");
            const radio = document.createElement("input");
            radio.type = "radio";
            radio.name = "estilo";
            radio.value = estilo.id;
            radio.checked = estilo.id === salvo;
            radio.addEventListener("change", () => {
                try {
                    localStorage.setItem(CHAVE_ESTILO, estilo.id);
                } catch {
                    // sem armazenamento: só não lembra a escolha
                }
            });
            const nome = document.createElement("span");
            nome.textContent = estilo.nome;
            rotulo.append(radio, nome);
            return rotulo;
        })
    );
    if (!el.form.querySelector("input[name=estilo]:checked")) el.form.querySelector("input[name=estilo]")?.click();
}

const estiloEscolhido = () => el.form.querySelector("input[name=estilo]:checked")?.value || "nenhum";

// ========== Partir de uma imagem sua ==========
// A imagem é recortada e redimensionada no navegador para o formato escolhido (sem distorcer)
let imagemOriginal = null;

function prepararImagemBase() {
    if (!imagemOriginal) return null;
    const { largura, altura } = formatoEscolhido();
    const canvas = document.createElement("canvas");
    canvas.width = largura;
    canvas.height = altura;
    const escala = Math.max(largura / imagemOriginal.width, altura / imagemOriginal.height);
    const w = imagemOriginal.width * escala;
    const h = imagemOriginal.height * escala;
    canvas.getContext("2d").drawImage(imagemOriginal, (largura - w) / 2, (altura - h) / 2, w, h);
    return canvas.toDataURL("image/png");
}

// Editar com instrução: só com imagem enviada e num modelo de edição (FLUX.2 klein)
const modoEditar = () => Boolean(imagemOriginal && modeloAtual()?.edicao && el.form.modo.value === "editar");
const PROMPT_EXEMPLO = el.prompt.placeholder;

// Ajusta a tela ao modo: no "editar" some a força e o campo do prompt vira o campo da instrução
function atualizarModo() {
    const podeEditar = Boolean(modeloAtual()?.edicao);
    const editar = modoEditar();
    el.modos.hidden = !podeEditar;
    el.forcaCampo.hidden = editar;
    el.modoDica.textContent = editar
        ? 'A imagem é mantida e a IA muda só o que você pedir. Escreva a instrução lá em cima, ex.: "deixe o céu à noite" ou "coloque um chapéu de mago no gato".'
        : podeEditar
          ? ""
          : 'Dica: com o modelo FLUX.2 klein dá para editar a imagem com instruções, ex.: "deixe o céu à noite".';
    el.modoDica.hidden = !el.modoDica.textContent;
    el.promptRotulo.textContent = editar ? "O que mudar na imagem?" : "O que você quer ver?";
    el.prompt.placeholder = editar ? "Ex.: deixe o céu à noite, com estrelas" : PROMPT_EXEMPLO;
}

// Passa a usar esta imagem como ponto de partida
function usarImagem(img, url, nome) {
    imagemOriginal = img;
    el.envioPrevia.src = url;
    el.envioPrevia.hidden = false;
    el.envioTexto.textContent = nome;
    el.envioOpcoes.hidden = false;
    atualizarModo();
}

function carregarArquivo(arquivo) {
    if (!arquivo || !/^image\/(png|jpeg|webp)$/.test(arquivo.type)) {
        avisar("Escolha uma imagem PNG, JPG ou WebP.");
        return;
    }
    const url = URL.createObjectURL(arquivo);
    const img = new Image();
    img.onload = () => usarImagem(img, url, arquivo.name);
    img.onerror = () => avisar("Não consegui abrir essa imagem.");
    img.src = url;
}

function removerImagem() {
    imagemOriginal = null;
    el.arquivo.value = "";
    el.envioPrevia.hidden = true;
    el.envioPrevia.removeAttribute("src");
    el.envioTexto.textContent = "Clique ou arraste uma foto ou desenho aqui";
    el.envioOpcoes.hidden = true;
    atualizarModo();
}
for (const radio of el.form.querySelectorAll("input[name=modo]")) radio.addEventListener("change", atualizarModo);

el.arquivo.addEventListener("change", () => carregarArquivo(el.arquivo.files[0]));
el.envioRemover.addEventListener("click", removerImagem);
el.forca.addEventListener("input", () => (el.forcaValor.textContent = `${Math.round(el.forca.value * 100)}%`));
el.forcaValor.textContent = `${Math.round(el.forca.value * 100)}%`;
for (const tipo of ["dragenter", "dragover"]) {
    el.envio.addEventListener(tipo, (evento) => {
        evento.preventDefault();
        el.envio.classList.add("arrastando");
    });
}
for (const tipo of ["dragleave", "drop"]) el.envio.addEventListener(tipo, () => el.envio.classList.remove("arrastando"));
el.envio.addEventListener("drop", (evento) => {
    evento.preventDefault();
    carregarArquivo(evento.dataTransfer.files[0]);
});

// ========== Tradução para conferir ==========
const traduzirAtivo = () => !el.traduzirOpcao.hidden && el.traduzir.checked;
const precisaConferir = () => traduzirAtivo() && el.conferir.checked && el.conferencia.hidden;

function atualizarRotuloGerar() {
    el.conferirOpcao.hidden = !traduzirAtivo();
    el.gerar.firstChild.textContent = precisaConferir() ? "Traduzir e conferir " : "Gerar ";
}

function fecharConferencia() {
    el.conferencia.hidden = true;
    atualizarRotuloGerar();
}

// Traduz o prompt (e o "evitar") e mostra em caixas editáveis; a geração só vem no próximo clique
async function traduzirParaConferir() {
    if (!el.prompt.value.trim()) {
        mostrarErro("Escreva o que você quer ver na imagem.");
        return;
    }
    trabalhoAtual = "traduzindo";
    atualizarBotoes();
    el.gerar.firstChild.textContent = "Traduzindo... ";
    try {
        const traduzirTexto = async (texto) =>
            texto.trim() ? (await api("/api/traduzir", { method: "POST", body: JSON.stringify({ texto }) })).traducao : "";
        el.promptEn.value = await traduzirTexto(el.prompt.value);
        el.negativoEn.value = await traduzirTexto(el.negativo.value);
        el.negativoEnCampo.hidden = !el.negativoEn.value;
        el.conferencia.hidden = false;
        el.promptEn.focus();
    } catch (erro) {
        mostrarErro(erro.message);
    } finally {
        trabalhoAtual = null;
        atualizarBotoes();
        atualizarRotuloGerar();
    }
}

// Mudou o texto em português: a tradução mostrada ficou velha
for (const campo of [el.prompt, el.negativo]) campo.addEventListener("input", () => el.conferencia.hidden || fecharConferencia());
el.traduzir.addEventListener("change", fecharConferencia);
el.conferir.addEventListener("change", fecharConferencia);
el.conferenciaFechar.addEventListener("click", () => {
    fecharConferencia();
    el.prompt.focus();
});

// ========== Gerar ==========
async function gerar() {
    el.erroForm.hidden = true;
    if (precisaConferir()) return traduzirParaConferir();

    const seedTexto = el.seed.value.trim();
    if (seedTexto && !/^\d+$/.test(seedTexto)) {
        mostrarErro("A semente deve ser um número inteiro, ou fique vazia para ser aleatória.");
        return;
    }
    const pedido = {
        prompt: el.prompt.value,
        negativo: el.negativo.value,
        ...formatoEscolhido(),
        quantidade: Number(el.quantidade.value),
        seed: seedTexto ? Number(seedTexto) : -1,
        passos: Number(el.passos.value),
        cfg: Number(el.cfg.value),
        traduzir: traduzirAtivo(),
        estilo: estiloEscolhido(),
    };
    const imagemBase = prepararImagemBase();
    if (imagemBase) Object.assign(pedido, { imagemBase, forca: Number(el.forca.value), modo: modoEditar() ? "editar" : "redesenhar" });
    // Tradução já conferida: manda o inglês (talvez corrigido) e guarda o português junto
    if (traduzirAtivo() && !el.conferencia.hidden) {
        Object.assign(pedido, {
            promptOriginal: el.prompt.value,
            prompt: el.promptEn.value,
            negativo: el.negativoEn.value || el.negativo.value,
            traduzir: false,
        });
    }
    await enviar("/api/gerar", pedido, pedido);
}

// Envia um pedido de geração (normal ou de variações) e acompanha até o fim
async function enviar(caminho, corpo, formato) {
    // Já trava o botão: a tradução pode levar alguns segundos antes de a geração começar
    trabalhoAtual = "enviando";
    atualizarBotoes();
    iniciarAtual(formato);
    if (corpo.traduzir) el.atualStatus.textContent = "Traduzindo o prompt...";
    // A placa estava liberada: o pedido religa o modelo antes de gerar
    const dormia = el.estado.dataset.estado !== "pronto";
    if (dormia) el.atualStatus.textContent = "Ligando o modelo de novo...";
    try {
        const resposta = await api(caminho, { method: "POST", body: JSON.stringify(corpo) });
        if (dormia) acompanharMotor();
        trabalhoAtual = resposta.id;
        el.atualTraducao.hidden = !resposta.traduzido;
        el.atualTraducao.textContent = resposta.traduzido ? `Em inglês: ${resposta.prompt}` : "";
        await acompanhar(resposta.id, formato);
    } catch (erro) {
        mostrarErro(erro.message);
        el.atual.hidden = true;
    } finally {
        trabalhoAtual = null;
        atualizarBotoes();
    }
}

function mostrarErro(texto) {
    el.erroForm.textContent = texto;
    el.erroForm.hidden = false;
}

function iniciarAtual(pedido) {
    el.atual.hidden = false;
    el.atualTraducao.hidden = true;
    el.atualStatus.textContent = "Na fila...";
    el.barra.style.width = "0%";
    el.atualImagens.replaceChildren(
        ...Array.from({ length: pedido.quantidade }, () => {
            const caixa = document.createElement("div");
            caixa.className = "carregando";
            caixa.style.aspectRatio = `${pedido.largura} / ${pedido.altura}`;
            return caixa;
        })
    );
}

async function acompanhar(id, pedido) {
    const inicio = performance.now();
    for (;;) {
        await esperar(400);
        const t = await api(`/api/trabalhos/${id}`);
        const segundos = ((performance.now() - inicio) / 1000).toFixed(1);

        if (t.status === "queued") {
            // "aviso": faltou memória e o NidraIA está recarregando o modelo para tentar de novo
            el.atualStatus.textContent = t.aviso || (t.fila > 0 ? `Na fila (posição ${t.fila})...` : "Preparando...");
            el.barra.style.width = "0%";
        } else if (t.status === "generating") {
            el.atualStatus.textContent = t.totalPassos ? `Gerando: passo ${t.passo} de ${t.totalPassos} · ${segundos}s` : `Gerando... ${segundos}s`;
            if (t.totalPassos) el.barra.style.width = `${(t.passo / t.totalPassos) * 100}%`;
            if (t.previa) {
                const primeira = el.atualImagens.firstElementChild;
                primeira.style.backgroundImage = `url(data:image/png;base64,${t.previa})`;
                primeira.classList.add("com-previa");
            }
        } else if (t.status === "completed") {
            el.barra.style.width = "100%";
            const tempo = t.imagens?.[0]?.segundos;
            el.atualStatus.textContent = `Pronto em ${tempo ? tempo.toFixed(1) : segundos}s`;
            mostrarResultado(t.imagens || []);
            galeria = [...(t.imagens || []), ...galeria];
            desenharGaleria();
            return;
        } else if (t.status === "cancelled") {
            el.atualStatus.textContent = "Cancelado.";
            el.atualImagens.replaceChildren();
            return;
        } else {
            throw new Error(t.erro || "A geração falhou.");
        }
    }
}

function mostrarResultado(imagens) {
    el.atualImagens.replaceChildren(...imagens.map((item) => criarMiniatura(item)));
}

el.form.addEventListener("submit", (evento) => {
    evento.preventDefault();
    if (!el.gerar.disabled) gerar();
});

document.addEventListener("keydown", (evento) => {
    if (evento.key === "Enter" && (evento.ctrlKey || evento.metaKey) && !el.gerar.disabled) {
        evento.preventDefault();
        gerar();
    }
});

el.cancelar.addEventListener("click", async () => {
    if (!trabalhoAtual || trabalhoAtual === "enviando") return;
    el.atualStatus.textContent = "Cancelando...";
    try {
        await api(`/api/trabalhos/${trabalhoAtual}/cancelar`, { method: "POST" });
    } catch (erro) {
        avisar(erro.message);
    }
});

el.passos.addEventListener("input", mostrarValores);
el.cfg.addEventListener("input", mostrarValores);

// ========== Galeria ==========
function criarMiniatura(item) {
    const botao = document.createElement("button");
    botao.type = "button";
    botao.className = "miniatura";
    botao.title = item.prompt;
    botao.style.aspectRatio = `${item.largura} / ${item.altura}`;
    const img = document.createElement("img");
    img.src = `/galeria/${item.arquivo}`;
    img.alt = item.prompt;
    img.loading = "lazy";
    botao.append(img);
    botao.addEventListener("click", () => abrir(item));
    return botao;
}

function desenharGaleria() {
    el.galeria.replaceChildren(...galeria.map(criarMiniatura));
    el.galeriaVazia.hidden = galeria.length > 0;
    el.galeriaTotal.textContent = galeria.length ? `(${galeria.length})` : "";
}

async function carregarGaleria() {
    try {
        galeria = await api("/api/galeria");
        desenharGaleria();
    } catch (erro) {
        avisar(erro.message);
    }
}

// ========== Visualizador ==========
function abrir(item) {
    abertoNoVisualizador = item;
    el.visImagem.src = `/galeria/${item.arquivo}`;
    el.visImagem.alt = item.prompt;
    el.visPrompt.textContent = item.prompt;
    el.visOriginal.hidden = !item.promptOriginal;
    el.visOriginal.textContent = item.promptOriginal ? `Você escreveu: ${item.promptOriginal}` : "";
    el.visBaixar.href = `/galeria/${item.arquivo}`;
    // Até 4096 px por lado depois de aumentar (mais que isso fica lento e pesado demais)
    el.visAumentarGrupo.hidden = !upscalers.length || item.largura * 4 > 4096 || item.altura * 4 > 4096;
    el.visBaixar.download = `${item.id}.png`;

    const dados = [
        ["Tamanho", `${item.largura} × ${item.altura}`],
        ["Semente", item.seed ?? "aleatória"],
        ["Passos", item.passos],
        ["CFG", item.cfg],
        ["Tempo", `${item.segundos.toFixed(1)} s`],
        ["Modelo", item.modelo.replace(/\.safetensors$/, "")],
        ...(item.estiloNome ? [["Estilo", item.estiloNome]] : []),
        ...(item.editadaPorInstrucao ? [["Editada", "a partir de outra imagem, com instrução"]] : []),
        ...(item.partiuDeImagem ? [["Partiu de", `uma imagem sua (a IA pôde mudar ${Math.round(item.forca * 100)}%)`]] : []),
        ...(item.variacaoDe ? [["Variação", `de outra imagem (força ${item.forca})`]] : []),
        ...(item.aumentadaDe ? [["Aumentada", `4× com ${upscalers.find((u) => u.id === item.upscaler)?.nome || item.upscaler}`]] : []),
        ["Criada em", new Date(item.criadoEm).toLocaleString("pt-BR")],
    ];
    if (item.negativo) dados.splice(1, 0, ["Evitar", item.negativo]);
    el.visDados.replaceChildren(
        ...dados.flatMap(([rotulo, valor]) => {
            const dt = document.createElement("dt");
            const dd = document.createElement("dd");
            dt.textContent = rotulo;
            dd.textContent = valor;
            return [dt, dd];
        })
    );
    el.vis.showModal();
}

$("#vis-fechar").addEventListener("click", () => el.vis.close());
el.vis.addEventListener("click", (evento) => {
    if (evento.target === el.vis) el.vis.close();
});

$("#vis-reusar").addEventListener("click", () => {
    const item = abertoNoVisualizador;
    // Usa o prompt em inglês que gerou a imagem (sem os termos do estilo) e desliga a tradução, para repetir exatamente
    el.prompt.value = item.promptBase || item.prompt;
    const radioEstilo = el.form.querySelector(`input[name=estilo][value="${item.estilo || "nenhum"}"]`);
    if (radioEstilo) radioEstilo.checked = true;
    el.negativo.value = item.negativo || "";
    el.traduzir.checked = false;
    fecharConferencia();
    const formato = el.form.querySelector(`input[name=formato][value="${item.largura}x${item.altura}"]`);
    if (formato) formato.checked = true;
    el.quantidade.value = "1";
    el.seed.value = item.seed ?? "";
    el.passos.value = item.passos;
    el.cfg.value = item.cfg;
    mostrarValores();
    el.vis.close();
    el.prompt.focus();
    const outroModelo = item.modelo !== modeloAtual()?.nome;
    avisar(outroModelo ? `Configurações carregadas. A imagem foi feita com ${item.modelo}: troque o modelo para repetir igual.` : "Configurações carregadas. Mesma semente = mesma imagem.");
});

// Editar com instrução: leva a imagem da galeria para "Partir de uma imagem sua" já no modo de edição
$("#vis-editar").addEventListener("click", () => {
    const item = abertoNoVisualizador;
    if (!modeloAtual()?.edicao) {
        avisar("Para editar com instruções, troque o modelo para o FLUX.2 klein.");
        return;
    }
    const url = `/galeria/${item.arquivo}`;
    const img = new Image();
    img.onload = () => {
        el.form.modo.value = "editar";
        const formato = el.form.querySelector(`input[name=formato][value="${item.largura}x${item.altura}"]`);
        if (formato) formato.checked = true;
        usarImagem(img, url, "Imagem da galeria");
        el.prompt.value = "";
        fecharConferencia();
        el.vis.close();
        el.prompt.focus();
        avisar("Escreva o que mudar na imagem e clique em Gerar.");
    };
    img.onerror = () => avisar("Não consegui abrir essa imagem.");
    img.src = url;
});

// ========== Aumentar resolução 4× ==========
let upscalers = [];

function montarUpscalers(lista) {
    upscalers = lista;
    el.visUpscaler.replaceChildren(
        ...lista.map((u) => {
            const opcao = document.createElement("option");
            opcao.value = u.id;
            opcao.textContent = u.nome;
            return opcao;
        })
    );
}

el.visAumentar.addEventListener("click", async () => {
    const item = abertoNoVisualizador;
    if (trabalhoAtual) {
        avisar("Espere a geração atual terminar.");
        return;
    }
    el.visAumentar.disabled = true;
    el.visAumentar.textContent = "Aumentando...";
    try {
        const nova = await api("/api/aumentar", {
            method: "POST",
            body: JSON.stringify({ id: item.id, upscaler: el.visUpscaler.value }),
        });
        galeria = [nova, ...galeria];
        desenharGaleria();
        abrir(nova);
        avisar(`Pronto: ${nova.largura} × ${nova.altura} em ${nova.segundos.toFixed(1)} s. A original continua na galeria.`);
    } catch (erro) {
        avisar(erro.message);
    } finally {
        el.visAumentar.disabled = false;
        el.visAumentar.textContent = "Aumentar 4×";
    }
});

// Variações: 4 imagens parecidas, partindo desta imagem (imagem para imagem) com sementes novas
$("#vis-variacoes").addEventListener("click", () => {
    const item = abertoNoVisualizador;
    if (!motorPronto || trabalhoAtual) {
        avisar("Espere o motor ficar pronto ou a geração atual terminar.");
        return;
    }
    el.vis.close();
    el.erroForm.hidden = true;
    enviar("/api/variacoes", { id: item.id, quantidade: 4 }, { quantidade: 4, largura: item.largura, altura: item.altura });
    el.atual.scrollIntoView({ behavior: "smooth", block: "start" });
});

$("#vis-apagar").addEventListener("click", async () => {
    const item = abertoNoVisualizador;
    if (!confirm("Apagar esta imagem? Ela será removida da pasta galeria.")) return;
    try {
        await api(`/api/galeria/${item.id}`, { method: "DELETE" });
        galeria = galeria.filter((i) => i.id !== item.id);
        desenharGaleria();
        el.vis.close();
        avisar("Imagem apagada.");
    } catch (erro) {
        avisar(erro.message);
    }
});

// ========== Início ==========
carregarGaleria();
acompanharMotor();
