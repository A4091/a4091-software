"""Check that AmigaDOS Version can still read an executable's version tag."""

from pathlib import Path
import re
import sys


def check_version_tag(source, packed):
    original = source.read_bytes()
    # Some programs also contain the bare marker as a search string.
    # Find the first populated tag, including its NUL terminator. Version
    # stops at the first marker even when it is just a search string.
    tag = re.search(rb"\$VER:[ \t]*[^ \t\r\n\x00][^\x00]*\x00", original)
    if tag is None:
        return

    for path, data in ((source, original), (packed, packed.read_bytes())):
        first = data.find(b"$VER:")
        if first < 0 or not data.startswith(tag.group(), first):
            raise SystemExit(
                f"{path}: first $VER: marker does not contain the original version tag"
            )


if __name__ == "__main__":
    check_version_tag(Path(sys.argv[1]), Path(sys.argv[2]))
