"""Textual user interface for building and deploying KCD2Online."""

from __future__ import annotations

from pathlib import Path
from typing import Callable, Optional

from textual import work
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.widgets import Button, Footer, Header, Input, Label, RichLog, Select, Static

from .core import (
    BUILD_PROFILES,
    BuildProfile,
    BuildResult,
    BuildService,
    BuildToolError,
    ConfigStore,
    GameLocation,
    deploy_artifacts,
    detect_game_root,
    is_valid_game_root,
    normalize_game_root,
    resolve_game_location,
)

LocationResolver = Callable[[], Optional[GameLocation]]


class BuildApp(App[None]):
    TITLE = "KCD2Online Build Tool"
    SUB_TITLE = "Build and deploy without leaving the terminal"

    CSS = """
    Screen {
        layout: vertical;
        background: $surface;
    }

    #content {
        height: 1fr;
        padding: 1 2;
    }

    #settings {
        height: auto;
        border: round $primary;
        padding: 1 2;
        margin-bottom: 1;
    }

    #settings.build-active {
        display: none;
    }

    .field-label {
        margin-top: 1;
        color: $text-muted;
    }

    #profile {
        width: 30;
        margin-bottom: 1;
    }

    #game-root {
        width: 1fr;
    }

    #path-actions, #build-actions {
        height: auto;
        margin-top: 1;
    }

    Button {
        margin-right: 1;
    }

    #audit {
        width: 22;
    }

    #update-address-library {
        width: 27;
    }

    #build {
        width: 20;
    }

    #build-deploy {
        width: 24;
        background: $success;
    }

    #status {
        height: 3;
        border: round $secondary;
        padding: 0 1;
        margin-bottom: 1;
    }

    #log {
        height: 1fr;
        border: round $secondary;
        padding: 0 1;
        background: $panel;
    }
    """

    BINDINGS = [
        ("q", "quit", "Quit"),
        ("ctrl+a", "audit", "Audit signatures"),
        ("ctrl+l", "update_address_library", "Update Address Library"),
        ("ctrl+b", "build", "Build"),
        ("ctrl+d", "build_deploy", "Build & Deploy"),
    ]

    def __init__(
        self,
        service: Optional[BuildService] = None,
        config_store: Optional[ConfigStore] = None,
        location_resolver: Optional[LocationResolver] = None,
    ) -> None:
        super().__init__()
        self.service = service or BuildService()
        self.config_store = config_store or ConfigStore()
        self.location_resolver = location_resolver or (
            lambda: resolve_game_location(self.config_store)
        )
        self._busy = False

    def compose(self) -> ComposeResult:
        yield Header()
        with Vertical(id="content"):
            with Vertical(id="settings"):
                yield Label("Build profile", classes="field-label")
                yield Select(
                    [(profile.label, profile.key) for profile in BUILD_PROFILES.values()],
                    value="debug",
                    allow_blank=False,
                    id="profile",
                )
                yield Label("Kingdom Come: Deliverance II root", classes="field-label")
                yield Input(
                    placeholder=r"F:\Steam\steamapps\common\KingdomComeDeliverance2",
                    id="game-root",
                )
                with Horizontal(id="path-actions"):
                    yield Button("Save path", id="save-path")
                    yield Button("Use Steam auto-detection", id="auto-detect")
                    yield Button("Update Address Library", id="update-address-library")
                with Horizontal(id="build-actions"):
                    yield Button("Audit signatures", id="audit")
                    yield Button("Build", id="build", variant="primary")
                    yield Button("Build & Deploy", id="build-deploy", variant="success")
            yield Static("Ready", id="status")
            yield RichLog(
                id="log",
                highlight=True,
                markup=False,
                wrap=True,
                auto_scroll=True,
                max_lines=10000,
            )
        yield Footer()

    def on_mount(self) -> None:
        self._load_location()

    def _load_location(self) -> None:
        try:
            location = self.location_resolver()
        except Exception as exc:
            self._set_status("Game detection failed: {}".format(exc), error=True)
            return
        if location is None:
            self._set_status("Game not found. Enter the KCD2 root directory.", error=True)
            return

        self.query_one("#game-root", Input).value = str(location.root)
        message = "Game found via {}.".format(location.source)
        if location.warning:
            message += " {}".format(location.warning)
        self._set_status(message)

    def on_button_pressed(self, event: Button.Pressed) -> None:
        button_id = event.button.id
        if button_id == "save-path":
            self._save_path()
        elif button_id == "auto-detect":
            self._use_auto_detection()
        elif button_id == "build":
            self._start_build(False)
        elif button_id == "build-deploy":
            self._start_build(True)
        elif button_id == "audit":
            self._start_audit()
        elif button_id == "update-address-library":
            self._start_address_library_update()

    def action_audit(self) -> None:
        self._start_audit()

    def action_update_address_library(self) -> None:
        self._start_address_library_update()

    def action_build(self) -> None:
        self._start_build(False)

    def action_build_deploy(self) -> None:
        self._start_build(True)

    def _selected_profile(self) -> BuildProfile:
        value = self.query_one("#profile", Select).value
        key = value if isinstance(value, str) else "debug"
        return BUILD_PROFILES.get(key, BUILD_PROFILES["debug"])

    def _input_game_root(self) -> Path:
        value = self.query_one("#game-root", Input).value.strip().strip('"')
        if not value:
            raise BuildToolError("Select a valid game directory before deploying.")
        root = normalize_game_root(Path(value))
        if not is_valid_game_root(root):
            raise BuildToolError(
                "The selected game root does not contain KingdomCome.exe."
            )
        return root

    def _save_path(self) -> None:
        if self._busy:
            return
        try:
            root = self._input_game_root()
            saved = self.config_store.save_override(root)
        except BuildToolError as exc:
            self._report_error(str(exc))
            return
        self.query_one("#game-root", Input).value = str(saved)
        self._set_status("Saved game-path override.")
        self.notify("Game path saved.")

    def _use_auto_detection(self) -> None:
        if self._busy:
            return
        try:
            self.config_store.clear_override()
            detected = detect_game_root()
        except BuildToolError as exc:
            self._report_error(str(exc))
            return
        if detected is None:
            self.query_one("#game-root", Input).value = ""
            self._report_error("Steam auto-detection could not find KCD2.")
            return
        self.query_one("#game-root", Input).value = str(detected)
        self._set_status("Game found via Steam auto-detection.")
        self.notify("Steam game path detected.")

    def _start_build(self, deploy: bool) -> None:
        if self._busy:
            return
        try:
            game_root = self._input_game_root()
        except BuildToolError as exc:
            self._report_error(str(exc))
            return

        profile = self._selected_profile()
        log = self.query_one("#log", RichLog)
        log.clear()
        log.border_title = "{} build output".format(profile.label)
        log.write(
            "=== KCD2Online {} build started{} ===".format(
                profile.label, " (deploy requested)" if deploy else ""
            )
        )
        log.scroll_end(animate=False)
        self._set_busy(True)
        self._set_status(
            "Building {}{}... Live output is shown below.".format(
                profile.label, " and deploying" if deploy else ""
            )
        )
        self._execute_build(profile, deploy, game_root)

    def _start_audit(self) -> None:
        if self._busy:
            return
        try:
            game_root = self._input_game_root()
        except BuildToolError as exc:
            self._report_error(str(exc))
            return
        profile = self._selected_profile()
        log = self.query_one("#log", RichLog)
        log.clear()
        log.border_title = "Signature audit output"
        log.write("=== KCD2Online signature audit started ===")
        self._set_busy(True)
        self._set_status("Auditing installed WHGame.dll... Live output is shown below.")
        self._execute_audit(profile, game_root)

    def _start_address_library_update(self) -> None:
        if self._busy:
            return
        log = self.query_one("#log", RichLog)
        log.clear()
        log.border_title = "Address Library update"
        log.write("=== Checking Address Library upstream ===")
        self._set_busy(True)
        self._set_status("Checking and validating the latest Address Library...")
        self._execute_address_library_update()

    @work(thread=True, exclusive=True, group="build")
    def _execute_address_library_update(self) -> None:
        def write_log(message: str) -> None:
            self.call_from_thread(self._write_log, message)

        try:
            commit = self.service.update_address_library(write_log)
            self.call_from_thread(
                self._finish_operation,
                True,
                "Address Library ready at {}. Commit the updated submodule pointer after testing.".format(
                    commit[:12]
                ),
            )
        except BuildToolError as exc:
            write_log("ERROR: {}".format(exc))
            self.call_from_thread(self._finish_operation, False, str(exc))
        except Exception as exc:
            write_log("UNEXPECTED ERROR: {}".format(exc))
            self.call_from_thread(
                self._finish_operation,
                False,
                "Unexpected build-tool error: {}".format(exc),
            )

    @work(thread=True, exclusive=True, group="build")
    def _execute_audit(self, profile: BuildProfile, game_root: Path) -> None:
        def write_log(message: str) -> None:
            self.call_from_thread(self._write_log, message)

        try:
            self.service.audit(profile, game_root, write_log)
            self.call_from_thread(
                self._finish_operation, True, "Signature audit completed successfully."
            )
        except BuildToolError as exc:
            write_log("ERROR: {}".format(exc))
            self.call_from_thread(self._finish_operation, False, str(exc))
        except Exception as exc:
            write_log("UNEXPECTED ERROR: {}".format(exc))
            self.call_from_thread(
                self._finish_operation,
                False,
                "Unexpected build-tool error: {}".format(exc),
            )

    @work(thread=True, exclusive=True, group="build")
    def _execute_build(
        self, profile: BuildProfile, deploy: bool, game_root: Optional[Path]
    ) -> None:
        def write_log(message: str) -> None:
            self.call_from_thread(self._write_log, message)

        try:
            result = self.service.build(
                profile, write_log, game_root=game_root
            )
            if deploy:
                assert game_root is not None
                destination = deploy_artifacts(result, game_root)
                write_log(
                    "Deployed d3d12, dinput8/KCSE, and the KCD2Online KCSE bridge to "
                    "{}".format(destination)
                )
                message = "{} build deployed successfully.".format(profile.label)
            else:
                if result.package is not None:
                    message = "{} build completed. Packages: {}".format(
                        profile.label, result.package.root
                    )
                else:
                    message = "{} build completed successfully.".format(profile.label)
            self.call_from_thread(self._finish_operation, True, message)
        except BuildToolError as exc:
            write_log("ERROR: {}".format(exc))
            self.call_from_thread(self._finish_operation, False, str(exc))
        except Exception as exc:
            write_log("UNEXPECTED ERROR: {}".format(exc))
            self.call_from_thread(
                self._finish_operation, False, "Unexpected build-tool error: {}".format(exc)
            )

    def _write_log(self, message: str) -> None:
        log = self.query_one("#log", RichLog)
        log.write(message)
        log.scroll_end(animate=False)

    def _finish_operation(self, success: bool, message: str) -> None:
        self.query_one("#log", RichLog).border_title = (
            "Build log - succeeded" if success else "Build log - failed"
        )
        self._set_busy(False)
        self._set_status(message, error=not success)
        self.notify(message, severity="information" if success else "error")

    def _set_busy(self, busy: bool) -> None:
        self._busy = busy
        self.query_one("#settings", Vertical).set_class(busy, "build-active")
        for button in self.query(Button):
            button.disabled = busy
        self.query_one("#profile", Select).disabled = busy
        self.query_one("#game-root", Input).disabled = busy

    def _set_status(self, message: str, error: bool = False) -> None:
        prefix = "Error: " if error else ""
        self.query_one("#status", Static).update(prefix + message)

    def _report_error(self, message: str) -> None:
        self._set_status(message, error=True)
        self.notify(message, severity="error")
