#include <WiFi.h>
#include <PubSubClient.h>
#include "DHTesp.h"

// Pinos usados no projeto
#define PINO_LED_VERDE      25
#define PINO_LED_AMARELO    27
#define PINO_LED_VERMELHO   26
#define PINO_BUZZER         33
#define PINO_BOTAO          14

#define PINO_LDR            34   
#define PINO_POT            35   
#define PINO_DHT            15   

// Score do foco
int score = 100;

// Controle de tempo para atualizar o score
unsigned long ultimoUpdateScore = 0;
const unsigned long INTERVALO_SCORE_MS = 5000;

// Controle do botão (pausa)
unsigned long ultimoTempoPausa = 0;
bool ultimoEstadoBotao = HIGH;

// Limites usados pra penalizar
const unsigned long LIMITE_SEM_PAUSA_SEG = 30;
const float TEMP_LIMITE_ALTA = 28.0;
const int LDR_LIMITE_ESCURO  = 2000;
const int RUIDO_LIMITE_ALTO  = 3000;

DHTesp dht;

// WiFi/MQTT do Wokwi
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

const char* MQTT_BROKER = "test.mosquitto.org";
const int   MQTT_PORT   = 1883;

const char* MQTT_TOPIC_SENSOR = "mindbreak/sensor";
const char* MQTT_TOPIC_ALERTA = "mindbreak/alerta";

WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Conecta no WiFi do Wokwi
void conectaWiFi() {
  Serial.print("Conectando ao WiFi ");
  Serial.print(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi conectado!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());
}

// Conecta no broker MQTT
void conectaMQTT() {
  while (!mqttClient.connected()) {
    Serial.print("Conectando ao broker MQTT...");
    String clientId = "MindBreak-" + String(random(0xffff), HEX);

    if (mqttClient.connect(clientId.c_str())) {
      Serial.println("conectado!");
    } else {
      Serial.print("falha (rc=");
      Serial.print(mqttClient.state());
      Serial.println("), tentando de novo...");
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(PINO_LED_VERDE, OUTPUT);
  pinMode(PINO_LED_AMARELO, OUTPUT);
  pinMode(PINO_LED_VERMELHO, OUTPUT);
  pinMode(PINO_BUZZER, OUTPUT);

  pinMode(PINO_BOTAO, INPUT_PULLUP);

  dht.setup(PINO_DHT, DHTesp::DHT22);

  ultimoTempoPausa = millis();

  conectaWiFi();
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  conectaMQTT();

  Serial.println("Mind Break iniciado!");
}

void loop() {
  if (!mqttClient.connected()) conectaMQTT();
  mqttClient.loop();

  unsigned long agora = millis();

  // Detecta clique do botão
  bool estadoBotao = (digitalRead(PINO_BOTAO) == LOW);
  if (estadoBotao && !ultimoEstadoBotao) {
    ultimoTempoPausa = agora;
    score = 100; // reset total do score
    Serial.println(">>> Pausa registrada (score voltou pra 100)");
  }
  ultimoEstadoBotao = estadoBotao;

  // Atualiza o score a cada 5s
  if (agora - ultimoUpdateScore >= INTERVALO_SCORE_MS) {
    ultimoUpdateScore = agora;

    TempAndHumidity clima = dht.getTempAndHumidity();
    bool dhtOk = !(isnan(clima.temperature) || isnan(clima.humidity));

    int luz = analogRead(PINO_LDR);
    int ruido = analogRead(PINO_POT);

    unsigned long tempoSemPausa_seg = (agora - ultimoTempoPausa) / 1000;

    int scoreAntes = score;
    int delta = 0;

    if (dhtOk && clima.temperature > TEMP_LIMITE_ALTA) delta -= 3;
    if (luz < LDR_LIMITE_ESCURO) delta -= 2;
    if (ruido > RUIDO_LIMITE_ALTO) delta -= 3;
    if (tempoSemPausa_seg > LIMITE_SEM_PAUSA_SEG) delta -= 5;

    // Recupera um pouco se tudo estiver ok
    if (delta == 0 && score < 100) {
      bool tempOk = (!dhtOk) || (clima.temperature <= TEMP_LIMITE_ALTA);
      bool luzOk = (luz >= LDR_LIMITE_ESCURO);
      bool ruidoOk = (ruido <= RUIDO_LIMITE_ALTO);
      bool pausaOk = (tempoSemPausa_seg <= LIMITE_SEM_PAUSA_SEG);

      if (tempOk && luzOk && ruidoOk && pausaOk) delta += 1;
    }

    score += delta;
    if (score < 0) score = 0;
    if (score > 100) score = 100;

    // Debug pra explicar o que aconteceu
    Serial.println("------ CICLO SCORE ------");

    if (dhtOk) {
      Serial.print("Temperatura: ");
      Serial.print(clima.temperature);
      Serial.print(" °C");
      Serial.println(clima.temperature > TEMP_LIMITE_ALTA ? "  (alta)" : "  (ok)");
    }

    Serial.print("Luz (LDR): ");
    Serial.print(luz);
    Serial.println(luz < LDR_LIMITE_ESCURO ? "  (escuro)" : "  (ok)");

    Serial.print("Ruído: ");
    Serial.print(ruido);
    Serial.println(ruido > RUIDO_LIMITE_ALTO ? "  (alto)" : "  (ok)");

    Serial.print("Tempo sem pausa: ");
    Serial.print(tempoSemPausa_seg);
    Serial.println(tempoSemPausa_seg > LIMITE_SEM_PAUSA_SEG ? "  (muito tempo)" : "  (ok)");

    Serial.print("Score anterior: ");
    Serial.println(scoreAntes);
    Serial.print("Variação: ");
    Serial.println(delta);
    Serial.print("Score novo: ");
    Serial.println(score);
    Serial.println("-------------------------\n");

    // Monta JSON para o MQTT
    const char* alerta;
    if (score >= 70) alerta = "OK";
    else if (score >= 40) alerta = "ATENCAO";
    else alerta = "CRITICO";

    char payload[256];
    if (dhtOk) {
      snprintf(payload, sizeof(payload),
        "{ \"temperatura\": %.2f, \"luz\": %d, \"ruido\": %d, \"tempoSemPausa\": %lu, \"score\": %d, \"alerta\": \"%s\" }",
        clima.temperature, luz, ruido, tempoSemPausa_seg, score, alerta
      );
    } else {
      snprintf(payload, sizeof(payload),
        "{ \"temperatura\": null, \"luz\": %d, \"ruido\": %d, \"tempoSemPausa\": %lu, \"score\": %d, \"alerta\": \"%s\" }",
        luz, ruido, tempoSemPausa_seg, score, alerta
      );
    }

    mqttClient.publish(MQTT_TOPIC_SENSOR, payload);

    if (score < 40) {
      mqttClient.publish(MQTT_TOPIC_ALERTA, "ALERTA: score baixo, sugerir pausa");
    }
  }

  // LEDs pelo nível do score
  digitalWrite(PINO_LED_VERDE,   score >= 70);
  digitalWrite(PINO_LED_AMARELO, score >= 40 && score < 70);
  digitalWrite(PINO_LED_VERMELHO, score < 40);

  // Buzzer só no caso crítico
  digitalWrite(PINO_BUZZER, score < 20);

  delay(50);
}
