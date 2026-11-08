// Interface do Gerador de Imagens: envia o pedido, mostra a prévia e cuida da galeria
const $ = (seletor) => document.querySelector(seletor);

const el = {
    estado: $("#estado"),
    estadoTexto: $("#estado-texto"),
    log: $("#log"),
    form: $("#form"),
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
};

async function acompanharMotor() {
    let primeiraVez = true;
    for (;;) {
        try {
            const estado = await api("/api/estado");
            if (primeiraVez) {
                aplicarPadroes(estado.padroes);
                primeiraVez = false;
            }
            el.estado.dataset.estado = estado.estado;
            el.estadoTexto.textContent =
                estado.estado === "pronto" ? `Pronto · ${estado.modelo.replace(/\.safetensors$/, "")}` : TEXTOS_ESTADO[estado.estado] || estado.estado;
            el.log.textContent = estado.log.join("\n");
            motorPronto = estado.estado === "pronto";
            atualizarBotoes();
            if (estado.estado === "erro") el.log.hidden = false;
            if (motorPronto) return;
        } catch {
            el.estado.dataset.estado = "erro";
            el.estadoTexto.textContent = "Servidor desligado";
        }
        await esperar(1500);
    }
}

function aplicarPadroes(padroes) {
    el.passos.value = padroes.passos;
    el.cfg.value = padroes.cfg;
    mostrarValores();
}

function mostrarValores() {
    el.passosValor.textContent = el.passos.value;
    el.cfgValor.textContent = Number(el.cfg.value).toFixed(1);
}

function atualizarBotoes() {
    el.gerar.disabled = !motorPronto || Boolean(trabalhoAtual);
    el.cancelar.hidden = !trabalhoAtual;
}

el.estado.addEventListener("click", () => {
    el.log.hidden = !el.log.hidden;
    el.estado.setAttribute("aria-expanded", String(!el.log.hidden));
});

// ========== Gerar ==========
async function gerar() {
    el.erroForm.hidden = true;
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
    };

    try {
        const { id } = await api("/api/gerar", { method: "POST", body: JSON.stringify(pedido) });
        trabalhoAtual = id;
        atualizarBotoes();
        iniciarAtual(pedido);
        await acompanhar(id, pedido);
    } catch (erro) {
        mostrarErro(erro.message);
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
            el.atualStatus.textContent = t.fila > 0 ? `Na fila (posição ${t.fila})...` : "Preparando...";
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
    if (!trabalhoAtual) return;
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
    el.visBaixar.href = `/galeria/${item.arquivo}`;
    el.visBaixar.download = `${item.id}.png`;

    const dados = [
        ["Tamanho", `${item.largura} × ${item.altura}`],
        ["Semente", item.seed ?? "aleatória"],
        ["Passos", item.passos],
        ["CFG", item.cfg],
        ["Tempo", `${item.segundos.toFixed(1)} s`],
        ["Modelo", item.modelo.replace(/\.safetensors$/, "")],
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
    el.prompt.value = item.prompt;
    el.negativo.value = item.negativo || "";
    const formato = el.form.querySelector(`input[name=formato][value="${item.largura}x${item.altura}"]`);
    if (formato) formato.checked = true;
    el.quantidade.value = "1";
    el.seed.value = item.seed ?? "";
    el.passos.value = item.passos;
    el.cfg.value = item.cfg;
    mostrarValores();
    el.vis.close();
    el.prompt.focus();
    avisar("Configurações carregadas. Mesma semente = mesma imagem.");
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
