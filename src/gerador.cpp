// Gerador de Imagens: liga o motor (sd-server.exe), serve a interface em public/
// e guarda as imagens geradas na pasta galeria/. Só aceita conexões deste computador.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "httplib.h"
#include "json.hpp"

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

// ========== Log do motor ==========
static std::mutex mutexLog;
static std::deque<std::string> linhasLog;

static void registrar(const std::string& linha) {
    std::lock_guard trava(mutexLog);
    linhasLog.push_back(linha);
    if (linhasLog.size() > 200) linhasLog.pop_front();
}

static std::vector<std::string> ultimasLinhas(size_t quantidade) {
    std::lock_guard trava(mutexLog);
    auto inicio = linhasLog.size() > quantidade ? linhasLog.end() - quantidade : linhasLog.begin();
    return {inicio, linhasLog.end()};
}

// ========== Motor (sd-server.exe) ==========
enum class EstadoMotor { Carregando, Pronto, Erro };

static std::atomic<EstadoMotor> estadoMotor{EstadoMotor::Carregando};
static std::mutex mutexErro;
static std::string erroMotor;
static HANDLE processoMotor = nullptr;

static void falharMotor(const std::string& mensagem) {
    {
        std::lock_guard trava(mutexErro);
        erroMotor = mensagem;
    }
    estadoMotor = EstadoMotor::Erro;
    registrar("[gerador] " + mensagem);
}

// Lê a saída do motor linha por linha e guarda no log
static void lerSaida(HANDLE leitura) {
    std::string pendente;
    char buffer[4096];
    DWORD lidos = 0;
    while (ReadFile(leitura, buffer, sizeof(buffer), &lidos, nullptr) && lidos > 0) {
        pendente.append(buffer, lidos);
        size_t fim;
        while ((fim = pendente.find_first_of("\r\n")) != std::string::npos) {
            if (fim > 0) registrar(pendente.substr(0, fim));
            pendente.erase(0, fim + 1);
        }
    }
    if (!pendente.empty()) registrar(pendente);
    CloseHandle(leitura);
}

static std::wstring aspas(const fs::path& caminho) {
    return L"\"" + caminho.wstring() + L"\"";
}

static bool ligarMotor() {
    fs::path exe = pastas.motor / "sd-server.exe";
    fs::path modelo = pastas.modelos / caminhoU8(config["modelo"].get<std::string>());
    if (!fs::exists(exe)) return falharMotor("Motor não encontrado em " + u8(exe)), false;
    if (!fs::exists(modelo)) return falharMotor("Modelo não encontrado: " + u8(modelo.filename())), false;

    // Job object: se o gerador fechar (mesmo à força), o Windows encerra o motor junto
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limites{};
    limites.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limites, sizeof(limites));

    SECURITY_ATTRIBUTES herdavel{sizeof(herdavel), nullptr, TRUE};
    HANDLE leitura = nullptr, escrita = nullptr;
    CreatePipe(&leitura, &escrita, &herdavel, 0);
    SetHandleInformation(leitura, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW inicio{sizeof(inicio)};
    inicio.dwFlags = STARTF_USESTDHANDLES;
    inicio.hStdOutput = escrita;
    inicio.hStdError = escrita;
    inicio.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    std::wstring linha = aspas(exe) + L" --listen-ip 127.0.0.1 --listen-port " +
                         std::to_wstring(config["portaMotor"].get<int>()) + L" -m " + aspas(modelo) +
                         L" --model-dir " + aspas(pastas.modelos);

    PROCESS_INFORMATION info{};
    BOOL ok = CreateProcessW(nullptr, linha.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                             nullptr, pastas.motor.wstring().c_str(), &inicio, &info);
    CloseHandle(escrita);
    if (!ok) {
        CloseHandle(leitura);
        return falharMotor(std::format("Não foi possível iniciar o motor (erro {}).", GetLastError())), false;
    }
    AssignProcessToJobObject(job, info.hProcess);
    ResumeThread(info.hThread);
    CloseHandle(info.hThread);
    processoMotor = info.hProcess;

    std::thread(lerSaida, leitura).detach();
    return true;
}

static httplib::Client clienteMotor() {
    httplib::Client cliente("127.0.0.1", config["portaMotor"].get<int>());
    cliente.set_connection_timeout(2s);
    cliente.set_read_timeout(120s);
    return cliente;
}

// O modelo leva alguns segundos para carregar; consulta até o motor responder
static void esperarMotor() {
    for (int tentativa = 0; tentativa < 300; tentativa++) {
        if (WaitForSingleObject(processoMotor, 0) == WAIT_OBJECT_0) {
            DWORD codigo = 0;
            GetExitCodeProcess(processoMotor, &codigo);
            return falharMotor(std::format("O motor parou (código {}). Veja o log.", codigo));
        }
        auto resposta = clienteMotor().Get("/sdcpp/v1/capabilities");
        if (resposta && resposta->status == 200) {
            estadoMotor = EstadoMotor::Pronto;
            registrar("[gerador] Motor pronto.");
            break;
        }
        std::this_thread::sleep_for(1s);
    }
    if (estadoMotor == EstadoMotor::Carregando) return falharMotor("O motor demorou demais para responder.");

    // Avisa se o motor cair depois de pronto
    WaitForSingleObject(processoMotor, INFINITE);
    falharMotor("O motor parou de funcionar. Feche e abra o gerador de novo.");
}

// ========== Erros HTTP ==========
struct ErroHttp : std::runtime_error {
    int status;
    ErroHttp(int status, const std::string& mensagem) : std::runtime_error(mensagem), status(status) {}
};

