#pragma once

namespace anki {

/// Register the SdFat-backed SQLite VFS as SQLite's default VFS.
///
/// The project's SD card is accessed through SDCardManager's SdFat FsVolume
/// (SPI or native SDMMC), which is NOT exposed to the ESP-IDF POSIX layer —
/// the stock sqlite3 "ESP32" VFS (fopen/fread) cannot reach it. This VFS
/// bridges every sqlite3_file operation onto SdFat FsFile handles instead.
///
/// Idempotent; call before the first sqlite3_open() once the SD card is
/// mounted. VFS registration itself touches no files, so calling it before
/// SDCardManager::begin() is harmless (opens will simply fail).
void registerSdFatVfs();

}  // namespace anki
