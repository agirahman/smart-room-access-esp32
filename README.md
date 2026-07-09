# Smart Door Lock — ESP32-CAM

Sistem akses pintar berbasis RFID menggunakan ESP32-CAM dengan pengambilan foto real-time dan autentikasi backend.

## Fitur

- Autentikasi kartu RFID (MFRC522)
- Pengambilan foto real-time (OV2640)
- Integrasi API backend dengan multipart upload (UID + foto)
- Penguncian solenoid otomatis setelah waktu tunggu yang dapat diatur
- Umpan balik audio melalui relay buzzer (nada ketuk, izin, tolak)
- Sinkronisasi waktu NTP
- Mekanisme percobaan ulang untuk cold-start backend (Cloud Run)

## Perangkat Keras

| Komponen        | Pin  | Catatan                      |
|-----------------|------|------------------------------|
| ESP32-CAM       | —    | Board AI-Thinker             |
| RFID-RC522      | 13   | SDA (SS)                     |
|                 | 2    | RST                          |
|                 | 14   | SCK                          |
|                 | 15   | MISO                         |
|                 | 12   | MOSI                         |
| Relay/Solenoid  | 4    | Active LOW                   |
| Kamera          | —    | OV2640 (default AI-Thinker)  |

## Memulai

### 1. Clone & konfigurasi

```bash
git clone https://github.com/yourusername/smart-door-cam.git
cd smart-door-cam
cp config.h.example config.h
```

### 2. Edit `config.h`

Isi kredensial WiFi, URL backend, kunci API, dan nama ruangan Anda:

```c
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASSWORD   "your_wifi_password"
#define SERVER_URL      "https://your-backend/api/v1/access"
#define API_KEY         "your-api-key"
```

### 3. Upload ke ESP32-CAM

Buka `smart_door_cam.ino` di Arduino IDE dengan dukungan board ESP32 terinstal.

**Pengaturan Board:**
- Board: `AI Thinker ESP32-CAM`
- Flash Mode: `QIO`
- Flash Size: `4MB`
- Partition Scheme: `Huge APP (3MB No OTA)`

### 4. Buka Serial Monitor (115200 baud)

Tempelkan kartu RFID — sistem akan:
1. Membaca UID kartu
2. Membuang 3 frame kamera usang
3. Mengambil foto baru
4. POST ke backend dengan multipart form-data (uid, room, foto)
5. Membuka solenoid jika akses diberikan (terkunci otomatis setelah 5 detik)

## Kontrak API Backend

### Permintaan

```
POST /api/v1/access
Content-Type: multipart/form-data
X-API-KEY: <api_key>

Field:
  uid   — UID kartu RFID (mis. "A1 B2 C3 D4")
  room  — Identitas ruangan (mis. "lab-iot")
  photo — Berkas gambar JPEG (opsional, fallback ke JSON)
```

### Respons (200 OK — diizinkan)

```json
{
  "data": {
    "status": "allowed",
    "message": "Akses diberikan untuk pengguna John Doe"
  }
}
```

### Respons (200 OK — ditolak)

```json
{
  "data": {
    "status": "denied",
    "message": "Kartu tidak dikenal"
  }
}
```

## Struktur Proyek

```
├── smart_door_cam.ino    # Firmware utama
├── config.h              # ⚠ Konfigurasi lokal (gitignored)
├── config.h.example      # Template untuk config.h
├── .gitignore
└── README.md
```

## Catatan

- `config.h` ada di `.gitignore` — **jangan pernah** di-commit untuk menghindari kebocoran kredensial
- Kamera membuang 3 frame setelah RFID ditempelkan untuk memastikan foto baru
- POST backend menyertakan 1 percobaan ulang dengan jeda 2 detik untuk menangani cold-start Cloud Run
