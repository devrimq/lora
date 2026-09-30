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
#define DEVICE_NAME "LoRa-1" // 2. cihaz için "LoRa-2" yapın

#define I2C_SDA 5
#define I2C_SCL 6
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

#define BTN_PIN GPIO_NUM_21
#define SHORT_PRESS_MAX 1500  // SEND_PRESS_MIN ile ayni deger: aralarinda "olu bolge" kalmasin
#define SEND_PRESS_MIN  1500  
#define WAKE_PRESS_MIN  1200  
#define IDLE_SLEEP_TIMEOUT 150000UL // 2.5 Dakika (150 sn)

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
// KESME (INTERRUPT) BAYRAĞI
// =================================================================
volatile bool packetReceived = false;
volatile bool radioReady = false; // radio.begin() basarili oldu mu?

#if defined(ESP32)
  IRAM_ATTR
#elif defined(ESP8266)
  ICACHE_RAM_ATTR
#endif
void setFlag(void) {
  packetReceived = true;
}

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
float lastRxSNR = 0;
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
    // Uzun bir BLE oturumu boyunca lastActivityTime guncellenmemis olabilir;
    // baglanti koptugu anda sayaci sifirliyoruz ki cihaz aniden (bekleme
    // suresi zaten dolmus gibi) uykuya dalmasin.
    markActivity();
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
  u8g2.setPowerSave(1);
  isSleeping = true;

  // ONEMLI: RadioLib, setPacketReceivedAction() ile DIO1 pinine RISING-EDGE
  // tipinde bir donanim kesmesi (attachInterrupt) baglamis durumda. Asagida
  // gpio_wakeup_enable() ayni pin icin HIGH-LEVEL tipinde bir kesme tipi
  // ayarliyor. Ikisi AYNI GPIO kesme-tipi yazmacini kullaniyor; bu yuzden
  // gpio_wakeup_enable RadioLib'in RISING-EDGE ayarini sessizce LEVEL'e
  // cevirir. Uyanip normal moda donuldugunde bu deger geri RISING'e
  // dondurulmezse, DIO1 hattinin sonraki bir RX olayinda kisa sure HIGH'da
  // kalmasi (radyo tarafindan temizlenene kadar) LEVEL-triggered kesmeyi
  // surekli yeniden tetikleyip bir "interrupt storm" olusturuyor ve bu da
  // "Interrupt wdt timeout" panigine yol aciyor. Cozum: uykuya girmeden
  // once RadioLib'in kesmesini gecici olarak kaldiriyoruz, uyandiktan sonra
  // da (asagida, fonksiyon sonunda) dogru RISING-EDGE moduna geri donduruyoruz.
  if (radioReady) {
    radio.clearPacketReceivedAction();
  }

  gpio_wakeup_enable(BTN_PIN, GPIO_INTR_LOW_LEVEL);
  if (radioReady) {
    gpio_wakeup_enable((gpio_num_t)LORA_DIO1, GPIO_INTR_HIGH_LEVEL);
  }
  esp_sleep_enable_gpio_wakeup();

  while (isSleeping) {
    esp_light_sleep_start();

    // 1. LoRa'dan Mesaj Geldi
    if (radioReady && digitalRead(LORA_DIO1) == HIGH) {
      String rxStr;
      if (radio.readData(rxStr) == RADIOLIB_ERR_NONE) {
        handleIncomingPacket(rxStr);
      }
      radio.startReceive();
      isSleeping = false;
      u8g2.setPowerSave(0);
      markActivity();
      screenNeedsUpdate = true;
      break;
    }

    // 2. Butona Basıldı
    if (digitalRead(BTN_PIN) == LOW) {
      unsigned long pressStart = millis();
      while (digitalRead(BTN_PIN) == LOW) {
        delay(10);
      }
      unsigned long pressDuration = millis() - pressStart;

      if (pressDuration >= WAKE_PRESS_MIN) {
        isSleeping = false;
        u8g2.setPowerSave(0);
        markActivity();
        screenNeedsUpdate = true;
        break;
      } else {
        delay(50);
      }
    }
  }
  
  gpio_wakeup_disable(BTN_PIN);
  if (radioReady) {
    gpio_wakeup_disable((gpio_num_t)LORA_DIO1);
    // DIO1 kesme tipini gpio_wakeup_enable'in LEVEL moduna cevirmesinden once
    // oldugu gibi RISING-EDGE'e geri donduruyoruz - "interrupt storm" fixi.
    radio.setPacketReceivedAction(setFlag);
  }
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
  
  // 868MHz, 125kHz, SF9, CR 4/7, Private SyncWord, 14dBm, Preamble 8, TCXO 1.8V, DC-DC
  // ONEMLI: TCXO gerilimi kasitli olarak 1.8V'a sabitlendi. Onceki test firmware'inde
  // (calisan.txt / Meshtastic referansi) bu deger 1.8V idi; 1.6V ile bu karttaki TCXO
  // stabil calismiyor ve TX guvenilirligini dusuruyor. Bu satiri geri 1.6V yapmayin.
  int state = RADIOLIB_ERR_UNKNOWN;
  for (uint8_t attempt = 0; attempt < 3 && state != RADIOLIB_ERR_NONE; attempt++) {
    state = radio.begin(868.0, 125.0, 9, 7, RADIOLIB_SX126X_SYNC_WORD_PRIVATE, 14, 8, 1.8, false);
    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("[LoRa] Baslatma denemesi %d basarisiz, kod: %d\n", attempt + 1, state);
      delay(300);
    }
  }

  radioReady = (state == RADIOLIB_ERR_NONE);
  if (radioReady) {
    Serial.println("[LoRa] Basariyla baslatildi.");
    radio.setPacketReceivedAction(setFlag);
    radio.startReceive();
  } else {
    Serial.printf("[LoRa] KRITIK HATA - LoRa baslatilamadi: %d\n", state);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_ncenB08_tr);
    u8g2.drawStr(0, 15, "LORA MODUL HATASI!");
    u8g2.setCursor(0, 32);
    u8g2.printf("Hata kodu: %d", state);
    u8g2.drawStr(0, 48, "Anten/B2B baglantisini");
    u8g2.drawStr(0, 60, "kontrol edip resetleyin");
    u8g2.sendBuffer();
    delay(3000);
  }

  // BLE Kurulumu
  NimBLEDevice::init(DEVICE_NAME);
  // Varsayilan BLE MTU (23 bayt, ~20 bayt kullanilabilir) ozel mesaj (34 bayt) icin
  // yetersiz kalabilir; MTU'yu yukseltiyoruz (telefon tarafi da negotiate etmeli).
  NimBLEDevice::setMTU(185);
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
  if (radioReady && packetReceived) {
    packetReceived = false;
    String rxStr;
    int state = radio.readData(rxStr);
    if (state == RADIOLIB_ERR_NONE) {
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
    if (radioReady) radio.startReceive();
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
    if (duration > 50) {
      markActivity();
      // NOT: Eskiden 3.5 sn+ basili tutma "Yardim Gerekli!" mesajini
      // otomatik gonderiyordu. Bu, cepte/cantada kazara uzun sure basili
      // kalma ihtimaliyle YANLIS ALARM riski tasidigi icin kaldirildi.
      // "Yardim Gerekli!" mesaji zaten menude (4. sirada) mevcut; kullanici
      // bilinçli olarak secip normal gonderim basisiyla (>=SEND_PRESS_MIN)
      // yollayabilir.
      if (duration >= SEND_PRESS_MIN) {
        // ~1.5 sn basılı tutma -> Seçili Mesajı Gönder
        sendMessage(predefinedMessages[currentIndex].id);
      } else if (duration < SHORT_PRESS_MAX) {
        // Kısa basış -> Menüde gez
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

  // 2.5 Dakika Hareketsizlik Kontrolü -> Light Sleep Moduna Geçiş
  // ONEMLI: BLE baglantisi acikken uykuya dalmiyoruz. Aksi halde telefon
  // uzerinden ozel mesaj yazarken cihaz esp_light_sleep_start() ile CPU'yu
  // durdurur, BLE baglantisi kopar/kararsizlasir ve yazilan mesaj gidemez.
  if (!bleConnected && (millis() - lastActivityTime > IDLE_SLEEP_TIMEOUT)) {
    enterLightSleep();
  }
}

// =================================================================
// YARDIMCI FONKSİYONLAR
// =================================================================
void sendMessage(uint8_t messageID) {
  const char *text = getMessageText(messageID);
  if (text == nullptr) return;

  if (!radioReady) {
    addToQueue(messageID, text);
    updateScreen("LORA HATALI - KUYRUGA EKLENDI");
    screenNeedsUpdate = true;
    return;
  }

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

  radio.startReceive();
  screenNeedsUpdate = true;
}

void sendCustomMessage(const String &text) {
  if (text.length() == 0) return;

  if (!radioReady) {
    addToQueue(0, text.c_str());
    updateScreen("LORA HATALI - KUYRUGA EKLENDI");
    screenNeedsUpdate = true;
    return;
  }

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

  radio.startReceive();
  screenNeedsUpdate = true;
}

int16_t lastTxState = RADIOLIB_ERR_NONE; // son transmit() donus kodu - hata ayiklama icin

bool transmitPacket(const char *payload) {
  if (!radioReady) return false;

  // ONEMLI: setPacketReceivedAction() ile DIO1'e baglanan kesme sadece
  // "RX Done" degil, pindeki HER YUKSELEN KENAR icin tetikleniyor. Bu da
  // radio.transmit() bittiginde olusan "TX Done" kenarini da RX sanip
  // packetReceived=true yapiyor. Bir sonraki loop() turunde readData()
  // cagirilinca, SX126x TX/RX icin ayni dahili tamponu kullandigindan,
  // henuz gercek bir paket gelmemisken kendi az once gonderdigimiz baytlar
  // geri okunuyor -> cihaz "kendi mesajini almis gibi" davraniyordu.
  // Cozum: transmit() suresince RX-done kesmesini gecici olarak kaldirip,
  // bittikten sonra temiz bir sekilde yeniden takiyoruz.
  radio.clearPacketReceivedAction();
  packetReceived = false;

  lastTxState = radio.transmit(payload);
  if (lastTxState != RADIOLIB_ERR_NONE) {
    Serial.printf("[LoRa] TX hatasi, kod: %d\n", lastTxState);
  }

  packetReceived = false; // TX Done kenarinin tetiklemis olabilecegi bayragi temizle
  radio.setPacketReceivedAction(setFlag);

  return (lastTxState == RADIOLIB_ERR_NONE);
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
  if (waitingForAck || !radioReady) return;

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
    radio.startReceive();
    return;
  }

  if (packet.startsWith("MSG:")) {
    int firstColon = packet.indexOf(':', 4);
    if (firstColon < 0) {
      radio.startReceive();
      return;
    }

    int msgID = packet.substring(4, firstColon).toInt();
    String msgText = packet.substring(firstColon + 1);

    lastRxMsg = msgText;
    lastRxRSSI = radio.getRSSI();
    lastRxSNR = radio.getSNR();
    screenNeedsUpdate = true;

    delay(50);
    char ackPayload[16];
    snprintf(ackPayload, sizeof(ackPayload), "ACK:%d", msgID);
    transmitPacket(ackPayload);

    radio.startReceive();

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

  u8g2.setCursor(0, 22);
  if (lastRxMsg == "") {
    u8g2.print("[Henuz Mesaj Yok]");
  } else {
    u8g2.print("<- ");
    u8g2.print(lastRxMsg);
    u8g2.setFont(u8g2_font_u8glib_4_tf);
    u8g2.setCursor(0, 27);
    u8g2.printf("RSSI: %d dBm | SNR: %.1f dB", (int)lastRxRSSI, lastRxSNR);
  }

  u8g2.drawHLine(0, 29, 128);

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
    u8g2.drawStr(0, 63, "[Gez:<1s|Gnd:1.5s|Acil:>3.5s]");
  }

  u8g2.sendBuffer();
}