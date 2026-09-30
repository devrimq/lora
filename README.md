# Dağcı-LoRa: Hücresel Şebekeden Bağımsız Saha Haberleşme Terminali

**Dağcı-LoRa**, GSM kapsamasının ve internet altyapısının bulunmadığı dağcılık, arama-kurtarma ve afet senaryolarında uçtan uca güvenilir, çift yönlü veri iletimi sağlayan taşınabilir bir haberleşme terminalidir.

Sistem, harici bir mobil cihaza ihtiyaç duymadan üzerindeki OLED ekran ve akıllı buton arayüzüyle **bağımsız (standalone)** çalışabildiği gibi; akıllı telefonlara ek bir mobil uygulama (APK/Store) yükletmeksizin doğrudan **Web Bluetooth API** üzerinden bağlanarak serbest metin mesajlaşmasına olanak tanır.

---

## 🚀 Öne Çıkan Teknik Kabiliyetler

- **Paket Doğrulama ve Alındı Teyidi (ACK):** Gönderilen mesajlar için alıcı taraftan donanımsal teyit (`ACK:<id>`) beklenir. Karşı tarafa ulaşmayan mesajlar kaybolmaz.
- **Dinamik Yeniden Deneme Kuyruğu (Queue Engine):** ACK alınamayan paketler sistem belleğindeki 5 elemanlı mesaj kuyruğuna (`messageQueue`) alınır; 8 saniye aralıklarla 3 defaya kadar otomatik olarak yeniden iletilir.
- **Akıllı Güç Yönetimi ve Light Sleep:** 
  - Sistem 60 saniye boyunca işlem görmediğinde OLED ekranı kapatarak ESP32-S3'ü **Light Sleep** moduna alır.
  - Havadan LoRa paketi geldiğinde `DIO1` kesmesi (ISR) ile anında uyanır.
  - İstenmeyen tuş dokunmalarına karşı buton filtreleme devrededir; uykudan uyanmak için butona en az **1.2 saniye** basılması gerekir.
- **Uygulamasız Web Bluetooth Köprüsü (NimBLE):** Standart ağır BLE yığınları yerine optimize **NimBLE** kullanılmıştır. Mobil Chrome üzerinden Web Bluetooth API ile terminale bağlanılır (`https://dagci-lora.vercel.app/`).
- **Tek Butonla Durum Makinesi (Finite State Machine):** 
  - *Kısa basış (< 1.0 sn):* Menüde ve hazır mesajlar arasında gezinme.
  - *Uzun basış (~1.5 sn):* Seçili mesajı LoRa üzerinden fırlatma.
  - *Uykudayken basış (1.2 - 2.0 sn):* Cihazı uyandırma.

---

## 🛠 Donanım Mimarisi ve Pin Eşleşmesi

| Birim | Bileşen | Arayüz / Pinler | Açıklama |
| :--- | :--- | :--- | :--- |
| **MCU** | Seeed Studio XIAO ESP32-S3 | Çift Çekirdek 240 MHz | BLE 5.0 ve ana durum kontrolcüsü |
| **LoRa Alıcı-Verici** | Semtech SX1262 (868 MHz) | SPI (SCK:7, MISO:8, MOSI:9)<br>NSS:41, BUSY:40, RST:42, DIO1:39 | TCXO: 1.6V, DIO2 RF Switch aktif, +22 dBm |
| **Ekran** | 0.96" I2C OLED (SSD1306) | I2C (SDA: GPIO 5, SCL: GPIO 6) | 128x64 piksel durum, RSSI ve menü arayüzü |
| **Girdi / Kontrol** | Akıllı Menü Butonu | GPIO 21 (Dahili Pull-up) | Menü geçişi, gönderme ve uyandırma pini |
| **Enerji & Kasa** | Dahili Li-Po Batarya + Özel Kasa | 3.7V Li-Po / 3D Baskı Gövde | Tek şarjla ortalama 3 gün saha dayanımı |

---

## 📡 Haberleşme ve Paket Protokolü

### LoRa RF Katmanı
* **Merkez Frekans:** 868.0 MHz (Avrupa ISM Bandı)
* **Paket Yapısı:** 
  * Hazır Mesaj: `MSG:<id>:<mesaj_metni>`
  * Serbest Metin: `MSG:0:<ozel_metin>`
  * Doğrulama: `ACK:<id>`

### BLE Katmanı (Custom GATT Service)
* **Servis UUID:** `0000FF00-0000-1000-8000-00805F9B34FB`
* **Yazma Karakteristiği (RX):** `0000FF01-0000-1000-8000-00805F9B34FB` (Telefondan cihaza)
* **Bildirim Karakteristiği (TX):** `0000FF02-0000-1000-8000-00805F9B34FB` (Cihazdan telefona `RX:<mesaj>`)

---

## 📂 Proje Ağacı

```text
.
├── include/               # Konfigürasyon ve başlık dosyaları
├── src/
│   └── main.cpp           # Firmware ana gövdesi (LoRa, NimBLE, Kuyruk, Uyku, Ekran)
├── platformio.ini         # PlatformIO ortam ve bağımlılık tanımları
└── README.md              # Teknik dökümantasyon
⚙️ Kurulum ve Derleme (PlatformIO)
platformio.ini dosyasında tanımlı bağımlılıklar:

jgromes/RadioLib @ 6.6.0

olikraus/U8g2 @ ^2.34.2

h2zero/NimBLE-Arduino @ ^2.1.2

Projeyi derlemek ve karta yüklemek için:

Bash
# Projeyi derle
pio run

# XIAO ESP32-S3 kartına firmware yükle
pio run --target upload

# Seri port çıktısını izle
pio device monitor -b 115200
🌐 Canlı Web Bluetooth Arayüzü
Akıllı telefon Chrome tarayıcısı üzerinden ek bir uygulama kurmadan terminale bağlanmak için test adresi:

👉 https://dagci-lora.vercel.app/