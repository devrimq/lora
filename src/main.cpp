#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <RadioLib.h>
#include <U8g2lib.h>
#include <NimBLEDevice.h>
#include "esp_sleep.h"
#include "driver/gpio.h"

// =================================================================
// DONANIM PİNLERİ VE TANIMLAR
// =================================================================
#define DEVICE_NAME "LoRa-1"

#define I2C_SDA 5
#define I2C_SCL 6
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

#define BTN_PIN GPIO_NUM_21
#define SHORT_PRESS_MAX 1000  // Uyanıkken menü geçişi (< 1.0 sn)
#define SEND_PRESS_MIN  1500  // Uyanıkken mesaj gönderme (~1.5 sn)
#define WAKE_PRESS_MIN  1200  // Uyurken uyanma eşiği (1.2 - 2.0 sn)
#define IDLE_SLEEP_TIMEOUT 60000UL // 1 dakika (60000 ms)

#define LORA_NSS  41
#define LORA_DIO1 39
#define LORA_RST  42
#define LORA_BUSY 40
#define LORA_SCK  7
#define LORA_MISO 8
#define LORA_MOSI 9

SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_RST, LORA_BUSY);

// =================================================================
// BLE (NimBLE)
// =================================================================
#define SERVICE_UUID     "0000FF00-0000-1000-8000-00805F9B34FB"
#define CHAR_WRITE_UUID  "0000FF01-0000-1000-8000-00805F9B34FB"
#define CHAR_NOTIFY_UUID "0000FF02-0000-1000-8000-00805F9B34FB"

NimBLEServer *pServer = nullptr;
NimBLECharacteristic *pNotifyChar = nullptr;
volatile bool bleConnected = false;

#define BLE_CUSTOM_TEXT_MAX 34
portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool blePending = false;
volatile bool bleIsCustom = false;
volatile uint8_t bleRequestedID = 0;
char bleCustomText[BLE_CUSTOM_TEXT_MAX];

// =================================================================
// MESAJ VE SİSTEM DURUM DEĞİŞKENLERİ
// =================================================================
struct PredefinedMessage {
  uint8_t id;
  const char *text;
};

PredefinedMessage predefinedMessages[] = {
  {1, "Durumum Iyi"},
  {2, "Kampa Dondum"},
  {3, "Yola Ciktim"},
  {4, "Yardim Gerekli!"}
};
const int totalMessages = 4;
int currentIndex = 0;

#define QUEUE_SIZE 5
#define MAX_RETRY 3
#define ACK_TIMEOUT 4000
#define RETRY_INTERVAL 8000

struct QueuedMessage {
  bool used;
  uint8_t messageID;
  char text[32];
  uint8_t retryCount;
  unsigned long lastAttempt;
};

QueuedMessage messageQueue[QUEUE_SIZE];

String lastRxMsg = "";
float lastRxRSSI = 0;
bool screenNeedsUpdate = true;
bool isSleeping = false;
unsigned long lastActivityTime = 0;

bool waitingForAck = false;
uint8_t pendingAckID = 0;
unsigned long ackWaitStart = 0;

// Fonksiyon Prototipleri
void markActivity();
void updateScreen(const char *systemStatus = "");
void sendMessage(uint8_t messageID);
void sendCustomMessage(const String &text);
bool transmitPacket(const char *payload);
void handleIncomingPacket(String &packet);
void processQueue();
bool addToQueue(uint8_t messageID, const char *text);
void enterLightSleep();

const char *getMessageText(uint8_t id) {
  for (int i = 0; i < totalMessages; i++) {
    if (predefinedMessages[i].id == id) return predefinedMessages[i].text;
  }
  return nullptr;
}

void markActivity() {
  lastActivityTime = millis();
}

// =================================================================
// BLE CALLBACKS
// =================================================================
class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    bleConnected = true;
    screenNeedsUpdate = true;
    markActivity();
  }
  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleConnected = false;
    screenNeedsUpdate = true;
    NimBLEDevice::startAdvertising();
  }
};

