"""Create the isolated build-tool environment and launch the TUI."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
import venv
from pathlib import Path
from typing import Optional, Sequence

MINIMUM_PYTHON = (3, 9)
PROJECT_ROOT = Path(__file__).resolve().parents[2]
VENV_DIR = PROJECT_ROOT / ".venv-build"
REQUIREMENTS = Path(__file__).with_name("requirements.txt")
MARKER = VENV_DIR / ".kcd2o-requirements.json"


def venv_python() -> Path:
    if sys.platform == "win32":
        return VENV_DIR / "Scripts" / "python.exe"
    return VENV_DIR / "bin" / "python"


def requirements_hash() -> str:
    return hashlib.sha256(REQUIREMENTS.read_bytes()).hexdigest()


def marker_hash() -> Optional[str]:
    try:
        payload = json.loads(MARKER.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return None
    value = payload.get("requirements_sha256")
    return value if isinstance(value, str) else None


def bootstrap() -> Path:
    if sys.version_info < MINIMUM_PYTHON:
        raise RuntimeError(
            "Python {}.{} or newer is required; found {}.{}.".format(
                MINIMUM_PYTHON[0],
                MINIMUM_PYTHON[1],
                sys.version_info.major,
                sys.version_info.minor,
            )
        )
    if not REQUIREMENTS.is_file():
        raise RuntimeError("Missing requirements file: {}".format(REQUIREMENTS))

    python = venv_python()
    if not python.is_file():
        print("Creating isolated build-tool environment at {}...".format(VENV_DIR))
        venv.EnvBuilder(with_pip=True).create(VENV_DIR)
    if not python.is_file():
        raise RuntimeError("The virtual environment did not provide a Python executable.")

    expected_hash = requirements_hash()
    if marker_hash() != expected_hash:
        print("Installing build-tool dependencies...")
        subprocess.run(
            [
                str(python),
                "-m",
                "pip",
                "install",
                "--disable-pip-version-check",
                "--requirement",
                str(REQUIREMENTS),
            ],
            cwd=str(PROJECT_ROOT),
            check=True,
        )
        MARKER.write_text(
            json.dumps(
                {
                    "requirements_sha256": expected_hash,
                    "python": "{}.{}.{}".format(
                        sys.version_info.major,
                        sys.version_info.minor,
                        sys.version_info.micro,
                    ),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
    return python


def main(argv: Optional[Sequence[str]] = None) -> int:
    del argv
    try:
        python = bootstrap()
        return subprocess.call(
            [str(python), "-m", "tools.build_tui"],
            cwd=str(PROJECT_ROOT),
        )
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print("Build-tool bootstrap failed: {}".format(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
