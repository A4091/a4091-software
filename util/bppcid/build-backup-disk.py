#!/usr/bin/env python3
"""Build a minimal backup ADF without overwriting an existing image."""

import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    source = Path(__file__).resolve().parent
    output = Path(sys.argv[1] if len(sys.argv) > 1 else
                  source / "blizzard-backup.adf").resolve()
    if output.exists():
        raise SystemExit(f"Refusing to overwrite {output}; it may contain a backup")
    xdftool = os.environ.get("XDFTOOL", "xdftool")
    files = {
        "C/bppcid": source / "bppcid",
        "C/ncr7xx": source.parent.parent / "ncr7xx",
        "S/Startup-Sequence": source / "backup-disk/Startup-Sequence",
        "ReadMe": source / "backup-disk/ReadMe",
    }
    for path in files.values():
        if not path.is_file():
            raise SystemExit(f"Missing input: {path}")
    with tempfile.TemporaryDirectory(prefix="bppc-adf-", dir=output.parent) as tmp:
        image = Path(tmp) / "disk.adf"

        def xdf(*args):
            subprocess.run([xdftool, str(image), *map(str, args)], check=True)

        xdf("format", "BlizzardBackup")
        xdf("makedir", "C")
        xdf("makedir", "S")
        for destination, path in files.items():
            xdf("write", path, destination)
        xdf("boot", "install")
        xdf("list")
        # Exclusive creation also protects against a file appearing during build.
        os.link(image, output)
    print(f"Created {output}")


if __name__ == "__main__":
    main()
