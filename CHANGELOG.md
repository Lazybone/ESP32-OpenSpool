# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.2.1] - 2025-01-18

### Fixed

- **Brand/Type deletion not working**: Custom brands and material types could not be deleted via the web UI
  - Root cause: ESPAsyncWebServer was matching `/api/filaments/brand` before `/api/filaments/brand/delete` due to prefix matching
  - Solution: Reordered endpoint registration so more specific routes (with `/delete`) are registered first
  - Also fixed the array element removal logic in `deleteCustomBrand()` and `deleteCustomType()` functions

### Changed

- Cleaned up debug output from `saveFilamentDatabase()`, `deleteCustomBrand()`, and `deleteCustomType()` functions
- Removed unused `partitions_4MB.csv` file

## [0.2.0] - 2025-01-18

### Added

- **Custom Filament Database**: Full filament management on the ESP32
  - **Custom Brands**: Add your own filament brands (appear in Brand dropdown)
  - **Custom Material Types**: Add custom materials like PLA+, PETG-HT, etc.
  - **Temperature Presets**: Save custom temperature settings per brand/type combination
  - "Customize" button in Temperature Settings card opens management modal
  - Custom entries automatically extend the Brand and Type dropdowns
  - Export complete database (brands, types, presets) to JSON file
  - Import database from JSON file
  - Reset all custom data to defaults
  - Data stored in ESP32 NVS (survives reboots)
  - Supports ~150-200 custom entries total

### New API Endpoints

- `GET /api/filaments` - Get complete database (brands, types, presets)
- `POST /api/filaments` - Save/update a temperature preset
- `POST /api/filaments/delete` - Delete a specific preset
- `POST /api/filaments/brand` - Add a custom brand
- `POST /api/filaments/brand/delete` - Delete a custom brand (and its presets)
- `POST /api/filaments/type` - Add a custom material type
- `POST /api/filaments/type/delete` - Delete a custom type (and its presets)
- `POST /api/filaments/reset` - Clear all custom data
- `POST /api/filaments/import` - Bulk import from JSON

## [0.1.9] - 2025-01-18

### Added

- **Progressive Web App (PWA)**: Install as app on mobile devices
  - Add to homescreen support for iOS and Android
  - Fullscreen standalone mode
  - Custom app icon (SVG)
  - Service Worker for offline caching of UI
  - Automatic cache updates
- **Sound Feedback**: Optional piezo buzzer support (GPIO14)
  - Short beep on tag detection
  - Double beep on successful write/erase
  - Long low beep on errors
  - Ready beep on device startup
  - Can be disabled via `BUZZER_ENABLED` constant

### Technical

- manifest.json for PWA configuration
- sw.js Service Worker with cache-first strategy for static assets
- Network-first strategy for API calls

## [0.1.8] - 2025-01-18

### Added

- **Filament Database**: Built-in temperature presets for popular filament brands and materials
  - Supports: Bambu Lab, Prusament, eSun, Polymaker, Hatchbox, Overture, SUNLU, Jayo
  - Materials: PLA, PETG, ABS, ASA, TPU, PA, PC, PEEK, PVA, HIPS, PCTG, PLA-CF, PETG-CF, PA-CF
  - Temperatures auto-fill when changing brand or material type
  - Falls back to generic defaults for unknown combinations
- **Auto-fill indicator**: Shows "Auto-filled from filament database" hint when temperatures are set

## [0.1.7] - 2025-01-18

### Fixed

