# Crosspoint Reader → M5PaperS3 Porting Plan

## 1. Hardware Comparison

| Feature | Xteink X4 (original) | M5PaperS3 (target) |
|---|---|---|
| MCU | ESP32-C3 (RISC-V, single-core) | ESP32-S3 (Xtensa LX7, dual-core 240MHz) |
| Flash | 16MB | 16MB |
| PSRAM | None | 8MB OPI-PSRAM |
| Display | 800x480 SSD1677 SPI e-ink | 960x540 parallel 8-bit e-ink (EPD bus) |
| Touch | None (buttons only) | GT911 capacitive touch (I2C) |
| Input | 7 buttons (4 front ADC, 2 side ADC, 1 power) | 1 power button + touch |
| SD Card | SPI (shared bus with display) | SPI (GPIO47 CS) |
| Battery | ADC on GPIO0 | ADC (via M5Unified) |
| RTC | None | BM8563 (I2C) |
| Power | GPIO3 wakeup | Power button + RTC wakeup |

## 2. M5PaperS3 Pin Map (from M5GFX source)

### E-Paper Display (8-bit parallel EPD bus)
- Data[0-7]: GPIO6, GPIO14, GPIO7, GPIO12, GPIO9, GPIO11, GPIO8, GPIO10
- PWR: GPIO46
- SPV: GPIO17
- CKV: GPIO18
- SPH: GPIO13
- OE: GPIO45
- LE: GPIO15
- CL: GPIO16
- Panel: 960x540, offset_rotation=3

### GT911 Touch (I2C)
- SDA: GPIO41
- SCL: GPIO42
- INT: GPIO48
- I2C Port: I2C_NUM_1
- Freq: 400kHz
- Addresses: 0x14 or 0x5D
- Touch range: x[0-539], y[0-959], offset_rotation=1

### SD Card
- CS: GPIO47
- Uses SPI bus (managed by M5Unified/SD library)

### Power
- PWROFF_PULSE_PIN: GPIO44

## 3. Architecture: What Changes vs What Stays

### KEEP UNCHANGED (application logic)
- `src/activities/*` - All UI activities (reader, library, settings, etc.)
- `src/components/*` - UI components
- `src/CrossPointSettings.*` - Settings management
- `src/CrossPointState.*` - App state
- `src/JsonSettingsIO.*` - JSON settings
- `src/RecentBooksStore.*` - Recent books
- `src/WifiCredentialStore.*` - WiFi credentials
- `src/fontIds.h` - Font IDs
- `src/images/*` - UI images/icons
- `src/network/*` - Network features
- `src/util/*` - Utilities
- `lib/Epub/*` - EPUB parsing
- `lib/EpdFont/*` - Font rendering
- `lib/GfxRenderer/*` - Graphics renderer (needs resolution constants update)
- `lib/FsHelpers/*` - File system helpers
- `lib/I18n/*` - Internationalization
- `lib/InflateReader/*` - Decompression
- `lib/JpegToBmpConverter/*` - Image converters
- `lib/PngToBmpConverter/*`
- `lib/KOReaderSync/*` - KOReader sync
- `lib/Logging/*` - Logging
- `lib/OpdsParser/*` - OPDS parser
- `lib/Serialization/*` - Serialization
- `lib/Txt/*` - TXT file support
- `lib/Utf8/*` - UTF-8 handling
- `lib/Xtc/*` - XTC support
- `lib/ZipFile/*` - ZIP handling
- `lib/expat/*` - XML parser
- `lib/picojpeg/*` - JPEG decoder
- `lib/uzlib/*` - Decompression

### REPLACE (HAL layer - the core of the port)
- `lib/hal/HalDisplay.*` → Use M5GFX/epdiy for 960x540 EPD (or porting freeink-sdk?)
- `lib/hal/HalGPIO.*` → Replace buttons with GT911 touch + power button
- `lib/hal/HalPowerManager.*` → ESP32-S3 deep sleep + GPIO44 power control
- `lib/hal/HalStorage.*` → SD card via M5Unified (GPIO47 CS)
- `lib/hal/HalSystem.*` → ESP32-S3 Xtensa panic handler (not RISC-V)

### REPLACE (SDK - no longer using open-x4-sdk)
- `open-x4-sdk/libs/display/EInkDisplay` → M5GFX Panel_EPD (960x540)
- `open-x4-sdk/libs/hardware/InputManager` → GT911 touch input manager
- `open-x4-sdk/libs/hardware/SDCardManager` → Arduino SD library via M5Unified
- `open-x4-sdk/libs/hardware/BatteryMonitor` → M5Unified battery API

