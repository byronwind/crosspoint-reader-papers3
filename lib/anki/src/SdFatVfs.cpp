// Guarded so non-ANKIEINK builds compile this as an empty translation unit
// (see DeckStore.cpp for why).
#ifdef ANKIEINK

#ifdef SIMULATOR

#include "SdFatVfs.h"

namespace anki {

// The desktop simulator links the host sqlite3, whose default unix VFS reads
// and writes real filesystem paths under CROSSPOINT_SIM_SD — no SdFat bridge
// is needed (or possible: esp_random/SDCardManager don't exist on the host).
// Callers convert SD-logical paths with HalStorage::hostPath() before
// sqlite3_open instead. Keep the symbol so call sites need no #ifdefs.
void registerSdFatVfs() {}

}  // namespace anki

#else  // device build: full SdFat-backed VFS below

#include "SdFatVfs.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <SDCardManager.h>
#include <esp_random.h>
#include <sqlite3.h>

#include <stdio.h>
#include <string.h>
#include <time.h>

namespace anki {
namespace {

// SdFat supports long (UTF-8) names; SQLite's default path cap is 512.
constexpr int kMaxPathname = 255;

// SQLite file handle: the sqlite3_file base struct followed by a
// placement-new'd FsFile (FsFile is move-only, so it cannot be memcpy'd in).
struct SdFatFile {
  sqlite3_file base;
  FsFile file;
};

// Directory of the most recently opened .db. SQLite opens temp files with a
// NULL name, so they are placed next to the database (same FAT volume by
// definition — SdFat has no notion of a working directory).
char dbRootDir[kMaxPathname + 1] = "/";
uint32_t tempFileSeq = 0;

void trackDbDir(const char* zName) {
  const char* ext = strrchr(zName, '.');
  if (!ext || strcmp(ext, ".db") != 0) return;
  const char* slash = strrchr(zName, '/');
  if (!slash) return;
  const size_t len = static_cast<size_t>(slash - zName + 1);
  if (len > kMaxPathname) return;
  memcpy(dbRootDir, zName, len);
  dbRootDir[len] = '\0';
}

// ── sqlite3_io_methods ──

// SdFat is not thread-safe and its single instance is shared with the main
// task (fonts, settings, book files), so every VFS call must run under
// HalStorage's StorageLock. Without it, concurrent SPI transactions from
// both cores race SdSpiCard::m_spiActive and trip FreeRTOS's
// xTaskPriorityDisinherit assert (see HalStorage.cpp impl notes).

int sdFatClose(sqlite3_file* pFile) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  p->file.~FsFile();
  return SQLITE_OK;
}

int sdFatRead(sqlite3_file* pFile, void* zBuf, int iAmt, sqlite_int64 iOfst) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  const uint64_t fileSize = p->file.fileSize();
  // FAT cannot seek past end-of-file, but SQLite grows a database by writing
  // pages beyond the current size and re-reading them. Emulate POSIX pread,
  // which returns zero bytes past EOF: SQLite treats SHORT_READ as a
  // zero-filled new page and requires the VFS to zero-fill the buffer.
  if (iOfst > static_cast<sqlite_int64>(fileSize)) {
    memset(zBuf, 0, static_cast<size_t>(iAmt));
    return SQLITE_IOERR_SHORT_READ;
  }
  if (!p->file.seek(static_cast<uint64_t>(iOfst))) {
    LOG_ERR("ANKI", "VFS read: seek(%lld) failed (fileSize=%llu)", static_cast<long long>(iOfst),
            static_cast<unsigned long long>(fileSize));
    return SQLITE_IOERR_READ;
  }
  const size_t n = p->file.read(zBuf, static_cast<size_t>(iAmt));
  if (n != static_cast<size_t>(iAmt)) {
    // Short read at EOF: zero-fill the remainder (VFS contract, see the
    // SQLITE_IOERR_SHORT_READ docs in sqlite3.h).
    memset(static_cast<uint8_t*>(zBuf) + n, 0, static_cast<size_t>(iAmt) - n);
    return SQLITE_IOERR_SHORT_READ;
  }
  return SQLITE_OK;
}

