#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h> // Para desserializar o estado e serializar os inputs

// [FIX] Cabecalhos para desabilitar o detector de brownout por hardware
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// 1. Configurações da tela OLED SSD1306 via I2C
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
const uint32_t OLED_I2C_CLOCK_HZ = 400000;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// 2. Definição dos pinos físicos dos botões (Pull-up interno)
#define PIN_BTN_UP   12
#define PIN_BTN_DOWN 14

// 3. Configurações de Conectividade
#ifndef WIFI_SSID
#define WIFI_SSID "GOAT"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "brasiliaazul"
#endif

const char* wifiSSID = WIFI_SSID;
const char* wifiPass = WIFI_PASSWORD;
const char* wsHost   = "se.maiconda.com";
const uint16_t wsPort = 443;
const char* wsPath   = "/ws";

const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
const unsigned long WIFI_RECONNECT_INTERVAL_MS = 10000UL;
const unsigned long WIFI_STATUS_LOG_INTERVAL_MS = 1000UL;

// Instâncias Globais
WebSocketsClient webSocket;
String playerSide = "";      // "left" ou "right" (atribuído pelo servidor)
String gameState = "waiting_players"; // Estado atual do jogo
bool wifiReady = false;
bool webSocketStarted = false;
wl_status_t lastWiFiStatus = (wl_status_t)-1;
unsigned long lastWiFiReconnectAttempt = 0;

// Variáveis para otimização de envio de mensagens e debouncing dos botões
int lastSentDir = 0;         // Guarda a última direção enviada (-1, 1, 0)
bool lastUpState = HIGH;     // Estado anterior do botão Cima (HIGH = solto)
bool lastDownState = HIGH;   // Estado anterior do botão Baixo

// Função para exibir textos formatados simples no visor OLED
void drawText(const char* title, const char* line1 = "", const char* line2 = "", const char* line3 = "") {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Desenha Título
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(title);
  display.println("---------------------");

  // Linhas de Conteúdo
  display.setCursor(0, 20);
  display.println(line1);
  display.setCursor(0, 35);
  display.println(line2);
  display.setCursor(0, 50);
  display.println(line3);
  
  display.display();
}

// Renderiza a partida de Ping Pong em tempo real na resolução de 128x64 (Sem Placar)
void drawActiveGame(int bx, int by, int p1, int p2) {
  display.clearDisplay();
  
  // 1. Linha central tracejada
  for (int y = 0; y < SCREEN_HEIGHT; y += 4) {
    display.drawFastVLine(64, y, 2, SSD1306_WHITE);
  }

  // 2. Raquete Esquerda (Player 1) - largura 3, altura 16
  display.fillRect(2, p1, 3, 16, SSD1306_WHITE);

  // 3. Raquete Direita (Player 2) - largura 3, altura 16
  display.fillRect(123, p2, 3, 16, SSD1306_WHITE);

  // 4. Bola (Quadrado de 4x4)
  display.fillRect(bx, by, 4, 4, SSD1306_WHITE);

  display.display();
}

// Desenha a tela de congelamento pós-ponto (Mostrando Placar Gigante)
void drawPointScored(int s1, int s2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Placar Gigante centralizado
  display.setTextSize(2);
  display.setCursor(35, 10);
  display.printf("%d - %d", s1, s2);

  // Mensagem auxiliar
  display.setTextSize(1);
  display.setCursor(15, 45);
  display.println("PONTO MARCADO!");
  
  display.display();
}

// Desenha a tela de Game Over e Vencedor
void drawGameOver(int s1, int s2) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Título e Placar Final
  display.setTextSize(1);
  display.setCursor(30, 0);
  display.println("FIM DE JOGO");
  display.setCursor(45, 15);
  display.printf("[%d - %d]", s1, s2);

  // Determina e exibe o vencedor
  display.setTextSize(1);
  display.setCursor(10, 40);
  if (s1 >= 11 && (s1 - s2) >= 2) {
    display.println("VENCEDOR: ESQUERDA");
  } else {
    display.println("VENCEDOR: DIREITA");
  }
  
  display.display();
}

// Envia comando JSON para o WebSocket do servidor Go
void sendWSMessage(String jsonPayload) {
  if (!wifiReady || !webSocketStarted) {
    Serial.println("[WS] Envio ignorado: Wi-Fi/WebSocket offline.");
    return;
  }

  webSocket.sendTXT(jsonPayload);
}

