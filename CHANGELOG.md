# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
