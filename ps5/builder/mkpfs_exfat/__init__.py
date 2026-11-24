"""exFAT image writer vendored from MkPFS 1.1.0 (PSBrew/MkPFS, GPL-3.0-or-later).

exfat_writer.py and _exfat_upcase.py are MkPFS's own files, unchanged; pbar.py
and utils.py are minimal stand-ins for the two MkPFS helpers they import.
Images use 64 KiB clusters and 512-byte sectors, the layout ShadowMountPlus's
fast exFAT path expects.
"""
from .exfat_writer import write_exfat_image  # noqa: F401
