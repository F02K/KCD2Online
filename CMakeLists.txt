cmake_minimum_required(VERSION 3.20)

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Every DLL loaded into WHGame must use the retail MSVC container/CRT ABI,
# including Debug builds. Debug still retains symbols and disables optimizer
# defaults; it does not switch to the incompatible checked STL.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_MAP_IMPORTED_CONFIG_DEBUG Release)

project(KCD2Online VERSION 0.1.3 LANGUAGES CXX C ASM ASM_MASM)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/multiplayer/version.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/kcd2o_version.hpp"
    @ONLY
)
set(GENERATED_RESOURCE_FILE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/resources.rc"
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/resources.rc.in"
    "${GENERATED_RESOURCE_FILE}"
    @ONLY
)

add_compile_definitions(
    "$<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>"
)

set(SRC_DIR "${PROJECT_SOURCE_DIR}/src")
set(KCD2Online_WHGAME_PATH "" CACHE FILEPATH "Path to WHGame.dll for the offline signature audit")
option(KCD2Online_BUILD_KCSE "Build the bundled libKCD2 KCSE loader and KCD2Online client plugin" ON)

# Fetch modules
message("\nFetching modules")
include(cmake_scripts/git.cmake)
include(cmake_scripts/mimalloc.cmake)
include(cmake_scripts/rom.cmake)
include(cmake_scripts/imguizmo.cmake)
include(cmake_scripts/zip.cmake)

# FetchContent projects may override CMAKE_MSVC_RUNTIME_LIBRARY or set their
# target property before returning to us. Every static library linked into a
# WHGame DLL must nevertheless use the same retail CRT ABI. Apply the rule to
# all dependency targets that exist at this point; targets created below keep
# inheriting the project-wide default set above.
function(kcd2o_force_retail_msvc_runtime directory)
    get_property(directory_targets DIRECTORY "${directory}"
        PROPERTY BUILDSYSTEM_TARGETS
    )
    foreach(target IN LISTS directory_targets)
        get_target_property(target_type "${target}" TYPE)
        if(NOT target_type STREQUAL "INTERFACE_LIBRARY"
            AND NOT target_type STREQUAL "UTILITY"
        )
            set_property(TARGET "${target}" PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded"
            )
        endif()
    endforeach()

    get_property(subdirectories DIRECTORY "${directory}"
        PROPERTY SUBDIRECTORIES
    )
    foreach(subdirectory IN LISTS subdirectories)
        kcd2o_force_retail_msvc_runtime("${subdirectory}")
    endforeach()
endfunction()
kcd2o_force_retail_msvc_runtime("${CMAKE_CURRENT_SOURCE_DIR}")

find_package(Protobuf CONFIG REQUIRED)
find_package(GameNetworkingSockets CONFIG REQUIRED)

