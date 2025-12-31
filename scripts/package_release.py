"""
PlatformIO extra script to package build artifacts into a ZIP per environment.
Creates a zip in project_dir/dist including firmware.bin, littlefs.bin, bootloader.bin, partitions.bin if present,
plus a README with flash addresses. When a project-local partitions CSV is used, the filesystem offset is parsed
and included for convenience.
"""
from __future__ import print_function
import os
import time
import zipfile

Import("env")

PROJECT_DIR = env["PROJECT_DIR"]
# Use env.subst to ensure we get the actual string path
BUILD_DIR = env.subst("$BUILD_DIR")
ENV_NAME = str(env["PIOENV"])  # e.g. esp32-ESPCAN

DIST_DIR = os.path.join(PROJECT_DIR, "dist")
os.makedirs(DIST_DIR, exist_ok=True)

# Typical ESP32 flash addresses (Arduino default)
FLASH_ADDR = {
    "bootloader": "0x1000",
    "partitions": "0x8000",
    "firmware": "0x10000",
}

def _to_hex(val):
    try:
        s = str(val).strip()
        if s.lower().startswith("0x"):
            return s.lower()
        # decimal to hex
        return hex(int(s)).lower()
    except Exception:
        return None

def find_fs_offset(partitions_csv_path):
    """Parse partitions CSV to locate LittleFS/SPIFFS/FAT data offset.
    Returns hex string like '0x290000' or None if not found/parsable.
    """
    try:
        if not partitions_csv_path or not os.path.exists(partitions_csv_path):
            return None
        f"Helper scripts included:\n"
        f"- flash_example.cmd: Windows batch file. Edit COM port (COM3) and ensure esptool.py is in PATH (e.g., 'pip install esptool').\n"
        f"- flash_example.sh: macOS/Linux shell script. Auto-detects /dev/ttyUSB*/ttyACM* or set PORT=/dev/ttyUSB0 before running.\n"
        f"  If auto-detect fails: 'PORT=/dev/ttyACM0 ./flash_example.sh'\n"
        f"Alternative usage: 'python -m esptool' can be used instead of 'esptool.py' if PATH is not set.\n\n"
        with open(partitions_csv_path, 'r', encoding='utf-8') as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith('#'):
                    continue
                parts = [p.strip() for p in line.split(',')]
                if len(parts) < 5:
                    continue
                name, ptype, subtype, offset, size = parts[:5]
                name_l = name.lower()
                ptype_l = ptype.lower()
                subtype_l = subtype.lower()
                is_data = (ptype_l == 'data')
                is_fs = (subtype_l in ('spiffs', 'littlefs', 'fat')) or ('spiffs' in name_l) or ('littlefs' in name_l) or ('storage' in name_l)
                if is_data and is_fs:
                    return _to_hex(offset)
        return None
    except Exception:
        return None

def build_readme_text(env_name, fs_addr, partitions_csv_name):
    fs_line = f"- littlefs.bin (filesystem) @ {fs_addr}\n" if fs_addr else "- littlefs.bin (filesystem) -> address per your partitions CSV\n"
    csv_note = f"Active partitions CSV: {partitions_csv_name}\n" if partitions_csv_name else ""

    # Determine chip and bootloader address recommendation per env
    def chip_for_env(name):
        if name.startswith('esp32s3'):
            return 'esp32s3'
        if name.startswith('esp32c3'):
            return 'esp32c3'
        return 'esp32'

    chip = chip_for_env(env_name)
    # Bootloader address differs across chips; default to classic ESP32 0x1000
    boot_addr = FLASH_ADDR['bootloader'] if chip == 'esp32' else '0x0'
    part_addr = FLASH_ADDR['partitions']
    app_addr = FLASH_ADDR['firmware']
    fs_addr_display = fs_addr or '<FS_OFFSET_FROM_CSV>'

    # Example esptool command for Windows (adjust COM port and baud)
    esptool_cmd = (
        f"esptool.py --chip {chip} --port COM3 --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin {fs_addr_display} littlefs.bin\n"
    )

    boot_note = "(ESP32-S3/C3 often use bootloader @ 0x0; ESP32 uses 0x1000)\n"

    return (
        f"DIY Battery BMS - Release Artifacts ({env_name})\n\n"
        f"Files included:\n"
        f"- firmware.bin (app) @ {app_addr}\n"
        f"- partitions.bin @ {part_addr}\n"
        f"- bootloader.bin @ {boot_addr}\n"
        f"{fs_line}\n"
        f"{csv_note}"
        f"Notes:\n"
        f"- The filesystem (littlefs.bin) address depends on the partitions file in use.\n"
        f"  If using custom partitions in this project (e.g., partitions_8MB.csv), refer to that CSV to confirm.\n"
        f"- Typical Arduino defaults (4MB flash) place SPIFFS/LittleFS around 0x290000, but always confirm with your CSV.\n"
        f"- Use your preferred ESP32 flasher tool and supply addresses accordingly.\n"
        f"- {boot_note}\n"
        f"Flash via esptool (example - adjust COM port):\n"
        f"{esptool_cmd}\n"
        f"Generated on: {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
    )

def build_esptool_cmd(env_name, fs_addr):
    """Build an esptool.py flash command for the given environment."""
    def chip_for_env(name):
        if name.startswith('esp32s3'):
            return 'esp32s3'
        if name.startswith('esp32c3'):
            return 'esp32c3'
        return 'esp32'
    chip = chip_for_env(env_name)
    boot_addr = FLASH_ADDR['bootloader'] if chip == 'esp32' else '0x0'
    part_addr = FLASH_ADDR['partitions']
    app_addr = FLASH_ADDR['firmware']
    fs_addr_display = fs_addr or '<FS_OFFSET_FROM_CSV>'
    return (
        f"esptool.py --chip {chip} --port COM3 --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin {fs_addr_display} littlefs.bin"
    )