int sdFatWrite(sqlite3_file* pFile, const void* zBuf, int iAmt, sqlite_int64 iOfst) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  const uint64_t fileSize = p->file.fileSize();
  if (!p->file.seek(static_cast<uint64_t>(iOfst))) {
    if (iOfst <= static_cast<sqlite_int64>(fileSize)) {
      LOG_ERR("ANKI", "VFS write: seek(%lld) failed (fileSize=%llu)",
              static_cast<long long>(iOfst), static_cast<unsigned long long>(fileSize));
      return SQLITE_IOERR_WRITE;
    }
    // Writing past EOF: FAT refuses to seek beyond the file size, but SQLite
    // grows a database by writing pages past the current end. Extend the file
    // with zeros up to the write offset first (writes auto-grow the file).
    if (!p->file.seek(fileSize)) {
      LOG_ERR("ANKI", "VFS write: seek to EOF(%llu) failed", static_cast<unsigned long long>(fileSize));
      return SQLITE_IOERR_WRITE;
    }
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    uint64_t gap = static_cast<uint64_t>(iOfst) - fileSize;
    while (gap > 0) {
      const size_t chunk = gap < sizeof(zeros) ? static_cast<size_t>(gap) : sizeof(zeros);
      if (p->file.write(zeros, chunk) != chunk) {
        LOG_ERR("ANKI", "VFS write: extend failed (%llu bytes gap)",
                static_cast<unsigned long long>(gap));
        return SQLITE_IOERR_WRITE;
      }
      gap -= chunk;
    }
    // Position is now at iOfst; fall through to write the payload.
  }
  const size_t n = p->file.write(zBuf, static_cast<size_t>(iAmt));
  if (n != static_cast<size_t>(iAmt)) {
    LOG_ERR("ANKI", "VFS write: %d bytes at %lld -> %u written (fileSize=%llu)", iAmt,
            static_cast<long long>(iOfst), static_cast<unsigned>(n),
            static_cast<unsigned long long>(p->file.fileSize()));
    return SQLITE_IOERR_WRITE;
  }
  return SQLITE_OK;
}

int sdFatTruncate(sqlite3_file* pFile, sqlite_int64 size) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  const bool ok = p->file.truncate(static_cast<uint64_t>(size));
  if (!ok) LOG_ERR("ANKI", "VFS truncate(%lld) failed", static_cast<long long>(size));
  return ok ? SQLITE_OK : SQLITE_IOERR_TRUNCATE;
}

int sdFatSync(sqlite3_file* pFile, int /*flags*/) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  const bool ok = p->file.sync();
  if (!ok) LOG_ERR("ANKI", "VFS sync failed");
  return ok ? SQLITE_OK : SQLITE_IOERR_FSYNC;
}

int sdFatFileSize(sqlite3_file* pFile, sqlite_int64* pSize) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  *pSize = static_cast<sqlite_int64>(p->file.fileSize());
  return SQLITE_OK;
}

// FAT has no file locking; SQLite runs single-connection here, so all lock
// calls are no-ops (same trade-off as the stock ESP32 VFS).
int sdFatLock(sqlite3_file*, int) { return SQLITE_OK; }
int sdFatUnlock(sqlite3_file*, int) { return SQLITE_OK; }
int sdFatCheckReservedLock(sqlite3_file*, int* pResOut) {
  *pResOut = 0;
  return SQLITE_OK;
}
int sdFatFileControl(sqlite3_file*, int, void*) { return SQLITE_OK; }
int sdFatSectorSize(sqlite3_file*) { return 512; }
int sdFatDeviceCharacteristics(sqlite3_file*) { return 0; }

const sqlite3_io_methods sdFatIoMethods = {
    1,                     // iVersion
    sdFatClose,            // xClose
    sdFatRead,             // xRead
    sdFatWrite,            // xWrite
    sdFatTruncate,         // xTruncate
    sdFatSync,             // xSync
    sdFatFileSize,         // xFileSize
    sdFatLock,             // xLock
    sdFatUnlock,           // xUnlock
    sdFatCheckReservedLock,  // xCheckReservedLock
    sdFatFileControl,      // xFileControl
    sdFatSectorSize,       // xSectorSize
    sdFatDeviceCharacteristics,  // xDeviceCharacteristics
};

// ── sqlite3_vfs methods ──

