from __future__ import annotations

import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]


class SignatureArchitectureTests(unittest.TestCase):
    def test_every_configuration_builds_the_active_kcse_client(self) -> None:
        cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertFalse((PROJECT_ROOT / "src" / "kcse" / "plugin_stub.cpp").exists())
        self.assertNotIn("<CONFIG:Debug>:${SRC_DIR}/kcse/plugin_stub.cpp", cmake)
        self.assertIn('"${SRC_DIR}/kcse/plugin.cpp"', cmake)
        self.assertIn('set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")', cmake)
        self.assertIn("set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)", cmake)

    def test_registry_is_the_single_source_for_67_signatures(self) -> None:
        core = (
            PROJECT_ROOT / "src" / "signatures" / "signature_core.cpp"
        ).read_text(encoding="utf-8")
        init = (PROJECT_ROOT / "src" / "kcd2_init.cpp").read_text(encoding="utf-8")
        entries = re.findall(r'signature_spec\{"([^"]+)",\s*"([^"]+)"', core)

        self.assertEqual(len(entries), 67)
        self.assertEqual(len({name for name, _ in entries}), 67)
        self.assertNotIn("CXConsole_RegisterVar", core)
        self.assertNotIn("hook_CXConsole_AddCommand", init)
        self.assertIn("static_assert(signature_registry.size()", core)
        self.assertNotIn("kcd2_address::scan(", init)
        self.assertNotIn(".get_call()", init)
        self.assertNotIn(".rip()", init)
        self.assertIn(
            'validate_named_vtable("CXConsole vtable", "CXConsole vtable[35]"',
            core,
        )
        self.assertIn(
            'validate_named_vtable("CEntity vtable", "CEntity::Activate", 52',
            core,
        )
        self.assertIn(
            'validate_named_vtable("CEntity vtable", "CEntity::SetFlags", 5',
            core,
        )
        self.assertIn(
            'validate_named_vtable("CEntity vtable", "CEntity::Hide", 63',
            core,
        )
        self.assertIn('"CEntitySystem vtable"', core)
        self.assertIn('"CEntitySystem::SpawnEntity"', core)
        self.assertIn('"CEntitySystem::RemoveEntity"', core)
        self.assertIn('"CEntitySystem::GetEntityIterator"', core)
        self.assertIn('"IEntitySystem::AddSink ABI"', core)
        self.assertIn('"CCryAction::EndGameContext ABI"', core)
        self.assertIn('"CEntity::ResolvePhysicsProxy ABI"', core)
        self.assertIn('"C_SoulList::ApplySharedSoul ABI"', core)
        self.assertIn('"CEntitySystem::GetEntityLayerData"', core)
        self.assertIn('"gEnv pConsole pointer"', core)
        self.assertIn('resolved("gEnv pConsole pointer")', init)
        self.assertIn("attach_existing_engine_console()", init)
        self.assertIn("vtable[23]", init)

    def test_zydis_is_explicit_and_shared_with_native_audit(self) -> None:
        cmake = (PROJECT_ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        core = (
            PROJECT_ROOT / "src" / "signatures" / "signature_core.cpp"
        ).read_text(encoding="utf-8")
        audit = (PROJECT_ROOT / "tools" / "signature_audit_main.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn("add_library(KCD2OnlineSignatureCore STATIC", cmake)
        self.assertIn("target_link_libraries(KCD2OnlineSignatureCore PUBLIC Zydis)", cmake)
        self.assertIn("add_executable(KCD2OnlineSignatureAudit", cmake)
        self.assertIn("ZydisDecoderDecodeFull", core)
        self.assertIn("resolve_all(*image)", audit)
        self.assertFalse((PROJECT_ROOT / "tools" / "signature_audit.py").exists())

    def test_magic_address_offsets_were_removed(self) -> None:
        source = (PROJECT_ROOT / "src" / "kcd2_init.cpp").read_text(encoding="utf-8")
        self.assertNotIn("offset(0x3D)", source)
        self.assertNotIn("offset(0x95)", source)
        self.assertNotIn("offset(0x65)", source)
        self.assertIn('derived("CVegetation vtable")', source)
        self.assertIn('derived("gEnv pGame pointer")', source)


class StartupSafetyTests(unittest.TestCase):
    def test_dllmain_only_starts_bootstrap_after_proxy_setup(self) -> None:
        source = (PROJECT_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        dllmain = source[source.index("BOOL APIENTRY DllMain") :]
        self.assertIn("dll_proxy::init()", dllmain)
        self.assertIn("DisableThreadLibraryCalls", dllmain)
        self.assertIn("CreateThread", dllmain)
        self.assertNotIn("rom::init", dllmain)
        self.assertNotIn("kcd2_init()", dllmain)
        self.assertNotIn("new hooking", dllmain)

    def test_hook_creation_is_committed_only_after_full_preflight(self) -> None:
        source = (PROJECT_ROOT / "src" / "kcd2_init.cpp").read_text(encoding="utf-8")
        self.assertIn("#define hooking transactional_hooking", source)
        preflight = source.index("kcd2_address::begin_scan_session()")
        implementation = source.index("kcd2_init_impl();", preflight)
        commit = source.index("for (auto &register_hook", implementation)
        self.assertLess(preflight, implementation)
        self.assertLess(implementation, commit)
        self.assertIn("summary.requested == summary.resolved", source)
        self.assertIn("summary.derived_requested == summary.derived_resolved", source)

    def test_exception_filter_does_not_install_anti_remover(self) -> None:
        source = (PROJECT_ROOT / "src" / "main.cpp").read_text(encoding="utf-8")
        self.assertIn("new exception_handler(false, big_exception_handler)", source)

    def test_proxy_has_no_shared_dispatch_pointer(self) -> None:
        cpp = (PROJECT_ROOT / "src" / "dll_proxy" / "dll_proxy.cpp").read_text(
            encoding="utf-8"
        )
        assembly = (
            PROJECT_ROOT / "src" / "dll_proxy" / "d3d12_proxy.asm"
        ).read_text(encoding="utf-8")
        self.assertNotIn("FARPROC PA", cpp)
        self.assertNotIn("EXTERN PA", assembly)
        self.assertIn("pD3D12CreateDevice", cpp)
        self.assertIn("jmp qword ptr [pD3D12CreateDevice]", assembly)

    def test_environment_bootstrap_precedes_destructive_native_mutation(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.cpp"
        ).read_text(encoding="utf-8")
        begin = source.index("sandbox_start_result native_runtime::begin_sandbox")
        end = source.index("sandbox_poll_result native_runtime::poll_sandbox", begin)
        sandbox = source[begin:end]

        environment = sandbox.index("apply_environment_state(")
        save_lock = sandbox.index('"join.sandbox.save-load-lock.begin"')
        profile_apply = sandbox.index("reconcile_profile(m_profiles, target)")
        self.assertLess(environment, save_lock)
        self.assertLess(environment, profile_apply)

        environment_failure = sandbox.index(
            '"join.sandbox.environment.failed"', environment
        )
        environment_success = sandbox.index(
            '"join.sandbox.environment.ok"', environment_failure
        )
        self.assertNotIn(
            "begin_native_unload", sandbox[environment_failure:environment_success]
        )

    def test_environment_uses_forward_only_calendar_world_time(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("Calendar.GetWorldTime()", source)
        self.assertIn("Calendar.SetWorldTime(target)", source)
        self.assertIn("Calendar.SetWorldTimeRatio", source)
        self.assertIn("Calendar.SetWorldTimeRatio(0)", source)
        self.assertNotIn("Calendar.SetWorldTimePaused", source)
        self.assertNotIn('"e_TimeOfDay"', source)
        self.assertNotIn('"e_TimeOfDaySpeed"', source)

    def test_home_marker_resolution_is_constant_time_and_throttled(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.cpp"
        ).read_text(encoding="utf-8")
        header = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.hpp"
        ).read_text(encoding="utf-8")

        self.assertNotIn(
            "for (const auto &[wuid, object] : manager->m_objects", source
        )
        self.assertIn("XGenAIModule.GetMyWUID", source)
        self.assertIn("manager->m_objects.find", source)
        self.assertIn("now + std::chrono::seconds{1}", source)
        self.assertIn("m_next_home_marker_attempt", header)

    def test_multiplayer_sandbox_reveals_the_map_after_activation(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.cpp"
        ).read_text(encoding="utf-8")
        begin = source.index(
            "sandbox_start_result native_runtime::activate_loaded_sandbox"
        )
        end = source.index(
            "sandbox_start_result native_runtime::begin_native_world_start", begin
        )
        sandbox = source[begin:end]

        activated = sandbox.index("m_sandbox_active = true;")
        reveal = sandbox.index(
            'execute_console_command("player_revealFow")', activated
        )
        ready = sandbox.index("m_sandbox_progress.phase = sandbox_phase::ready", reveal)
        self.assertLess(activated, reveal)
        self.assertLess(reveal, ready)

    def test_failed_join_uses_deferred_unload_and_native_english_error(self) -> None:
        runtime = (
            PROJECT_ROOT / "src" / "kcse" / "native_runtime.cpp"
        ).read_text(encoding="utf-8")
        client = (
            PROJECT_ROOT / "src" / "multiplayer" / "client.cpp"
        ).read_text(encoding="utf-8")
        plugin = (PROJECT_ROOT / "src" / "kcse" / "plugin.cpp").read_text(
            encoding="utf-8"
        )
        menu = (
            PROJECT_ROOT / "src" / "gui" / "native_multiplayer_menu.cpp"
        ).read_text(encoding="utf-8")

        begin = runtime.index("void native_runtime::begin_native_unload")
        unload = runtime[begin:]
        self.assertIn('execute_console_command("disconnect", true)', unload)
        self.assertNotIn('execute_console_command("unload", true)', unload)
        self.assertNotIn("framework->EndGameContext()", unload)
        safe_queue = unload.index("void native_runtime::queue_native_unload_if_safe")
        load_gate = unload.index("if (!level_load_complete)", safe_queue)
        unload_command = unload.index(
            'execute_console_command("disconnect", true)', load_gate
        )
        self.assertLess(load_gate, unload_command)
        self.assertIn("m_remote_avatars.abandon_world();", unload)
        self.assertIn("m_remote_backend.abandon_world();", unload)
        self.assertIn("if (m_frame_sequence <= teardown_frame)", unload)
        self.assertNotIn("m_remote_avatars.clear();", unload)
        self.assertNotIn("m_world_start_deadline", runtime)
        self.assertIn("system_global_state_level_load_complete", runtime)
        self.assertNotIn("open_main_menu_if_pending", runtime)
        self.assertNotIn("interface->Open(1)", runtime)
        self.assertIn("return changed && !unload_transition;", runtime)

        self.assertIn("previous == client_state::closing", client)
        self.assertIn("!m_status.error.empty()", client)
        self.assertIn("m_runtime.end_sandbox(reason);", client)
        self.assertIn(
            "g_runtime->sandbox_active() || !client_status.error.empty()", plugin
        )

        self.assertIn('return "Connection failed: " + status.error;', menu)
        self.assertIn('"Multiplayer connection failed"', menu)
        self.assertIn("api.mode() == root_main_page", menu)
        self.assertIn("show_multiplayer_page();", menu)

    def test_multiplayer_time_is_corrected_frequently_without_skip_time(self) -> None:
        client = (
            PROJECT_ROOT / "src" / "multiplayer" / "client.cpp"
        ).read_text(encoding="utf-8")
        frontend = (PROJECT_ROOT / "src" / "kcd2_init.cpp").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "environment_correction_interval =\n"
            "\t\t    std::chrono::seconds{1}",
            client,
        )
        self.assertIn(
            ">= environment_correction_interval",
            client,
        )

        begin = frontend.index("hook_C_SkipTimeCutscene_Play")
        end = frontend.index("hook_C_FastTravel_StartTravel", begin)
        skip_time_hook = frontend[begin:end]
        self.assertIn("client_state::connected", skip_time_hook)
        self.assertIn("Blocked vanilla skip time", skip_time_hook)
        self.assertNotIn("attempt_sleep", skip_time_hook)

    def test_npc_isolation_preserves_vanilla_ownership_graph(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_entity_backend.cpp"
        ).read_text(encoding="utf-8")
        header = (
            PROJECT_ROOT / "src" / "kcse" / "native_entity_backend.hpp"
        ).read_text(encoding="utf-8")

        self.assertNotIn("RemoveEntity(", source)
        self.assertNotIn("guarded_remove_entity", source)
        self.assertIn("entity->Hide(true)", source)
        self.assertIn("m_isolated", header)
        self.assertIn("actor_preserved=true", source)

    def test_human_npc_spawns_require_exact_kcd2o_authorization(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_entity_backend.cpp"
        ).read_text(encoding="utf-8")
        header = (
            PROJECT_ROOT / "src" / "kcse" / "native_entity_backend.hpp"
        ).read_text(encoding="utf-8")
        remote = (
            PROJECT_ROOT / "src" / "kcse" / "native_remote_avatar_backend.cpp"
        ).read_text(encoding="utf-8")

        self.assertIn('(1U << 0) | (1U << 1)', source)
        self.assertNotIn("entity_class_view", source)
        self.assertNotIn("entity_class->GetName()", source)
        self.assertIn("max_spawn_entity_name_length", source)
        self.assertIn("it->thread == thread", source)
        self.assertIn("it->entity_name == spawn->entity_name", source)
        self.assertIn("it->consumed = true", source)
        self.assertIn("m_human_npc_classes.insert(spawn->entity_class)", source)
        self.assertIn("m_human_npc_classes.contains(spawn->entity_class)", source)
        self.assertIn("entity-control.spawn.blocked", source)
        self.assertIn("m_human_npc_classes", header)
        self.assertIn("human_npc_spawn_scope", header)
        self.assertIn("authorize_human_npc_spawn(name)", remote)
        self.assertNotIn("begin_player_spawn", remote)
        self.assertNotIn("end_player_spawn", remote)

    def test_remote_avatars_are_materialized_before_presentation(self) -> None:
        source = (
            PROJECT_ROOT / "src" / "kcse" / "native_remote_avatar_backend.cpp"
        ).read_text(encoding="utf-8")
        header = (
            PROJECT_ROOT / "src" / "kcse" / "native_remote_avatar_backend.hpp"
        ).read_text(encoding="utf-8")

        spawn_begin = source.index(
            "native_remote_avatar_backend::spawn("
        )
        status_begin = source.index(
            "native_remote_avatar_backend::status(", spawn_begin
        )
        spawn = source[spawn_begin:status_begin]
        self.assertIn("EnablePhysics(false)", spawn)
        self.assertIn("entity->Hide(true)", spawn)
        self.assertIn("entity->Activate(false)", spawn)
        self.assertIn("presentation=deferred", spawn)

        update_begin = source.index(
            "native_remote_avatar_backend::update("
        )
        display_name_begin = source.index(
            "native_remote_avatar_backend::apply_display_name(", update_begin
        )
        update = source[update_begin:display_name_begin]
        appearance = update.index("apply_appearance(")
        presentation = update.index("!present(*value, error)")
        self.assertLess(appearance, presentation)
        self.assertIn("value->presented && !activity_locked", update)
        self.assertIn("join.remote-presentation.failed", update)

        self.assertIn("bool presented{};", header)
        self.assertIn("join.remote-presentation.complete", source)

    def test_manual_disconnect_is_deferred_to_the_game_thread(self) -> None:
        client = (
            PROJECT_ROOT / "src" / "multiplayer" / "client.cpp"
        ).read_text(encoding="utf-8")
        plugin = (PROJECT_ROOT / "src" / "kcse" / "plugin.cpp").read_text(
            encoding="utf-8"
        )
        header = (
            PROJECT_ROOT / "src" / "multiplayer" / "client.hpp"
        ).read_text(encoding="utf-8")

        disconnect_begin = client.index("void multiplayer_client::disconnect()")
        fail_begin = client.index("void multiplayer_client::fail", disconnect_begin)
        disconnect = client[disconnect_begin:fail_begin]
        self.assertNotIn("m_runtime.local_profile()", disconnect)
        self.assertNotIn("queue_network(disconnect_command{})", disconnect)
        self.assertIn("m_manual_disconnect_pending = true", disconnect)
        self.assertIn("m_status.error.clear()", disconnect)

        tick_begin = client.index("void multiplayer_client::game_tick(")
        preflight_begin = client.index(
            "void multiplayer_client::advance_runtime_preflight", tick_begin
        )
        tick = client[tick_begin:preflight_begin]
        manual_begin = tick.index("if (manual_disconnect)")
        manual_end = tick.index("for (const auto &envelope", manual_begin)
        manual = tick[manual_begin:manual_end]
        self.assertIn("queue_network(disconnect_command{})", manual)
        self.assertNotIn("m_runtime.local_profile()", manual)
        self.assertNotIn("queue_profile_snapshot", manual)
        self.assertNotIn("m_disconnect_capture_profile", client)

        self.assertIn("bool m_manual_disconnect_pending{};", header)
        self.assertIn(
            "client_status.state == kcd2o::client_state::closing", plugin
        )
        self.assertIn(
            "client_status.state == kcd2o::client_state::connected\n"
            "\t\t    && sandbox_active",
            plugin,
        )


if __name__ == "__main__":
    unittest.main()
