"""Create KCD2Online client, server, test, and release-ZIP packages."""

from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import replace
from pathlib import Path
from typing import Optional, Sequence

from build_tui.core import (
    BUILD_PROFILES,
    GAME_DATA_GENERATOR_EXECUTABLE,
    SERVER_GAME_DATA_DIRECTORY,
    BuildResult,
    BuildToolError,
    discover_address_libraries,
    package_artifacts,
)


def _artifact(build_dir: Path, config: str, name: str) -> Path:
    candidates = (build_dir / config / name, build_dir / name)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    matches = tuple(
        path
        for path in build_dir.rglob(name)
        if "_deps" not in path.parts and path.is_file()
    )
    if len(matches) == 1:
        return matches[0]
    if not matches:
        raise BuildToolError("Build artifact is missing: {}".format(name))
    raise BuildToolError(
        "Build artifact is ambiguous: {}\n{}".format(
            name, "\n".join(str(path) for path in matches)
        )
    )


def _address_libraries(build_dir: Path, config: str):
    candidates = (
        build_dir / config / "KCSE" / "addresslib",
        build_dir / "KCSE" / "addresslib",
    )
    for candidate in candidates:
        if candidate.is_dir():
            return discover_address_libraries(candidate)
    raise BuildToolError("Bundled Address Library output was not found.")


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--config", default="RelWithDebInfo")
    parser.add_argument("--project-root", type=Path, default=Path(__file__).parents[1])
    parser.add_argument("--output", type=Path)
    options = parser.parse_args(argv)

    project_root = options.project_root.resolve()
    build_dir = options.build_dir.resolve()
    profile = BUILD_PROFILES[
        "debug" if options.config.lower() == "debug" else "release"
    ]
    server_path = _artifact(build_dir, options.config, "KCD2OnlineServer.exe")
    audit_path = _artifact(build_dir, options.config, "KCD2OnlineSignatureAudit.exe")
    property_catalog_path = _artifact(
        build_dir, options.config, "KCD2OnlinePropertyCatalog.exe"
    )
    generator_path = server_path.parent / GAME_DATA_GENERATOR_EXECUTABLE
    generator_command = [
        sys.executable,
        str(project_root / "tools" / "build_game_data_generator.py"),
        "--project-root",
        str(project_root),
        "--property-catalog-tool",
        str(property_catalog_path),
        "--signature-audit-tool",
        str(audit_path),
        "--output",
        str(generator_path),
    ]
    try:
        subprocess.run(generator_command, cwd=str(project_root), check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise BuildToolError(
            "Could not build the standalone game-data generator. "
            "Install tools/build_tui/requirements.txt and retry."
        ) from exc
    server_game_data_dir = server_path.parent / SERVER_GAME_DATA_DIRECTORY
    result = BuildResult(
        profile=profile,
        build_dir=build_dir,
        dll_path=_artifact(build_dir, options.config, "d3d12_.dll"),
        pdb_path=_artifact(build_dir, options.config, "d3d12_.pdb"),
        audit_path=audit_path,
        server_path=server_path,
        kcse_loader_path=_artifact(build_dir, options.config, "dinput8.dll"),
        kcse_loader_pdb_path=_artifact(build_dir, options.config, "dinput8.pdb"),
        kcse_client_path=_artifact(
            build_dir, options.config, "KCD2OnlineKCSEClient.dll"
        ),
        kcse_client_pdb_path=_artifact(
            build_dir, options.config, "KCD2OnlineKCSEClient.pdb"
        ),
        address_library_paths=_address_libraries(build_dir, options.config),
        game_data_generator_path=generator_path,
        server_game_data_dir=server_game_data_dir,
    )
    if not server_game_data_dir.is_dir():
        result = replace(result, server_game_data_dir=None)
    package = package_artifacts(
        result,
        project_root,
        options.output.resolve() if options.output is not None else None,
    )
    print(package.root)
    print(package.client_zip)
    print(package.server_zip)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except BuildToolError as exception:
        raise SystemExit("ERROR: {}".format(exception)) from exception
