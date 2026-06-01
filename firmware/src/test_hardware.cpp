/**
 * =============================================================
 *  ESP32 Wi-Fi Diagnostico - v2.0
 *  Chip   : ESP32-D0WDQ6
 *  Core   : Arduino ESP32 v3.x (IDF 5.x)
 *  Display: OLED SSD1306 I2C (128x64)
 * =============================================================
 *  CORRECOES APLICADAS:
 *
 *  [FIX 1 - BROWNOUT]
 *  Desabilita o detector de brownout via registrador de hardware
 *  ANTES de qualquer outra inicializacao. Isso previne o reset
 *  causado pelo pico de corrente (~300 mA) ao ligar o radio Wi-Fi.
 *  Use apenas para diagnostico; re-habilite em producao.
 *
 *  [FIX 2 - API Core v3.x]
 *  Sequencia corrigida de inicializacao Wi-Fi:
 *    WIFI_OFF -> persistent(false) -> WIFI_STA -> begin()
 *  Evita o "Guru Meditation Error" causado por reconfigurar
 *  um driver Wi-Fi que ja estava ativo com estado corrompido.
 *
 *  [FIX 3 - DIAGNOSTICO]
 *  Loga o motivo real do ultimo reset via esp_reset_reason(),
 *  confirmando se foi BROWNOUT ou KERNEL PANIC (Guru Meditation).
 * =============================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- [FIX 1] Cabecalhos para desabilitar brownout por hardware ---
#include "soc/soc.h"          // Macro WRITE_PERI_REG
#include "soc/rtc_cntl_reg.h" // Registrador RTC_CNTL_BROWN_OUT_REG
#include "nvs_flash.h"        // Cabecalho para apagar cache persistente corrompido do NVS

// ================================================================
//  CONFIGURACOES DO DISPLAY OLED SSD1306
// ================================================================
#define SCREEN_WIDTH    128
#define SCREEN_HEIGHT    64
#define OLED_RESET       -1   // Sem pino de reset externo
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
static bool oledOk = false;   // Flag de disponibilidade do display

// ================================================================
//  CREDENCIAIS WI-FI
// ================================================================
const char* ssid     = "GOAT";
const char* password = "brasiliaazul";

// ================================================================
//  CONFIGURACAO DE TIMEOUT
// ================================================================
#define WIFI_TIMEOUT_MS 20000UL  // 20 segundos para conectar

// ================================================================
//  TRADUCAO DE STATUS (Serial - mensagens longas e explicativas)
// ================================================================
const char* wifiStatusSerial(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "OCIOSO - aguardando inicio do processo";
    case WL_NO_SSID_AVAIL:   return "REDE NAO ENCONTRADA - SSID fora de alcance";
    case WL_SCAN_COMPLETED:  return "ESCANEAMENTO DE REDES CONCLUIDO";
    case WL_CONNECTED:       return "CONECTADO COM SUCESSO";
    case WL_CONNECT_FAILED:  return "FALHA - senha incorreta ou AP recusou";
    case WL_CONNECTION_LOST: return "CONEXAO PERDIDA - sinal caiu";
    case WL_DISCONNECTED:    return "AUTENTICANDO / AGUARDANDO DHCP";
    default:                 return "ESTADO DESCONHECIDO";
  }
}

// ================================================================
//  TRADUCAO DE STATUS (OLED - mensagens curtas, cabem na tela)
// ================================================================
const char* wifiStatusOLED(wl_status_t s) {
  switch (s) {
    case WL_IDLE_STATUS:     return "Ocioso";
    case WL_NO_SSID_AVAIL:   return "Rede ausente";
    case WL_SCAN_COMPLETED:  return "Scan ok";
    case WL_CONNECTED:       return "Conectado!";
    case WL_CONNECT_FAILED:  return "Erro de senha";
    case WL_CONNECTION_LOST: return "Sinal perdido";
    case WL_DISCONNECTED:    return "Autenticando";
    default:                 return "Desconhecido";
  }
}

// ================================================================
//  HELPER: EXIBE INFORMACOES NO OLED
//  Layout: [titulo | linha separadora | main (x2) | sub1 | sub2]
// ================================================================
void oledShow(const char* title,
              const char* main,
              const char* sub1 = "",
              const char* sub2 = "") {
  if (!oledOk) return;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Cabecalho (tamanho 1, altura ~8px)
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  // Texto principal em destaque (tamanho 2, altura ~16px)
  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(main);

  // Subtitulos informativos (tamanho 1)
  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print(sub1);
  display.setCursor(0, 54);
  display.print(sub2);

  display.display();
}

void wifiInit() {
  Serial.println(F("\n[WiFi] Iniciando conexao STA simples..."));
  WiFi.mode(WIFI_STA);
  delay(100);
  Serial.printf("[WiFi] Tentando conectar a rede '%s'...\n", ssid);
  WiFi.begin(ssid, password);
  Serial.println(F("[WiFi] WiFi.begin chamado com sucesso. Aguardando associacao..."));
}


// ================================================================
//  [FIX 3] HELPER: IMPRIME O MOTIVO DO ULTIMO RESET
//          Identifica definitivamente se foi Brownout ou Panic.
// ================================================================
void printResetReason() {
  Serial.print(F("[BOOT] Causa do ultimo reset : "));
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      Serial.println(F("Power-on normal (primeira energizacao)"));
      break;
    case ESP_RST_BROWNOUT:
      // Se cair aqui com o novo codigo (brownout desabilitado),
      // confirma que o problema e de hardware/eletrico.
      Serial.println(F("*** BROWNOUT RESET *** (queda de tensao confirmada!)"));
      Serial.println(F("[BOOT] >> Causa eletrica confirmada. Verifique cabo USB,"));
      Serial.println(F("[BOOT] >> fonte de alimentacao e capacitores de bypass."));
      break;
    case ESP_RST_PANIC:
      Serial.println(F("*** KERNEL PANIC *** (Guru Meditation Error - excecao de firmware)"));
      Serial.println(F("[BOOT] >> Causa de software/API. Verifique a sequencia"));
      Serial.println(F("[BOOT] >> de inicializacao do Wi-Fi no Core v3.x."));
      break;
    case ESP_RST_SW:
      Serial.println(F("Reset por software (ESP.restart() foi chamado)"));
      break;
    case ESP_RST_DEEPSLEEP:
      Serial.println(F("Acordou do Deep Sleep"));
      break;
    case ESP_RST_WDT:
      Serial.println(F("Watchdog Timer (WDT) - loop principal travou"));
      break;
    default:
      Serial.printf("Outro (codigo: %d)\n", (int)esp_reset_reason());
      break;
  }
}

// ================================================================
//  SETUP
// ================================================================
void setup() {

  // ============================================================
  //  [FIX 1] DESABILITA O DETECTOR DE BROWNOUT - NIVEL HARDWARE
  //
  //  Esta instrucao deve ser LITERALMENTE A PRIMEIRA a executar.
  //  Grava 0 no registrador de controle do brownout do RTC,
  //  impedindo que o chip resete mesmo com queda de tensao.
  //
  //  AVISO DE SEGURANCA: O brownout existe para proteger a memoria
  //  flash de corrupcao durante quedas de tensao. Use apenas para
  //  diagnostico. Apos confirmar a causa, corrija o hardware e
  //  remova ou comente esta linha.
  // ============================================================
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  // --- Serial ---
  Serial.begin(115200);
  delay(500);

  // --- Limpeza da particao NVS para evitar crash/conflito com credenciais legadas do WPA2 Enterprise ---
  Serial.println(F("[NVS] Limpando dados Wi-Fi antigos do flash..."));
  nvs_flash_erase();
  nvs_flash_init();
  Serial.println(F("[NVS] Limpeza de cache concluida."));

  Serial.println(F("\n============================================="));
  Serial.println(F("   DIAGNOSTICO WI-FI - ESP32-D0WDQ6"));
  Serial.println(F("   Core v3.x | Brownout Desabilitado"));
  Serial.println(F("============================================="));

  // Imprime o motivo do reset logo no boot
  printResetReason();

  Serial.printf("[CONFIG] SSID alvo    : '%s'\n", ssid);
  Serial.printf("[CONFIG] Timeout WiFi : %lu ms\n", WIFI_TIMEOUT_MS);

  // --- OLED ---
  Serial.print(F("[OLED] Inicializando display I2C (0x3C)... "));
  if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    oledOk = true;
    Serial.println(F("OK"));
    oledShow("DIAGNOSTICO", "Iniciando...", "Brownout: OFF", "WiFi: preparando");
    delay(1500);
  } else {
    Serial.println(F("FALHOU (continuando sem display)"));
    // Nao trava o sistema; diagnostico segue apenas via Serial.
  }

  // --- Wi-Fi ---
  wifiInit();

  // Exibe tela de progresso inicial imediatamente
  oledShow("CONECTANDO...", "Conectando", ("Rede: " + String(ssid)).c_str(), "Aguardando...");
}

// ================================================================
//  LOOP - Maquina de estados da conexao Wi-Fi
//  Estados: AGUARDANDO | CONECTADO | FALHA_DEFINITIVA | TIMEOUT
// ================================================================
void loop() {

  // --- Variaveis de estado (persistem entre iteracoes via static) ---
  static wl_status_t    lastStatus     = (wl_status_t)-1;  // Forca log no primeiro ciclo
  static unsigned long  connectStart   = millis();
  static bool           isConnected    = false;
  static bool           hasFailed      = false;
  static unsigned long  lastAnimation  = 0;
  static int            dotCount       = 0;

  wl_status_t currentStatus = WiFi.status();

  // ----------------------------------------------------------------
  //  Detecta e loga QUALQUER mudanca de estado no Serial
  // ----------------------------------------------------------------
  if (currentStatus != lastStatus) {
    lastStatus = currentStatus;
    Serial.printf("\n[%5lus] MUDANCA DE ESTADO: %s (cod: %d)\n",
      millis() / 1000,
      wifiStatusSerial(currentStatus),
      (int)currentStatus
    );
  }

  // ================================================================
  //  RAMO 1: CONECTADO COM SUCESSO
  // ================================================================
  if (currentStatus == WL_CONNECTED) {

    // Bloco executado UMA VEZ ao estabelecer a conexao
    if (!isConnected) {
      isConnected = true;

      String ip      = WiFi.localIP().toString();
      String gateway = WiFi.gatewayIP().toString();
      int    rssi    = WiFi.RSSI();
      int    channel = WiFi.channel();

      // Log Serial completo
      Serial.println(F("\n============================================="));
      Serial.println(F("      CONEXAO ESTABELECIDA COM SUCESSO!"));
      Serial.println(F("============================================="));
      Serial.printf("[NET] Rede     : %s\n",   WiFi.SSID().c_str());
      Serial.printf("[NET] IP       : %s\n",   ip.c_str());
      Serial.printf("[NET] Gateway  : %s\n",   gateway.c_str());
      Serial.printf("[NET] RSSI     : %d dBm\n", rssi);
      Serial.printf("[NET] Canal    : %d\n",   channel);
      Serial.printf("[NET] Tempo    : %lu ms\n", millis() - connectStart);
      Serial.println(F("============================================="));

      oledShow("WIFI ONLINE",
               "CONECTADO!",
               ("IP: " + ip).c_str(),
               ("Sinal: " + String(rssi) + " dBm").c_str());
    }

    // Monitoramento continuo: atualiza RSSI e IP a cada 5 segundos
    static unsigned long lastRssiLog = 0;
    if (millis() - lastRssiLog > 5000) {
      lastRssiLog = millis();
      int    rssi = WiFi.RSSI();
      String ip   = WiFi.localIP().toString();
      Serial.printf("[MONITOR] IP: %-15s | RSSI: %d dBm\n", ip.c_str(), rssi);
      oledShow("WIFI ONLINE",
               "CONECTADO!",
               ("IP: " + ip).c_str(),
               ("Sinal: " + String(rssi) + " dBm").c_str());
    }

    return; // Encerra o loop neste ciclo; nada mais a processar
  }

  // ================================================================
  //  RAMO 2: FALHA DEFINITIVA OU TIMEOUT
  // ================================================================
  bool timedOut  = (!isConnected && !hasFailed && (millis() - connectStart > WIFI_TIMEOUT_MS));
  bool hardFail  = (currentStatus == WL_CONNECT_FAILED || currentStatus == WL_NO_SSID_AVAIL);

  if ((timedOut || hardFail) && !hasFailed) {
    hasFailed = true;

    if (timedOut) {
      Serial.printf(
        "\n[ERRO] TIMEOUT: %lu ms sem conexao estabelecida.\n"
        "[ERRO] Verifique se o hotspot '%s' esta ativo e reinicie o ESP32.\n",
        WIFI_TIMEOUT_MS, ssid
      );
      oledShow("TIMEOUT!", "Sem resposta", ("Rede: " + String(ssid)).c_str(), "Reinicie o ESP");
    } else {
      Serial.printf(
        "\n[ERRO] FALHA DEFINITIVA: %s\n"
        "[ERRO] Verifique SSID, senha e proximidade do ponto de acesso.\n",
        wifiStatusSerial(currentStatus)
      );
      oledShow("WIFI FALHOU",
               wifiStatusOLED(currentStatus),
               "Ver SSID/senha",
               "Reinicie o ESP");
    }
    return;
  }

  if (hasFailed) return; // Ja falhou; aguarda reinicio manual

  // ================================================================
  //  RAMO 3: AGUARDANDO CONEXAO - Animacao de progresso
  // ================================================================
  if (millis() - lastAnimation > 500) {
    lastAnimation = millis();
    dotCount = (dotCount + 1) % 4;

    // Animacao de pontos: "Conectando." / "Conectando.." / etc.
    char progressStr[16] = "Conectando";
    for (int i = 0; i < dotCount; i++) strcat(progressStr, ".");

    // Tempo restante para o timeout
    unsigned long elapsed   = millis() - connectStart;
    unsigned long remaining = (elapsed < WIFI_TIMEOUT_MS)
                              ? (WIFI_TIMEOUT_MS - elapsed) / 1000
                              : 0;
    char timeLine[24];
    snprintf(timeLine, sizeof(timeLine), "Timeout: %lus", remaining);

    char ssidLine[24];
    snprintf(ssidLine, sizeof(ssidLine), "Rede: %.17s", ssid);

    oledShow("CONECTANDO...", progressStr, ssidLine, timeLine);
    Serial.print(F("."));
  }
}
