"""Composes the UST format verification report of one firmware as markdown.

    python3 xdos_report.py <fw_type> <project_dir> <build_dir> <findings.txt> <tests.txt>

Written for xdos_check.yml: the report is shown in the run summary and, for a release, is
appended to the release description, below what the release workflow writes there itself.
"""

import re
import subprocess
import sys
from importlib.metadata import version
from pathlib import Path

from ust_format_checker.platformio import SCENARIO
from ust_format_checker.requirements import load_requirements

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def read(path: Path) -> str:
    return ANSI.sub("", path.read_text(encoding="utf-8", errors="replace")).strip() if path.exists() else ""


def identity(capture: str) -> tuple[str, str, str, str]:
    """(device, version, commit, build type) as the firmware printed them in $DOS."""
    for line in capture.splitlines():
        fields = line.split(",")
        if fields[0] == "$DOS" and len(fields) >= 6:
            return fields[1], fields[2], fields[4], fields[5]
    return ("unknown",) * 4


def last_line(text: str) -> str:
    return text.splitlines()[-1].strip("= ") if text else ""


def main() -> int:
    fw_type = sys.argv[1]
    project, build_dir, findings_path, tests_path = (Path(value) for value in sys.argv[2:6])

    findings = read(findings_path)
    device, fw_version, commit, build_type = identity(read(build_dir / "xdos" / "capture.txt"))
    requirements = load_requirements()
    scenario = re.search(r"^name:\s*(\S+)", read(project / SCENARIO), re.M)

    counts = re.search(r"(\d+) error, (\d+) warning, (\d+) info", last_line(findings))
    if counts:
        errors, warnings, infos = (int(number) for number in counts.groups())
        passed = errors == 0 and warnings == 0
        result = (f"**Result: {'PASS' if passed else 'FAIL'}** ({errors} errors, {warnings} warnings, "
                  f"{infos} informational notes)")
        detail = "\n".join(findings.splitlines()[:-1]).strip()
        blocking = errors + warnings
        heading = (f"{blocking} blocking, {infos} informational" if blocking
                   else f"{infos} informational notes, none of them blocks")
    else:
        # the checker did not get as far as a verdict (no simulator, broken board model, ...)
        passed = False
        result = "**Result: NOT VERIFIED** (the check did not run to the end)"
        detail, heading = findings, "output of the check"

    matrix = subprocess.run(
        [sys.executable, "-m", "ust_format_checker.matrix", "--standalone"],
        check=True, capture_output=True, text=True,
    ).stdout.strip()

    lines = [
        f"# UST format verification: {fw_type} {fw_version}",
        "",
        result,
        "",
        "| Item | Value |",
        "|---|---|",
        f"| Firmware | `{fw_type}`, device `{device}` |",
        f"| Version | `{fw_version}` ({build_type} build) |",
        f"| Commit | `{commit}` |",
        f"| Format | [version {requirements.format_version}]({requirements.version_url}) |",
        f"| Checker | `ust-format-checker` {version('ust-format-checker')}, strict mode (warnings block the release) |",
        "| Method | The released binary is run in the simavr simulator against the board model "
        f"`xdos/board.yaml`, scenario `{scenario.group(1) if scenario else 'default'}` |",
        f"| Device tests | {last_line(read(tests_path)) or 'none'} |",
        "",
        "The run is a simulation. Device identifiers in it are placeholders of the board model, "
        "not those of a real detector.",
        "",
        "## Findings",
        "",
    ]
    if detail:
        # open when something blocks, folded away when there are only notes
        lines += [f"<details{'' if passed else ' open'}><summary>{heading}</summary>", "",
                  "```text", detail, "```", "", "</details>", ""]
    else:
        lines += ["None.", ""]
    lines += [matrix, ""]
    sys.stdout.write("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
