"""
PlatformIO pre-build script: raise Sqlite3Esp32's lemon parser stack depth.

esp32_arduino_sqlite3_lib ships config_ext.h with YYSTACKDEPTH=20 (the
upstream sqlite3.c default is 100) to shave RAM off the parser stack.
UPSERT statements (INSERT ... ON CONFLICT DO UPDATE) push the lemon
state stack past 20, so sqlite3_prepare_v2() fails with "parser stack
overflow" at runtime — every Anki upsert (deck/card state/daily stats/
media) uses UPSERT, so the define must match the upstream default.

RAM impact: yyStackEntry yystack[YYSTACKDEPTH] is a C-stack local inside
yy_parse(); 20 -> 100 costs ~1.3 KB of stack per parse, which the Anki
import task (16 KB) and the boot task comfortably fit.

It also re-enables the SQLite progress callback (config_ext.h ships
SQLITE_OMIT_PROGRESS_CALLBACK=1), which the Anki import uses via
sqlite3_progress_handler() to yield from inside long-running statements
(SD-card JOIN scans / batch commits) and keep the task watchdog fed.

Idempotent: each edit is applied only while the original line is present;
an already-patched tree is left untouched.
"""

Import("env")  # noqa: F821 (SCons-injected global)
import os
import re

OLD_STACK = "#define YYSTACKDEPTH                        20"
NEW_STACK = "#define YYSTACKDEPTH                        (100)"
# config_ext.h compiles the progress callback out; regex tolerates the
# column-aligned whitespace before the trailing "1".
PROG_RE = re.compile(r"#define\s+SQLITE_OMIT_PROGRESS_CALLBACK\s+\d+")
PROG_UNDEF = "#undef  SQLITE_OMIT_PROGRESS_CALLBACK"


def patch_sqlite3esp32(env):
    libdeps_dir = os.path.join(env["PROJECT_DIR"], ".pio", "libdeps")
    if not os.path.isdir(libdeps_dir):
        return
    for env_dir in os.listdir(libdeps_dir):
        cfg = os.path.join(libdeps_dir, env_dir, "Sqlite3Esp32", "src", "config_ext.h")
        if not os.path.isfile(cfg):
            continue
        with open(cfg, "r", encoding="utf-8") as f:
            text = f.read()
        original = text
        if OLD_STACK in text:
            text = text.replace(OLD_STACK, NEW_STACK)
            print(f"  [patch_sqlite3esp32] YYSTACKDEPTH 20 -> 100 in {cfg}")
        if PROG_RE.search(text):
            text = PROG_RE.sub(PROG_UNDEF, text)
            print(f"  [patch_sqlite3esp32] re-enabled sqlite3_progress_handler in {cfg}")
        if text != original:
            with open(cfg, "w", encoding="utf-8") as f:
                f.write(text)


patch_sqlite3esp32(env)