// Callback de processamento de eventos do WebSocket
void webSocketEvent(WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      Serial.println("[WS] Status: Desconectado.");
      gameState = "waiting_players";
      if (wifiReady) {
        String ipLine = "IP: " + WiFi.localIP().toString();
        drawText("PONG MULTIPLAYER", "Status: Offline", "Tentando conectar...", ipLine.c_str());
      } else {
        drawText("PONG MULTIPLAYER", "Status: Offline", "Wi-Fi indisponivel", "Reconectando...");
      }
      break;

    case WStype_CONNECTED:
      Serial.println("[WS] Status: Conectado!");
      drawText("PONG MULTIPLAYER", "Status: Conectado", "Aguardando setup...");
      break;

    case WStype_TEXT: {
      // Faz o parsing do JSON recebido do servidor Go
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        Serial.print("Erro no JSON: ");
        Serial.println(error.c_str());
        return;
      }

      String msgType = doc["type"] | "";

      // 1. Mensagem de Configuração Inicial (Lado do jogador)
      if (msgType == "setup") {
        playerSide = doc["side"] | "";
        Serial.printf("[WS] Setup recebido! Lado: %s\n", playerSide.c_str());
        drawText("PONG MULTIPLAYER", "Lado Atribuido:", playerSide.equalsIgnoreCase("left") ? "ESQUERDA" : "DIREITA");
      } 
      // 2. Mensagem de Frame e Estado do Jogo
      else if (msgType == "state") {
        gameState = doc["state"] | "waiting_players";
        
        int bx = doc["bx"] | 0;
        int by = doc["by"] | 0;
        int p1 = doc["p1"] | 0;
        int p2 = doc["p2"] | 0;
        int s1 = doc["s1"] | 0;
        int s2 = doc["s2"] | 0;
        bool p1Ready = doc["p1_ready"] | false;
        bool p2Ready = doc["p2_ready"] | false;

        // Renderiza na tela do OLED SSD1306 baseado na Máquina de Estados recebida do Go
        if (gameState == "waiting_players") {
          drawText("PONG MULTIPLAYER", "Lado Atribuido:", playerSide.equalsIgnoreCase("left") ? "ESQUERDA" : "DIREITA", "AGUARDANDO P2...");
        } 
        else if (gameState == "waiting_ready") {
          // Renderiza tela de Lobby com Prontidão dos dois lados
          char line1[30];
          char line2[30];
          sprintf(line1, "VOCE (%s): %s", playerSide.equalsIgnoreCase("left") ? "ESQ" : "DIR", 
                  (playerSide.equalsIgnoreCase("left") ? p1Ready : p2Ready) ? "PRONTO" : "ESPERANDO");
          sprintf(line2, "RIVAL: %s", 
                  (playerSide.equalsIgnoreCase("left") ? p2Ready : p1Ready) ? "PRONTO" : "ESPERANDO");
          
          drawText("LOBBY: APERTE BOTAO", line1, line2, "Clique p/ Ready!");
        } 
        else if (gameState == "playing") {
          // Gameplay ativo: OCULTA O PLACAR (Tela limpa)
          drawActiveGame(bx, by, p1, p2);
        } 
        else if (gameState == "point_scored") {
          // Ponto marcado: Mostra o placar gigante na tela
          drawPointScored(s1, s2);
        } 
        else if (gameState == "gameover") {
          // Fim de jogo: Mostra vencedor
          drawGameOver(s1, s2);
        }
      }
      break;
    }
    
    case WStype_ERROR:
      Serial.println("[WS] Erro ocorrido na conexao.");
      break;
  }
}

const char* wifiStatusName(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "WL_IDLE_STATUS - aguardando inicio";
    case WL_NO_SSID_AVAIL:   return "WL_NO_SSID_AVAIL - SSID nao encontrado";
    case WL_SCAN_COMPLETED:  return "WL_SCAN_COMPLETED - scan concluido";
    case WL_CONNECTED:       return "WL_CONNECTED - conectado";
    case WL_CONNECT_FAILED:  return "WL_CONNECT_FAILED - senha/AP recusou";
    case WL_CONNECTION_LOST: return "WL_CONNECTION_LOST - conexao perdida";
    case WL_DISCONNECTED:    return "WL_DISCONNECTED - desconectado";
    default:                 return "WL_UNKNOWN - estado desconhecido";
  }
}

