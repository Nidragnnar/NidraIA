// NidraIA: liga o motor de imagens (sd-server.exe) e o tradutor (llama-server.exe),
// serve a interface em public/ e guarda as imagens na pasta galeria/.
// Só aceita conexões deste computador.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "httplib.h"
#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

static constexpr const char* VERSAO = NIDRAIA_VERSAO;  // definida no CMakeLists.txt

// ========== Caminhos e configuração ==========
struct Pastas {
    fs::path raiz, motor, modelos, galeria, publico;
};

static Pastas pastas;
static json config;

static fs::path pastaDoExecutavel() {
    std::wstring caminho(MAX_PATH, L'\0');
    for (;;) {
        DWORD tamanho = GetModuleFileNameW(nullptr, caminho.data(), static_cast<DWORD>(caminho.size()));
        if (tamanho < caminho.size()) {
            caminho.resize(tamanho);
            break;
        }
        caminho.resize(caminho.size() * 2);
    }
    return fs::path(caminho).parent_path();
}

static std::string u8(const fs::path& caminho) {
    auto texto = caminho.u8string();
    return std::string(texto.begin(), texto.end());
}

// Texto UTF-8 (como vem do JSON) para caminho do Windows, mantendo acentos
static fs::path caminhoU8(const std::string& texto) {
    return fs::path(std::u8string(texto.begin(), texto.end()));
}

// ========== Erros HTTP ==========
struct ErroHttp : std::runtime_error {
    int status;
    ErroHttp(int status, const std::string& mensagem) : std::runtime_error(mensagem), status(status) {}
};

// ========== Log ==========
static std::mutex mutexLog;
static std::deque<std::string> linhasLog;

static void registrar(const std::string& linha) {
    std::lock_guard trava(mutexLog);
    linhasLog.push_back(linha);
    if (linhasLog.size() > 300) linhasLog.pop_front();
}

static std::vector<std::string> ultimasLinhas(size_t quantidade) {
    std::lock_guard trava(mutexLog);
    auto inicio = linhasLog.size() > quantidade ? linhasLog.end() - quantidade : linhasLog.begin();
    return {inicio, linhasLog.end()};
}

// ========== Processos filhos ==========
// Job object: se o NidraIA fechar (mesmo à força), o Windows encerra o motor e o tradutor junto
static HANDLE jobFilhos = nullptr;

static void prepararJob() {
    jobFilhos = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limites{};
    limites.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(jobFilhos, JobObjectExtendedLimitInformation, &limites, sizeof(limites));
}

// Lê a saída de um processo linha por linha e guarda no log com um prefixo
static void lerSaida(HANDLE leitura, std::string prefixo) {
    std::string pendente;
    char buffer[4096];
    DWORD lidos = 0;
    while (ReadFile(leitura, buffer, sizeof(buffer), &lidos, nullptr) && lidos > 0) {
        pendente.append(buffer, lidos);
        size_t fim;
        while ((fim = pendente.find_first_of("\r\n")) != std::string::npos) {
            if (fim > 0) registrar(prefixo + pendente.substr(0, fim));
            pendente.erase(0, fim + 1);
        }
    }
    if (!pendente.empty()) registrar(prefixo + pendente);
    CloseHandle(leitura);
}

static std::wstring aspas(const fs::path& caminho) {
    return L"\"" + caminho.wstring() + L"\"";
}

