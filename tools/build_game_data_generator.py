#!/usr/bin/env python3
"""Build the standalone Windows game-data generator used by server releases."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
from typing import Optional, Sequence


def build_generator(
    project_root: pathlib.Path,
    property_catalog_tool: pathlib.Path,
    signature_audit_tool: pathlib.Path,
    output: pathlib.Path,
) -> pathlib.Path:
    project_root = project_root.resolve()
    property_catalog_tool = property_catalog_tool.resolve()
    signature_audit_tool = signature_audit_tool.resolve()
    output = output.resolve()
    missing = [
        str(path)
        for path in (property_catalog_tool, signature_audit_tool)
        if not path.is_file()
    ]
    if missing:
        raise RuntimeError(
            "required native generator tools are missing:\n" + "\n".join(missing)
        )

    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="kcd2o-game-data-generator.", dir=str(output.parent)
    ) as temporary:
        root = pathlib.Path(temporary)
        dist = root / "dist"
        command = [
            sys.executable,
            "-m",
            "PyInstaller",
            "--noconfirm",
            "--clean",
            "--onefile",
            "--console",
            "--noupx",
            "--name",
            output.stem,
            "--distpath",
            str(dist),
            "--workpath",
            str(root / "work"),
            "--specpath",
            str(root / "spec"),
            "--paths",
            str(project_root / "tools"),
            "--add-binary",
            "{}{}.".format(property_catalog_tool, os.pathsep),
            "--add-binary",
            "{}{}.".format(signature_audit_tool, os.pathsep),
            str(project_root / "tools" / "generate_server_game_data.py"),
        ]
        try:
            subprocess.run(command, cwd=str(project_root), check=True)
        except subprocess.CalledProcessError as exc:
            raise RuntimeError(
                "PyInstaller could not build the game-data generator"
            ) from exc
        built = dist / output.name
        if not built.is_file():
            raise RuntimeError("PyInstaller did not produce {}".format(built))
        temporary_output = output.with_name(output.name + ".tmp")
        shutil.copy2(built, temporary_output)
        os.replace(temporary_output, output)
    return output


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--project-root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[1],
    )
    parser.add_argument("--property-catalog-tool", type=pathlib.Path, required=True)
    parser.add_argument("--signature-audit-tool", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    options = parser.parse_args(argv)
    try:
        result = build_generator(
            options.project_root,
            options.property_catalog_tool,
            options.signature_audit_tool,
            options.output,
        )
    except (OSError, RuntimeError) as exc:
        parser.error(str(exc))
    print(result)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
