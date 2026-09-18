"""PlatformIO hook: po buildu a před uploadem zkontroluje formát výstupu firmwaru.

Lokálně jen varuje, build nikdy nezastaví - iterativní vývoj se tím nesmí zdržovat. Blokuje
až CI, kde se stejná kontrola spouští s --warnings-as-errors.

Tenhle soubor umí jen tři věci: přečíst pin, zajistit venv a zavolat balíček. Co se vlastně
kontroluje, kde leží board.yaml, scénáře a golden - to všechno patří do balíčku
(ust_format_checker.platformio), aby se to nemuselo opravovat v každém repu zařízení zvlášť.

    XDOS_CHECK=0 pio run                    kontrola vypnutá
    XDOS_CHECKER_PATH=~/DOSPORTAL/backend   vývoj proti checkoutu místo pinované verze
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path

Import("env")  # noqa: F821 - poskytuje PlatformIO

# SCons skript nemá __file__, cestu proto bereme z projektu
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
    """Interpret venv s pinovanou verzí; poprvé ho vytvoří."""
    base = os.environ.get("XDG_CACHE_HOME") or (Path.home() / ".cache")
    venv = Path(base) / "xdos-check" / hashlib.sha256(spec.encode()).hexdigest()[:16]
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
    checkout = os.environ.get("XDOS_CHECKER_PATH")
    if checkout:
        # z checkoutu běží kontrola v interpretu PlatformIO a ten PyYAML mít nemusí; bez téhle
        # hlášky skončí chybějící závislost stejným návratovým kódem jako chyba formátu, takže
        # by hook mlčel a v logu zbyl jen traceback
        if subprocess.run([sys.executable, "-c", "import yaml"], capture_output=True).returncode:
            print(f"xdos-check: {sys.executable} nemá PyYAML, kontrola se přeskočila")
            print("xdos-check: u PlatformIO z pipx ho doplní: pipx inject platformio pyyaml")
            return None
        return [sys.executable, "-m", CHECKOUT_ENTRY_POINT], checkout

    spec = _pinned_spec()
    if spec is None:
        print(f"xdos-check: chybí {PIN.name}, kontrola formátu se přeskočila")
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
    # nálezy jdou na stdout a tečou rovnou ven; stderr držíme, aby se dalo poznat, že verze
    # v checker.txt je starší než tenhle hook - jinak z toho vypadne jen hláška Pythonu
    finished = subprocess.run(command + [str(PROJECT), str(build_dir), str(elf)],
                              cwd=cwd, text=True, stderr=subprocess.PIPE)
    problem = (finished.stderr or "").strip()
    if not problem:
        return
    if "No module named" in problem:
        print(f"xdos-check: pinovaná verze v {PIN.name} je starší než tenhle hook "
              "a kontrolu neumí spustit - aktualizuj pin")
    else:
        print(problem)


def after_build(source, target, env):  # noqa: ARG001 - podpis vyžaduje SCons
    # SCons předává cestu relativní k projektu, checker běží jinde
    elf = Path(str(target[0]))
    _run(elf if elf.is_absolute() else PROJECT / elf)


def before_upload(source, target, env):  # noqa: ARG001 - podpis vyžaduje SCons
    _run(Path(env.subst("$BUILD_DIR")) / (env.subst("$PROGNAME") + ".elf"))


# Na build i na upload zvlášť: při nezměněném zdrojáku se upload spustí bez rebuildu, takže
# akce navázaná jen na build by se nespustila. Druhý běh v jedné invokaci hlídá _already_ran
# a nezměněný ELF navíc odfiltruje --cache.
env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", after_build)  # noqa: F821
env.AddPreAction("upload", before_upload)  # noqa: F821
