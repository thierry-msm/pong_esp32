#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Cabecalhos para desativar brownout por hardware
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// 1. Configuracoes do OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOk = false;

// 2. Credenciais Wi-Fi (Dados Moveis - Celular)
const char* ssid = "GOAT";
const char* password = "brasiliaazul";

// 3. Hosts para o teste de latência (Ping)
const char* gameServerUrl = "http://se.maiconda.com/"; // Seu servidor Go do jogo
const char* referenceUrl  = "http://www.google.com/";  // Servidor de referencia global

// Estrutura para consolidar resultados de latencia
struct PingResult {
  int minLatency;
  int maxLatency;
  int avgLatency;
  int successCount;
  int failCount;
};

// Exibe informações formatadas na tela OLED
void oledShow(const char* title, const char* main, const char* sub1 = "", const char* sub2 = "") {
  if (!oledOk) return;
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  // Cabecalho
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print(title);
  display.drawFastHLine(0, 10, SCREEN_WIDTH, SSD1306_WHITE);

  // Texto Principal
  display.setTextSize(2);
  display.setCursor(0, 15);
  display.print(main);

  // Subtitulos
  display.setTextSize(1);
  display.setCursor(0, 42);
  display.print(sub1);
  display.setCursor(0, 54);
  display.print(sub2);

  display.display();
}

// Executa um ciclo de pings HTTP para um determinado host
PingResult runPingTest(const char* label, const char* url, int numPings = 5) {
  PingResult res = {9999, 0, 0, 0, 0};
  long totalLatency = 0;

  Serial.println("\n=============================================");
  Serial.printf("[PING] Iniciando teste para: %s (%s)\n", label, url);
  Serial.println("=============================================");

  for (int i = 1; i <= numPings; i++) {
    // Atualiza OLED com progresso
    char progressStr[20];
    sprintf(progressStr, "Ciclo: %d/%d", i, numPings);
    oledShow("TESTANDO LATENCIA", label, progressStr, "Medindo RTT...");

    WiFiClient client;
    HTTPClient http;
    http.setTimeout(3000); // 3 segundos de timeout por requisicao

    unsigned long start = millis();
    
    // Inicia a requisicao HTTP GET para medir o RTT (Round Trip Time)
    http.begin(client, url);
    int httpCode = http.GET();
    unsigned long duration = millis() - start;

    http.end();

    if (httpCode > 0) {
      res.successCount++;
      totalLatency += duration;
      if ((int)duration < res.minLatency) res.minLatency = duration;
      if ((int)duration > res.maxLatency) res.maxLatency = duration;

      Serial.printf("[PING %d/%d] SUCESSO | RTT = %lu ms | HTTP Codigo = %d\n", i, numPings, duration, httpCode);
    } else {
      res.failCount++;
      Serial.printf("[PING %d/%d] FALHA   | Timeout/Erro | Erro Codigo = %s\n", i, numPings, HTTPClient::errorToString(httpCode).c_str());
    }

    delay(500); // Aguarda meio segundo entre pings
  }

  if (res.successCount > 0) {
    res.avgLatency = totalLatency / res.successCount;
  } else {
    res.minLatency = 0;
  }

  // Imprime relatorio detalhado no console serial
  Serial.println("---------------------------------------------");
  Serial.printf("[INFO] Relatorio final para: %s\n", label);
  Serial.printf("   - Requisicoes enviadas : %d\n", numPings);
  Serial.printf("   - Sucessos             : %d\n", res.successCount);
  Serial.printf("   - Falhas/Perdas        : %d (%.1f%% de perda)\n", res.failCount, (float)res.failCount / numPings * 100.0);
  if (res.successCount > 0) {
    Serial.printf("   - Latencia Minima      : %d ms\n", res.minLatency);
    Serial.printf("   - Latencia Maxima      : %d ms\n", res.maxLatency);
    Serial.printf("   - Latencia Media       : %d ms\n", res.avgLatency);
  } else {
    Serial.println("   - Latencia             : INDETERMINADA (sem pacotes de resposta)");
  }
  Serial.println("=============================================");

  return res;
}

void setup() {
  // Desativa Brownout detector
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);

  Serial.begin(115200);
  delay(1000);

  Serial.println("\n=============================================");
  Serial.println("   TESTE DE PING E LATENCIA DE REDE - ESP32");
  Serial.println("=============================================");

  // Inicializacao do OLED
  if (display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    oledOk = true;
    oledShow("PING TESTER", "Iniciando...", "Aguardando Wi-Fi", "Banda: 2.4 GHz");
  } else {
    Serial.println("[OLED] Falha ao iniciar display I2C.");
  }

  // Conecta ao Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("[Wi-Fi] Conectando a rede: %s...\n", ssid);

  unsigned long startConnect = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    if (millis() - startConnect > 15000) {
      Serial.println("\n[Wi-Fi] Timeout conectando ao Wi-Fi.");
      oledShow("WI-FI FALHOU", "TIMEOUT", "Verifique o celular", "Reinicie o ESP");
      while (true);
    }
  }

  Serial.println("\n[Wi-Fi] Conectado com sucesso!");
  Serial.print("[Wi-Fi] IP Local: ");
  Serial.println(WiFi.localIP());
  
  oledShow("WI-FI OK", "CONECTADO", WiFi.localIP().toString().c_str(), "Iniciando Pings...");
  delay(2000);
}

void loop() {
  // 1. Testa a latência do servidor de jogo
  PingResult gameRes = runPingTest("GAME SERVER", gameServerUrl, 5);
  delay(1000);

  // 2. Testa a latência do Google (referência)
  PingResult refRes = runPingTest("GOOGLE REF", referenceUrl, 5);
  delay(1000);

  // Exibe tela final de resultados comparativos no OLED
  char gameLine[30];
  if (gameRes.successCount > 0) {
    sprintf(gameLine, "JOGO : %d ms (%d%% loss)", gameRes.avgLatency, (int)((float)gameRes.failCount / 5 * 100));
  } else {
    strcpy(gameLine, "JOGO : FALHOU (100% loss)");
  }

  char refLine[30];
  if (refRes.successCount > 0) {
    sprintf(refLine, "GOOG : %d ms (%d%% loss)", refRes.avgLatency, (int)((float)refRes.failCount / 5 * 100));
  } else {
    strcpy(refLine, "GOOG : FALHOU (100% loss)");
  }

  oledShow("RESULTADO PINGS", "LATENCIA", gameLine, refLine);

  // Aguarda 15 segundos antes de realizar um novo ciclo completo de teste de rede
  Serial.println("\n[LOOP] Ciclo de testes concluido. Aguardando 15s para repetir...");
  delay(15000);
}
