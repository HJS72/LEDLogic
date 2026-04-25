from datetime import datetime
from pathlib import Path
import json
import shutil

Import("env")

REPO_RAW_BASE = "https://raw.githubusercontent.com/HJS72/LEDLogic/main"


def build_version_string():
    now = datetime.now()
    return f"1.{now:%y%m%d}.{now:%H%M}"


BUILD_VERSION = build_version_string()
project_dir = Path(env["PROJECT_DIR"])
header_path = project_dir / "src" / "build_version_override.h"
header_path.write_text(
    f'#pragma once\n#define BUILD_VERSION_OVERRIDE "{BUILD_VERSION}"\n',
    encoding="utf-8",
)


def write_ota_metadata(source, target, env):
    project_dir = Path(env["PROJECT_DIR"])
    firmware_dir = project_dir / "firmware"
    ota_dir = project_dir / "ota"
    firmware_dir.mkdir(parents=True, exist_ok=True)
    ota_dir.mkdir(parents=True, exist_ok=True)

    source_bin = Path(str(target[0]))
    repo_firmware_bin = firmware_dir / "firmware.bin"
    shutil.copy2(source_bin, repo_firmware_bin)

    latest_json = ota_dir / "latest.json"
    latest_json.write_text(
        json.dumps(
            {
                "version": BUILD_VERSION,
                "bin_url": f"{REPO_RAW_BASE}/firmware/firmware.bin",
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", write_ota_metadata)
