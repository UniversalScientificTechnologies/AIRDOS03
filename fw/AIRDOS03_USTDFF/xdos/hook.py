"""PlatformIO hook: po buildu a před uploadem zkontroluje formát výstupu firmwaru.

Lokálně jen varuje, build nikdy nezastaví - iterativní vývoj se tím nesmí zdržovat. Blokuje
až CI, kde se stejná kontrola spouští s --warnings-as-errors.

Kontrola běží ve vlastním venv (ne v penv PlatformIO) a verze je pinovaná v checker.txt, takže
se při buildu nikam nesahá na síť. Poprvé se venv vytvoří, pak už se jen používá.

    XDOS_CHECK=0 pio run          kontrola vypnutá
    XDOS_CHECKER_PATH=~/DOSPORTAL/backend   vývoj proti checkoutu místo pinované verze
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 - poskytuje PlatformIO

# SCons skript nemá __file__, cestu proto bereme z projektu
HERE = Path(env.subst("$PROJECT_DIR")).resolve() / "xdos"  # noqa: F821
BOARD = HERE / "board.yaml"
SCENARIO = HERE / "scenarios" / "basic.yaml"
GOLDEN = HERE / "golden" / "basic.txt"
PIN = HERE / "checker.txt"

CHECKER_ENV_VAR = "XDOS_CHECKER_PATH"
INSTALL_TIMEOUT_S = 300

_already_ran = False


def _pinned_spec() -> str | None:
    if not PIN.exists():
        return None
    lines = [line.strip() for line in PIN.read_text(encoding="utf-8").splitlines()]
    return next((line for line in lines if line and not line.startswith("#")), None)


def _cache_root() -> Path:
    base = os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache")
    return Path(base) / "xdos-check"


def _venv_python(spec: str) -> Path | None:
    """Vrátí interpret venv s pinovanou verzí; poprvé ho vytvoří."""
    venv = _cache_root() / hashlib.sha256(spec.encode()).hexdigest()[:16]
    python = venv / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    if python.exists():
        return python

    print(f"xdos-check: instaluji {spec.split('@')[0].strip()} (jednorázově, chvíli to potrvá)")
    try:
        subprocess.run([sys.executable, "-m", "venv", str(venv)], check=True,
                       capture_output=True, text=True, timeout=INSTALL_TIMEOUT_S)
        subprocess.run([str(python), "-m", "pip", "install", "--quiet", spec], check=True,
                       capture_output=True, text=True, timeout=INSTALL_TIMEOUT_S)
    except subprocess.TimeoutExpired:
        print("xdos-check: instalace trvala příliš dlouho, kontrola se přeskočila")
        return None
    except subprocess.CalledProcessError as error:
        print("xdos-check: instalace selhala, kontrola se přeskočila")
        print((error.stderr or "").strip()[:800])
        return None
    print("xdos-check: hotovo, příště už se jen spustí")
    return python


def _command() -> tuple[list[str], str | None] | None:
    """(příkaz, pracovní adresář) pro spuštění kontroly, nebo None."""
    checkout = os.environ.get(CHECKER_ENV_VAR)
    if checkout:
        return [sys.executable, "-m", "packages.ust_format_checker.firmware"], checkout

    spec = _pinned_spec()
    if spec is None:
        print(f"xdos-check: chybí {PIN.name}, kontrola formátu se přeskočila")
        return None
    python = _venv_python(spec)
    if python is None:
        return None
    return [str(python), "-m", "ust_format_checker.firmware"], None


def _run(elf: Path) -> None:
    global _already_ran
    if _already_ran or os.environ.get("XDOS_CHECK") == "0" or not elf.exists():
        return
    _already_ran = True

    resolved = _command()
    if resolved is None:
        return
    command, cwd = resolved

    work_dir = Path(env.subst("$BUILD_DIR")) / "xdos"  # noqa: F821
    command += [
        str(elf),
        "--board", str(BOARD),
        "--scenario", str(SCENARIO),
        "--work-dir", str(work_dir),
        "--cache", str(work_dir / "last.json"),
    ]
    if GOLDEN.exists():
        command += ["--golden", str(GOLDEN)]

    finished = subprocess.run(command, cwd=cwd, text=True)
    if finished.returncode not in (0, 1):
        print("xdos-check: kontrola neproběhla, viz výpis výše")


def after_build(source, target, env):  # noqa: ARG001 - podpis vyžaduje SCons
    # SCons předává cestu relativní k projektu, checker běží jinde
    elf = Path(str(target[0]))
    _run(elf if elf.is_absolute() else Path(env.subst("$PROJECT_DIR")) / elf)


def before_upload(source, target, env):  # noqa: ARG001 - podpis vyžaduje SCons
    _run(Path(env.subst("$BUILD_DIR")) / (env.subst("$PROGNAME") + ".elf"))


# Na build i na upload zvlášť: při nezměněném zdrojáku se upload spustí bez rebuildu, takže
# akce navázaná jen na build by se nespustila. Druhý běh v jedné invokaci hlídá _already_ran
# a nezměněný ELF navíc odfiltruje --cache.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", after_build)  # noqa: F821
env.AddPreAction("upload", before_upload)  # noqa: F821