def build_esptool_cmd_sh(env_name, fs_addr):
    """Build a macOS/Linux esptool.py flash command for the given environment."""
    def chip_for_env(name):
        if name.startswith('esp32s3'):
            return 'esp32s3'
        if name.startswith('esp32c3'):
            return 'esp32c3'
        return 'esp32'
    chip = chip_for_env(env_name)
    boot_addr = FLASH_ADDR['bootloader'] if chip == 'esp32' else '0x0'
    part_addr = FLASH_ADDR['partitions']
    app_addr = FLASH_ADDR['firmware']
    fs_addr_display = fs_addr or '<FS_OFFSET_FROM_CSV>'
    return (
        f"esptool.py --chip {chip} --port \"$PORT\" --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin {fs_addr_display} littlefs.bin"
    )

README_TEXT = f"""
DIY Battery BMS - Release Artifacts ({ENV_NAME})\n\nFiles included:\n- firmware.bin (app) @ {FLASH_ADDR['firmware']}\n- partitions.bin @ {FLASH_ADDR['partitions']}\n- bootloader.bin @ {FLASH_ADDR['bootloader']}\n- littlefs.bin (filesystem) -> address per your partitions CSV\n\nNotes:\n- The filesystem (littlefs.bin) address depends on the partitions file in use.\n  If using custom partitions in this project (e.g., partitions_8MB.csv), refer to that CSV to find the offset.\n- Typical Arduino defaults (4MB flash): SPIFFS/LittleFS often starts near 0x290000, but confirm with your partitions.\n- Use your preferred ESP32 flasher tool and supply addresses accordingly.\n\nGenerated on: {time.strftime('%Y-%m-%d %H:%M:%S')}\n"""


def package_release(source, target, env):
    try:
        timestamp = time.strftime("%Y%m%d-%H%M")
        zip_name = f"DiyBatteryBMS-{ENV_NAME}-{timestamp}.zip"
        zip_path = os.path.join(DIST_DIR, zip_name)

        candidates = {
            "firmware.bin": os.path.join(BUILD_DIR, "firmware.bin"),
            "littlefs.bin": os.path.join(BUILD_DIR, "littlefs.bin"),
            "bootloader.bin": os.path.join(BUILD_DIR, "bootloader.bin"),
            "partitions.bin": os.path.join(BUILD_DIR, "partitions.bin"),
        }

        # Collect existing files
        files_to_include = [(name, path) for name, path in candidates.items() if os.path.exists(path)]
        if not files_to_include:
            print("[PACKAGER] No binaries found to package.")
            return

        # Determine partitions CSV path from project options (if local CSV is used)
        partitions_opt = None
        try:
            partitions_opt = env.GetProjectOption('board_build.partitions')
        except Exception:
            partitions_opt = None
        partitions_csv_path = None
        partitions_csv_name = None
        if partitions_opt:
            # If the option points to a file in the project dir, use it
            cand = os.path.join(PROJECT_DIR, partitions_opt)
            if os.path.exists(cand):
                partitions_csv_path = cand
                partitions_csv_name = os.path.basename(cand)
            else:
                partitions_csv_name = partitions_opt

        fs_addr = find_fs_offset(partitions_csv_path)
        readme_text = build_readme_text(ENV_NAME, fs_addr, partitions_csv_name)

        with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            # Add binaries
            for name, path in files_to_include:
                zf.write(path, arcname=name)
            # Add README
            zf.writestr("README.txt", readme_text)
            # Add Windows batch file with example esptool command
            flash_cmd = (
                "@echo off\n"
                "REM Adjust COM port (COM3) and ensure esptool.py is in PATH\n"
                f"{build_esptool_cmd(ENV_NAME, fs_addr)}\n"
                "pause\n"
            )
            zf.writestr("flash_example.cmd", flash_cmd)
            # Add macOS/Linux shell script with example esptool command
            flash_sh = (
                "#!/usr/bin/env bash\n"
                "set -euo pipefail\n"
                "# Auto-detect serial port or use PORT env var\n"
                "PORT=\"${PORT:-}\"\n"
                "if [ -z \"$PORT\" ]; then\n"
                "  for p in /dev/ttyUSB0 /dev/ttyUSB1 /dev/ttyACM0 /dev/ttyACM1; do\n"
                "    if [ -e \"$p\" ]; then PORT=\"$p\"; break; fi\n"
                "  done\n"
                "fi\n"
                "if [ -z \"$PORT\" ]; then\n"
                "  echo \"No serial port found. Set PORT=/dev/ttyUSBX and re-run.\"\n"
                "  exit 1\n"
                "fi\n"
                "echo \"Using serial port: $PORT\"\n"
                f"{build_esptool_cmd_sh(ENV_NAME, fs_addr)}\n"
            )
            # Ensure Unix executable permissions in zip
            zi = zipfile.ZipInfo("flash_example.sh")
            zi.external_attr = (0o755 << 16)
            zf.writestr(zi, flash_sh)

        print("[PACKAGER] Created:", zip_path)
        for name, _ in files_to_include:
            print("[PACKAGER]  -", name)
    except Exception as e:
        print("[PACKAGER] Error:", str(e))

# Package after firmware is built (ensures at least firmware.bin exists)
env.AddPostAction("$BUILD_DIR/firmware.bin", package_release)

# Also package after building filesystem image if invoked separately
try:
    env.AddPostAction("buildfs", package_release)
except Exception:
    pass
