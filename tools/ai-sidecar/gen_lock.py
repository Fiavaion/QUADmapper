"""Writes requirements.lock: every package pinned with hashes (ADR-053 section 1).

    python gen_lock.py PINS.txt requirements.lock

PINS.txt is one name==version per line: a `pip freeze` of the working .venv without
sam2, plus setuptools and wheel. Hashes are for the files this interpreter would
install (Windows, cp311): PyPI's JSON for ordinary packages, the PyTorch cu126
index for `+cu126` builds; an sdist only where no compatible wheel exists. Run it
from a throwaway venv that has `packaging` (never the global Python).
"""
import html
import json
import re
import sys
import urllib.request

from packaging.tags import sys_tags
from packaging.utils import canonicalize_name, parse_wheel_filename

TORCH_INDEX = "https://download.pytorch.org/whl/cu126"
HEADER = """\
# Hash-locked install set for the QUADmapper AI sidecar (Windows, Python 3.11, cp311).
# Every package, transitive ones included, at the version of the working .venv; hashes are
# for the Windows cp311 files (an sdist only where no such wheel exists). Installed by
# setup.ps1 with --require-hashes --no-deps. sam2 is separate: requirements-sam2.lock.
# Regenerate as README.md says; do not edit by hand.
"""


def fetch(url):
    return urllib.request.urlopen(url, timeout=60).read().decode()


def compatible(filename, tags):
    try:
        return bool(parse_wheel_filename(filename)[3] & tags)
    except Exception:
        return False


def hashes_for(name, version, tags):
    if "+cu126" in version:
        page = fetch(f"{TORCH_INDEX}/{name.lower()}/")
        found = []
        for m in re.finditer(r'href="[^"]*?([^"/#]+\.whl)#sha256=([0-9a-f]{64})"', page):
            filename = html.unescape(m.group(1)).replace("%2B", "+")
            n, v, _, _ = parse_wheel_filename(filename)
            if (canonicalize_name(n) == canonicalize_name(name) and str(v) == version
                    and compatible(filename, tags)):
                found.append(m.group(2))
        return found
    files = json.loads(fetch(f"https://pypi.org/pypi/{name}/{version}/json"))["urls"]
    wheels = [f["digests"]["sha256"] for f in files
              if f["packagetype"] == "bdist_wheel" and compatible(f["filename"], tags)]
    sdists = [f["digests"]["sha256"] for f in files if f["packagetype"] == "sdist"]
    return wheels or sdists


def main(pins_path, lock_path):
    tags = set(sys_tags())
    pins = [line.strip() for line in open(pins_path) if "==" in line]
    out = [HEADER + "--extra-index-url " + TORCH_INDEX]
    for pin in sorted(pins, key=lambda p: canonicalize_name(p.split("==")[0])):
        name, version = pin.split("==")
        digests = hashes_for(name, version, tags)
        if not digests:
            sys.exit(f"no compatible file for {pin}")
        lines = [f"{pin} \\"] + [f"    --hash=sha256:{d} \\" for d in digests]
        lines[-1] = lines[-1].rstrip(" \\")
        out.append("\n".join(lines))
    with open(lock_path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