static json chamarMotor(const std::string& metodo, const std::string& caminho, const json& corpo = nullptr) {
    if (estadoMotor != EstadoMotor::Pronto) throw ErroHttp(503, "O motor ainda não está pronto.");
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

static json montarPedido(const json& corpo) {
    const auto& p = config["padroes"];
    std::string prompt = texto(corpo, "prompt");
    if (prompt.empty()) throw ErroHttp(422, "Escreva o que você quer ver na imagem.");
    if (prompt.size() > 4000) throw ErroHttp(422, "O prompt está grande demais.");

    // Lados múltiplos de 64, como os modelos esperam
    auto lado = [&](const char* campo, int padrao) {
        return static_cast<int>(std::lround(numero(corpo, campo, 256, 1536, padrao) / 64.0) * 64);
    };
    return {
        {"prompt", prompt},
        {"negativo", texto(corpo, "negativo").substr(0, 4000)},
        {"largura", lado("largura", p["largura"].get<int>())},
        {"altura", lado("altura", p["altura"].get<int>())},
        {"passos", static_cast<int>(std::lround(numero(corpo, "passos", 1, 50, p["passos"].get<int>())))},
        {"cfg", numero(corpo, "cfg", 1, 20, p["cfg"].get<double>())},
        {"seed", sementeOuSorteio(corpo)},
        {"quantidade", static_cast<int>(std::lround(numero(corpo, "quantidade", 1, 4, 1)))},
        {"amostrador", p["amostrador"]},
    };
}

static json paraMotor(const json& pedido) {
    return {
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
        info["modelo"] = config["modelo"];
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

static json lerCorpo(const httplib::Request& req) {
    if (req.body.size() > 100'000) throw ErroHttp(413, "Pedido grande demais.");
    json corpo = json::parse(req.body.empty() ? "{}" : req.body, nullptr, false);
    if (corpo.is_discarded() || !corpo.is_object()) throw ErroHttp(400, "JSON inválido.");
    return corpo;
}

static const char* nomeEstado(EstadoMotor estado) {
    switch (estado) {
        case EstadoMotor::Pronto: return "pronto";
        case EstadoMotor::Erro: return "erro";
        default: return "carregando";
    }
}

static const std::regex ID_TRABALHO{"^[A-Za-z0-9_-]{1,80}$"};
static const std::regex ID_IMAGEM{"^[A-Za-z0-9_-]{1,90}$"};

static void configurarRotas(httplib::Server& servidor) {
    servidor.Get("/api/estado", [](const httplib::Request&, httplib::Response& res) {
        std::string erro;
        {
            std::lock_guard trava(mutexErro);
            erro = erroMotor;
        }
        responderJson(res, 200,
                      {{"estado", nomeEstado(estadoMotor)},
                       {"erro", erro.empty() ? json(nullptr) : json(erro)},
                       {"modelo", config["modelo"]},
                       {"padroes", config["padroes"]},
                       {"log", ultimasLinhas(40)}});
    });

    servidor.Post("/api/gerar", [](const httplib::Request& req, httplib::Response& res) {
        json pedido = montarPedido(lerCorpo(req));
        json trabalho = chamarMotor("POST", "/sdcpp/v1/img_gen", paraMotor(pedido));
        std::string id = trabalho["id"].get<std::string>();
        {
            std::lock_guard trava(mutexPedidos);
            pedidos[id] = pedido;
        }
        responderJson(res, 202, {{"id", id}});
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
        if (t.contains("error") && t["error"].is_object()) resposta["erro"] = t["error"].value("message", "");

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
        std::string mensagem = "Erro interno no gerador.";
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
    if (processoMotor) TerminateProcess(processoMotor, 0);
    return TRUE;
}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // --sem-navegador: não abre o navegador sozinho (útil para testes)
    bool abrirNavegador = !(argc > 1 && std::string(argv[1]) == "--sem-navegador");
    SetConsoleTitleW(L"Gerador de Imagens");

    pastas.raiz = pastaDoExecutavel();
    pastas.motor = pastas.raiz / "motor";
    pastas.modelos = pastas.raiz / "modelos";
    pastas.galeria = pastas.raiz / "galeria";
    pastas.publico = pastas.raiz / "public";
    fs::create_directories(pastas.galeria);

    std::ifstream arquivoConfig(pastas.raiz / "config.json");
    config = json::parse(arquivoConfig, nullptr, false);
    if (config.is_discarded()) {
        std::cerr << "Não consegui ler o config.json em " << u8(pastas.raiz) << "\n";
        system("pause");
        return 1;
    }

    httplib::Server servidor;
    servidorAtivo = &servidor;
    configurarRotas(servidor);
    SetConsoleCtrlHandler(aoFecharConsole, TRUE);

    int porta = config["portaApp"].get<int>();
    if (!servidor.bind_to_port("127.0.0.1", porta)) {
        std::cerr << "A porta " << porta << " já está em uso. O gerador já está aberto em outra janela?\n";
        system("pause");
        return 1;
    }

    std::string endereco = std::format("http://localhost:{}", porta);
    std::cout << "Gerador de Imagens aberto em " << endereco << "\n"
              << "Para fechar, feche esta janela.\n\n";

    if (ligarMotor()) std::thread(esperarMotor).detach();
    if (abrirNavegador) ShellExecuteA(nullptr, "open", endereco.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    servidor.listen_after_bind();
    if (processoMotor) TerminateProcess(processoMotor, 0);
    return 0;
}