### ADAPT
- `src/MappedInputManager.*` → Map touch zones to virtual buttons
- `src/main.cpp` → M5.begin() init, adapted power/sleep flow
- `platformio.ini` → ESP32-S3 board, M5Unified/M5GFX/epdiy deps
- `lib/GfxRenderer/GfxRenderer.h` → Update resolution constants, orientation

## 4. Touch Navigation Design

The X4 has 7 buttons: Back, Confirm, Left, Right, Up, Down, Power.
The PaperS3 has touch + 1 power button.

### Touch Zone Layout (960x540 display, portrait mode = 540x960)

```
+------------------------------------------+
|              [STATUS BAR]                 |  ← Tap: toggle status bar
|------------------------------------------|
|        |                    |             |
|  BACK  |                    |  FORWARD    |
|  zone  |    CENTER zone     |  zone       |
| (15%)  |     (50%)          |  (15%)      |
|        |  Tap: Confirm      |             |
|        |  Swipe L: Right    |             |
|        |  Swipe R: Left     |             |
|        |  Swipe U: Up/PgFwd |             |
|        |  Swipe D: Down/PgBk|             |
|        |                    |             |
|------------------------------------------|
|              [BOTTOM BAR]                 |  ← Tap: toggle bottom bar
+------------------------------------------+
```

### Touch → Button Mapping:
- **Tap left edge (0-15% width)**: PageBack (or Back in menus)
- **Tap right edge (85-100% width)**: PageForward (or Right in menus)
- **Tap center**: Confirm
- **Swipe left**: Right/Next
- **Swipe right**: Left/Previous  
- **Swipe up**: Up / Page Forward (in reader)
- **Swipe down**: Down / Page Back (in reader)
- **Long press center**: Back (go back / exit)
- **Tap top 10%**: Toggle status bar
- **Power button short press**: Sleep
- **Power button long press**: Power off

## 5. Implementation Strategy

### Phase 1: platformio.ini + Build Infrastructure
1. Create `[env:m5papers3]` targeting `esp32-s3-devkitm-1`
2. Add M5Unified, M5GFX, epdiy as dependencies
3. Remove open-x4-sdk dependencies (EInkDisplay, InputManager, SDCardManager, BatteryMonitor)
4. Update build flags for ESP32-S3 (OPI-PSRAM, 16MB flash)
5. Remove RISC-V specific flags, add Xtensa-appropriate ones

### Phase 2: HAL Layer Replacement
1. **HalDisplay** → Wrap M5GFX's EPD panel (960x540)
   - `begin()` → `M5.Display.begin()` via M5Unified
   - Frame buffer access via M5GFX APIs
   - Refresh modes mapped to M5GFX equivalents
   - Grayscale support via epdiy's 16-level grayscale
   
2. **HalGPIO** → Touch-based input via GT911
   - Create `TouchInputManager` that reads GT911 via M5Unified touch API
   - Translate touch events (tap, swipe, long press) into virtual button events
   - Maintain same interface: `isPressed()`, `wasPressed()`, `wasReleased()`, `getHeldTime()`
   - Power button via GPIO (M5Unified handles this)

3. **HalStorage** → SD card via M5Unified + Arduino SD
   - M5Unified initializes SD card internally
   - Replace SdFat with Arduino SD or keep SdFat with correct SPI pins
   - SD CS = GPIO47

4. **HalPowerManager** → ESP32-S3 power management
   - Deep sleep via `esp_deep_sleep_start()` with GPIO wakeup
   - Power off pulse via GPIO44
   - Battery reading via M5Unified Power API
   - RTC alarm wakeup (BM8563) for timed wake

5. **HalSystem** → Xtensa-specific panic handler
   - Replace RISC-V `RvExcFrame` with Xtensa `XtExcFrame`
   - Adapt backtrace capture for Xtensa architecture

### Phase 3: Resolution & Renderer Adaptation
1. Update `GfxRenderer` orientation handling for 960x540 (was 800x480)
2. Viewable margins may need adjustment
3. Font sizes scale well since 960x540 is ~20% larger in each dimension
4. Book rendering pages will fit more text per page

### Phase 4: Input Adaptation
1. Rewrite `MappedInputManager` to work with touch zones
2. Ensure all activities work with touch navigation
3. Button label display → replace with touch hint overlays or remove

### Phase 5: Sleep & Power
1. `/sleep` folder support for sleep cover images
2. Power button behavior (short=sleep, long=power off)
3. Deep sleep with touch wakeup or power button wakeup

## 6. Key Library Dependencies (new)