const char* wifiStatusShort(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "Ocioso";
    case WL_NO_SSID_AVAIL:   return "SSID ausente";
    case WL_SCAN_COMPLETED:  return "Scan OK";
    case WL_CONNECTED:       return "Conectado";
    case WL_CONNECT_FAILED:  return "Falha auth";
    case WL_CONNECTION_LOST: return "Sinal caiu";
    case WL_DISCONNECTED:    return "Desconectado";
    default:                 return "Desconhecido";
  }
}

void logWiFiStatus(wl_status_t status, bool force = false) {
  if (!force && status == lastWiFiStatus) {
    return;
  }

  lastWiFiStatus = status;
  Serial.printf("[WIFI] Estado: %s (cod: %d)\n", wifiStatusName(status), (int)status);
}

void drawWiFiConnected() {
  String ipLine = "IP: " + WiFi.localIP().toString();
  String rssiLine = "RSSI: " + String(WiFi.RSSI()) + " dBm";
  drawText("PONG MULTIPLAYER", "Wi-Fi: conectado", ipLine.c_str(), rssiLine.c_str());
}

void logWiFiDetails() {
  String ip = WiFi.localIP().toString();
  String gateway = WiFi.gatewayIP().toString();

  Serial.println("[WIFI] Conexao estabelecida.");
  Serial.printf("[WIFI] SSID    : %s\n", WiFi.SSID().c_str());
  Serial.printf("[WIFI] IP      : %s\n", ip.c_str());
  Serial.printf("[WIFI] Gateway : %s\n", gateway.c_str());
  Serial.printf("[WIFI] RSSI    : %d dBm\n", WiFi.RSSI());
  Serial.printf("[WIFI] Canal   : %d\n", WiFi.channel());
}

bool connectWiFi() {
  Serial.println();
  Serial.println("[WIFI] Inicializando cliente Wi-Fi (STA)...");
  Serial.printf("[WIFI] SSID alvo: %s\n", wifiSSID);
  Serial.printf("[WIFI] Timeout : %lu ms\n", WIFI_CONNECT_TIMEOUT_MS);
  drawText("PONG MULTIPLAYER", "Wi-Fi: iniciando", wifiSSID, "Aguarde...");

  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  delay(100);

  WiFi.begin(wifiSSID, wifiPass);
  lastWiFiReconnectAttempt = millis();

  unsigned long start = millis();
  unsigned long lastProgressLog = 0;

  while (millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    wl_status_t status = WiFi.status();
    logWiFiStatus(status);

    if (status == WL_CONNECTED) {
      wifiReady = true;
      logWiFiDetails();
      drawWiFiConnected();
      return true;
    }

    if (millis() - lastProgressLog >= WIFI_STATUS_LOG_INTERVAL_MS) {
      lastProgressLog = millis();
      Serial.printf("[WIFI] Aguardando conexao... %lus/%lus | %s\n",
                    (millis() - start) / 1000,
                    WIFI_CONNECT_TIMEOUT_MS / 1000,
                    wifiStatusShort(status));
    }

    delay(250);
  }

  wl_status_t status = WiFi.status();
  logWiFiStatus(status, true);
  Serial.printf("[WIFI] Timeout conectando em '%s'. Ultimo estado: %s\n",
                wifiSSID,
                wifiStatusName(status));
  Serial.println("[WIFI] Verifique SSID, senha, sinal e alimentacao do ESP32.");
  drawText("PONG MULTIPLAYER", "Wi-Fi: falhou", wifiStatusShort(status), "Tentando novamente");
  wifiReady = false;
  return false;
}

void startWebSocket() {
  if (!wifiReady || webSocketStarted) {
    return;
  }

  Serial.printf("[WS] Conectando em wss://%s:%u%s\n", wsHost, (unsigned)wsPort, wsPath);
  drawText("PONG MULTIPLAYER", "Wi-Fi: OK", "Conectando WS...");
  webSocket.beginSSL(wsHost, wsPort, wsPath);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);
  webSocketStarted = true;
}