// Inicia um programa escondido, com a saída indo para o log; devolve o handle do processo
static HANDLE iniciarProcesso(const fs::path& exe, const std::wstring& argumentos, const std::string& prefixo) {
    SECURITY_ATTRIBUTES herdavel{sizeof(herdavel), nullptr, TRUE};
    HANDLE leitura = nullptr, escrita = nullptr;
    CreatePipe(&leitura, &escrita, &herdavel, 0);
    SetHandleInformation(leitura, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW inicio{sizeof(inicio)};
    inicio.dwFlags = STARTF_USESTDHANDLES;
    inicio.hStdOutput = escrita;
    inicio.hStdError = escrita;
    inicio.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring linha = aspas(exe) + L" " + argumentos;
    PROCESS_INFORMATION info{};
    BOOL ok = CreateProcessW(nullptr, linha.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, exe.parent_path().wstring().c_str(), &inicio, &info);
    CloseHandle(escrita);
    if (!ok) {
        CloseHandle(leitura);
        throw ErroHttp(500, std::format("Não foi possível iniciar {} (erro {}).", u8(exe.filename()), GetLastError()));
    }
    AssignProcessToJobObject(jobFilhos, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    std::thread(lerSaida, leitura, prefixo).detach();
    return info.hProcess;
}

static void encerrarProcesso(HANDLE& processo) {
    if (!processo) return;
    TerminateProcess(processo, 0);
    WaitForSingleObject(processo, 5000);
    CloseHandle(processo);
    processo = nullptr;
}

static bool respondeOk(int porta, const char* caminho) {
    httplib::Client cliente("127.0.0.1", porta);
    cliente.set_connection_timeout(1s);
    cliente.set_read_timeout(3s);
    auto resposta = cliente.Get(caminho);
    return resposta && resposta->status == 200;
}

// ========== Modelos ==========
static const json* acharModelo(const std::string& id) {
    for (const auto& modelo : config["modelos"])
        if (modelo["id"] == id) return &modelo;
    return nullptr;
}

static std::vector<std::string> arquivosFaltando(const json& modelo) {
    std::vector<std::string> faltando;
    for (const auto& item : modelo["arquivos"].items())
        if (!fs::exists(pastas.modelos / caminhoU8(item.value().get<std::string>()))) faltando.push_back(item.value());
    return faltando;
}

// Preferências do usuário (modelo escolhido) ficam fora do config.json
static fs::path arquivoPreferencias() {
    return pastas.raiz / "preferencias.json";
}

static void salvarPreferencias(const std::string& modelo) {
    std::ofstream(arquivoPreferencias()) << json{{"modelo", modelo}}.dump(2);
}

static std::string modeloInicial() {
    std::ifstream entrada(arquivoPreferencias());
    json prefs = json::parse(entrada, nullptr, false);
    if (prefs.is_object() && prefs.contains("modelo") && acharModelo(prefs["modelo"].get<std::string>()))
        return prefs["modelo"].get<std::string>();
    return config["modeloPadrao"].get<std::string>();
}

// ========== Motor (sd-server.exe) ==========
enum class EstadoMotor { Parado, Carregando, Pronto, Erro };

static std::mutex mutexMotor;  // protege tudo abaixo
static EstadoMotor estadoMotor = EstadoMotor::Parado;
static std::string erroMotor;
static std::string modeloAtivo;
static HANDLE processoMotor = nullptr;
static int geracaoMotor = 0;  // muda a cada troca de modelo, para ignorar avisos do motor anterior

static void falharMotor(int geracao, const std::string& mensagem) {
    std::lock_guard trava(mutexMotor);
    if (geracao != geracaoMotor) return;
    estadoMotor = EstadoMotor::Erro;
    erroMotor = mensagem;
    registrar("[nidraia] " + mensagem);
}

// O modelo leva alguns segundos para carregar; consulta até o motor responder
static void vigiarMotor(int geracao, HANDLE processo) {
    int porta = config["portaMotor"].get<int>();
    for (int tentativa = 0; tentativa < 600; tentativa++) {
        if (WaitForSingleObject(processo, 0) == WAIT_OBJECT_0) {
            DWORD codigo = 0;
            GetExitCodeProcess(processo, &codigo);
            return falharMotor(geracao, std::format("O motor parou (código {}). Veja o log.", codigo));
        }
        if (respondeOk(porta, "/sdcpp/v1/capabilities")) {
            std::lock_guard trava(mutexMotor);
            if (geracao != geracaoMotor) return;
            estadoMotor = EstadoMotor::Pronto;
            registrar("[nidraia] Motor pronto: " + modeloAtivo);
            break;
        }
        std::this_thread::sleep_for(1s);
    }
    {
        std::lock_guard trava(mutexMotor);
        if (geracao != geracaoMotor) return;
        if (estadoMotor == EstadoMotor::Carregando) {
            estadoMotor = EstadoMotor::Erro;
            erroMotor = "O motor demorou demais para responder.";
            return;
        }
    }
    // Avisa se o motor cair depois de pronto (numa troca de modelo a geração muda e isso é ignorado)
    WaitForSingleObject(processo, INFINITE);
    falharMotor(geracao, "O motor parou de funcionar. Feche e abra o NidraIA de novo.");
}

// Liga o motor com o modelo pedido, encerrando o anterior se houver
static void ligarMotor(const std::string& id) {
    const json* modelo = acharModelo(id);
    if (!modelo) throw ErroHttp(404, "Modelo desconhecido.");
    auto faltando = arquivosFaltando(*modelo);
    if (!faltando.empty()) throw ErroHttp(409, "Faltam arquivos do modelo: " + faltando.front());

    fs::path exe = pastas.motor / "sd-server.exe";
    if (!fs::exists(exe)) throw ErroHttp(500, "Motor não encontrado em " + u8(exe));

    // Cada arquivo do modelo vira uma opção do motor: "model" → -m, "vae" → --vae...
    std::wstring argumentos = L"--listen-ip 127.0.0.1 --listen-port " + std::to_wstring(config["portaMotor"].get<int>());
    for (const auto& [chave, arquivo] : (*modelo)["arquivos"].items()) {
        std::wstring opcao = chave == "model" ? L"-m" : L"--" + std::wstring(chave.begin(), chave.end());
        argumentos += L" " + opcao + L" " + aspas(pastas.modelos / caminhoU8(arquivo.get<std::string>()));
    }
    // Modelos de aumento de resolução (Real-ESRGAN), se a pasta existir
    if (fs::is_directory(pastas.modelos / "upscale"))
        argumentos += L" --hires-upscalers-dir " + aspas(pastas.modelos / "upscale");

    std::lock_guard trava(mutexMotor);
    geracaoMotor++;
    encerrarProcesso(processoMotor);
    modeloAtivo = id;
    estadoMotor = EstadoMotor::Carregando;
    erroMotor.clear();
    registrar("[nidraia] Carregando " + (*modelo)["nome"].get<std::string>() + "...");
    try {
        processoMotor = iniciarProcesso(exe, argumentos, "");
    } catch (const ErroHttp& erro) {
        estadoMotor = EstadoMotor::Erro;
        erroMotor = erro.what();
        throw;
    }
    HANDLE copia = nullptr;
    DuplicateHandle(GetCurrentProcess(), processoMotor, GetCurrentProcess(), &copia, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread(
        [geracao = geracaoMotor, copia] {
            vigiarMotor(geracao, copia);
            CloseHandle(copia);
        })
        .detach();
}

static httplib::Client clienteMotor() {
    httplib::Client cliente("127.0.0.1", config["portaMotor"].get<int>());
    cliente.set_connection_timeout(2s);
    cliente.set_read_timeout(120s);
    return cliente;
}

static json chamarMotor(const std::string& metodo, const std::string& caminho, const json& corpo = nullptr) {
    {
        std::lock_guard trava(mutexMotor);
        if (estadoMotor != EstadoMotor::Pronto) throw ErroHttp(503, "O motor ainda não está pronto.");
    }
    auto cliente = clienteMotor();
    auto resposta = metodo == "POST" ? cliente.Post(caminho, corpo.is_null() ? "{}" : corpo.dump(), "application/json")
                                     : cliente.Get(caminho);
    if (!resposta) throw ErroHttp(502, "Sem resposta do motor.");
    json dados = json::parse(resposta->body, nullptr, false);
    if (resposta->status >= 400) {
        std::string mensagem = "Erro no motor.";
        if (dados.is_object() && dados.contains("error")) {
            const auto& erro = dados["error"];
            if (erro.is_object() && erro.contains("message")) mensagem = erro["message"].get<std::string>();
            else if (erro.is_string()) mensagem = erro.get<std::string>();
        }
        throw ErroHttp(resposta->status, mensagem);
    }
    if (dados.is_discarded()) throw ErroHttp(502, "Resposta inválida do motor.");
    return dados;
}

// ========== Tradutor (llama-server.exe) ==========
// Liga sob demanda na primeira tradução e fica na CPU, para não disputar a placa de vídeo com o motor
static std::mutex mutexTradutor;
static HANDLE processoTradutor = nullptr;
static std::chrono::steady_clock::time_point ultimoUsoTradutor;

// O tradutor ocupa uns 2,5 GB de RAM: desliga depois de alguns minutos sem uso (religa sozinho quando precisar)
static void desligarTradutorOcioso() {
    for (;;) {
        std::this_thread::sleep_for(30s);
        std::lock_guard trava(mutexTradutor);
        if (processoTradutor && std::chrono::steady_clock::now() - ultimoUsoTradutor > std::chrono::minutes(config["tradutor"].value("desligarAposMinutos", 5))) {
            encerrarProcesso(processoTradutor);
            registrar("[nidraia] Tradutor desligado por falta de uso (libera memória).");
        }
    }
}

static bool tradutorDisponivel() {
    const auto& t = config["tradutor"];
    return fs::exists(pastas.raiz / caminhoU8(t["executavel"].get<std::string>())) &&
           fs::exists(pastas.raiz / caminhoU8(t["modelo"].get<std::string>()));
}

static void garantirTradutor() {
    int porta = config["portaTradutor"].get<int>();
    std::lock_guard trava(mutexTradutor);
    ultimoUsoTradutor = std::chrono::steady_clock::now();
    if (processoTradutor && WaitForSingleObject(processoTradutor, 0) == WAIT_TIMEOUT && respondeOk(porta, "/health")) return;
    if (!tradutorDisponivel()) throw ErroHttp(409, "O tradutor não está instalado (pasta tradutor/).");

    encerrarProcesso(processoTradutor);
    const auto& t = config["tradutor"];
    registrar("[nidraia] Ligando o tradutor...");
    processoTradutor = iniciarProcesso(pastas.raiz / caminhoU8(t["executavel"].get<std::string>()),
                                       L"--host 127.0.0.1 --port " + std::to_wstring(porta) + L" -m " +
                                           aspas(pastas.raiz / caminhoU8(t["modelo"].get<std::string>())) +
                                           L" -ngl 0 -c 2048",
                                       "[tradutor] ");
    for (int tentativa = 0; tentativa < 120; tentativa++) {
        if (WaitForSingleObject(processoTradutor, 0) == WAIT_OBJECT_0) throw ErroHttp(500, "O tradutor parou ao iniciar. Veja o log.");
        if (respondeOk(porta, "/health")) {
            registrar("[nidraia] Tradutor pronto.");
            return;
        }
        std::this_thread::sleep_for(500ms);
    }
    throw ErroHttp(504, "O tradutor demorou demais para iniciar.");
}

static std::string traduzir(const std::string& texto) {
    garantirTradutor();
    // Exemplos curtos ensinam o formato e a não trocar um animal ou objeto por outro
    static const std::vector<std::pair<std::string, std::string>> exemplos{
        {"uma raposa correndo na neve, foto", "a fox running in the snow, photo"},
        {"menino tocando violão na praia, aquarela", "boy playing acoustic guitar on the beach, watercolor"},
        {"a cute cat astronaut, digital art", "a cute cat astronaut, digital art"},
        {"onça-pintada e capivara na beira do rio, ilustração", "jaguar and capybara on the riverbank, illustration"},
        {"crianças soltando pipa no fim de tarde", "children flying kites in the late afternoon"},
    };
    json mensagens = json::array();
    mensagens.push_back({{"role", "system"},
                         {"content",
                          "You are a precise Portuguese-to-English translator for image-generation prompts. Translate "
                          "faithfully: never replace an animal, object or person with a different one and never add "
                          "details. Keep the same order, commas and tags. If the text is already English, repeat it "
                          "unchanged. Output ONLY the English text."}});
    for (const auto& [pt, en] : exemplos) {
        mensagens.push_back({{"role", "user"}, {"content", pt}});
        mensagens.push_back({{"role", "assistant"}, {"content", en}});
    }
    mensagens.push_back({{"role", "user"}, {"content", texto}});
    json pedido = {
        {"messages", mensagens},
        {"temperature", 0},
        {"max_tokens", 600},
    };
    httplib::Client cliente("127.0.0.1", config["portaTradutor"].get<int>());
    cliente.set_read_timeout(60s);
    auto resposta = cliente.Post("/v1/chat/completions", pedido.dump(), "application/json");
    if (!resposta || resposta->status != 200) throw ErroHttp(502, "O tradutor não respondeu.");
    json dados = json::parse(resposta->body, nullptr, false);
    if (dados.is_discarded()) throw ErroHttp(502, "Resposta inválida do tradutor.");
    std::string traducao = dados["choices"][0]["message"]["content"].get<std::string>();
    // Tira espaços e aspas que o modelo às vezes coloca em volta
    auto inicio = traducao.find_first_not_of(" \t\r\n\"");
    auto fim = traducao.find_last_not_of(" \t\r\n\"");
    if (inicio == std::string::npos) throw ErroHttp(502, "O tradutor devolveu um texto vazio.");
    return traducao.substr(inicio, fim - inicio + 1);
}

// ========== Pedidos de geração ==========
static std::mutex mutexPedidos;
static std::map<std::string, json> pedidos;  // id do trabalho → configurações usadas

static double numero(const json& corpo, const char* campo, double minimo, double maximo, double padrao) {
    double valor = padrao;
    if (corpo.contains(campo)) {
        const auto& v = corpo[campo];
        if (v.is_number()) valor = v.get<double>();
        else if (v.is_string()) {
            try {
                valor = std::stod(v.get<std::string>());
            } catch (...) {
            }
        }
    }
    return std::clamp(valor, minimo, maximo);
}

static std::string texto(const json& corpo, const char* campo) {
    if (!corpo.contains(campo) || !corpo[campo].is_string()) return "";
    std::string valor = corpo[campo].get<std::string>();
    auto inicio = valor.find_first_not_of(" \t\r\n");
    auto fim = valor.find_last_not_of(" \t\r\n");
    return inicio == std::string::npos ? "" : valor.substr(inicio, fim - inicio + 1);
}

// Semente sorteada aqui (e não pelo motor) para ficar salva e a imagem poder ser repetida
static int64_t sementeOuSorteio(const json& corpo) {
    if (corpo.contains("seed") && corpo["seed"].is_number_integer()) {
        auto seed = corpo["seed"].get<int64_t>();
        if (seed >= 0 && seed < (1LL << 31)) return seed;
    }
    static std::mt19937 gerador{std::random_device{}()};
    return std::uniform_int_distribution<int64_t>(0, (1LL << 31) - 1)(gerador);
}

// Estilo pronto do config.json: acrescenta termos em inglês ao prompt e ao "evitar".
// O prompt sem o estilo fica guardado em "promptBase", para "Reutilizar" não repetir os termos.
static void aplicarEstilo(json& pedido, const std::string& id) {
    if (id.empty() || id == "nenhum" || !config.contains("estilos")) return;
    for (const auto& estilo : config["estilos"]) {
        if (estilo["id"] != id) continue;
        auto juntar = [](const std::string& a, const std::string& b) { return a.empty() ? b : b.empty() ? a : a + ", " + b; };
        pedido["promptBase"] = pedido["prompt"];
        pedido["prompt"] = juntar(pedido["prompt"].get<std::string>(), estilo["sufixo"].get<std::string>());
        pedido["negativo"] = juntar(pedido["negativo"].get<std::string>(), estilo["negativo"].get<std::string>());
        pedido["estilo"] = estilo["id"];
        pedido["estiloNome"] = estilo["nome"];
        return;
    }
    throw ErroHttp(422, "Estilo desconhecido.");
}

static json montarPedido(const json& corpo, const json& modelo) {
    const auto& p = modelo["padroes"];
    std::string prompt = texto(corpo, "prompt");
    if (prompt.empty()) throw ErroHttp(422, "Escreva o que você quer ver na imagem.");
    if (prompt.size() > 4000) throw ErroHttp(422, "O prompt está grande demais.");

    // Lados múltiplos de 64, como os modelos esperam
    auto lado = [&](const char* campo, int padrao) {
        return static_cast<int>(std::lround(numero(corpo, campo, 256, 1536, padrao) / 64.0) * 64);
    };
    std::string negativo = texto(corpo, "negativo").substr(0, 4000);
    bool traduzirPrompt = corpo.value("traduzir", false);

    json pedido = {
        {"prompt", prompt},
        {"promptOriginal", nullptr},
        {"negativo", negativo},
        {"largura", lado("largura", p["largura"].get<int>())},
        {"altura", lado("altura", p["altura"].get<int>())},
        {"passos", static_cast<int>(std::lround(numero(corpo, "passos", 1, modelo["maxPassos"].get<int>(), p["passos"].get<int>())))},
        {"cfg", numero(corpo, "cfg", 1, 20, p["cfg"].get<double>())},
        {"seed", sementeOuSorteio(corpo)},
        {"quantidade", static_cast<int>(std::lround(numero(corpo, "quantidade", 1, 4, 1)))},
        {"amostrador", p["amostrador"]},
        {"traduzido", traduzirPrompt},
    };
    if (traduzirPrompt) {
        pedido["promptOriginal"] = prompt;
        pedido["prompt"] = traduzir(prompt);
        if (!negativo.empty()) pedido["negativo"] = traduzir(negativo);
    } else if (std::string original = texto(corpo, "promptOriginal"); !original.empty()) {
        // Tradução já conferida (e talvez corrigida) na tela: guarda o português junto
        pedido["promptOriginal"] = original.substr(0, 4000);
        pedido["traduzido"] = true;
    }
    aplicarEstilo(pedido, texto(corpo, "estilo"));
    return pedido;
}

static json paraMotor(const json& pedido, const std::string& imagemBase = "", double forca = 0) {
    json motor = {
        {"prompt", pedido["prompt"]},
        {"negative_prompt", pedido["negativo"]},
        {"width", pedido["largura"]},
        {"height", pedido["altura"]},
        {"seed", pedido["seed"]},
        {"batch_count", pedido["quantidade"]},
        {"sample_params",
         {{"sample_method", pedido["amostrador"]},
          {"sample_steps", pedido["passos"]},
          {"guidance", {{"txt_cfg", pedido["cfg"]}}}}},
        {"preview", "proj"},
        {"preview_interval", 1},
        {"output_format", "png"},
    };
    // Imagem para imagem: parte de uma imagem existente; "strength" diz o quanto ela pode mudar
    if (!imagemBase.empty()) {
        motor["init_image"] = imagemBase;
        motor["strength"] = forca;
        // Codificar a imagem de entrada pede bastante memória de vídeo; em blocos cabe em placas de 8 GB
        motor["vae_tiling_params"] = {{"enabled", true}};
    }
    return motor;
}

// ========== Galeria ==========
static std::string decodificarBase64(const std::string& entrada) {
    static const std::string alfabeto = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string saida;
    saida.reserve(entrada.size() * 3 / 4);
    int valor = 0, bits = -8;
    for (unsigned char c : entrada) {
        auto posicao = alfabeto.find(c);
        if (posicao == std::string::npos) continue;  // ignora '=' e quebras de linha
        valor = (valor << 6) + static_cast<int>(posicao);
        bits += 6;
        if (bits >= 0) {
            saida.push_back(static_cast<char>((valor >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return saida;
}

static std::string codificarBase64(const std::string& entrada) {
    static const char* alfabeto = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string saida;
    saida.reserve((entrada.size() + 2) / 3 * 4);
    int valor = 0, bits = -6;
    for (unsigned char c : entrada) {
        valor = (valor << 8) + c;
        bits += 8;
        while (bits >= 0) {
            saida.push_back(alfabeto[(valor >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) saida.push_back(alfabeto[((valor << 8) >> (bits + 8)) & 0x3F]);
    while (saida.size() % 4) saida.push_back('=');
    return saida;
}

static std::string lerArquivo(const fs::path& arquivo) {
    std::ifstream entrada(arquivo, std::ios::binary);
    if (!entrada) throw ErroHttp(404, "Imagem não encontrada no disco.");
    return {std::istreambuf_iterator<char>(entrada), std::istreambuf_iterator<char>()};
}

static void gravar(const fs::path& arquivo, const std::string& conteudo) {
    std::ofstream saida(arquivo, std::ios::binary);
    saida.write(conteudo.data(), static_cast<std::streamsize>(conteudo.size()));
    if (!saida) throw ErroHttp(500, "Não foi possível salvar em " + u8(arquivo));
}

static std::string agoraIso() {
    return std::format("{:%FT%T}Z", std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
}

static std::string diaLocal() {
    auto agora = std::chrono::zoned_time{std::chrono::current_zone(), std::chrono::system_clock::now()};
    return std::format("{:%F}", agora);
}

// Cada imagem fica como AAAA-MM-DD/<id>.png com um <id>.json do lado (prompt e configurações)
static json salvarImagens(const json& trabalho, const json& pedido) {
    std::string dia = diaLocal();
    fs::path pasta = pastas.galeria / dia;
    fs::create_directories(pasta);

    double segundos = 0;
    if (trabalho["completed"].is_number() && trabalho["started"].is_number()) {
        segundos = (trabalho["completed"].get<double>() - trabalho["started"].get<double>()) / 1000.0;
    }

    json salvas = json::array();
    for (const auto& imagem : trabalho["result"]["images"]) {
        int indice = imagem["index"].get<int>();
        std::string id = std::format("{}-{}", trabalho["id"].get<std::string>(), indice);
        gravar(pasta / (id + ".png"), decodificarBase64(imagem["b64_json"].get<std::string>()));

        json info = pedido;
        info["id"] = id;
        info["arquivo"] = dia + "/" + id + ".png";
        info["criadoEm"] = agoraIso();
        info["segundos"] = segundos;
        // Em lote, o motor usa semente, semente + 1, semente + 2...
        info["seed"] = pedido["seed"].get<int64_t>() + indice;
        gravar(pasta / (id + ".json"), info.dump(2));
        salvas.push_back(info);
    }
    return salvas;
}

static json listarGaleria() {
    json itens = json::array();
    if (!fs::exists(pastas.galeria)) return itens;
    for (const auto& dia : fs::directory_iterator(pastas.galeria)) {
        if (!dia.is_directory()) continue;
        for (const auto& arquivo : fs::directory_iterator(dia.path())) {
            if (arquivo.path().extension() != ".json") continue;
            std::ifstream entrada(arquivo.path());
            json item = json::parse(entrada, nullptr, false);
            if (item.is_object() && item.contains("criadoEm")) itens.push_back(item);  // corrompido: ignora
        }
    }
    std::sort(itens.begin(), itens.end(), [](const json& a, const json& b) {
        return a["criadoEm"].get<std::string>() > b["criadoEm"].get<std::string>();
    });
    return itens;
}

// ========== HTTP ==========
static void responderJson(httplib::Response& res, int status, const json& dados) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(dados.dump(), "application/json; charset=utf-8");
}

// limite: quase todo pedido é texto pequeno; só o "gerar" com uma imagem enviada pela tela é maior
static json lerCorpo(const httplib::Request& req, size_t limite = 100'000) {
    if (req.body.size() > limite) throw ErroHttp(413, "Pedido grande demais.");
    json corpo = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (corpo.is_discarded() || !corpo.is_object()) throw ErroHttp(400, "JSON inválido.");
    return corpo;
}

static const char* nomeEstado(EstadoMotor estado) {
    switch (estado) {
        case EstadoMotor::Pronto: return "pronto";
        case EstadoMotor::Erro: return "erro";
        case EstadoMotor::Parado: return "parado";
        default: return "carregando";
    }
}

static json listaModelos() {
    json lista = json::array();
    for (const auto& modelo : config["modelos"]) {
        auto faltando = arquivosFaltando(modelo);
        lista.push_back({{"id", modelo["id"]},
                         {"nome", modelo["nome"]},
                         {"descricao", modelo["descricao"]},
                         {"licenca", modelo["licenca"]},
                         {"padroes", modelo["padroes"]},
                         {"maxPassos", modelo["maxPassos"]},
                         {"disponivel", faltando.empty()}});
    }
    return lista;
}

static const json& modeloProntoOuErro() {
    std::lock_guard trava(mutexMotor);
    if (estadoMotor != EstadoMotor::Pronto) throw ErroHttp(503, "O motor ainda não está pronto.");
    return *acharModelo(modeloAtivo);
}

// Manda o pedido para a fila do motor e guarda as configurações para salvar junto da imagem
static std::string enfileirar(const json& pedido, const json& corpoMotor) {
    json trabalho = chamarMotor("POST", "/sdcpp/v1/img_gen", corpoMotor);
    std::string id = trabalho["id"].get<std::string>();
    std::lock_guard trava(mutexPedidos);
    pedidos[id] = pedido;
    return id;
}

// Troca a mensagem técnica do motor por uma explicação, olhando o log recente
static std::string explicarFalha(const std::string& mensagem) {
    for (const auto& linha : ultimasLinhas(40)) {
        if (linha.find("cannot make enough memory") != std::string::npos || linha.find("out of memory") != std::string::npos ||
            linha.find("ErrorOutOfDeviceMemory") != std::string::npos) {
            return "A placa de vídeo ficou sem memória. Feche jogos ou programas pesados, ou gere menos imagens por vez, "
                   "e tente de novo.";
        }
    }
    return mensagem.empty() ? "A geração falhou. Veja o log (clique na bolinha do topo)." : mensagem;
}

// Upscalers do config.json cujo arquivo .pth existe em modelos/upscale
static json listaUpscalers() {
    json lista = json::array();
    if (!config.contains("upscalers")) return lista;
    for (const auto& u : config["upscalers"]) {
        std::string id = u["id"].get<std::string>();
        auto pasta = pastas.modelos / "upscale";
        if (fs::exists(pasta / caminhoU8(id + ".pth")) || fs::exists(pasta / caminhoU8(id + ".safetensors")))
            lista.push_back({{"id", id}, {"nome", u["nome"]}});
    }
    return lista;
}

static bool temGeracaoEmAndamento() {
    std::lock_guard trava(mutexPedidos);
    return !pedidos.empty();
}

static const std::regex ID_TRABALHO{"^[A-Za-z0-9_-]{1,80}$"};
static const std::regex ID_IMAGEM{"^[A-Za-z0-9_-]{1,90}$"};

static void configurarRotas(httplib::Server& servidor) {
    servidor.Get("/api/estado", [](const httplib::Request&, httplib::Response& res) {
        json estado;
        {
            std::lock_guard trava(mutexMotor);
            estado = {{"estado", nomeEstado(estadoMotor)},
                      {"erro", erroMotor.empty() ? json(nullptr) : json(erroMotor)},
                      {"modelo", modeloAtivo}};
        }
        estado["versao"] = VERSAO;
        estado["modelos"] = listaModelos();
        estado["tradutor"] = tradutorDisponivel();
        estado["upscalers"] = listaUpscalers();
        estado["estilos"] = json::array();
        for (const auto& e : config.value("estilos", json::array())) estado["estilos"].push_back({{"id", e["id"]}, {"nome", e["nome"]}});
        estado["log"] = ultimasLinhas(60);
        responderJson(res, 200, estado);
    });

    servidor.Post("/api/modelo", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = texto(lerCorpo(req), "id");
        if (temGeracaoEmAndamento()) throw ErroHttp(409, "Espere a geração atual terminar para trocar de modelo.");
        ligarMotor(id);
        salvarPreferencias(id);
        responderJson(res, 202, {{"modelo", id}});
    });

    servidor.Post("/api/traduzir", [](const httplib::Request& req, httplib::Response& res) {
        std::string original = texto(lerCorpo(req), "texto");
        if (original.empty()) throw ErroHttp(422, "Escreva algo para traduzir.");
        responderJson(res, 200, {{"traducao", traduzir(original)}});
    });

    servidor.Post("/api/gerar", [](const httplib::Request& req, httplib::Response& res) {
        json corpo = lerCorpo(req, 25'000'000);
        const json& modelo = modeloProntoOuErro();

        // Imagem enviada pela tela (opcional): "data:image/png;base64,..." já no tamanho escolhido
        std::string imagemBase;
        if (corpo.contains("imagemBase") && corpo["imagemBase"].is_string()) {
            imagemBase = corpo["imagemBase"].get<std::string>();
            if (!imagemBase.starts_with("data:image/png;base64,") && !imagemBase.starts_with("data:image/jpeg;base64,"))
                throw ErroHttp(422, "A imagem enviada precisa ser PNG ou JPG.");
        }
        json pedido = montarPedido(corpo, modelo);
        pedido["modelo"] = modelo["nome"];
        double forca = numero(corpo, "forca", 0.1, 1.0, 0.6);
        if (!imagemBase.empty()) {
            pedido["partiuDeImagem"] = true;
            pedido["forca"] = forca;
        }
        std::string id = enfileirar(pedido, paraMotor(pedido, imagemBase, forca));
        responderJson(res, 202, {{"id", id}, {"prompt", pedido["prompt"]}, {"traduzido", pedido["traduzido"]}});
    });

    // Aumentar resolução 4×: roda o Real-ESRGAN numa imagem da galeria e salva o resultado como imagem nova
    servidor.Post("/api/aumentar", [](const httplib::Request& req, httplib::Response& res) {
        json corpo = lerCorpo(req);
        std::string idImagem = texto(corpo, "id");
        std::string upscaler = texto(corpo, "upscaler");
        json disponiveis = listaUpscalers();
        if (disponiveis.empty()) throw ErroHttp(409, "Os modelos de aumento de resolução não estão instalados.");
        if (upscaler.empty()) upscaler = disponiveis[0]["id"].get<std::string>();
        if (std::none_of(disponiveis.begin(), disponiveis.end(), [&](const json& u) { return u["id"] == upscaler; }))
            throw ErroHttp(404, "Modelo de aumento desconhecido.");
        if (temGeracaoEmAndamento()) throw ErroHttp(409, "Espere a geração atual terminar.");

        json original;
        for (const auto& item : listarGaleria())
            if (item["id"] == idImagem) original = item;
        if (original.is_null()) throw ErroHttp(404, "Imagem não encontrada.");
        // Até 4096 px por lado: acima disso fica lento demais e a imagem pesa centenas de MB
        if (original["largura"].get<int>() * 4 > 4096 || original["altura"].get<int>() * 4 > 4096)
            throw ErroHttp(422, "Esta imagem já é grande demais para aumentar 4× (o limite é 4096 px por lado).");

        auto inicio = std::chrono::steady_clock::now();
        std::string imagem = codificarBase64(lerArquivo(pastas.galeria / caminhoU8(original["arquivo"].get<std::string>())));
        json resultado = chamarMotor("POST", "/sdcpp/v1/upscale",
                                     {{"image", imagem}, {"upscaler", upscaler}, {"output_format", "png"}});
        double segundos = std::chrono::duration<double>(std::chrono::steady_clock::now() - inicio).count();

        // Salva ao lado da original, com as mesmas informações e a marca do aumento
        std::string dia = diaLocal();
        fs::path pasta = pastas.galeria / dia;
        fs::create_directories(pasta);
        std::string id = idImagem + "-4x";
        for (int n = 2; fs::exists(pasta / (id + ".png")); n++) id = std::format("{}-4x{}", idImagem, n);
        gravar(pasta / (id + ".png"), decodificarBase64(resultado["images"][0]["b64_json"].get<std::string>()));

        json info = original;
        info["id"] = id;
        info["arquivo"] = dia + "/" + id + ".png";
        info["criadoEm"] = agoraIso();
        info["largura"] = resultado["width"];
        info["altura"] = resultado["height"];
        info["segundos"] = segundos;
        info["aumentadaDe"] = idImagem;
        info["upscaler"] = upscaler;
        gravar(pasta / (id + ".json"), info.dump(2));
        responderJson(res, 200, info);
    });

    // Variações: imagem para imagem a partir de uma imagem da galeria, com sementes novas.
    // A força baixa mantém a composição e muda os detalhes.
    servidor.Post("/api/variacoes", [](const httplib::Request& req, httplib::Response& res) {
        json corpo = lerCorpo(req);
        std::string idImagem = texto(corpo, "id");
        json original;
        for (const auto& item : listarGaleria())
            if (item["id"] == idImagem) original = item;
        if (original.is_null()) throw ErroHttp(404, "Imagem não encontrada.");

        const json& modelo = modeloProntoOuErro();
        json base = {
            {"prompt", original["prompt"]},
            {"negativo", original.value("negativo", "")},
            {"largura", original["largura"]},
            {"altura", original["altura"]},
            {"quantidade", corpo.contains("quantidade") ? corpo["quantidade"] : json(4)},
        };
        if (original.contains("promptOriginal") && original["promptOriginal"].is_string())
            base["promptOriginal"] = original["promptOriginal"];
        json pedido = montarPedido(base, modelo);
        double forca = numero(corpo, "forca", 0.2, 0.8, 0.45);
        pedido["modelo"] = modelo["nome"];
        pedido["variacaoDe"] = idImagem;
        pedido["forca"] = forca;

        std::string imagem = codificarBase64(lerArquivo(pastas.galeria / caminhoU8(original["arquivo"].get<std::string>())));
        std::string id = enfileirar(pedido, paraMotor(pedido, imagem, forca));
        responderJson(res, 202, {{"id", id}, {"prompt", pedido["prompt"]}, {"traduzido", pedido["traduzido"]}});
    });

    servidor.Get(R"(/api/trabalhos/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        if (!std::regex_match(id, ID_TRABALHO)) throw ErroHttp(404, "Não encontrado.");
        json t = chamarMotor("GET", "/sdcpp/v1/jobs/" + id);
        std::string status = t.value("status", "");

        json resposta = {
            {"status", status},
            {"fila", t.value("queue_position", 0)},
            {"previa", nullptr},
            {"passo", nullptr},
            {"totalPassos", nullptr},
            {"erro", nullptr},
        };
        if (t.contains("preview") && t["preview"].is_object()) {
            resposta["previa"] = t["preview"].value("b64_json", "");
            resposta["passo"] = t["preview"]["step"];
            resposta["totalPassos"] = t["preview"]["total_steps"];
        }
        if (t.contains("error") && t["error"].is_object()) resposta["erro"] = explicarFalha(t["error"].value("message", ""));

        // Ao terminar, salva na galeria uma vez só
        if (status == "completed" || status == "failed" || status == "cancelled") {
            json pedido;
            {
                std::lock_guard trava(mutexPedidos);
                auto encontrado = pedidos.find(id);
                if (encontrado != pedidos.end()) {
                    pedido = encontrado->second;
                    pedidos.erase(encontrado);
                }
            }
            if (status == "completed" && !pedido.is_null()) resposta["imagens"] = salvarImagens(t, pedido);
        }
        responderJson(res, 200, resposta);
    });

    servidor.Post(R"(/api/trabalhos/([^/]+)/cancelar)", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        if (!std::regex_match(id, ID_TRABALHO)) throw ErroHttp(404, "Não encontrado.");
        responderJson(res, 200, chamarMotor("POST", "/sdcpp/v1/jobs/" + id + "/cancel"));
    });

    servidor.Get("/api/galeria", [](const httplib::Request&, httplib::Response& res) {
        responderJson(res, 200, listarGaleria());
    });

    servidor.Delete(R"(/api/galeria/([^/]+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string id = req.matches[1];
        if (!std::regex_match(id, ID_IMAGEM)) throw ErroHttp(404, "Imagem não encontrada.");
        for (const auto& item : listarGaleria()) {
            if (item["id"] != id) continue;
            fs::path png = pastas.galeria / caminhoU8(item["arquivo"].get<std::string>());
            fs::path info = fs::path(png).replace_extension(".json");
            // Enquanto a imagem está sendo enviada ao navegador o Windows trava o arquivo; tenta por até 2 s
            for (int tentativa = 0; tentativa < 20; tentativa++) {
                std::error_code erro;
                fs::remove(png, erro);
                if (!erro) {
                    fs::remove(info, erro);
                    res.status = 204;
                    return;
                }
                std::this_thread::sleep_for(100ms);
            }
            throw ErroHttp(409, "A imagem está em uso. Feche-a em outros programas e tente de novo.");
        }
        throw ErroHttp(404, "Imagem não encontrada.");
    });

    // Arquivos estáticos (o httplib já bloqueia caminhos com "..").
    // "no-cache" faz o navegador conferir se há versão nova da interface a cada abertura
    httplib::Headers semCache{{"Cache-Control", "no-cache"}};
    servidor.set_mount_point("/galeria", u8(pastas.galeria));
    servidor.set_mount_point("/", u8(pastas.publico), semCache);

    servidor.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr ep) {
        int status = 500;
        std::string mensagem = "Erro interno no NidraIA.";
        try {
            std::rethrow_exception(ep);
        } catch (const ErroHttp& erro) {
            status = erro.status;
            mensagem = erro.what();
        } catch (const std::exception& erro) {
            std::cerr << "[erro] " << erro.what() << "\n";
        }
        responderJson(res, status, {{"erro", mensagem}});
    });
}

// ========== Início ==========
static httplib::Server* servidorAtivo = nullptr;

static BOOL WINAPI aoFecharConsole(DWORD) {
    if (servidorAtivo) servidorAtivo->stop();
    if (jobFilhos) TerminateJobObject(jobFilhos, 0);
    return TRUE;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"NidraIA");
    // --sem-navegador: não abre o navegador sozinho (útil para testes)
    bool abrirNavegador = !(argc > 1 && std::string(argv[1]) == "--sem-navegador");

    pastas.raiz = pastaDoExecutavel();
    pastas.motor = pastas.raiz / "motor";
    pastas.modelos = pastas.raiz / "modelos";
    pastas.galeria = pastas.raiz / "galeria";
    pastas.publico = pastas.raiz / "public";
    fs::create_directories(pastas.galeria);

    {
        // Lê e já fecha o arquivo, para ele poder ser editado com o programa aberto
        std::ifstream arquivoConfig(pastas.raiz / "config.json");
        config = json::parse(arquivoConfig, nullptr, false);
    }
    if (config.is_discarded() || !config.contains("modelos")) {
        std::cerr << "Não consegui ler o config.json em " << u8(pastas.raiz) << "\n";
        system("pause");
        return 1;
    }
    prepararJob();

    httplib::Server servidor;
    servidorAtivo = &servidor;
    configurarRotas(servidor);
    SetConsoleCtrlHandler(aoFecharConsole, TRUE);

    int porta = config["portaApp"].get<int>();
    if (!servidor.bind_to_port("127.0.0.1", porta)) {
        std::cerr << "A porta " << porta << " já está em uso. O NidraIA já está aberto em outra janela?\n";
        system("pause");
        return 1;
    }

    std::string endereco = std::format("http://localhost:{}", porta);
    std::cout << "NidraIA " << VERSAO << " aberto em " << endereco << "\n"
              << "Para fechar, feche esta janela.\n\n";

    // Liga o último modelo usado; se ele não estiver baixado, tenta os outros
    std::vector<std::string> tentativas{modeloInicial()};
    for (const auto& modelo : config["modelos"]) tentativas.push_back(modelo["id"]);
    for (const auto& id : tentativas) {
        try {
            ligarMotor(id);
            break;
        } catch (const ErroHttp& erro) {
            registrar(std::string("[nidraia] ") + erro.what());
        }
    }

    std::thread(desligarTradutorOcioso).detach();
    if (abrirNavegador) ShellExecuteA(nullptr, "open", endereco.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    servidor.listen_after_bind();
    TerminateJobObject(jobFilhos, 0);
    return 0;
}