```ini
lib_deps =
  m5stack/M5Unified @ ^0.2.2
  m5stack/M5GFX @ ^0.2.7
  epdiy=https://github.com/vroland/epdiy.git
  bblanchon/ArduinoJson @ 7.4.2
  ricmoo/QRCode @ 0.0.1
  bitbank2/PNGdec @ ^1.0.0
  bitbank2/JPEGDEC @ ^1.8.0
  links2004/WebSockets @ 2.7.3
```

## 7. Risk Areas
- **Display buffer format**: X4 uses 1-bit SPI framebuffer (800x480/8 = 48000 bytes). PaperS3 uses parallel EPD bus with potentially different buffer format. M5GFX abstracts this.
- **Grayscale**: X4 has 2-bit grayscale (LSB+MSB buffers). PaperS3/epdiy supports 16-level grayscale natively. Should be better.
- **Touch debouncing**: Need careful implementation to avoid phantom inputs on e-ink (slow refresh).
- **SdFat vs SD.h**: crosspoint-reader uses SdFat heavily. May need to keep SdFat but configure for PaperS3 SPI pins.
- **Memory**: ESP32-S3 with 8MB PSRAM is far more capable. Can remove memory optimization workarounds.
- **Panic handler**: RISC-V specific code in HalSystem.cpp must be rewritten for Xtensa.

## 8. Syncing from Upstream — Cherry-Pick Conflict Discipline

Upstream commits are pulled in one at a time with `git cherry-pick -x <hash>` and recorded in the commit message as `port upstream <hash>`. Most cherry-picks conflict against the port's HAL replacements and the `#if CROSSPOINT_PAPERS3` device-specific branches sprinkled through `lib/` and `src/`.

### Do NOT use `git checkout --theirs <file>` to resolve a conflict.

This is the single most expensive failure mode discovered during the 1.3.0 sync. `--theirs` takes upstream's complete file verbatim, which silently drops every Paper S3-specific change the port has made. The damage is invisible at conflict-resolution time (the commit lands cleanly) and only surfaces when the build fails several commits later — by which point the regression is buried in the cherry-pick history and hard to attribute.

Concrete regressions caused by `--theirs` during the 1.3.0 sync:
- `src/activities/boot_sleep/SleepActivity.cpp` — lost the wallpaper-recency rewrite (`recentSleepImages` ring buffer) that an earlier cherry-pick had introduced; left a dangling reference to a removed `lastSleepImage` field.
- `src/components/themes/BaseTheme.cpp` — lost three `CROSSPOINT_PAPERS3` blocks including the 540-px-wide touch-tuned button-hint positions (`{12, 144, 276, 408}`), replaced with X4's `{25, 130, 245, 350}`.
- `src/components/themes/lyra/LyraTheme.cpp` — lost two more `CROSSPOINT_PAPERS3` blocks for the same reason.
- `lib/JpegToBmpConverter/JpegToBmpConverter.cpp` — lost the Paper S3-only `jpegMemTo1BitBmp` fast path that `Epub::generateThumbBmp` depends on. Linker failure several commits later.

### Use this resolution flow instead

1. **Inspect the conflict before resolving.** Run `git diff --name-only --diff-filter=U` and then `git diff` on each conflicted file. Look for `#if CROSSPOINT_PAPERS3` blocks on the `HEAD` side — those are what `--theirs` would drop.
2. **Hand-merge by default.** Open the file in an editor, keep the port's `#if CROSSPOINT_PAPERS3` blocks intact, integrate upstream's new logic into the `#else` branch (or the shared path).
3. **`--theirs` is reserved for content the port doesn't customise.** Translation YAML files are the only routine case (e.g. `lib/I18n/translations/ukrainian.yaml`) — the entire file's purpose is to mirror upstream language coverage.
4. **`--ours` is rare but legal** when upstream changes something the port has intentionally diverged from (e.g. an upstream X3-only feature wired into a shared file that we want to keep Paper-S3-disabled).
5. **Audit before pushing.** After all cherry-picks land but before opening a PR, run:
   ```bash
   git diff <pre-sync-master>..HEAD --stat | grep -E "src/.*\.cpp|lib/.*\.cpp" | \
     while read line; do
       file=$(echo "$line" | awk '{print $1}')
       pre=$(git show <pre-sync-master>:"$file" 2>/dev/null | grep -c "CROSSPOINT_PAPERS3")
       post=$(grep -c "CROSSPOINT_PAPERS3" "$file" 2>/dev/null || echo 0)
       [ "$pre" -gt "$post" ] && echo "REGRESSED: $file ($pre → $post)"
     done
   ```
   This catches dropped device-specific blocks before CI does.

### When a cherry-pick is reported as empty

`git cherry-pick` will say `nothing to commit, working tree clean` if upstream's change was already applied (often the case for translation refinements that were independently merged into the port). Use `git cherry-pick --skip` and continue — don't `--abort`.