if(KCD2Online_BUILD_KCSE)
    find_package(spdlog CONFIG REQUIRED)
    find_package(boost_container CONFIG REQUIRED)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)

    set(KCD2Online_LIBKCD2_ROOT "${PROJECT_SOURCE_DIR}/libKCD2")
    set(KCD2Online_KCSE_ROOT "${KCD2Online_LIBKCD2_ROOT}/Projects/KCSE")
    set(KCD2Online_ADDRESS_LIBRARY_ROOT
        "${PROJECT_SOURCE_DIR}/vendor/Address-Library-For-KCSE/kcd2_address_library"
    )
    if(NOT EXISTS "${KCD2Online_LIBKCD2_ROOT}/.buildenv/PCH.h")
        message(FATAL_ERROR
            "libKCD2 is not initialized. Run "
            "`powershell -ExecutionPolicy Bypass -File tools/init_vendor.ps1` "
            "from the repository root.")
    endif()
    if(NOT EXISTS "${KCD2Online_KCSE_ROOT}/src/dllmain.cpp")
        message(FATAL_ERROR
            "libKCD2's nested KCSE submodule is not initialized. Run "
            "`powershell -ExecutionPolicy Bypass -File tools/init_vendor.ps1` "
            "from the repository root.")
    endif()
    file(GLOB KCD2Online_ADDRESS_LIBRARY_FILES CONFIGURE_DEPENDS
        "${KCD2Online_ADDRESS_LIBRARY_ROOT}/kcd_addresslib_*.bin"
    )
    if(NOT KCD2Online_ADDRESS_LIBRARY_FILES)
        message(FATAL_ERROR
            "Address-Library-For-KCSE is not initialized. Run "
            "`powershell -ExecutionPolicy Bypass -File tools/init_vendor.ps1` "
            "from the repository root.")
    endif()

    add_custom_target(KCD2OnlineAddressLibraryAudit
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/generate_address_library_manifest.py"
            --project-root "${PROJECT_SOURCE_DIR}"
            --check
        COMMAND
            "${Python3_EXECUTABLE}"
            "${PROJECT_SOURCE_DIR}/tools/audit_address_library.py"
            --project-root "${PROJECT_SOURCE_DIR}"
        DEPENDS
            "${PROJECT_SOURCE_DIR}/tools/generate_address_library_manifest.py"
            "${PROJECT_SOURCE_DIR}/src/multiplayer/address_library_manifest.generated.hpp"
            "${PROJECT_SOURCE_DIR}/tools/audit_address_library.py"
            ${KCD2Online_ADDRESS_LIBRARY_FILES}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
        COMMENT "Auditing native REL::ID coverage in the vendored Address Library"
        VERBATIM
    )

    get_target_property(
        KCD2Online_SPDLOG_INCLUDE_DIRECTORIES
        spdlog::spdlog_header_only
        INTERFACE_INCLUDE_DIRECTORIES
    )
    add_library(KCD2OnlineSpdlogHeaderOnly INTERFACE)
    target_include_directories(KCD2OnlineSpdlogHeaderOnly INTERFACE
        ${KCD2Online_SPDLOG_INCLUDE_DIRECTORIES}
    )
    target_compile_definitions(KCD2OnlineSpdlogHeaderOnly INTERFACE
        "FMT_HEADER_ONLY"
        "SPDLOG_FMT_EXTERNAL"
        "SPDLOG_HEADER_ONLY"
    )

    file(GLOB_RECURSE KCD2Online_LIBKCD2_HEADERS CONFIGURE_DEPENDS
        "${KCD2Online_LIBKCD2_ROOT}/include/*.h"
    )
    file(GLOB_RECURSE KCD2Online_LIBKCD2_SOURCES CONFIGURE_DEPENDS
        "${KCD2Online_LIBKCD2_ROOT}/src/*.cpp"
    )
    add_library(kcd_re STATIC
        "${KCD2Online_LIBKCD2_ROOT}/.buildenv/check_sizes.cpp"
        "${KCD2Online_LIBKCD2_ROOT}/.buildenv/CrySDKStubs.cpp"
        ${KCD2Online_LIBKCD2_SOURCES}
        ${KCD2Online_LIBKCD2_HEADERS}
    )
    set_property(TARGET kcd_re PROPERTY CXX_STANDARD 17)
    set_property(TARGET kcd_re PROPERTY CXX_STANDARD_REQUIRED ON)
    target_precompile_headers(kcd_re PRIVATE
        "${KCD2Online_LIBKCD2_ROOT}/.buildenv/PCH.h"
    )
    target_compile_definitions(kcd_re PRIVATE
        "_ITERATOR_DEBUG_LEVEL=0"
        "FORCE_STANDARD_ASSERT"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_compile_options(kcd_re PRIVATE /utf-8 /W4 /wd4100 /wd4819)
    target_include_directories(kcd_re PUBLIC
        "${KCD2Online_LIBKCD2_ROOT}/include"
        "${KCD2Online_LIBKCD2_ROOT}/include/CryEngine/CryCommon"
    )
    target_link_libraries(kcd_re PUBLIC Boost::container)

    file(GLOB_RECURSE KCD2Online_KCSE_SOURCES CONFIGURE_DEPENDS
        "${KCD2Online_KCSE_ROOT}/src/*.cpp"
    )
    add_library(KCSE SHARED ${KCD2Online_KCSE_SOURCES})
    set_property(TARGET KCSE PROPERTY CXX_STANDARD 17)
    set_property(TARGET KCSE PROPERTY CXX_STANDARD_REQUIRED ON)
    target_precompile_headers(KCSE PRIVATE
        "${KCD2Online_LIBKCD2_ROOT}/.buildenv/PCH.h"
    )
    target_compile_definitions(KCSE PRIVATE
        "_ITERATOR_DEBUG_LEVEL=0"
        "FORCE_STANDARD_ASSERT"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_compile_options(KCSE PRIVATE /utf-8 /W4 /wd4100 /wd4819)
    target_include_directories(KCSE PRIVATE
        "${KCD2Online_LIBKCD2_ROOT}/include"
        "${KCD2Online_LIBKCD2_ROOT}/include/CryEngine/CryCommon"
        "${KCD2Online_KCSE_ROOT}/src"
    )
    target_link_libraries(KCSE PRIVATE
        kcd_re
        KCD2OnlineSpdlogHeaderOnly
        version.lib
    )
    set_target_properties(KCSE PROPERTIES
        OUTPUT_NAME "dinput8"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
        PDB_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
    )
endif()

if (TARGET GameNetworkingSockets::GameNetworkingSockets)
    set(KCD2Online_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets)
elseif (TARGET GameNetworkingSockets::GameNetworkingSockets_s)
    set(KCD2Online_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets_s)
elseif (TARGET GameNetworkingSockets)
    set(KCD2Online_GNS_TARGET GameNetworkingSockets)
else()
    message(FATAL_ERROR "The GameNetworkingSockets package did not export a supported CMake target.")
endif()

message(STATUS "KCD2Online")
file(GLOB_RECURSE SRC_MAIN CONFIGURE_DEPENDS
    "${SRC_DIR}/**.hpp"
    "${SRC_DIR}/**.h"
    "${SRC_DIR}/**.cpp"
    "${SRC_DIR}/**.cc"
    "${SRC_DIR}/**.cxx"
    "${SRC_DIR}/**.asm"
    "${SRC_DIR}/**.def"
    "${SRC_DIR}/**.rc"
)
# Ignore files generated by older checkouts, which wrote version.cpp into src/.
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]version\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]signatures[/\\\\]signature_core\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]multiplayer[/\\\\]protocol\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]multiplayer[/\\\\]networking\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]server[/\\\\].*")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]npc[/\\\\]catalog\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]npc[/\\\\]equipment_catalog\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]kcse[/\\\\]plugin\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]kcse[/\\\\]native_.*\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]multiplayer[/\\\\]client\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]multiplayer[/\\\\]identity_store\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]account[/\\\\].*\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]gui[/\\\\]native_multiplayer_menu\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]gui[/\\\\]native_ingame_menu_api\\.cpp$")
list(FILTER SRC_MAIN EXCLUDE REGEX "[/\\\\]gui[/\\\\]native_ui_localization\\.cpp$")

if (NOT TARGET Zydis)
    message(FATAL_ERROR
        "KCD2Online requires the Zydis target supplied by the pinned "
        "ReturnOfModdingBase/PolyHook dependency.")
endif()