class WriteCallback : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    std::string value = pChar->getValue();
    if (value.empty()) return;

    portENTER_CRITICAL(&bleMux);
    if ((uint8_t)value[0] == 0xFF && value.length() > 1) {
      bleIsCustom = true;
      size_t len = value.length() - 1;
      if (len > BLE_CUSTOM_TEXT_MAX - 1) len = BLE_CUSTOM_TEXT_MAX - 1;
      memcpy(bleCustomText, value.data() + 1, len);
      bleCustomText[len] = '\0';
    } else {
      bleIsCustom = false;
      bleRequestedID = (uint8_t)value[0];
    }
    blePending = true;
    portEXIT_CRITICAL(&bleMux);
  }
};

// =================================================================
// LIGHT SLEEP YÖNETİMİ
// =================================================================
void enterLightSleep() {
  u8g2.setPowerSave(1); // Ekranı kapat
  isSleeping = true;

  // Wakeup Kaynakları Hazırlığı
  gpio_wakeup_enable(BTN_PIN, GPIO_INTR_LOW_LEVEL);
  gpio_wakeup_enable((gpio_num_t)LORA_DIO1, GPIO_INTR_HIGH_LEVEL);
  esp_sleep_enable_gpio_wakeup();

  while (isSleeping) {
    esp_light_sleep_start();

    // Uyanma Sonrası Kontroller
    // 1. Uyanma Nedeni: LoRa'dan Mesaj Gelmesi
    if (digitalRead(LORA_DIO1) == HIGH) {
      String rxStr;
      if (radio.readData(rxStr) == RADIOLIB_ERR_NONE) {
        handleIncomingPacket(rxStr);
      }
      radio.startReceive();
      isSleeping = false; // Cihaz tamamen uyanır
      u8g2.setPowerSave(0);
      markActivity();
      screenNeedsUpdate = true;
      break;
    }

    // 2. Uyanma Nedeni: Butona Basılması
    if (digitalRead(BTN_PIN) == LOW) {
      unsigned long pressStart = millis();
      // Buton bırakılana kadar süreyi ölç
      while (digitalRead(BTN_PIN) == LOW) {
        delay(10);
      }
      unsigned long pressDuration = millis() - pressStart;

      if (pressDuration >= WAKE_PRESS_MIN) {
        // ~1.2 - 2.0 sn basıldı: Geçerli uyanma
        isSleeping = false;
        u8g2.setPowerSave(0);
        markActivity();
        screenNeedsUpdate = true;
        break;
      } else {
        // < 1.2 sn basıldı: Yanlışlıkla basma, tekrar uykudayız
        // Ekran açılmaz, döngü devam eder ve tekrar light sleep'e girer
        delay(50); // Debounce
      }
    }
  }
  
  gpio_wakeup_disable(BTN_PIN);
  gpio_wakeup_disable((gpio_num_t)LORA_DIO1);
}

// =================================================================
// SETUP & LOOP
// =================================================================
void setup() {
  Serial.begin(115200);

  Wire.begin(I2C_SDA, I2C_SCL);
  u8g2.begin();
  u8g2.setPowerSave(0);

  pinMode(BTN_PIN, INPUT_PULLUP);
  pinMode(LORA_DIO1, INPUT);

  // SPI ve LoRa Kurulumu
  SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI);
  radio.setDio2AsRfSwitch(true);
  radio.begin(868.0);
  radio.setTCXO(1.6);
  radio.startReceive();

  // BLE Kurulumu
  NimBLEDevice::init(DEVICE_NAME);
  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new ServerCallbacks());

  NimBLEService *pService = pServer->createService(SERVICE_UUID);
  NimBLECharacteristic *pWriteChar = pService->createCharacteristic(CHAR_WRITE_UUID, NIMBLE_PROPERTY::WRITE);
  pWriteChar->setCallbacks(new WriteCallback());

  pNotifyChar = pService->createCharacteristic(CHAR_NOTIFY_UUID, NIMBLE_PROPERTY::NOTIFY);

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->enableScanResponse(true);
  pAdvertising->setName(DEVICE_NAME);
  pAdvertising->start();

  for (int i = 0; i < QUEUE_SIZE; i++) messageQueue[i].used = false;

  markActivity();
  updateScreen();
}

