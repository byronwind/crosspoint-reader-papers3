#pragma once

namespace anki {

// SD card root paths for AnkiEInk. SdFat paths are absolute from the volume
// root (no mount-point prefix like "/sd").
inline constexpr const char* kAnkiRootDir = "/AnkiEInk";
inline constexpr const char* kAnkiDbPath = "/AnkiEInk/anki.db";
inline constexpr const char* kAnkiMediaDir = "/AnkiEInk/media";
inline constexpr const char* kAnkiTmpDir = "/AnkiEInk/tmp";

}  // namespace anki
