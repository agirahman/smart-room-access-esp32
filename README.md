# Smart Door Lock — ESP32-CAM

RFID-based smart door access system using ESP32-CAM with real-time photo capture and backend authentication.

## Features

- RFID card authentication (MFRC522)
- Real-time photo capture (OV2640)
- Backend API integration with multipart upload (UID + photo)
- Auto-lock solenoid after configurable timeout
- Audio feedback via relay buzzer (tap, grant, deny tones)
- NTP time synchronization
- Retry mechanism for cold-start backend (Cloud Run)

## Hardware

| Component       | Pin  | Notes                        |
|-----------------|------|------------------------------|
| ESP32-CAM       | —    | AI-Thinker board             |
| RFID-RC522      | 13   | SDA (SS)                     |
|                 | 2    | RST                          |
|                 | 14   | SCK                          |
|                 | 15   | MISO                         |
|                 | 12   | MOSI                         |
| Relay/Solenoid  | 4    | Active LOW                   |
| Camera          | —    | OV2640 (default AI-Thinker)  |


## Getting Started

### 1. Clone & configure

```bash
git clone https://github.com/yourusername/smart-door-cam.git
cd smart-door-cam
cp config.h.example config.h
```

### 2. Edit `config.h`

Fill in your WiFi credentials, backend URL, API key, and room name:

```c
#define WIFI_SSID       "your_wifi_ssid"
#define WIFI_PASSWORD   "your_wifi_password"
#define SERVER_URL      "https://your-backend/api/v1/access"
#define API_KEY         "your-api-key"
```

### 3. Upload to ESP32-CAM

Open `smart_door_cam.ino` in Arduino IDE with ESP32 board support installed.

**Board Settings:**
- Board: `AI Thinker ESP32-CAM`
- Flash Mode: `QIO`
- Flash Size: `4MB`
- Partition Scheme: `Huge APP (3MB No OTA)`

### 4. Open Serial Monitor (115200 baud)

Tap an RFID card — the system will:
1. Read the card UID
2. Discard 3 stale camera frames
3. Capture a fresh photo
4. POST to backend with multipart form-data (uid, room, photo)
5. Open the solenoid if access is granted (auto-lock after 5s)

## Backend API Contract

### Request

```
POST /api/v1/access
Content-Type: multipart/form-data
X-API-KEY: <api_key>

Fields:
  uid   — RFID card UID (e.g. "A1 B2 C3 D4")
  room  — Room identifier (e.g. "lab-iot")
  photo — JPEG image file (optional fallback to JSON)
```

### Response (200 OK)

```json
{
  "data": {
    "status": "allowed",
    "message": "Access granted for user John Doe"
  }
}
```

### Response (200 OK — denied)

```json
{
  "data": {
    "status": "denied",
    "message": "Unknown card"
  }
}
```

## Project Structure

```
├── smart_door_cam.ino    # Main firmware
├── config.h              # ⚠ Local config (gitignored)
├── config.h.example      # Template for config.h
├── .gitignore
└── README.md
```

## Notes

- `config.h` is in `.gitignore` — **never** commit it to avoid leaking credentials
- The camera discards 3 frames after RFID tap to ensure a fresh capture
- Backend POST includes 1 retry with 2s delay to handle Cloud Run cold starts