void loop() {
  // LoRa Paketi Dinleme (Uyanık Modda)
  if (digitalRead(LORA_DIO1) == HIGH) {
    String rxStr;
    if (radio.readData(rxStr) == RADIOLIB_ERR_NONE) {
      handleIncomingPacket(rxStr);
      markActivity();
    }
    radio.startReceive();
  }

  // ACK Zaman Aşımı Kontrolü
  if (waitingForAck && (millis() - ackWaitStart > ACK_TIMEOUT)) {
    waitingForAck = false;
    const char *pendingText = (pendingAckID == 0) ? nullptr : getMessageText(pendingAckID);
    if (pendingText != nullptr) addToQueue(pendingAckID, pendingText);
    screenNeedsUpdate = true;
  }

  // BLE Mesaj İstekleri
  if (blePending) {
    bool isCustom;
    uint8_t reqID;
    char textBuf[BLE_CUSTOM_TEXT_MAX];

    portENTER_CRITICAL(&bleMux);
    isCustom = bleIsCustom;
    reqID = bleRequestedID;
    if (isCustom) {
      strncpy(textBuf, bleCustomText, sizeof(textBuf));
      textBuf[sizeof(textBuf) - 1] = '\0';
    }
    blePending = false;
    portEXIT_CRITICAL(&bleMux);

    markActivity();
    if (isCustom) sendCustomMessage(String(textBuf));
    else sendMessage(reqID);
  }

  // UYANIKKEN BUTON KONTROLÜ
  static bool lastState = HIGH;
  static unsigned long btnPressStart = 0;
  bool currentState = digitalRead(BTN_PIN);

  if (lastState == HIGH && currentState == LOW) {
    btnPressStart = millis();
  } else if (lastState == LOW && currentState == HIGH) {
    unsigned long duration = millis() - btnPressStart;
    if (duration > 50) { // Debounce
      markActivity();
      if (duration >= SEND_PRESS_MIN) {
        // ~1.5 sn basılı tutma -> Gönder
        sendMessage(predefinedMessages[currentIndex].id);
      } else if (duration < SHORT_PRESS_MAX) {
        // Kısa basış (< 1 sn) -> Menüde gez
        currentIndex = (currentIndex + 1) % totalMessages;
        screenNeedsUpdate = true;
      }
    }
  }
  lastState = currentState;

  // Kuyruk İşleme
  static unsigned long lastQueueCheck = 0;
  if (millis() - lastQueueCheck > 1000) {
    lastQueueCheck = millis();
    processQueue();
  }

  // Ekran Güncelleme
  if (screenNeedsUpdate) {
    updateScreen();
    screenNeedsUpdate = false;
  }

  // 1 Dakika Hareketsizlik Kontrolü -> Light Sleep Moduna Geçiş
  if (millis() - lastActivityTime > IDLE_SLEEP_TIMEOUT) {
    enterLightSleep();
  }
}

// =================================================================
// YARDIMCI FONKSİYONLAR
// =================================================================
void sendMessage(uint8_t messageID) {
  const char *text = getMessageText(messageID);
  if (text == nullptr) return;

  char payload[40];
  snprintf(payload, sizeof(payload), "MSG:%d:%s", messageID, text);

  updateScreen("GONDERILIYOR...");
  bool sent = transmitPacket(payload);

  if (sent) {
    waitingForAck = true;
    pendingAckID = messageID;
    ackWaitStart = millis();
    updateScreen("ACK BEKLENIYOR...");
  } else {
    addToQueue(messageID, text);
    updateScreen("HATA - KUYRUGA EKLENDI");
  }

  delay(200);
  radio.startReceive();
  screenNeedsUpdate = true;
}

void sendCustomMessage(const String &text) {
  if (text.length() == 0) return;

  char payload[40];
  snprintf(payload, sizeof(payload), "MSG:0:%s", text.c_str());

  updateScreen("GONDERILIYOR (OZEL)...");
  bool sent = transmitPacket(payload);

  if (sent) {
    waitingForAck = true;
    pendingAckID = 0;
    ackWaitStart = millis();
    updateScreen("ACK BEKLENIYOR...");
  } else {
    addToQueue(0, text.c_str());
    updateScreen("HATA - KUYRUGA EKLENDI");
  }

  delay(200);
  radio.startReceive();
  screenNeedsUpdate = true;
}

