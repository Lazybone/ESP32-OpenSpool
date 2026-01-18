# ESP32-OpenSpool

A standalone NTAG21x NFC tag reader/writer for the [OpenSpool](https://github.com/spuder/OpenSpool) protocol. This project runs on an ESP32-S3 Zero with a PN532 NFC module and provides a modern web interface for reading and writing filament data to NFC tags.

## Features

- **Standalone Device**: No computer required after initial setup - just power and use
- **Web Interface**: Modern, responsive dark-mode UI accessible from any device
- **Progressive Web App (PWA)**: Install on your phone's homescreen for app-like experience
- **Filament Database**: Auto-fill temperatures for popular brands (Bambu Lab, Prusament, eSun, etc.)
- **Custom Filaments**: Add your own brands, material types, and temperature presets
- **Sound Feedback**: Optional piezo buzzer for audible feedback on tag operations
- **mDNS Support**: Access via `http://openspool.local` - no need to remember IP addresses
- **Flexible WiFi**: Connect to existing network OR use as standalone access point
- **WiFi Manager**: Built-in network scanner and configuration via web UI
- **OTA Updates**: Update firmware directly through the web interface
- **NTAG21x Support**: Read and write OpenSpool data to NTAG213, NTAG215, and NTAG216 tags
- **Tag Info Display**: Shows tag type, UID, and memory usage when reading
- **Tag Erase**: Clear all data from a tag with one click
- **Full OpenSpool Protocol**: Supports all standard fields including the optional subtype
- **Color Palette**: 32 preset colors plus custom hex input and color picker
- **Real-time Status**: Live NFC and WiFi status indicators
- **Persistent Settings**: WiFi credentials saved to flash memory

## Hardware Requirements

| Component | Description |
|-----------|-------------|
| ESP32-S3 Zero | Waveshare ESP32-S3 Zero or compatible ESP32-S3 board (4MB Flash) |
| PN532 NFC Module | NFC/RFID module with SPI interface |
| NTAG21x Tags | NTAG213, NTAG215, or NTAG216 NFC tags |
| USB-C Cable | For power and initial programming |
| Piezo Buzzer | (Optional) For sound feedback - connect to GPIO6 |

## Wiring Diagram

Connect the PN532 module to the ESP32-S3 Zero via SPI:

| PN532 Pin | ESP32-S3 Zero Pin |
|-----------|-------------------|
| VCC | 3.3V |
| GND | GND |
| SCK | GPIO12 |
| MISO | GPIO13 |
| MOSI | GPIO11 |
| SS (CS) | GPIO10 |

### Optional: Piezo Buzzer

For audible feedback on tag operations, connect a piezo buzzer:

| Buzzer Pin | ESP32-S3 Zero Pin |
|------------|-------------------|
| + (Signal) | GPIO6 |
| - (GND) | GND |

> **Note**: The buzzer is optional. To disable sound, set `BUZZER_ENABLED` to `false` in `main.cpp`.

> **Note**: Make sure your PN532 module is set to SPI mode. Most modules have a DIP switch or solder jumpers to select the communication mode.

### PN532 SPI Mode Configuration

Set the DIP switches or solder jumpers on your PN532 module:

| Switch 1 | Switch 2 | Mode |
|----------|----------|------|
| OFF | ON | SPI |

## Installation

### Option 1: Easy Install (Web Flasher)

No development environment needed - flash directly from your browser!

1. **Download the firmware**
   - Go to [Releases](https://github.com/Lazybone/ESP32-OpenSpool/releases)
   - Download `ESP32-OpenSpool-vX.X.X-full.bin` (combined firmware + web interface)

2. **Flash via Web Flasher**
   - Open [ESP Web Tools](https://espressif.github.io/esptool-js/) in Chrome or Edge
   - Connect your ESP32-S3 Zero via USB
   - Click "Connect" and select the serial port
   - Set flash address to `0x0`
   - Select the downloaded `.bin` file
   - Click "Program" and wait for completion

3. **Done!**
   - The ESP32 will restart automatically
   - Connect to the `OpenSpool` WiFi (password: `openspool`)
   - Open `http://openspool.local` or `http://192.168.4.1`

> **Note**: The Web Flasher requires a Chromium-based browser (Chrome, Edge, Opera) with Web Serial API support.

### Option 2: Build from Source (PlatformIO)

#### Prerequisites

- [PlatformIO](https://platformio.org/) (VS Code extension or CLI)
- USB-C cable
- ESP32-S3 Zero board

#### Build and Flash

1. **Clone the repository**
   ```bash
   git clone https://github.com/Lazybone/ESP32-OpenSpool.git
   cd ESP32-OpenSpool
   ```

2. **Build the firmware**
   ```bash
   pio run
   ```

3. **Flash the firmware**
   ```bash
   pio run -t upload
   ```

4. **Upload the web interface (LittleFS)**
   ```bash
   pio run -t uploadfs
   ```

### First Boot

After flashing, the ESP32 will:
1. Create a WiFi Access Point named `OpenSpool`
2. Initialize the NFC module
3. Start the web server with mDNS
4. Be accessible at `http://192.168.4.1` or `http://openspool.local`

## Usage

### Initial Setup (Access Point Mode)

On first boot or when no WiFi is configured:

1. **Connect to the ESP32's WiFi**
   - SSID: `OpenSpool`
   - Password: `openspool`

2. **Open Web Interface**
   - Navigate to `http://openspool.local` or `http://192.168.4.1` in your browser

3. **Configure WiFi (Optional)**
   - Click on "AP Mode" in the status bar to open WiFi settings
   - Click "Scan Networks" to find available networks
   - Select your network and enter the password
   - Click "Connect"
   - The device will connect to your network while keeping the AP active

### Network Modes

The device supports three WiFi modes:

| Mode | Description | Access |
|------|-------------|--------|
| **AP Only** | Access Point only (default) | Connect to `OpenSpool`, access via `192.168.4.1` |
| **AP + Client** | Connected to network + AP active | Access via network IP OR `192.168.4.1` (via AP) |
| **Client Only** | Connected to your network only | Access via network-assigned IP |

> **Note**: The AP remains active even when connected to a network, allowing you to always access the device via `192.168.4.1` for configuration.

### Connecting When on Your Network

If you've configured WiFi, you can access the device:
- Via `http://openspool.local` (recommended)
- Via the network IP shown in the web interface
- Via the AP at `192.168.4.1` (connect to `OpenSpool` first)

### Writing a Tag

1. Select the **Brand** from the dropdown (or use "Generic")
2. Select the **Material Type** (PLA, PETG, ABS, etc.)
3. Optionally enter a **Subtype** (Basic, Rapid, Silk, Matte, etc.)
4. Choose a **Color** using presets, hex input, or color picker
5. Set the **Temperature** ranges for nozzle and bed
6. Place an NTAG215 tag on the PN532 reader
7. Click **Write Tag**

### Reading a Tag

1. Place an NTAG21x tag on the PN532 reader
2. Click **Read Tag**
3. The form will populate with the tag's data
4. The **Tag Info** section shows the tag type, UID, and memory usage

### Erasing a Tag

1. Place an NTAG21x tag on the PN532 reader
2. Click **Erase Tag**
3. Confirm the action
4. The tag will be cleared of all OpenSpool data

### Settings

The **Settings** section (collapsed by default) contains WiFi configuration and firmware updates.

#### WiFi Configuration

1. Click on **Settings** to expand the panel
2. View current connection status (Mode, IP, etc.)
3. Click **Scan Networks** to find available WiFi networks
4. Select a network or enter SSID manually
5. Enter the password and click **Connect**
6. The device connects while keeping the AP active for fallback

#### Firmware Update (OTA)

You can update the firmware without USB connection:

1. Build the new firmware: `pio run`
2. Open the web interface and expand **Settings**
3. Scroll down to **Firmware Update (OTA)**
4. Select the `.pio/build/esp32-s3-zero/firmware.bin` file
5. Click **Upload Firmware**
6. Wait for the upload and automatic restart

## OpenSpool Protocol

This device implements the OpenSpool v1.0 protocol. Data is stored as JSON on the NFC tag:

```json
{
  "protocol": "openspool",
  "version": "1.0",
  "brand": "Generic",
  "type": "PLA",
  "subtype": "Rapid",
  "color_hex": "#FF0000",
  "min_temp": 190,
  "max_temp": 220,
  "bed_min_temp": 50,
  "bed_max_temp": 60
}
```

### Fields

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `protocol` | string | Yes | Always "openspool" |
| `version` | string | Yes | Protocol version (currently "1.0") |
| `brand` | string | Yes | Filament manufacturer |
| `type` | string | Yes | Material type (PLA, PETG, ABS, etc.) |
| `subtype` | string | No | Material variant (Basic, Rapid, Silk, etc.) |
| `color_hex` | string | Yes | Color in hex format (#RRGGBB) |
| `min_temp` | integer | Yes | Minimum nozzle temperature (°C) |
| `max_temp` | integer | Yes | Maximum nozzle temperature (°C) |
| `bed_min_temp` | integer | Yes | Minimum bed temperature (°C) |
| `bed_max_temp` | integer | Yes | Maximum bed temperature (°C) |

### Slicer Compatibility

For slicers like **Orca Slicer** (Snapmaker edition) to recognize the filament, use the naming convention:

```
<Brand> <Type> <Subtype>
```

Examples:
- `Generic PLA Basic`
- `Elegoo PETG Rapid`
- `Bambu Lab PLA Matte`

## Supported Materials

| Type | Description |
|------|-------------|
| PLA | Polylactic Acid |
| PETG | Polyethylene Terephthalate Glycol |
| ABS | Acrylonitrile Butadiene Styrene |
| ASA | Acrylonitrile Styrene Acrylate |
| TPU | Thermoplastic Polyurethane |
| PA | Polyamide (Nylon) |
| PA12 | Polyamide 12 |
| PC | Polycarbonate |
| PEEK | Polyether Ether Ketone |
| PVA | Polyvinyl Alcohol |
| HIPS | High Impact Polystyrene |
| PCTG | Polycyclohexylenedimethylene Terephthalate Glycol |
| PLA-CF | PLA Carbon Fiber |
| PETG-CF | PETG Carbon Fiber |
| PA-CF | Nylon Carbon Fiber |

## Supported Brands

- Bambu Lab
- Hatchbox
- eSun
- Overture
- SUNLU
- Polymaker
- Prusament
- Jayo
- Generic

## API Endpoints

The ESP32 provides a REST API for integration:

### NFC Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Get NFC and WiFi status (includes firmware version) |
| `/api/read` | GET | Read tag data (includes tag info) |
| `/api/write` | POST | Write JSON data to tag |
| `/api/erase` | POST | Erase all data from tag |

> **Note**: All API endpoints return JSON with a `success` field (`true`/`false`) and an `error` field on failure.

### WiFi Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/wifi/status` | GET | Get WiFi connection status |
| `/api/wifi/scan` | GET | Scan for available networks |
| `/api/wifi/connect` | POST | Connect to a network |
| `/api/wifi/disconnect` | POST | Forget saved network |
| `/api/restart` | POST | Restart the device |

### Filament Database Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/filaments` | GET | Get complete filament database (brands, types, presets) |
| `/api/filaments` | POST | Save/update a temperature preset |
| `/api/filaments/brand` | POST | Add a custom brand |
| `/api/filaments/brand/delete` | POST | Delete a custom brand |
| `/api/filaments/type` | POST | Add a custom material type |
| `/api/filaments/type/delete` | POST | Delete a custom material type |
| `/api/filaments/delete` | POST | Delete a specific preset |
| `/api/filaments/reset` | POST | Clear all custom data |
| `/api/filaments/import` | POST | Bulk import from JSON |

### System Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/ota` | POST | Upload new firmware (multipart/form-data) |

### Example: Get Status via API

```bash
curl http://192.168.4.1/api/status
# Returns: {"version":"0.1.7","nfcReady":true,"wifi":{"mode":"ap_sta","connected":true,...}}
```

### Example: Read Tag via API

```bash
curl http://192.168.4.1/api/read
# Returns: {"tag":{"uid":"04:A1:B2:C3:D4:E5:F6","type":"NTAG215","capacity":504},"data":{...}}
```

### Example: Write Tag via API

```bash
curl -X POST http://192.168.4.1/api/write \
  -H "Content-Type: application/json" \
  -d '{"protocol":"openspool","version":"1.0","brand":"Generic","type":"PLA","color_hex":"#FF0000","min_temp":190,"max_temp":220,"bed_min_temp":50,"bed_max_temp":60}'
# Returns: {"success":true} or {"success":false,"error":"No tag found"}
```

### Example: Connect to WiFi via API

```bash
curl -X POST http://192.168.4.1/api/wifi/connect \
  -H "Content-Type: application/json" \
  -d '{"ssid":"YourNetwork","password":"YourPassword"}'
```

## Troubleshooting

### NFC Module Not Detected

- Check wiring connections (SCK, MISO, MOSI, SS, VCC, GND)
- Ensure PN532 is set to SPI mode (Switch 1: OFF, Switch 2: ON)
- Try power cycling the device
- Check serial monitor for debug output: `pio device monitor`

### Cannot Connect to AP

- Make sure you're connecting to `OpenSpool`
- Password is `openspool` (all lowercase)
- Try forgetting the network and reconnecting

### WiFi Connection to Home Network Fails

- Ensure you entered the correct password
- Check that your network is 2.4 GHz (ESP32 doesn't support 5 GHz)
- Try moving closer to your router
- Use "Forget Network" button and try again

### Tag Not Reading/Writing

- Ensure you're using NTAG21x tags (NTAG213, NTAG215, or NTAG216)
- **Don't place the tag directly on the reader** - hold it 1-3mm above for best signal
- Hold the tag steady during read/write operations
- Check the status indicator in the web interface
- Make sure the tag is not write-protected
- For NTAG213, ensure the data fits within 144 bytes (~134 bytes usable)

### Web Interface Not Loading

- Clear browser cache
- Try a different browser
- Re-upload the filesystem: `pio run -t uploadfs`

## Technical Details

### Current Version

**Firmware**: v0.2.1 (see [CHANGELOG.md](CHANGELOG.md) for details)

### Memory Usage

- **Flash**: ~70% (914 KB of 1.3 MB app partition)
- **RAM**: ~14% (47 KB of 320 KB)

### NTAG21x Specifications

| Tag Type | Total Memory | User Memory | Pages | OpenSpool Compatible |
|----------|--------------|-------------|-------|----------------------|
| NTAG213 | 180 bytes | 144 bytes | 4-39 | Yes (minimal JSON) |
| NTAG215 | 540 bytes | 504 bytes | 4-129 | Yes (recommended) |
| NTAG216 | 924 bytes | 888 bytes | 4-225 | Yes |

- Page size: 4 bytes
- OpenSpool JSON typically requires ~150-200 bytes
- NTAG215 is recommended for best compatibility

### Libraries Used

- [Adafruit PN532](https://github.com/adafruit/Adafruit-PN532) - NFC module driver
- [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer) - Async web server
- [ArduinoJson](https://github.com/bblanchon/ArduinoJson) - JSON parsing
- [LittleFS](https://github.com/littlefs-project/littlefs) - Filesystem for web UI

## Customization

### Changing Access Point Credentials

Edit `src/main.cpp`:

```cpp
const char* AP_SSID = "OpenSpool";  // Change AP name
const char* AP_PASS = "openspool";         // Change AP password
```

### Changing SPI Pins

Edit `src/main.cpp`:

```cpp
#define PN532_SCK  12  // Change to your SCK pin
#define PN532_MISO 13  // Change to your MISO pin
#define PN532_MOSI 11  // Change to your MOSI pin
#define PN532_SS   10  // Change to your SS/CS pin
```

## Project Structure

```
ESP32-OpenSpool/
├── platformio.ini      # PlatformIO configuration
├── README.md           # This file
├── CHANGELOG.md        # Version history
├── src/
│   └── main.cpp        # Main firmware (NFC + WebServer)
├── data/
│   ├── index.html      # Web UI (single-page app)
│   ├── manifest.json   # PWA manifest
│   ├── sw.js           # Service Worker for offline support
│   └── icon.svg        # App icon
└── release/            # Pre-built binaries (not in repo)
    ├── ESP32-OpenSpool-vX.X.X-full.bin      # Complete flash image
    ├── ESP32-OpenSpool-vX.X.X-firmware.bin  # Firmware only
    └── ESP32-OpenSpool-vX.X.X-littlefs.bin  # Web UI only
```

## Contributing

Contributions are welcome! Please feel free to submit a Pull Request.

## License

This project is licensed under the [CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/) license.

You are free to use, modify, and share this project for non-commercial purposes, as long as you give appropriate credit and distribute your contributions under the same license.

## Acknowledgments

- [OpenSpool](https://github.com/spuder/OpenSpool) - The open filament spool protocol
