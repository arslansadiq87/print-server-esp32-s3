#!/usr/bin/env python3
"""Create a reviewed-source ZIP without local credentials or build output."""

from pathlib import Path
import re
import zipfile


ROOT = Path(__file__).resolve().parents[1]
FILES = (
    ".gitignore",
    "README.md",
    "HELP.md",
    "SECURITY.md",
    "PUBLISHING.md",
    "CONTRIBUTING.md",
    "FLASHING.md",
    "CMakeLists.txt",
    "platformio.ini",
    "partitions.csv",
    "sdkconfig.defaults",
    "dependencies.lock",
    "boards/esp32-s3-n16r8.json",
    "main/CMakeLists.txt",
    "main/main.c",
    "main/wifi_credentials.example.h",
    "scripts/raw9100_test.py",
    "scripts/package_source.py",
)
SECRET_PATTERNS = (
    re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH |DSA )?PRIVATE KEY-----"),
    re.compile(r"\bgh[pousr]_[A-Za-z0-9]{30,}\b"),
    re.compile(r"\bgithub_pat_[A-Za-z0-9_]{30,}\b"),
    re.compile(r"\bAKIA[A-Z0-9]{16}\b"),
)


def main() -> None:
    paths = list(FILES)
    # Include an owner-selected license automatically if one has been added.
    if (ROOT / "LICENSE").is_file():
        paths.append("LICENSE")
    contents = {}
    for name in paths:
        path = ROOT / name
        if path.is_symlink() or path.resolve().parent != (ROOT / name).absolute().parent:
            raise SystemExit(f"Unexpected source path: {name}")
        data = path.read_bytes()
        source = data.decode("utf-8")
        if any(pattern.search(source) for pattern in SECRET_PATTERNS):
            raise SystemExit(f"Possible secret found in {name}; archive not created")
        if path.suffix in (".c", ".h"):
            definitions = re.findall(
                r'^\s*#define\s+(WIFI_SSID|WIFI_PASSWORD|X_API_KEY)\s+"([^"\n]*)"',
                source, re.MULTILINE,
            )
            if any(value for _, value in definitions):
                raise SystemExit(f"Nonblank credential definition in {name}")
        contents[name] = data
    template = contents["main/wifi_credentials.example.h"].decode("utf-8")
    for macro in ("WIFI_SSID", "WIFI_PASSWORD", "X_API_KEY"):
        if not re.search(rf'^#define {macro} ""\r?$', template, re.MULTILINE):
            raise SystemExit(f"Public template must define blank {macro}")
    output = ROOT / "publish" / "tsc-esp32-source.zip"
    output.parent.mkdir(exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, data in contents.items():
            archive.writestr(name, data)
    with zipfile.ZipFile(output) as archive:
        if archive.testzip() is not None or set(archive.namelist()) != set(paths):
            raise SystemExit("Archive verification failed")
    print(f"Created {output} ({len(paths)} source files)")
    print("Basic credential checks passed. Review source and Git history before publication.")


if __name__ == "__main__":
    main()
