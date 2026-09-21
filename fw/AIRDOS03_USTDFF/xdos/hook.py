"""PlatformIO hook: checks the firmware output format after a build and before an upload.

Locally it only warns, never stops the build; CI blocks instead. Reads the pin, provides a
venv, calls the package - everything else lives in ust_format_checker.platformio.

    XDOS_CHECK=0 pio run                    check disabled
    XDOS_CHECKER_PATH=~/DOSPORTAL/backend   develop against a checkout instead of the pin
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 - provided by PlatformIO

# an SCons script has no __file__
PROJECT = Path(env.subst("$PROJECT_DIR")).resolve()  # noqa: F821
PIN = PROJECT / "xdos" / "checker.txt"

ENTRY_POINT = "ust_format_checker.platformio"
CHECKOUT_ENTRY_POINT = "packages.ust_format_checker.platformio"
INSTALL_TIMEOUT_S = 300

_already_ran = False


def _pinned_spec() -> str | None:
    if not PIN.exists():
        return None
    lines = [line.strip() for line in PIN.read_text(encoding="utf-8").splitlines()]
    return next((line for line in lines if line and not line.startswith("#")), None)


def _venv_python(spec: str) -> Path | None:
    """Interpreter of the venv holding the pinned version; created on first use."""
    base = os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache")
    venv = Path(base) / "xdos-check" / hashlib.sha256(spec.encode()).hexdigest()[:16]
    python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if python.exists():
        return python

    print(f"xdos-check: installing {spec.split('@')[0].strip()} (one-off, takes a moment)")
    try:
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True,
                       capture_output=True, text=True, timeout=INSTALL_TIMEOUT_S)
        subprocess.run([str(python), "-m", "pip", "install", "--quiet", spec], check=True,
                       capture_output=True, text=True, timeout=INSTALL_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print("xdos-check: installation took too long, check skipped")
        return None
    except subprocess.CalledProcessError as error:
        print("xdos-check: installation failed, check skipped")
        print((error.stderr or "").strip()[:800])
        return None
    print("xdos-check: done, from now on it only runs")
    return python


def _command() -> tuple[list[str], str | None] | None:
    """(command, working directory) to run the check with, or None."""
    checkout = os.environ.get("XDOS_CHECKER_PATH")
    if checkout:
        # without this, a missing PyYAML exits like a format error and the hook stays silent
        if subprocess.run([sys.executable, "-c", "import yaml"], capture_output=True).returncode:
            print(f"xdos-check: {sys.executable} has no PyYAML, check skipped")
            print("xdos-check: with PlatformIO from pipx, add it: pipx inject platformio pyyaml")
            return None
        return [sys.executable, "-m", CHECKOUT_ENTRY_POINT], checkout

    spec = _pinned_spec()
    if spec is None:
        print(f"xdos-check: {PIN.name} is missing, format check skipped")
        return None
    python = _venv_python(spec)
    if python is None:
        return None
    return [str(python), "-m", ENTRY_POINT], None


def _run(elf: Path) -> None:
    global _already_ran
    if _already_ran or os.environ.get("XDOS_CHECK") == "0" or not elf.exists():
        return
    _already_ran = True

    resolved = _command()
    if resolved is None:
        return
    command, cwd = resolved
    build_dir = Path(env.subst("$BUILD_DIR"))  # noqa: F821
    # findings flow out on stdout; stderr is held back to recognise a stale pin
    finished = subprocess.run(command + [str(PROJECT), str(build_dir), str(elf)],
                              cwd=cwd, text=True, stderr=subprocess.PIPE)
    problem = (finished.stderr or "").strip()
    if not problem:
        return
    if "No module named" in problem:
        print(f"xdos-check: the version pinned in {PIN.name} is older than this hook "
              "and cannot run the check - update the pin")
    else:
        print(problem)


def _elf_from(env) -> Path:
    return Path(env.subst("$BUILD_DIR")) / (env.subst("$PROGNAME") + ".elf")


def after_build(source, target, env):  # noqa: ARG001 - signature required by SCons
    _run(_elf_from(env))


def before_upload(source, target, env):  # noqa: ARG001 - signature required by SCons
    _run(_elf_from(env))


# checkprogsize, not the ELF: ELF actions only fire on a relink, so an unchanged `pio run`
# would stay silent even with a broken format
env.AddPostAction("checkprogsize", after_build)  # noqa: F821
# an upload can happen without a build; _already_ran guards a double run
env.AddPreAction("upload", before_upload)  # noqa: F821
