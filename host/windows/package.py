"""Build the single offline x64 Windows installer using the Windows toolchain."""
import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
from urllib.request import urlopen
from zipfile import ZipFile

SOURCE = Path(__file__).resolve().parent
APP_NAME = "Wireless-tag USB Display"
PACKAGE_PREFIX = APP_NAME.replace(" ", "-")
PYTHON = "https://www.python.org/ftp/python/3.14.10/python-3.14.10-embed-amd64.zip"
LIBUSB = "https://github.com/libusb/libusb/releases/download/v1.0.30/libusb-1.0.30"
DRIVER = "https://www.amyuni.com/downloads/usbmmidd_v2.zip"
LANGUAGE = ("https://raw.githubusercontent.com/jrsoftware/issrc/is-6_4_3/"
            "Files/Languages/Unofficial/ChineseSimplified.isl")


def dependency(cache, url, digest):
    path = cache / url.rsplit("/", 1)[-1]
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest:
        return path
    with tempfile.NamedTemporaryFile(dir=cache, delete=False) as tmp:
        part = Path(tmp.name)
    try:
        with urlopen(url, timeout=90) as response, part.open("wb") as output:
            shutil.copyfileobj(response, output)
        if hashlib.sha256(part.read_bytes()).hexdigest() != digest:
            raise ValueError(f"SHA-256 mismatch for {url}")
        part.replace(path)
    finally:
        part.unlink(missing_ok=True)
    return path


def build(stage, cache, output, iscc, sign_command):
    python_zip = dependency(cache, PYTHON, "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159")
    usb_binary = dependency(cache, LIBUSB + ".7z", "7fb1dfec805b97983763d7d0ae244320da12add1003d4249c96cc4d586398c79")
    usb_source = dependency(cache, LIBUSB + ".tar.bz2", "fea36f34f9156400209595e300840767ab1a385ede1dc7ee893015aea9c6dbaf")
    driver = dependency(cache, DRIVER, "629b51e9944762bae73948171c65d09a79595cf4c771a82ebc003fbba5b24f51")
    language = dependency(cache, LANGUAGE, "dceca9fea16ea057a64012ac8744a1eca7dc6ce3187d0c8991accbb539f2d4a0")

    native = stage / "native"
    env = {**os.environ, "WT_BUILD_SCRIPT": str(SOURCE / "build.cmd"), "WT_BUILD_DEST": str(native)}
    cmd = r'cmd.exe /d /v:off /s /c ""%WT_BUILD_SCRIPT%" "%WT_BUILD_DEST%" app"'
    subprocess.run(cmd, cwd=stage, env=env, check=True)
    payload = stage / "app"
    payload.mkdir()
    app_exe = APP_NAME + ".exe"
    for name in (app_exe, "P4Desktop.exe", "GpuCapture.dll"):
        shutil.copy2(native / name, payload)
    for name in ("serve.py", "usb_stream.py", "live_sender.py", "topology.py", "display_paths.py", "virtual_monitor.py"):
        shutil.copy2(SOURCE / name, payload)
    with ZipFile(python_zip) as archive:
        archive.extractall(payload / "python")
    # Isolated embedded Python imports only its stdlib, DLL directory and our installed modules.
    with (payload / "python/python314._pth").open("a", encoding="ascii") as paths:
        paths.write("\n..\n")
    with (payload / "libusb-1.0.dll").open("wb") as dll:
        subprocess.run([Path(os.environ["SystemRoot"]) / "System32/tar.exe", "-xOf", usb_binary,
                        "VS2022/MS64/dll/libusb-1.0.dll"], stdout=dll, check=True)
    with ZipFile(driver) as archive:
        archive.extractall(stage)
    driver_files = stage / "usbmmidd_v2"
    licenses = payload / "licenses"
    licenses.mkdir()
    shutil.copy2(SOURCE / "THIRD_PARTY_NOTICES.txt", licenses)
    shutil.copy2(driver_files / "License.txt", licenses / "Amyuni-License.txt")
    shutil.copy2(usb_source, licenses)
    with tarfile.open(usb_source) as archive:
        (licenses / "libusb-COPYING.txt").write_bytes(archive.extractfile("libusb-1.0.30/COPYING").read())

    subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-STA", "-ExecutionPolicy", "Bypass",
                    "-File", SOURCE.parents[1] / "scripts/windows/check_display_ui.ps1",
                    "-App", payload / app_exe], cwd=stage, check=True)
    # Verify the shipped interpreter and its import paths before compiling either installer.
    subprocess.run([payload / "python/python.exe", "-B", "-c",
                    "from usb_stream import USB; import serve, topology; "
                    "USB(str(serve.SOURCE / 'libusb-1.0.dll'))"], cwd=stage, check=True)
    compiler = [iscc, "/Q", "/O" + str(stage), "/DChineseMessages=" + str(language),
                "/DAppExe=" + str(payload / app_exe), "/DPackagePrefix=" + PACKAGE_PREFIX]
    if sign_command:
        compiler.extend(["/DSignArtifacts", "/Swt=" + sign_command])
    script = SOURCE / "setup.iss"
    subprocess.run([*compiler, "/DDriverOnly", "/DDriverFiles=" + str(driver_files), script], check=True)
    subprocess.run([*compiler, "/DPayload=" + str(payload),
                    "/DDriverInstaller=" + str(stage / (PACKAGE_PREFIX + "-Driver.exe")), script], check=True)
    package = stage / (PACKAGE_PREFIX + "-Setup.exe")
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=output, delete=False) as tmp:
        pending = Path(tmp.name)
    try:
        shutil.copyfile(package, pending)
        pending.replace(output / package.name)
    finally:
        pending.unlink(missing_ok=True)
    print(f"{output / package.name}\nSHA-256 {hashlib.sha256(package.read_bytes()).hexdigest()}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iscc", type=Path, help="path to Inno Setup 6 ISCC.exe")
    parser.add_argument("--cache", type=Path, help="directory for checksum-verified downloads")
    parser.add_argument("--output", type=Path, default=SOURCE.parents[1] / "outputs/windows-installer")
    parser.add_argument("--sign-command", help="publisher's Inno Setup SignTool command containing $f")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("Run with Windows Python and the x64 Visual Studio build tools.")
    iscc = args.iscc or shutil.which("ISCC.exe") or Path(os.environ["ProgramFiles(x86)"]) / "Inno Setup 6/ISCC.exe"
    if not Path(iscc).is_file():
        parser.error("Install Inno Setup 6 or pass --iscc.")
    if args.sign_command and "$f" not in args.sign_command:
        parser.error("--sign-command must contain the Inno Setup $f filename placeholder.")
    cache = (args.cache or Path(os.environ["LOCALAPPDATA"]) / "WTUSBDisplay-build/downloads").resolve()
    cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="wt-usb-package-") as work:
        build(Path(work), cache, args.output.resolve(), Path(iscc).resolve(), args.sign_command)


if __name__ == "__main__":
    main()