- **NFC init error message**: Corrected "5 attempts" to "3 attempts" to match actual retry count
- **Erase endpoint HTTP status**: Now returns HTTP 500 on error instead of always 200
- **Documentation**: Fixed CLAUDE.md to document SPI pins instead of incorrect I2C pins
- **Board configuration**: Changed platformio.ini board from `lolin_s3_mini` to `esp32-s3-devkitc-1` (ESP32-S3 Zero compatible)
- **Blocking delays in async handlers**: Replaced `delay()` calls with deferred actions processed in main loop
- **Race condition**: Changed `scanInProgress` from `bool` to `std::atomic<bool>` for thread safety
- **Input validation**: Added payload size limits (512 bytes for WiFi, 1024 bytes for tag write)
- **Color validation**: Added client-side hex color format validation (#RRGGBB)
- **Temperature validation**: Added client-side range validation (0-400°C nozzle, 0-150°C bed) and min<=max checks
- **JSON parsing**: Added try-catch around JSON.parse in OTA upload handler
- **Inconsistent error responses**: All API errors now return `{"success": false, "error": "..."}` format
- **Password visibility**: Password field is now cleared after successful WiFi connection
- **LittleFS recovery**: Added automatic format and retry on mount failure
- **XSS vulnerability**: WiFi SSIDs are now HTML-escaped before rendering in network list
- **Code formatting**: Fixed inconsistent indentation in `/api/write` handler

### Added

- **Firmware version constant**: Added `FIRMWARE_VERSION` constant exposed in `/api/status` response
- **HTTP security headers**: Added `X-Content-Type-Options`, `X-Frame-Options`, `X-XSS-Protection` headers
- **Named constants**: Replaced magic numbers with descriptive constants (`NFC_INIT_DELAY_MS`, `WIFI_CHECK_INTERVAL_MS`, etc.)
- **Thread safety**: Added `std::mutex` for global state protection
- **UID validation**: Added warning log for unexpected UID lengths (not 4 or 7 bytes)

### Removed

- Unused `lastTagData` variable
- Unused `ndefRecordSize` variable
- Console.log/console.error calls in production JavaScript

### Technical

- Deferred action system for WiFi connect/disconnect and restart operations
- Improved error handling consistency across all API endpoints
- Network list uses `data-ssid` attribute for safer SSID handling

## [0.1.6] - 2025-01-17

### Fixed

- **NDEF format compatibility**: Changed from Text Record (TNF=0x01) to MIME type Record (TNF=0x02) with `application/json`
- Tags written by ESP32-OpenSpool are now recognized by Orca Slicer and other OpenSpool-compatible applications

### Added

- Comprehensive NFC tag information output on Serial when reading:
    - UID, tag type, memory capacity
    - ATQA and SAK values
    - Password protection status
    - Write protection and config lock status
    - Authentication attempts remaining
    - Originality signature (if readable)
    - Raw hex dump of tag pages
    - Parsed OpenSpool data fields

### Technical

- NDEF Record format: `D2 10 [len] "application/json" + JSON payload`
- Updated file header comments (I2C → SPI documentation)

## [0.1.5] - 2025-01-16

### Changed

- Switched from I2C to SPI for PN532 communication (more reliable without pull-up resistors)
- Improved WiFi scan with auto-retry and progress indicator
- New modern confirmation modal for destructive actions (Erase Tag)
- Moved static file handler after API routes (fixes API not responding)

### Fixed

- WiFi scan causing watchdog timeout and device restart
- API endpoints returning 404 (static handler was catching all requests)
- Erase confirmation dialog not triggering erase action

### Technical

- SPI Pins: SCK=GPIO12, MISO=GPIO13, MOSI=GPIO11, SS=GPIO10
- Using software SPI for better compatibility
- Background task for WiFi scanning to prevent blocking

## [0.1.0] - 2025-01-16

### Added

- Initial release
- NTAG21x support (NTAG213, NTAG215, NTAG216)
- Automatic tag type detection
- Tag info display (type, UID, capacity, used bytes)
- Tag erase functionality
- OpenSpool protocol v1.0 support
- Web interface with dark mode
- 32 color presets plus hex input and color picker
- WiFi Access Point mode (SSID: OpenSpool)
- WiFi client mode with persistent credentials
- mDNS support (openspool.local)
- OTA firmware updates via web interface
- REST API for integration
- Temperature settings for nozzle and bed