add_library(KCD2OnlineSignatureCore STATIC
    "${SRC_DIR}/signatures/signature_core.cpp"
    "${SRC_DIR}/signatures/signature_core.hpp"
)
set_property(TARGET KCD2OnlineSignatureCore PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineSignatureCore PUBLIC "${SRC_DIR}")
target_compile_definitions(KCD2OnlineSignatureCore PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
target_link_libraries(KCD2OnlineSignatureCore PUBLIC Zydis)
set_property(TARGET KCD2OnlineSignatureCore PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX>:/bigobj>")
add_compile_options("$<$<COMPILE_LANG_AND_ID:C,MSVC>:/utf-8>")
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>")

if (FINAL)
    add_compile_definitions(FINAL)
endif()

add_library(KCD2OnlineNpcCatalog STATIC
    "${SRC_DIR}/npc/catalog.cpp"
    "${SRC_DIR}/npc/catalog.hpp"
    "${SRC_DIR}/npc/equipment_catalog.cpp"
    "${SRC_DIR}/npc/equipment_catalog.hpp"
)
set_property(TARGET KCD2OnlineNpcCatalog PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineNpcCatalog PUBLIC
    "${SRC_DIR}"
    "${zip_SOURCE_DIR}"
)
target_link_libraries(KCD2OnlineNpcCatalog PUBLIC
    pugixml::pugixml
    nlohmann_json::nlohmann_json
    zip
)
target_compile_definitions(KCD2OnlineNpcCatalog PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineNpcCatalog PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_library(KCD2OnlineProtocol STATIC
    "${SRC_DIR}/multiplayer/protocol.cpp"
    "${SRC_DIR}/multiplayer/protocol.hpp"
    "${PROJECT_SOURCE_DIR}/protocol/kcd2o.proto"
)
set_property(TARGET KCD2OnlineProtocol PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineProtocol PUBLIC
    "${SRC_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(KCD2OnlineProtocol PUBLIC protobuf::libprotobuf)
target_compile_definitions(KCD2OnlineProtocol PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineProtocol PROPERTY COMPILE_WARNING_AS_ERROR ON)
protobuf_generate(
    TARGET KCD2OnlineProtocol
    LANGUAGE cpp
    PROTOS "${PROJECT_SOURCE_DIR}/protocol/kcd2o.proto"
    IMPORT_DIRS "${PROJECT_SOURCE_DIR}/protocol"
)

add_library(KCD2OnlineNetworking STATIC
    "${SRC_DIR}/multiplayer/networking.cpp"
    "${SRC_DIR}/multiplayer/networking.hpp"
)
set_property(TARGET KCD2OnlineNetworking PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineNetworking PUBLIC "${SRC_DIR}")
target_link_libraries(KCD2OnlineNetworking PUBLIC KCD2OnlineProtocol ${KCD2Online_GNS_TARGET})
target_compile_definitions(KCD2OnlineNetworking PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineNetworking PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_library(KCD2OnlineProperty STATIC
    "${SRC_DIR}/property/catalog.cpp"
    "${SRC_DIR}/property/catalog.hpp"
    "${SRC_DIR}/property/service.cpp"
    "${SRC_DIR}/property/service.hpp"
)
set_property(TARGET KCD2OnlineProperty PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineProperty PUBLIC "${SRC_DIR}")
target_link_libraries(KCD2OnlineProperty PUBLIC KCD2OnlineProtocol)
target_link_libraries(KCD2OnlineProperty PRIVATE pugixml::pugixml zip)
target_compile_definitions(KCD2OnlineProperty PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineProperty PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_library(KCD2OnlineServerCore STATIC
	"${SRC_DIR}/server/item_ledger.cpp"
	"${SRC_DIR}/server/item_ledger.hpp"
	"${SRC_DIR}/server/npc_registry.cpp"
	"${SRC_DIR}/server/npc_registry.hpp"
    "${SRC_DIR}/server/server_config.cpp"
    "${SRC_DIR}/server/server_config.hpp"
    "${SRC_DIR}/server/server_core.cpp"
    "${SRC_DIR}/server/server_core.hpp"
    "${SRC_DIR}/server/starter_profile.cpp"
    "${SRC_DIR}/server/starter_profile.hpp"
    "${SRC_DIR}/server/world_store.cpp"
    "${SRC_DIR}/server/world_store.hpp"
)
set_property(TARGET KCD2OnlineServerCore PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineServerCore PUBLIC "${SRC_DIR}")
target_link_libraries(KCD2OnlineServerCore PUBLIC
    KCD2OnlineProtocol
    KCD2OnlineProperty
    tomlplusplus::tomlplusplus
    bcrypt
	 nlohmann_json::nlohmann_json
)
target_compile_definitions(KCD2OnlineServerCore PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineServerCore PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_executable(KCD2OnlineServer "${SRC_DIR}/server/server_main.cpp")
target_sources(KCD2OnlineServer PRIVATE
    "${SRC_DIR}/server/backend_client.cpp"
    "${SRC_DIR}/server/backend_client.hpp"
)
set_property(TARGET KCD2OnlineServer PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineServer PRIVATE "${SRC_DIR}")
target_link_libraries(KCD2OnlineServer PRIVATE
    KCD2OnlineServerCore
    KCD2OnlineNetworking
    nlohmann_json::nlohmann_json
    winhttp
)
target_compile_definitions(KCD2OnlineServer PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
set_property(TARGET KCD2OnlineServer PROPERTY COMPILE_WARNING_AS_ERROR ON)
add_custom_command(TARGET KCD2OnlineServer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/server.toml.example"
        "$<TARGET_FILE_DIR:KCD2OnlineServer>/server.toml.example"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/starter_profile.toml"
        "$<TARGET_FILE_DIR:KCD2OnlineServer>/starter_profile.toml"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/data/npc_archetypes.json"
        "$<TARGET_FILE_DIR:KCD2OnlineServer>/npc_archetypes.json"
)

# The native Scaleform menu uses CryEngine ABI types and must therefore be
# compiled with libKCD2's PCH instead of the main module's ROM/ImGui PCH.
if(KCD2Online_BUILD_KCSE)
    add_library(KCD2OnlineAccount STATIC
        "${SRC_DIR}/account/account_api.cpp"
        "${SRC_DIR}/account/account_api.hpp"
        "${SRC_DIR}/account/account_crypto.cpp"
        "${SRC_DIR}/account/account_crypto.hpp"
        "${SRC_DIR}/account/account_service.cpp"
        "${SRC_DIR}/account/account_service.hpp"
        "${SRC_DIR}/account/account_store.cpp"
        "${SRC_DIR}/account/account_store.hpp"
        "${SRC_DIR}/account/server_directory.cpp"
        "${SRC_DIR}/account/server_directory.hpp"
    )
    set_property(TARGET KCD2OnlineAccount PROPERTY CXX_STANDARD 23)
    set_property(TARGET KCD2OnlineAccount PROPERTY CXX_STANDARD_REQUIRED ON)
    target_include_directories(KCD2OnlineAccount PUBLIC "${SRC_DIR}")
    target_compile_definitions(KCD2OnlineAccount PRIVATE
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_compile_options(KCD2OnlineAccount PRIVATE /utf-8 /W4 /permissive-)
    target_link_libraries(KCD2OnlineAccount PUBLIC nlohmann_json::nlohmann_json)
    target_link_libraries(KCD2OnlineAccount PRIVATE
        advapi32
        bcrypt
        crypt32
        winhttp
    )
    set_property(TARGET KCD2OnlineAccount PROPERTY COMPILE_WARNING_AS_ERROR ON)

    add_library(KCD2OnlineNativeMultiplayerMenu OBJECT
		"${SRC_DIR}/gui/ingame_menu.hpp"
		"${SRC_DIR}/gui/native_ingame_menu_api.cpp"
		"${SRC_DIR}/gui/native_ingame_menu_api.hpp"
		"${SRC_DIR}/gui/native_ui_localization.cpp"
		"${SRC_DIR}/gui/native_ui_localization.hpp"
        "${SRC_DIR}/gui/native_multiplayer_menu.cpp"
        "${SRC_DIR}/gui/native_multiplayer_menu.hpp"
    )
    set_property(TARGET KCD2OnlineNativeMultiplayerMenu PROPERTY CXX_STANDARD 23)
    set_property(TARGET KCD2OnlineNativeMultiplayerMenu PROPERTY CXX_STANDARD_REQUIRED ON)
    target_precompile_headers(KCD2OnlineNativeMultiplayerMenu PRIVATE
        "${KCD2Online_LIBKCD2_ROOT}/.buildenv/PCH.h"
    )
    target_include_directories(KCD2OnlineNativeMultiplayerMenu PRIVATE
        "${SRC_DIR}"
        "${KCD2Online_LIBKCD2_ROOT}/include"
        "${KCD2Online_LIBKCD2_ROOT}/include/CryEngine/CryCommon"
    )
    target_compile_definitions(KCD2OnlineNativeMultiplayerMenu PRIVATE
        "KCD2Online_NATIVE_MULTIPLAYER_MENU"
        "_ITERATOR_DEBUG_LEVEL=0"
        "FORCE_STANDARD_ASSERT"
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_compile_options(KCD2OnlineNativeMultiplayerMenu PRIVATE
        /utf-8 /W4 /wd4100 /wd4819 /permissive
    )
    target_link_libraries(KCD2OnlineNativeMultiplayerMenu PRIVATE
        kcd_re
        KCD2OnlineAccount
        KCD2OnlineProtocol
        KCD2OnlineNetworking
    )
    set_property(TARGET KCD2OnlineNativeMultiplayerMenu PROPERTY COMPILE_WARNING_AS_ERROR ON)
endif()

add_library(KCD2Online MODULE
    "${SRC_MAIN}"
    "${GENERATED_VERSION_SOURCE}"
    "${GENERATED_RESOURCE_FILE}"
)

if(KCD2Online_BUILD_KCSE)
    target_sources(KCD2Online PRIVATE
        $<TARGET_OBJECTS:KCD2OnlineNativeMultiplayerMenu>
    )
    target_compile_definitions(KCD2Online PRIVATE
        "KCD2Online_NATIVE_MULTIPLAYER_MENU"
    )
    target_link_libraries(KCD2Online PRIVATE kcd_re KCD2OnlineAccount)
endif()

set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(TARGET KCD2Online PROPERTY CXX_STANDARD 23)

source_group(TREE ${SRC_DIR} PREFIX "src" FILES ${SRC_MAIN})
source_group("generated" FILES
    "${GENERATED_VERSION_SOURCE}"
    "${GENERATED_RESOURCE_FILE}"
)

target_include_directories(KCD2Online PRIVATE
    "${SRC_DIR}"
    "${imguizmo_SOURCE_DIR}"
    "${zip_SOURCE_DIR}"
)

target_precompile_headers(KCD2Online PRIVATE "${SRC_DIR}/common.hpp")

target_link_libraries(KCD2Online PRIVATE
    ReturnOfModdingBase
    KCD2OnlineProtocol
    KCD2OnlineSignatureCore
    KCD2OnlineNpcCatalog
    Zydis
    zip
    imguizmo
    mimalloc-static
)

if(KCD2Online_BUILD_KCSE)
    add_library(KCD2OnlineKCSENativeRuntime OBJECT
		"${SRC_DIR}/kcse/native_avatar_combat.cpp"
		"${SRC_DIR}/kcse/native_avatar_combat.hpp"
		"${SRC_DIR}/kcse/native_combat_observer.cpp"
		"${SRC_DIR}/kcse/native_combat_observer.hpp"
        "${SRC_DIR}/kcse/native_entity_backend.cpp"
        "${SRC_DIR}/kcse/native_entity_backend.hpp"
		"${SRC_DIR}/kcse/native_equipment.hpp"
		"${SRC_DIR}/kcse/native_inventory.cpp"
		"${SRC_DIR}/kcse/native_inventory.hpp"
        "${SRC_DIR}/kcse/native_profile_backend.cpp"
        "${SRC_DIR}/kcse/native_profile_backend.hpp"
		"${SRC_DIR}/kcse/native_remote_avatar_backend.cpp"
		"${SRC_DIR}/kcse/native_remote_avatar_backend.hpp"
		"${SRC_DIR}/kcse/native_remote_avatar_equipment.cpp"
		"${SRC_DIR}/kcse/native_remote_avatar_equipment.hpp"
		"${SRC_DIR}/kcse/remote_avatar_readiness.hpp"
		"${SRC_DIR}/kcse/native_weapon_controller.cpp"
		"${SRC_DIR}/kcse/native_weapon_controller.hpp"
		"${SRC_DIR}/kcse/player_respawn_guard.hpp"
		"${SRC_DIR}/kcse/native_runtime.cpp"
        "${SRC_DIR}/kcse/native_runtime.hpp"
		"${SRC_DIR}/kcse/native_world_object_sync.cpp"
		"${SRC_DIR}/kcse/native_world_object_sync.hpp"
		"${SRC_DIR}/kcse/native_world_item_sync.cpp"
		"${SRC_DIR}/kcse/native_world_item_sync.hpp"
    )
    set_property(TARGET KCD2OnlineKCSENativeRuntime PROPERTY CXX_STANDARD 23)
    set_property(TARGET KCD2OnlineKCSENativeRuntime PROPERTY CXX_STANDARD_REQUIRED ON)
    target_precompile_headers(KCD2OnlineKCSENativeRuntime PRIVATE
        "${PROJECT_SOURCE_DIR}/libKCD2/.buildenv/PCH.h"
    )
    target_include_directories(KCD2OnlineKCSENativeRuntime PRIVATE
        "${SRC_DIR}"
        "${PROJECT_SOURCE_DIR}/libKCD2/include"
        "${PROJECT_SOURCE_DIR}/libKCD2/include/CryEngine/CryCommon"
    )
    target_compile_definitions(KCD2OnlineKCSENativeRuntime PRIVATE
        "_ITERATOR_DEBUG_LEVEL=0"
        "FORCE_STANDARD_ASSERT"
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    # CryEngine's legacy SDK headers still rely on MSVC's delayed template
    # lookup. Keep the runtime on C++23 while retaining that source-compatible
    # lookup mode; this does not alter generated ABI. /FS serializes access to
    # the target PDB when MSBuild launches several compiler processes in
    # parallel.
    target_compile_options(KCD2OnlineKCSENativeRuntime PRIVATE
        /utf-8 /W4 /wd4100 /wd4819 /permissive /FS
    )
    target_link_libraries(KCD2OnlineKCSENativeRuntime PRIVATE
        kcd_re
        KCD2OnlineNpcCatalog
        KCD2OnlineProtocol
    )
    set_property(TARGET KCD2OnlineKCSENativeRuntime PROPERTY COMPILE_WARNING_AS_ERROR ON)

    add_library(KCD2OnlineKCSEClient SHARED
        "${SRC_DIR}/kcse/client_api.hpp"
        "${SRC_DIR}/kcse/join_trace.cpp"
        "${SRC_DIR}/kcse/join_trace.hpp"
        "${SRC_DIR}/kcse/plugin.cpp"
        "${SRC_DIR}/kcse/native_runtime.hpp"
        "${SRC_DIR}/multiplayer/client.cpp"
        "${SRC_DIR}/multiplayer/client.hpp"
        "${SRC_DIR}/multiplayer/client_message_gate.hpp"
        "${SRC_DIR}/multiplayer/client_state.hpp"
        "${SRC_DIR}/multiplayer/identity_store.cpp"
        "${SRC_DIR}/multiplayer/identity_store.hpp"
    )
    set_property(TARGET KCD2OnlineKCSEClient PROPERTY CXX_STANDARD 23)
    set_property(TARGET KCD2OnlineKCSEClient PROPERTY CXX_STANDARD_REQUIRED ON)
    target_include_directories(KCD2OnlineKCSEClient PRIVATE
        "${SRC_DIR}"
        "${PROJECT_SOURCE_DIR}/libKCD2/include"
        "${PROJECT_SOURCE_DIR}/libKCD2/include/CryEngine/CryCommon"
    )
    target_compile_definitions(KCD2OnlineKCSEClient PRIVATE
        "_ITERATOR_DEBUG_LEVEL=0"
        "FORCE_STANDARD_ASSERT"
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_compile_options(KCD2OnlineKCSEClient PRIVATE /utf-8 /W4 /wd4100 /wd4819)
    target_link_libraries(KCD2OnlineKCSEClient PRIVATE
        KCD2OnlineKCSENativeRuntime
        kcd_re
        KCD2OnlineProtocol
        KCD2OnlineNetworking
        KCD2OnlineNpcCatalog
        KCD2OnlineAccount
        crypt32
    )
    set_property(TARGET KCD2OnlineKCSEClient PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_dependencies(KCSE KCD2OnlineAddressLibraryAudit)
    add_dependencies(KCD2OnlineKCSEClient KCD2OnlineAddressLibraryAudit)

    add_custom_command(TARGET KCD2OnlineKCSEClient POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:KCD2OnlineKCSEClient>/KCSE/addresslib"
        COMMENT "Bundling KCSE Address Library tables"
    )
    foreach(KCD2Online_ADDRESS_LIBRARY_FILE IN LISTS KCD2Online_ADDRESS_LIBRARY_FILES)
        add_custom_command(TARGET KCD2OnlineKCSEClient POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${KCD2Online_ADDRESS_LIBRARY_FILE}"
                "$<TARGET_FILE_DIR:KCD2OnlineKCSEClient>/KCSE/addresslib"
        )
    endforeach()

    add_custom_target(KCD2OnlineRuntime
        DEPENDS KCD2Online KCSE KCD2OnlineKCSEClient
    )
else()
    add_custom_target(KCD2OnlineRuntime DEPENDS KCD2Online)
endif()

# Warnings as errors
set_property(TARGET KCD2Online PROPERTY COMPILE_WARNING_AS_ERROR ON)

target_compile_definitions(KCD2Online PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)

# Keep the raw CMake build output usable as a client artifact tree as well.
# The runtime resolves translations below <game-root>/Mods/KCD2Online/Lang, so the
# same relative layout is emitted beside the other build artifacts. The custom
# target intentionally runs on every KCD2Online build, allowing translation-only
# edits to be picked up without relinking the DLL.
set(KCD2Online_LANGUAGE_ROOT "${PROJECT_SOURCE_DIR}/data/lang")
file(GLOB KCD2Online_LANGUAGE_FILES CONFIGURE_DEPENDS
    "${KCD2Online_LANGUAGE_ROOT}/*.lang"
)
if(NOT EXISTS "${KCD2Online_LANGUAGE_ROOT}/en.lang")
    message(FATAL_ERROR
        "KCD2Online requires data/lang/en.lang as its localization fallback."
    )
endif()
if(EXISTS "${KCD2Online_LANGUAGE_ROOT}/README.md")
    list(APPEND KCD2Online_LANGUAGE_FILES
        "${KCD2Online_LANGUAGE_ROOT}/README.md"
    )
endif()

add_custom_target(KCD2OnlineLanguageFiles
    COMMAND "${CMAKE_COMMAND}" -E remove_directory
        "$<TARGET_FILE_DIR:KCD2Online>/Mods/KCD2Online/Lang"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:KCD2Online>/Mods/KCD2Online/Lang"
    DEPENDS ${KCD2Online_LANGUAGE_FILES}
    COMMENT "Bundling KCD2Online language files"
    VERBATIM
)
foreach(KCD2Online_LANGUAGE_FILE IN LISTS KCD2Online_LANGUAGE_FILES)
    add_custom_command(TARGET KCD2OnlineLanguageFiles POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${KCD2Online_LANGUAGE_FILE}"
            "$<TARGET_FILE_DIR:KCD2Online>/Mods/KCD2Online/Lang"
    )
endforeach()
add_dependencies(KCD2Online KCD2OnlineLanguageFiles)
if(TARGET KCD2OnlineKCSEClient)
    add_dependencies(KCD2OnlineKCSEClient KCD2OnlineLanguageFiles)
endif()

if (MSVC)
    if (FINAL)
        # Aggresive inlining (Release)
        string(REPLACE "Ob2" "Ob3" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
        # Aggresive inlining (RelWithDebInfo)
        string(REPLACE "Ob1" "Ob3" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
        # Enable intrinsics
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Oi")
        # Favor fast code
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Ot")
        # -ffast-math (fused multiply-add if the arch allows)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /fp:fast")
        # Whole program optimization
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GL")
        # Function-level linking
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Gy")
        # Disable Buffer Security Check
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /GS-")
        # Package global data in COMDAT sections for optimization.
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /Gw")

        # For all objects:
        # - strip unused functions
        # - fold identical functions
        # - link-time code generation
        string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO}")
        string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO}")
        string(REPLACE "INCREMENTAL" "INCREMENTAL:NO" CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO}")
        set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")
        set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")
        set(CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO} /OPT:ICF,REF /LTCG")

        set(CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO} /PDBALTPATH:d3d12.pdb")
        set(CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO} /PDBALTPATH:d3d12.pdb")
        set(CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO} /PDBALTPATH:d3d12.pdb")
    endif()
endif()

set_target_properties(KCD2Online PROPERTIES OUTPUT_NAME "d3d12_")

add_executable(KCD2OnlineSignatureAudit "${PROJECT_SOURCE_DIR}/tools/signature_audit_main.cpp")
set_property(TARGET KCD2OnlineSignatureAudit PROPERTY CXX_STANDARD 23)
target_include_directories(KCD2OnlineSignatureAudit PRIVATE "${SRC_DIR}")
target_compile_definitions(KCD2OnlineSignatureAudit PRIVATE
    "_CRT_SECURE_NO_WARNINGS"
    "NOMINMAX"
    "WIN32_LEAN_AND_MEAN"
)
target_link_libraries(KCD2OnlineSignatureAudit PRIVATE KCD2OnlineSignatureCore bcrypt)
set_property(TARGET KCD2OnlineSignatureAudit PROPERTY COMPILE_WARNING_AS_ERROR ON)

add_executable(KCD2OnlinePropertyCatalog
    "${PROJECT_SOURCE_DIR}/tools/property_catalog_main.cpp"
)
set_property(TARGET KCD2OnlinePropertyCatalog PROPERTY CXX_STANDARD 23)
target_link_libraries(KCD2OnlinePropertyCatalog PRIVATE KCD2OnlineProperty)
set_property(TARGET KCD2OnlinePropertyCatalog PROPERTY COMPILE_WARNING_AS_ERROR ON)

include(CTest)
if (BUILD_TESTING)
    add_executable(KCD2OnlineSignatureCoreTests "${PROJECT_SOURCE_DIR}/tests/signature_core_tests.cpp")
    set_property(TARGET KCD2OnlineSignatureCoreTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineSignatureCoreTests PRIVATE "${SRC_DIR}")
    target_compile_definitions(KCD2OnlineSignatureCoreTests PRIVATE
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_link_libraries(KCD2OnlineSignatureCoreTests PRIVATE KCD2OnlineSignatureCore)
    set_property(TARGET KCD2OnlineSignatureCoreTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineSignatureCoreTests COMMAND KCD2OnlineSignatureCoreTests)

    add_executable(KCD2OnlineEnginePathTests "${PROJECT_SOURCE_DIR}/tests/engine_path_tests.cpp")
    set_property(TARGET KCD2OnlineEnginePathTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineEnginePathTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineEnginePathTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineEnginePathTests COMMAND KCD2OnlineEnginePathTests)

    add_executable(KCD2OnlineProtocolTests "${PROJECT_SOURCE_DIR}/tests/protocol_tests.cpp")
    set_property(TARGET KCD2OnlineProtocolTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineProtocolTests PRIVATE KCD2OnlineProtocol)
    set_property(TARGET KCD2OnlineProtocolTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineProtocolTests COMMAND KCD2OnlineProtocolTests)

    add_executable(KCD2OnlineServerCoreTests "${PROJECT_SOURCE_DIR}/tests/server_core_tests.cpp")
    set_property(TARGET KCD2OnlineServerCoreTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineServerCoreTests PRIVATE KCD2OnlineServerCore)
    target_compile_definitions(KCD2OnlineServerCoreTests PRIVATE
        "KCD2Online_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    )
    set_property(TARGET KCD2OnlineServerCoreTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineServerCoreTests COMMAND KCD2OnlineServerCoreTests)

	add_executable(KCD2OnlineItemLedgerTests
		"${PROJECT_SOURCE_DIR}/tests/item_ledger_tests.cpp"
	)
	set_property(TARGET KCD2OnlineItemLedgerTests PROPERTY CXX_STANDARD 23)
	target_link_libraries(KCD2OnlineItemLedgerTests PRIVATE KCD2OnlineServerCore)
	set_property(TARGET KCD2OnlineItemLedgerTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlineItemLedgerTests COMMAND KCD2OnlineItemLedgerTests)

	add_executable(KCD2OnlineNpcRegistryTests
		"${PROJECT_SOURCE_DIR}/tests/npc_registry_tests.cpp"
	)
	set_property(TARGET KCD2OnlineNpcRegistryTests PROPERTY CXX_STANDARD 23)
	target_link_libraries(KCD2OnlineNpcRegistryTests PRIVATE KCD2OnlineServerCore)
	set_property(TARGET KCD2OnlineNpcRegistryTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlineNpcRegistryTests COMMAND KCD2OnlineNpcRegistryTests)

    add_executable(KCD2OnlinePropertyTests
        "${PROJECT_SOURCE_DIR}/tests/property_tests.cpp"
    )
    set_property(TARGET KCD2OnlinePropertyTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlinePropertyTests PRIVATE
        KCD2OnlineServerCore
        KCD2OnlineProperty
        zip
    )
    set_property(TARGET KCD2OnlinePropertyTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlinePropertyTests COMMAND KCD2OnlinePropertyTests)

    add_executable(KCD2OnlineGameCommandQueueTests
        "${PROJECT_SOURCE_DIR}/tests/game_command_queue_tests.cpp"
    )
    set_property(TARGET KCD2OnlineGameCommandQueueTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineGameCommandQueueTests PRIVATE KCD2OnlineProtocol)
    target_include_directories(KCD2OnlineGameCommandQueueTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineGameCommandQueueTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineGameCommandQueueTests COMMAND KCD2OnlineGameCommandQueueTests)

    add_executable(KCD2OnlineClientStateTests
        "${PROJECT_SOURCE_DIR}/tests/client_state_tests.cpp"
    )
    set_property(TARGET KCD2OnlineClientStateTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineClientStateTests PRIVATE KCD2OnlineProtocol)
    target_include_directories(KCD2OnlineClientStateTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineClientStateTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineClientStateTests COMMAND KCD2OnlineClientStateTests)

	add_executable(KCD2OnlinePlayerRespawnGuardTests
		"${PROJECT_SOURCE_DIR}/tests/player_respawn_guard_tests.cpp"
	)
	set_property(TARGET KCD2OnlinePlayerRespawnGuardTests PROPERTY CXX_STANDARD 23)
	target_include_directories(KCD2OnlinePlayerRespawnGuardTests PRIVATE "${SRC_DIR}")
	set_property(TARGET KCD2OnlinePlayerRespawnGuardTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlinePlayerRespawnGuardTests COMMAND KCD2OnlinePlayerRespawnGuardTests)

    add_executable(KCD2OnlineEntityControlTests
        "${PROJECT_SOURCE_DIR}/tests/entity_control_tests.cpp"
    )
    set_property(TARGET KCD2OnlineEntityControlTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineEntityControlTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineEntityControlTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineEntityControlTests COMMAND KCD2OnlineEntityControlTests)

    add_executable(KCD2OnlineRuntimeHandleTests
        "${PROJECT_SOURCE_DIR}/tests/runtime_handle_tests.cpp"
    )
    set_property(TARGET KCD2OnlineRuntimeHandleTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineRuntimeHandleTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineRuntimeHandleTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineRuntimeHandleTests COMMAND KCD2OnlineRuntimeHandleTests)

    add_executable(KCD2OnlineRemoteAvatarTests
        "${PROJECT_SOURCE_DIR}/tests/remote_avatar_tests.cpp"
    )
    set_property(TARGET KCD2OnlineRemoteAvatarTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineRemoteAvatarTests PRIVATE KCD2OnlineProtocol)
    target_include_directories(KCD2OnlineRemoteAvatarTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineRemoteAvatarTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineRemoteAvatarTests COMMAND KCD2OnlineRemoteAvatarTests)

    add_executable(KCD2OnlineNpcTests
        "${PROJECT_SOURCE_DIR}/tests/npc_tests.cpp"
    )
    set_property(TARGET KCD2OnlineNpcTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineNpcTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineNpcTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineNpcTests COMMAND KCD2OnlineNpcTests)

    add_executable(KCD2OnlineKCSEClientApiTests
        "${PROJECT_SOURCE_DIR}/tests/client_api_tests.cpp"
    )
    set_property(TARGET KCD2OnlineKCSEClientApiTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineKCSEClientApiTests PRIVATE
        "${SRC_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
    )
    set_property(TARGET KCD2OnlineKCSEClientApiTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineKCSEClientApiTests COMMAND KCD2OnlineKCSEClientApiTests)

    add_executable(KCD2OnlineNpcCatalogTests
        "${PROJECT_SOURCE_DIR}/tests/npc_catalog_tests.cpp"
    )
    set_property(TARGET KCD2OnlineNpcCatalogTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineNpcCatalogTests PRIVATE KCD2OnlineNpcCatalog)
    target_compile_definitions(KCD2OnlineNpcCatalogTests PRIVATE
        "KCD2Online_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
    )
    set_property(TARGET KCD2OnlineNpcCatalogTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineNpcCatalogTests COMMAND KCD2OnlineNpcCatalogTests)

    add_executable(KCD2OnlineEquipmentCatalogTests
        "${PROJECT_SOURCE_DIR}/tests/equipment_catalog_tests.cpp"
    )
    set_property(TARGET KCD2OnlineEquipmentCatalogTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineEquipmentCatalogTests PRIVATE KCD2OnlineNpcCatalog)
    set_property(TARGET KCD2OnlineEquipmentCatalogTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineEquipmentCatalogTests COMMAND KCD2OnlineEquipmentCatalogTests)

    add_executable(KCD2OnlineAvatarVisualTests
        "${PROJECT_SOURCE_DIR}/tests/avatar_visual_tests.cpp"
    )
    set_property(TARGET KCD2OnlineAvatarVisualTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineAvatarVisualTests PRIVATE KCD2OnlineProtocol)
    target_include_directories(KCD2OnlineAvatarVisualTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineAvatarVisualTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineAvatarVisualTests COMMAND KCD2OnlineAvatarVisualTests)

	add_executable(KCD2OnlineIngameMenuTests
		"${PROJECT_SOURCE_DIR}/tests/ingame_menu_tests.cpp"
	)
	set_property(TARGET KCD2OnlineIngameMenuTests PROPERTY CXX_STANDARD 23)
	target_include_directories(KCD2OnlineIngameMenuTests PRIVATE "${SRC_DIR}")
	set_property(TARGET KCD2OnlineIngameMenuTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlineIngameMenuTests COMMAND KCD2OnlineIngameMenuTests)

	add_executable(KCD2OnlineLocalizationTests
		"${PROJECT_SOURCE_DIR}/tests/localization_tests.cpp"
		"${SRC_DIR}/gui/localization.cpp"
		"${SRC_DIR}/gui/localization.hpp"
	)
	set_property(TARGET KCD2OnlineLocalizationTests PROPERTY CXX_STANDARD 23)
	target_include_directories(KCD2OnlineLocalizationTests PRIVATE "${SRC_DIR}")
	target_compile_definitions(KCD2OnlineLocalizationTests PRIVATE
		"KCD2Online_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\""
	)
	set_property(TARGET KCD2OnlineLocalizationTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlineLocalizationTests COMMAND KCD2OnlineLocalizationTests)

    add_executable(KCD2OnlineProfileReconcilerTests
        "${PROJECT_SOURCE_DIR}/tests/profile_reconciler_tests.cpp"
    )
    set_property(TARGET KCD2OnlineProfileReconcilerTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineProfileReconcilerTests PRIVATE
        KCD2OnlineServerCore
    )
    target_include_directories(KCD2OnlineProfileReconcilerTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineProfileReconcilerTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineProfileReconcilerTests COMMAND KCD2OnlineProfileReconcilerTests)

    add_executable(KCD2OnlineEngineConsoleQueueTests
        "${PROJECT_SOURCE_DIR}/tests/engine_console_queue_tests.cpp"
    )
    set_property(TARGET KCD2OnlineEngineConsoleQueueTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineEngineConsoleQueueTests PRIVATE "${SRC_DIR}")
    set_property(TARGET KCD2OnlineEngineConsoleQueueTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineEngineConsoleQueueTests COMMAND KCD2OnlineEngineConsoleQueueTests)

    add_executable(KCD2OnlineNetworkingTests
        "${PROJECT_SOURCE_DIR}/tests/networking_tests.cpp"
    )
    set_property(TARGET KCD2OnlineNetworkingTests PROPERTY CXX_STANDARD 23)
    target_link_libraries(KCD2OnlineNetworkingTests PRIVATE KCD2OnlineNetworking)
    set_property(TARGET KCD2OnlineNetworkingTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineNetworkingTests COMMAND KCD2OnlineNetworkingTests)

    add_executable(KCD2OnlineIdentityStoreTests
        "${PROJECT_SOURCE_DIR}/tests/identity_store_tests.cpp"
        "${SRC_DIR}/multiplayer/identity_store.cpp"
        "${SRC_DIR}/multiplayer/identity_store.hpp"
    )
    set_property(TARGET KCD2OnlineIdentityStoreTests PROPERTY CXX_STANDARD 23)
    target_include_directories(KCD2OnlineIdentityStoreTests PRIVATE "${SRC_DIR}")
    target_compile_definitions(KCD2OnlineIdentityStoreTests PRIVATE
        "_CRT_SECURE_NO_WARNINGS"
        "NOMINMAX"
        "WIN32_LEAN_AND_MEAN"
    )
    target_link_libraries(KCD2OnlineIdentityStoreTests PRIVATE crypt32)
    set_property(TARGET KCD2OnlineIdentityStoreTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
    add_test(NAME KCD2OnlineIdentityStoreTests COMMAND KCD2OnlineIdentityStoreTests)

    if(TARGET KCD2OnlineAccount)
        add_executable(KCD2OnlineAccountClientTests
            "${PROJECT_SOURCE_DIR}/tests/account_client_tests.cpp"
        )
        set_property(TARGET KCD2OnlineAccountClientTests PROPERTY CXX_STANDARD 23)
        target_link_libraries(KCD2OnlineAccountClientTests PRIVATE KCD2OnlineAccount)
        set_property(TARGET KCD2OnlineAccountClientTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
        add_test(NAME KCD2OnlineAccountClientTests COMMAND KCD2OnlineAccountClientTests)
    endif()

	add_executable(KCD2OnlineWorldObjectSyncTests
		"${PROJECT_SOURCE_DIR}/tests/world_object_sync_tests.cpp"
	)
	set_property(TARGET KCD2OnlineWorldObjectSyncTests PROPERTY CXX_STANDARD 23)
	target_include_directories(KCD2OnlineWorldObjectSyncTests PRIVATE "${SRC_DIR}")
	target_link_libraries(KCD2OnlineWorldObjectSyncTests PRIVATE KCD2OnlineProtocol)
	set_property(TARGET KCD2OnlineWorldObjectSyncTests PROPERTY COMPILE_WARNING_AS_ERROR ON)
	add_test(NAME KCD2OnlineWorldObjectSyncTests COMMAND KCD2OnlineWorldObjectSyncTests)

    add_custom_target(KCD2OnlineTests
        DEPENDS
            KCD2OnlineSignatureCoreTests
            KCD2OnlineEnginePathTests
            KCD2OnlineProtocolTests
            KCD2OnlineServerCoreTests
			KCD2OnlineItemLedgerTests
			KCD2OnlineNpcRegistryTests
            KCD2OnlinePropertyTests
            KCD2OnlineGameCommandQueueTests
            KCD2OnlineClientStateTests
            KCD2OnlinePlayerRespawnGuardTests
            KCD2OnlineEntityControlTests
            KCD2OnlineRuntimeHandleTests
            KCD2OnlineRemoteAvatarTests
            KCD2OnlineNpcTests
            KCD2OnlineKCSEClientApiTests
            KCD2OnlineNpcCatalogTests
            KCD2OnlineEquipmentCatalogTests
            KCD2OnlineAvatarVisualTests
			KCD2OnlineIngameMenuTests
			KCD2OnlineLocalizationTests
            KCD2OnlineProfileReconcilerTests
            KCD2OnlineEngineConsoleQueueTests
            KCD2OnlineNetworkingTests
            KCD2OnlineIdentityStoreTests
			KCD2OnlineWorldObjectSyncTests
    )
    if(TARGET KCD2OnlineAccountClientTests)
        add_dependencies(KCD2OnlineTests KCD2OnlineAccountClientTests)
    endif()
endif()

if (KCD2Online_WHGAME_PATH)
    add_custom_target(KCD2OnlineAuditInstalledGame
        COMMAND "$<TARGET_FILE:KCD2OnlineSignatureAudit>" "${KCD2Online_WHGAME_PATH}"
        DEPENDS KCD2OnlineSignatureAudit
        COMMENT "Auditing KCD2Online signatures against WHGame.dll"
        VERBATIM
    )
endif()