bool transmitPacket(const char *payload) {
  return (radio.transmit(payload) == RADIOLIB_ERR_NONE);
}

bool addToQueue(uint8_t messageID, const char *text) {
  if (text == nullptr) return false;
  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (!messageQueue[i].used) {
      messageQueue[i].used = true;
      messageQueue[i].messageID = messageID;
      strncpy(messageQueue[i].text, text, sizeof(messageQueue[i].text) - 1);
      messageQueue[i].text[sizeof(messageQueue[i].text) - 1] = '\0';
      messageQueue[i].retryCount = 0;
      messageQueue[i].lastAttempt = millis();
      return true;
    }
  }
  return false;
}

void processQueue() {
  if (waitingForAck) return;

  for (int i = 0; i < QUEUE_SIZE; i++) {
    if (messageQueue[i].used && (millis() - messageQueue[i].lastAttempt >= RETRY_INTERVAL)) {
      char payload[40];
      snprintf(payload, sizeof(payload), "MSG:%d:%s", messageQueue[i].messageID, messageQueue[i].text);

      bool sent = transmitPacket(payload);
      messageQueue[i].lastAttempt = millis();
      messageQueue[i].retryCount++;

      if (sent) {
        waitingForAck = true;
        pendingAckID = messageQueue[i].messageID;
        ackWaitStart = millis();
      }

      if (messageQueue[i].retryCount >= MAX_RETRY) {
        messageQueue[i].used = false;
      }

      radio.startReceive();
      return;
    }
  }
}

void handleIncomingPacket(String &packet) {
  if (packet.startsWith("ACK:")) {
    int ackID = packet.substring(4).toInt();
    if (waitingForAck && ackID == pendingAckID) {
      waitingForAck = false;
      for (int i = 0; i < QUEUE_SIZE; i++) {
        if (messageQueue[i].used && messageQueue[i].messageID == ackID) {
          messageQueue[i].used = false;
        }
      }
      screenNeedsUpdate = true;
    }
    return;
  }

  if (packet.startsWith("MSG:")) {
    int firstColon = packet.indexOf(':', 4);
    if (firstColon < 0) return;

    int msgID = packet.substring(4, firstColon).toInt();
    String msgText = packet.substring(firstColon + 1);

    lastRxMsg = msgText;
    lastRxRSSI = radio.getRSSI();
    screenNeedsUpdate = true;

    char ackPayload[16];
    snprintf(ackPayload, sizeof(ackPayload), "ACK:%d", msgID);
    transmitPacket(ackPayload);

    if (bleConnected && pNotifyChar) {
      String notifyPayload = "RX:" + msgText;
      pNotifyChar->setValue(notifyPayload.c_str());
      pNotifyChar->notify();
    }
  }
}

void updateScreen(const char *systemStatus) {
  u8g2.clearBuffer();

  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 10, "ALINAN (RX):");

  u8g2.setCursor(0, 23);
  if (lastRxMsg == "") {
    u8g2.print("[Henuz Mesaj Yok]");
  } else {
    u8g2.print("<- ");
    u8g2.print(lastRxMsg);
    u8g2.setFont(u8g2_font_u8glib_4_tf);
    u8g2.print(" (");
    u8g2.print((int)lastRxRSSI);
    u8g2.print("dB)");
  }

  u8g2.drawHLine(0, 28, 128);

  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.drawStr(0, 42, "SECILEN (TX):");

  u8g2.setCursor(0, 54);
  u8g2.print("> ");
  u8g2.print(predefinedMessages[currentIndex].text);

  u8g2.setFont(u8g2_font_u8glib_4_tf);
  u8g2.setCursor(100, 10);
  u8g2.print(bleConnected ? "[BLE]" : "[---]");

  int queued = 0;
  for (int i = 0; i < QUEUE_SIZE; i++) if (messageQueue[i].used) queued++;
  if (queued > 0) {
    u8g2.setCursor(112, 18);
    u8g2.print("Q:");
    u8g2.print(queued);
  }

  if (systemStatus != nullptr && strlen(systemStatus) > 0) {
    u8g2.drawStr(0, 63, systemStatus);
  } else {
    u8g2.drawStr(0, 63, "[Gez: <1s | Gonder: 1.5s]");
  }

  u8g2.sendBuffer();
}