int sdFatOpen(sqlite3_vfs*, const char* zName, sqlite3_file* pFile, int flags, int* pOutFlags) {
  HalStorage::StorageLock lock;
  auto* p = reinterpret_cast<SdFatFile*>(pFile);
  new (&p->file) FsFile();

  char tempPath[kMaxPathname + 1];
  if (zName == nullptr) {
    // SQLite temp file (sorts, temp tables): unique name next to the DB.
    snprintf(tempPath, sizeof(tempPath), "%ssqlite_tmp_%lu.tmp", dbRootDir,
             static_cast<unsigned long>(tempFileSeq++));
    zName = tempPath;
  } else {
    trackDbDir(zName);
  }

  oflag_t oflag = (flags & SQLITE_OPEN_READONLY) ? O_RDONLY : O_RDWR;
  if (flags & SQLITE_OPEN_CREATE) oflag |= O_CREAT;

  p->file = SdMan.open(zName, oflag);
  if (!p->file) {
    LOG_ERR("ANKI", "VFS open failed: %s (flags=0x%x)", zName ? zName : "(temp)", flags);
    p->file.~FsFile();
    return SQLITE_CANTOPEN;
  }
  if (zName == tempPath) {
    // Unlink the directory entry while keeping the open handle: SQLite never
    // xDeletes unnamed temp files, so this keeps a crashed run from leaving
    // stale files behind. If the FAT driver refuses, the next seq name still
    // avoids clobbering live data.
    SdMan.remove(tempPath);
  }

  p->base.pMethods = &sdFatIoMethods;
  if (pOutFlags) *pOutFlags = flags;
  return SQLITE_OK;
}

int sdFatDelete(sqlite3_vfs*, const char* zPath, int /*dirSync*/) {
  HalStorage::StorageLock lock;
  if (!SdMan.exists(zPath)) return SQLITE_OK;  // ENOENT is not an error
  const bool ok = SdMan.remove(zPath);
  if (!ok) LOG_ERR("ANKI", "VFS delete failed: %s", zPath);
  return ok ? SQLITE_OK : SQLITE_IOERR_DELETE;
}

int sdFatAccess(sqlite3_vfs*, const char* zPath, int /*flags*/, int* pResOut) {
  HalStorage::StorageLock lock;
  *pResOut = SdMan.exists(zPath) ? 1 : 0;
  return SQLITE_OK;
}

int sdFatFullPathname(sqlite3_vfs*, const char* zPath, int nOut, char* zOut) {
  // SdFat paths are absolute from the volume root; qualify bare names.
  if (zPath[0] == '/') {
    strncpy(zOut, zPath, static_cast<size_t>(nOut));
  } else {
    snprintf(zOut, static_cast<size_t>(nOut), "/%s", zPath);
  }
  zOut[nOut - 1] = '\0';
  return SQLITE_OK;
}

void* sdFatDlOpen(sqlite3_vfs*, const char*) { return nullptr; }
void sdFatDlError(sqlite3_vfs*, int nByte, char* zErrMsg) {
  sqlite3_snprintf(nByte, zErrMsg, "Loadable extensions are not supported");
  zErrMsg[nByte - 1] = '\0';
}
void (*sdFatDlSym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }
void sdFatDlClose(sqlite3_vfs*, void*) {}

int sdFatRandomness(sqlite3_vfs*, int nByte, char* zByte) {
  int n = 0;
  while (n < nByte) {
    const uint32_t r = esp_random();
    const int copy = (nByte - n) < 4 ? (nByte - n) : 4;
    memcpy(zByte + n, &r, static_cast<size_t>(copy));
    n += copy;
  }
  return SQLITE_OK;
}

int sdFatSleep(sqlite3_vfs*, int nMicro) {
  delay(nMicro / 1000);
  return nMicro;
}

int sdFatCurrentTime(sqlite3_vfs*, double* pTime) {
  const time_t t = time(nullptr);
  *pTime = static_cast<double>(t) / 86400.0 + 2440587.5;
  return SQLITE_OK;
}

sqlite3_vfs* vfsInstance() {
  static sqlite3_vfs vfs = {
      1,                       // iVersion — v1, no WAL/xShm (WAL is compiled out
                               // of esp32_arduino_sqlite3_lib anyway)
      sizeof(SdFatFile),       // szOsFile
      kMaxPathname,            // mxPathname
      nullptr,                 // pNext
      "sdfat",                 // zName
      nullptr,                 // pAppData
      sdFatOpen,               // xOpen
      sdFatDelete,             // xDelete
      sdFatAccess,             // xAccess
      sdFatFullPathname,       // xFullPathname
      sdFatDlOpen,             // xDlOpen
      sdFatDlError,            // xDlError
      sdFatDlSym,              // xDlSym
      sdFatDlClose,            // xDlClose
      sdFatRandomness,         // xRandomness
      sdFatSleep,              // xSleep
      sdFatCurrentTime,        // xCurrentTime
  };
  return &vfs;
}

}  // namespace

void registerSdFatVfs() {
  static bool registered = false;
  if (registered) return;
  sqlite3_vfs_register(vfsInstance(), /*makeDefault=*/1);
  registered = true;
}

}  // namespace anki

#endif  // !SIMULATOR

#endif  // ANKIEINK
