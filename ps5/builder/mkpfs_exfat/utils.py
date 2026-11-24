"""Stand-ins for the two MkPFS helpers exfat_writer.py uses."""
import json
import re
from pathlib import Path

# OS-generated metadata that never belongs in an image (macOS and Windows).
IGNORED_NAMES = {".ds_store", "thumbs.db", "desktop.ini", ".spotlight-v100", ".trashes", ".fseventsd"}


def default_image_basename(source_root: Path) -> str:
    """The title ID from sce_sys/param.json, else the folder name, made filename-safe."""
    base = source_root.name or "image"
    try:
        base = json.loads((source_root / "sce_sys" / "param.json").read_text(encoding="utf-8"))["titleId"]
    except (OSError, ValueError, KeyError):
        pass
    return re.sub(r"[^A-Za-z0-9._-]", "_", base) or "image"


def is_ignored_name(name: str) -> bool:
    return name.startswith("._") or name.lower() in IGNORED_NAMES
