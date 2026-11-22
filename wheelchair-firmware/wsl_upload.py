# PlatformIO post-script: make `pio run -t upload` work from WSL.
#
# WSL can't see USB devices, so the Linux teensy_loader_cli waits forever
# ("Waiting for Teensy device..."). Windows can see the board, and WSL can
# run Windows .exe files, so under WSL we swap in the Windows build of the
# same Teensy tools and hand them a Windows path to the .hex.
#
# Does nothing on Windows, macOS or plain Linux. On first use under WSL it
# downloads the Windows tools (checksum-verified) to
# ~/.platformio/packages/tool-teensy-windows.

import hashlib
import os
import tarfile
import tempfile
import urllib.request

Import("env")  # noqa: F821  (provided by PlatformIO)

TOOLS_VERSION = "1.162.0"
TOOLS_URL = (
    "https://dl.registry.platformio.org/download/platformio/tool/tool-teensy/"
    f"{TOOLS_VERSION}/tool-teensy-windows-{TOOLS_VERSION}.tar.gz"
)
TOOLS_SHA256 = "ffb1cf7b024ead31cf5276beac187c9ba96174fe5bc17dbe5b83a5bbc8638461"
TOOLS_DIR = os.path.expanduser("~/.platformio/packages/tool-teensy-windows")


def running_under_wsl():
    try:
        with open("/proc/version") as f:
            return "microsoft" in f.read().lower()
    except OSError:
        return False


def ensure_windows_tools():
    loader = os.path.join(TOOLS_DIR, "teensy_loader_cli.exe")
    if os.path.isfile(loader):
        return
    print(f"WSL detected: fetching Windows Teensy tools {TOOLS_VERSION}...")
    with tempfile.TemporaryDirectory() as tmp:
        archive = os.path.join(tmp, "tool-teensy.tar.gz")
        urllib.request.urlretrieve(TOOLS_URL, archive)
        with open(archive, "rb") as f:
            digest = hashlib.sha256(f.read()).hexdigest()
        if digest != TOOLS_SHA256:
            raise SystemExit(f"Windows Teensy tools checksum mismatch: {digest}")
        os.makedirs(TOOLS_DIR, exist_ok=True)
        with tarfile.open(archive) as tar:
            tar.extractall(TOOLS_DIR)
    for name in os.listdir(TOOLS_DIR):
        if name.endswith(".exe"):
            os.chmod(os.path.join(TOOLS_DIR, name), 0o755)


if running_under_wsl() and env.subst("$UPLOAD_PROTOCOL") == "teensy-cli":
    ensure_windows_tools()
    env.Replace(
        REBOOTER=os.path.join(TOOLS_DIR, "teensy_reboot.exe"),
        UPLOADER=os.path.join(TOOLS_DIR, "teensy_loader_cli.exe"),
        # Windows programs need a Windows path to the .hex ($$ = literal $).
        UPLOADCMD='"$UPLOADER" $UPLOADERFLAGS "$$(wslpath -w ${SOURCE.abspath})"',
    )