void maintainWiFi() {
  wl_status_t status = WiFi.status();
  logWiFiStatus(status);

  if (status == WL_CONNECTED) {
    if (!wifiReady) {
      wifiReady = true;
      Serial.println("[WIFI] Reconexao concluida.");
      logWiFiDetails();
      drawWiFiConnected();
      startWebSocket();
    }
    return;
  }

  if (wifiReady) {
    wifiReady = false;
    gameState = "waiting_players";
    Serial.println("[WIFI] Conexao perdida. WebSocket sera reiniciado apos o Wi-Fi voltar.");
    drawText("PONG MULTIPLAYER", "Wi-Fi: offline", wifiStatusShort(status), "Reconectando...");

    if (webSocketStarted) {
      webSocket.disconnect();
      webSocketStarted = false;
    }
  }

  if (millis() - lastWiFiReconnectAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWiFiReconnectAttempt = millis();
    Serial.printf("[WIFI] Tentando reconectar em '%s'...\n", wifiSSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSSID, wifiPass);
  }
}

void setup() {
  // [FIX 1] Desabilita o detector de brownout antes de qualquer outra instrução.
  // Evita o reset causado pelo pico de corrente (~300mA) ao ligar o rádio Wi-Fi.
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(100);

  // Inicializa botões físicos com pull-up interno
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);

  Wire.begin();
  Wire.setClock(OLED_I2C_CLOCK_HZ);
  Serial.printf("[OLED] I2C clock configurado para %lu Hz\n", (unsigned long)OLED_I2C_CLOCK_HZ);

  // Inicializa a tela OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("ERRO: Display nao encontrado!"));
    for(;;);
  }
  
  drawText("PONG MULTIPLAYER", "Iniciando...", "Buscando Wi-Fi...");

  if (connectWiFi()) {
    startWebSocket();
  } else {
    Serial.println("[WIFI] Sem conexao inicial. O loop tentara reconectar automaticamente.");
  }
}

void loop() {
  maintainWiFi();

  // Mantem a escuta do WebSocket apenas quando a rede estiver disponivel.
  if (wifiReady && webSocketStarted) {
    webSocket.loop();
  }

  // Lê os botões físicos
  bool upPressed = (digitalRead(PIN_BTN_UP) == LOW);
  bool downPressed = (digitalRead(PIN_BTN_DOWN) == LOW);

  // 1. SE ESTIVER NO LOBBY (Lógica de Pronto)
  // Ao clicar em QUALQUER botão (Cima ou Baixo), envia {"type": "ready"}
  // Usamos detecção de borda de descida (pressionar o botão) para evitar envios infinitos
  if (gameState == "waiting_ready") {
    bool upJustPressed = (upPressed && lastUpState == HIGH);
    bool downJustPressed = (downPressed && lastDownState == HIGH);

    if (upJustPressed || downJustPressed) {
      Serial.println("[BOTAO] Pressionado no Lobby! Alternando status de Pronto.");
      sendWSMessage("{\"type\":\"ready\"}");
      delay(150); // debounce simples
    }
  }

  // 2. SE ESTIVER JOGANDO (Lógica de Movimento da Raquete)
  // Envia a direção apenas quando o estado mudar para otimizar largura de banda
  else if (gameState == "playing") {
    int targetDir = 0;
    if (upPressed) {
      targetDir = -1; // Mover para Cima (decrementa Y)
    } else if (downPressed) {
      targetDir = 1;  // Mover para Baixo (incrementa Y)
    }

    // Só envia pacote se a direção mudar
    if (targetDir != lastSentDir) {
      lastSentDir = targetDir;
      
      // Constrói o JSON dinamicamente
      char payload[60];
      sprintf(payload, "{\"type\":\"input\",\"dir\":%d}", lastSentDir);
      
      Serial.printf("[INPUT] Enviando direcao: %d\n", lastSentDir);
      sendWSMessage(payload);
    }
  }

  // Guarda o estado anterior dos botões para a detecção de borda no próximo loop
  lastUpState = digitalRead(PIN_BTN_UP) ? HIGH : LOW;
  lastDownState = digitalRead(PIN_BTN_DOWN) ? HIGH : LOW;
  
  delay(10); // Intervalo confortável de ciclo de polling
}
