"""
PlatformIO extra script to package build artifacts into a ZIP per environment.
Creates a zip in project_dir/dist including firmware.bin, bootloader.bin, partitions.bin if present,
plus a README with flash addresses.
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

def build_readme_text(env_name, partitions_csv_name):
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
    # Example esptool command for Windows (adjust COM port and baud)
    esptool_cmd = (
        f"esptool.py --chip {chip} --port COM3 --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin\n"
    )

    boot_note = "(ESP32-S3/C3 often use bootloader @ 0x0; ESP32 uses 0x1000)\n"

    return (
        f"DIY Battery BMS - Release Artifacts ({env_name})\n\n"
        f"Files included:\n"
        f"- firmware.bin (app) @ {app_addr}\n"
        f"- partitions.bin @ {part_addr}\n"
        f"- bootloader.bin @ {boot_addr}\n"
        f"{csv_note}"
        f"Notes:\n"
        f"- Flash addresses above follow Arduino defaults; confirm against your partitions CSV if using a custom layout.\n"
        f"- Use your preferred ESP32 flasher tool and supply addresses accordingly.\n"
        f"- {boot_note}\n"
        f"Flash via esptool (example - adjust COM port):\n"
        f"{esptool_cmd}\n"
        f"Generated on: {time.strftime('%Y-%m-%d %H:%M:%S')}\n"
    )

def build_esptool_cmd(env_name):
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
    return (
        f"esptool.py --chip {chip} --port COM3 --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin"
    )

def build_esptool_cmd_sh(env_name):
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
    return (
        f"esptool.py --chip {chip} --port \"$PORT\" --baud 921600 write_flash -z --flash_mode dio --flash_size detect "
        f"{boot_addr} bootloader.bin {part_addr} partitions.bin {app_addr} firmware.bin"
    )

README_TEXT = f"""
DIY Battery BMS - Release Artifacts ({ENV_NAME})\n\nFiles included:\n- firmware.bin (app) @ {FLASH_ADDR['firmware']}\n- partitions.bin @ {FLASH_ADDR['partitions']}\n- bootloader.bin @ {FLASH_ADDR['bootloader']}\n\nNotes:\n- Flash addresses above follow Arduino defaults; confirm against your partitions CSV if using a custom layout.\n- Use your preferred ESP32 flasher tool and supply addresses accordingly.\n\nGenerated on: {time.strftime('%Y-%m-%d %H:%M:%S')}\n"""


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

        readme_text = build_readme_text(ENV_NAME, partitions_csv_name)

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
                f"{build_esptool_cmd(ENV_NAME)}\n"
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
                f"{build_esptool_cmd_sh(ENV_NAME)}\n"
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
