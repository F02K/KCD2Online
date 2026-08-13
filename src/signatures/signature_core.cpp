#include "signatures/signature_core.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <utility>

#include <Zydis/Zydis.h>

namespace kcd2::signatures
{
	namespace
	{
		constexpr DWORD readable_section_flags =
		    IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE;

		constexpr std::array signature_registry{
		    signature_spec{"lua_call", "E8 ? ? ? ? FF C3 3B DF 7E", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_checkstack", "E8 ? ? ? ? 85 C0 75 ? 48 8D 15 ? ? ? ? 48 8B CF E8 ? ? ? ? 80 7B", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_concat", "E8 ? ? ? ? 2B DF 01 5E 08 48 8B 5C 24 30", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_createtable", "E8 ? ? ? ? 48 8B 5F ? 48 8B CF 48 2B 5F", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_error", "E8 ? ? ? ? 41 83 C8 ? 33 D2", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_gc", "E8 ? ? ? ? 41 83 3C 9E", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_getfenv", "E8 ? ? ? ? 41 8B C3 48 83 C4", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_getfield", "E8 ? ? ? ? 44 8D 7D", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_getmetatable", "E8 ? ? ? ? 85 C0 75 ? 33 D2 44 8D 40", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_gettable", "E8 ? ? ? ? 41 83 CB FF 48 8B CB 41 8B D3 E8", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_insert", "E8 ? ? ? ? 8B 56 ? 44 8B CF", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_pcall", "E8 ? ? ? ? 48 8B 4E ? 8B D7 8B D8", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"luaV_execute", "48 8B C4 48 89 58 ? 89 50 ? 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 ? ? ? ? 48 81 EC ? ? ? ? 0F 29 70 ? 8B F2", resolution_kind::direct, target_region::executable},
		    signature_spec{"lua_load", "E8 ? ? ? ? 48 83 CE ? 85 C0", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CScriptableBase_Init", "E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 39 3D", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"lua_setmetatable", "40 53 48 83 EC ? 48 8B DA E8 ? ? ? ? 48 8B D3 E8 ? ? ? ? 48 8B 0D", resolution_kind::direct, target_region::executable},
		    signature_spec{"lua_custom_alloc", "E8 ? ? ? ? 33 FF 48 8B D8 48 85 C0 0F 84 ? ? ? ? 48 8D 90", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"game_pushref", "E8 ? ? ? ? 48 8B CB E8 ? ? ? ? 8D 4E ? 8D 56", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"game index2adr", "85 D2 7F ? B8", resolution_kind::direct, target_region::executable},
		    signature_spec{"game luaH_new", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 8B F0 8B DA 45 33 C0", resolution_kind::direct, target_region::executable},
		    signature_spec{"C3DEngine_UnRegisterEntityImpl", "E8 ? ? ? ? 49 8D 8E ? ? ? ? 48 8B D7 4C 8D 5C 24", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CXConsole_ctor", "E8 ? ? ? ? 48 8B C8 EB 03 49 8B CF 48 8B 46 20 48 89 88 A8 00 00 00", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"gEnv pConsole pointer", "48 8B 0D ? ? ? ? 48 8D 94 24 C0 00 00 00 45 33 C9 41 B0 01 48 8B 01 FF 90 18 01 00 00", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CEntity vtable", "48 8D 05 ? ? ? ? 48 89 01 4C 89 A1 A0 00 00 00", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CStatObj vtable", "48 8D 05 ? ? ? ? 48 89 77 58 48 89 07", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CGeomCacheRenderNode vtable", "48 8D 05 ? ? ? ? 33 ED 48 89 01 4C 8D 71 50 48 8D 05", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CVegetation_ctor", "E8 ? ? ? ? 48 8B D0 F2 0F 10 43", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CMergedMeshRenderNode_ctor", "B9 E0 02 00 00 E8", resolution_kind::second_relative_call, target_region::executable},
		    signature_spec{"CBrush vtable", "48 8D 05 ? ? ? ? 83 A1 B0 00 00 00 F8", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CPhysicalEntity vtable", "48 8D 05 ? ? ? ? 48 8B 5C 24 30 48 8D 0D ? ? ? ? 48 89 06", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"C3DEngine vtable", "48 8D 0D ? ? ? ? 48 89 0E 48 8D 4E 10", resolution_kind::rip_relative_memory, target_region::readable},
		    signature_spec{"CryEngine attachVariable", "E8 ? ? ? ? 4C 8D 0D ? ? ? ? 4C 8D 05 ? ? ? ? 48 8B CB", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"LoadCommonData", "E8 ? ? ? ? F2 41 0F 10 46", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CPhysicalEntity_ctor", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 49 8B D8 48 8B FA 48 8B F1 E8 ? ? ? ? 33 D2 81 4E 7C FF FF 00 00", resolution_kind::direct, target_region::executable},
		    signature_spec{"CryEngine REGISTER_CVAR", "E8 ? ? ? ? 48 8B 0D ? ? ? ? 48 8D 1D ? ? ? ? 48 85 C9 74 ? 48 8B 01 4C 8D 05", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"Initializing Direct3D", "E8 ? ? ? ? 48 83 3D ? ? ? ? ? 75 ? 48 8D 0D", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"PostInputEvent", "48 89 5C 24 ? 57 48 83 EC ? 48 8B DA 48 8B F9 45 84 C0 75 ? 44 38 81 ? ? ? ? 0F 84 ? ? ? ? 83 7A", resolution_kind::direct, target_region::executable},
		    signature_spec{"C_UIMenu_ShowPage", "40 53 48 83 EC 20 48 8B D9 48 8D 15 ? ? ? ? 48 8D 4C 24 38 E8 ? ? ? ? 48 8D 54 24 38 48 8B CB E8 ? ? ? ? 48 8D 4C 24 38 E8 ? ? ? ? 48 83 C4 20 5B C3 CC CC CC CC CC CC CC CC CC 48 89 54 24 10 55 48 8B EC", resolution_kind::direct, target_region::executable},
		    signature_spec{"C_SkipTimeCutscene_Play", "48 89 5C 24 ? 57 48 83 EC ? 48 8B D9 E8 ? ? ? ? E8 ? ? ? ? 0F 10 05 ? ? ? ? 44 8B 43 64", resolution_kind::direct, target_region::executable},
		    signature_spec{"C_FastTravel_StartTravel", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B F9 E8 ? ? ? ? 40 8A F0 84 C0 0F 85 ? ? ? ? 48 8B 4F 58", resolution_kind::direct, target_region::executable},
		    signature_spec{"CLog_LogV", "40 53 56 57 41 54 41 55 41 56 41 57 B8 ? ? ? ? E8 ? ? ? ? 48 2B E0 0F 29 B4 24 ? ? ? ? 48 8B 05", resolution_kind::direct, target_region::executable},
		    signature_spec{"XmlParserImp_ParseFile", "40 53 56 57 41 56 41 57 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 84 24 ? ? ? ? 41 8A D9", resolution_kind::direct, target_region::executable},
		    signature_spec{"XML_Parse", "E8 ? ? ? ? 48 8D 4D ? 83 F8", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CCryFile_Open", "E8 ? ? ? ? B3 ? 84 C0 75", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CCryPak_ctor", "E8 ? ? ? ? 48 8B F8 8A 83", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"wh_db_table_patched", "E8 ? ? ? ? E9 ? ? ? ? 8B 52 ? 44 8B 79", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"wh_db_table_patch_find_line", "E8 ? ? ? ? 83 F8 ? 75 ? 48 8B CB E8 ? ? ? ? 45 33 C9", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"XmlParserReadOnly_Read_caller", "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 8A E8 48 8B DA 48 8B F1 E8 ? ? ? ? B9 18 00 00 00 4C 8B 88", resolution_kind::direct, target_region::executable},
		    signature_spec{"CEntitySystem_ctor", "E8 ? ? ? ? 48 8B D8 48 8B D7 48 89 1D", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CEntity_ctor", "E8 ? ? ? ? 48 8B D8 EB ? 48 8B DF 41 8B C7", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CBrush_ctor", "E8 ? ? ? ? 48 8B D8 4C 8B 8C 24", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CPlayerStateMovement_Ledge_callback", "E8 ? ? ? ? 84 C0 0F 84 ? ? ? ? 4C 8B 45 ? 4C 3B C7", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CMovableBrush_ctor", "E8 ? ? ? ? EB ? 45 33 C0 F7 43", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"StepDataSBrush", "E8 ? ? ? ? 84 C0 74 ? 48 8B 45 ? 48 89 18", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CTerrain_Load", "E8 ? ? ? ? 84 C0 75 ? 48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B 01 41 8B D5", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CBrush_SetStatObj", "E8 ? ? ? ? 8B 57 5C 49 8B CF E8 ? ? ? ? 48 8B D0 48 8B CB E8", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CStatObj_ctor", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 33 F6 48 8D 05 ? ? ? ? 48 89 01 0F 57 C0", resolution_kind::direct, target_region::executable},
		    signature_spec{"CGeomCacheRenderNode_ctor", "E8 ? ? ? ? E9 ? ? ? ? B9 ? ? ? ? E8 ? ? ? ? 48 8B C8 33 C0 48 85 C9 0F 84 ? ? ? ? E8 ? ? ? ? EB", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"C_PlayerStateMovement_ctor", "48 89 5C 24 ? 48 89 74 24 ? 48 89 7C 24 ? 55 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC ? 48 8B F1 E8", resolution_kind::direct, target_region::executable},
		    signature_spec{"C_Player_ctor", "48 89 5C 24 ? 48 89 74 24 ? 55 57 41 56 48 8B EC 48 83 EC ? 48 8B D9 E8 ? ? ? ? 41 83 CE FF 48 8D 05", resolution_kind::direct, target_region::executable},
		    signature_spec{"C3DEngine_ctor", "E8 ? ? ? ? 48 8B 5C 24 ? 48 89 47 ? B0", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CD3D9Renderer_UnProjectFromScreen", "E8 ? ? ? ? F3 44 0F 10 0D ? ? ? ? 48 8D 45 ? 48 89 44 24 ? 41 0F 28 D9", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CD3D9Renderer_ProjectToScreen", "48 83 EC ? 48 8B 0D ? ? ? ? 0F 29 74 24 ? 0F 28 F2 0F 29 7C 24", resolution_kind::direct, target_region::executable},
		    signature_spec{"game_lua_call_internal", "E8 ? ? ? ? 48 FF 03 48 81 C4", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CryScriptSystem_Init", "E8 ? ? ? ? 84 C0 74 ? E8 ? ? ? ? 41 38 BE", resolution_kind::relative_call, target_region::executable},
		    signature_spec{"CScriptSystem_Update", "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 48 8B 3D ? ? ? ? 48 8B F1 33 D2", resolution_kind::direct, target_region::executable},
		    signature_spec{"CScriptSystem_ExecuteBuffer", "48 8B C4 48 89 58 ? 48 89 68 ? 48 89 70 ? 48 89 78 ? 41 56 48 83 EC ? 48 8B F9 48 89 50", resolution_kind::direct, target_region::executable},
		};

		static_assert(signature_registry.size() == expected_signature_count);

		struct pattern_byte
		{
			uint8_t value{};
			bool wildcard{};
		};

		bool checked_range(uint64_t start, size_t length, uint64_t limit)
		{
			return start <= limit && length <= limit - start;
		}

		bool parse_pattern(std::string_view text, std::vector<pattern_byte> &output)
		{
			output.clear();
			for (size_t cursor = 0; cursor < text.size();)
			{
				while (cursor < text.size() && text[cursor] == ' ')
				{
					++cursor;
				}
				if (cursor == text.size())
				{
					break;
				}
				const auto end = text.find(' ', cursor);
				const auto token = text.substr(
				    cursor,
				    end == std::string_view::npos ? text.size() - cursor : end - cursor);
				if (token == "?" || token == "??")
				{
					output.push_back({0, true});
				}
				else
				{
					if (token.size() != 2)
					{
						return false;
					}
					unsigned int value{};
					const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value, 16);
					if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || value > 0xFF)
					{
						return false;
					}
					output.push_back({static_cast<uint8_t>(value), false});
				}
				cursor = end == std::string_view::npos ? text.size() : end + 1;
			}
			return !output.empty() && std::ranges::any_of(output, [](const auto &byte)
			{
				return !byte.wildcard;
			});
		}

		std::vector<uint64_t> find_matches(const pe_image &image, std::span<const pattern_byte> pattern)
		{
			std::vector<uint64_t> matches;
			size_t anchor_offset{};
			size_t anchor_size{};
			for (size_t index = 0; index < pattern.size();)
			{
				if (pattern[index].wildcard)
				{
					++index;
					continue;
				}
				size_t end = index;
				while (end < pattern.size() && !pattern[end].wildcard)
				{
					++end;
				}
				if (end - index > anchor_size)
				{
					anchor_offset = index;
					anchor_size = end - index;
				}
				index = end;
			}
			std::vector<uint8_t> anchor;
			anchor.reserve(anchor_size);
			for (size_t index = 0; index < anchor_size; ++index)
			{
				anchor.push_back(pattern[anchor_offset + index].value);
			}

			for (const auto &section : image.sections())
			{
				if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) || section.size < pattern.size())
				{
					continue;
				}
				const auto *bytes = image.data(section.rva);
				const auto *end = bytes + section.size;
				const auto *cursor = bytes + anchor_offset;
				while (cursor + anchor_size <= end)
				{
					const auto *anchor_match =
					    std::search(cursor, end, anchor.begin(), anchor.end());
					if (anchor_match == end)
					{
						break;
					}
					if (anchor_match < bytes + anchor_offset)
					{
						cursor = anchor_match + 1;
						continue;
					}
					const auto *candidate = anchor_match - anchor_offset;
					if (candidate + pattern.size() > end)
					{
						break;
					}
					bool match = true;
					for (size_t index = 0; index < pattern.size(); ++index)
					{
						if (!pattern[index].wildcard && candidate[index] != pattern[index].value)
						{
							match = false;
							break;
						}
					}
					if (match)
					{
						matches.push_back(
						    section.rva + static_cast<uint64_t>(candidate - bytes));
					}
					cursor = anchor_match + 1;
				}
			}
			return matches;
		}

		bool valid_target(const pe_image &image, uint64_t rva, target_region region)
		{
			return region == target_region::executable ? image.is_executable(rva) : image.is_readable(rva);
		}

		std::optional<uint64_t> resolve_nth_call(
		    const pe_image &image,
		    uint64_t start_rva,
		    size_t ordinal,
		    size_t max_bytes,
		    std::string &error)
		{
			ZydisDecoder decoder;
			if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
			{
				error = "Zydis decoder initialization failed";
				return std::nullopt;
			}
			size_t offset{};
			size_t calls{};
			while (offset < max_bytes && image.contains(start_rva + offset))
			{
				ZydisDecodedInstruction instruction;
				ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
				const auto available = std::min(max_bytes - offset, image.size() - static_cast<size_t>(start_rva + offset));
				if (ZYAN_FAILED(ZydisDecoderDecodeFull(
				        &decoder,
				        image.data(start_rva + offset),
				        available,
				        &instruction,
				        operands)))
				{
					error = "instruction stream is truncated or invalid";
					return std::nullopt;
				}
				if (instruction.mnemonic == ZYDIS_MNEMONIC_CALL)
				{
					if (calls++ == ordinal)
					{
						return resolve_relative_call(image, start_rva + offset, error);
					}
				}
				offset += instruction.length;
			}
			error = "expected relative CALL was not found in the bounded instruction window";
			return std::nullopt;
		}

		std::optional<uint64_t> resolve_constructor_vtable(
		    const pe_image &image,
		    uint64_t constructor_rva,
		    size_t max_bytes,
		    std::string &error)
		{
			ZydisDecoder decoder;
			if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
			{
				error = "Zydis decoder initialization failed";
				return std::nullopt;
			}

			std::vector<uint64_t> candidates;
			size_t offset{};
			while (offset < max_bytes && image.contains(constructor_rva + offset))
			{
				ZydisDecodedInstruction instruction;
				ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
				const auto available = std::min(max_bytes - offset, image.size() - static_cast<size_t>(constructor_rva + offset));
				if (ZYAN_FAILED(ZydisDecoderDecodeFull(
				        &decoder,
				        image.data(constructor_rva + offset),
				        available,
				        &instruction,
				        operands)))
				{
					if (candidates.empty())
					{
						error = "constructor instruction stream is truncated or invalid";
					}
					break;
				}

				if (instruction.mnemonic == ZYDIS_MNEMONIC_LEA && instruction.operand_count_visible >= 2
				    && operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER
				    && operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY
				    && operands[1].mem.base == ZYDIS_REGISTER_RIP)
				{
					ZyanU64 absolute{};
					if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
					        &instruction,
					        &operands[1],
					        image.rva_to_virtual_address(constructor_rva + offset),
					        &absolute)))
					{
						const auto target_rva = image.virtual_address_to_rva(absolute);
						const auto next_rva = constructor_rva + offset + instruction.length;
						if (target_rva && image.is_readable(*target_rva, sizeof(uint64_t))
						    && image.contains(next_rva))
						{
							bool stored_to_object{};
							size_t lookahead{};
							for (size_t decoded = 0; decoded < 3 && offset + instruction.length + lookahead < max_bytes; ++decoded)
							{
								const auto candidate_rva = next_rva + lookahead;
								ZydisDecodedInstruction next_instruction;
								ZydisDecodedOperand next_operands[ZYDIS_MAX_OPERAND_COUNT];
								const auto next_available = std::min(
								    max_bytes - (offset + instruction.length + lookahead),
								    image.size() - static_cast<size_t>(candidate_rva));
								if (!next_available
								    || ZYAN_FAILED(ZydisDecoderDecodeFull(
								        &decoder,
								        image.data(candidate_rva),
								        next_available,
								        &next_instruction,
								        next_operands)))
								{
									break;
								}
								if (next_instruction.mnemonic == ZYDIS_MNEMONIC_MOV
								    && next_instruction.operand_count_visible >= 2
								    && next_operands[0].type == ZYDIS_OPERAND_TYPE_MEMORY
								    && next_operands[0].mem.disp.value == 0
								    && next_operands[1].type == ZYDIS_OPERAND_TYPE_REGISTER
								    && next_operands[1].reg.value == operands[0].reg.value)
								{
									stored_to_object = true;
									break;
								}
								lookahead += next_instruction.length;
							}
							if (stored_to_object)
							{
								uint64_t first_entry{};
								std::memcpy(&first_entry, image.data(*target_rva), sizeof(first_entry));
								const auto entry_rva = image.virtual_address_to_rva(first_entry);
								if (entry_rva && image.is_executable(*entry_rva))
								{
									candidates.push_back(*target_rva);
								}
							}
						}
					}
				}

				offset += instruction.length;
				if (instruction.mnemonic == ZYDIS_MNEMONIC_RET)
				{
					break;
				}
			}

			std::ranges::sort(candidates);
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			if (candidates.size() != 1)
			{
				error = candidates.empty() ? "no constructor VTable assignment found"
				                           : "multiple constructor VTable assignments found";
				return std::nullopt;
			}
			return candidates.front();
		}

		std::optional<uint64_t> resolve_unique_rip_reference(
		    const pe_image &image,
		    uint64_t function_rva,
		    size_t max_bytes,
		    std::string &error)
		{
			ZydisDecoder decoder;
			if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
			{
				error = "Zydis decoder initialization failed";
				return std::nullopt;
			}
			std::vector<uint64_t> candidates;
			size_t offset{};
			while (offset < max_bytes && image.contains(function_rva + offset))
			{
				ZydisDecodedInstruction instruction;
				ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
				const auto available = std::min(max_bytes - offset, image.size() - static_cast<size_t>(function_rva + offset));
				if (ZYAN_FAILED(ZydisDecoderDecodeFull(
				        &decoder,
				        image.data(function_rva + offset),
				        available,
				        &instruction,
				        operands)))
				{
					if (candidates.empty())
					{
						error = "function instruction stream is truncated or invalid";
					}
					break;
				}
				if (instruction.mnemonic == ZYDIS_MNEMONIC_MOV)
				{
					for (uint8_t index = 0; index < instruction.operand_count_visible; ++index)
					{
						if (operands[index].type != ZYDIS_OPERAND_TYPE_MEMORY
						    || operands[index].mem.base != ZYDIS_REGISTER_RIP)
						{
							continue;
						}
						ZyanU64 absolute{};
						if (ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(
						        &instruction,
						        &operands[index],
						        image.rva_to_virtual_address(function_rva + offset),
						        &absolute)))
						{
							const auto target_rva = image.virtual_address_to_rva(absolute);
							if (target_rva && image.is_readable(*target_rva, sizeof(uint64_t)))
							{
								candidates.push_back(*target_rva);
							}
						}
					}
				}
				offset += instruction.length;
				if (instruction.mnemonic == ZYDIS_MNEMONIC_RET)
				{
					break;
				}
			}
			std::ranges::sort(candidates);
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			if (candidates.size() != 1)
			{
				error = candidates.empty() ? "no RIP-relative data reference found"
				                           : "multiple RIP-relative data references found";
				return std::nullopt;
			}
			return candidates.front();
		}

		void add_derived(
		    resolution_report &report,
		    std::string_view name,
		    std::optional<uint64_t> address,
		    std::string error)
		{
			++report.derived_requested;
			if (address)
			{
				++report.derived_resolved;
				report.derived_addresses.push_back({std::string(name), *address, *address});
				return;
			}
			report.diagnostics.push_back({
			    std::string(name),
			    error.starts_with("multiple") ? failure_kind::ambiguous_derivation : failure_kind::invalid_target,
			    0,
			    std::move(error),
			});
		}

		void validate_vtable_entry(
		    const pe_image &image,
		    resolution_report &report,
		    std::string_view name,
		    uint64_t vtable_rva,
		    size_t index)
		{
			++report.derived_requested;
			const auto entry_rva = vtable_rva + index * sizeof(uint64_t);
			if (!image.contains(entry_rva, sizeof(uint64_t)))
			{
				report.diagnostics.push_back({
				    std::string(name),
				    failure_kind::invalid_target,
				    0,
				    "VTable entry is outside the PE image",
				});
				return;
			}
			uint64_t entry{};
			std::memcpy(&entry, image.data(entry_rva), sizeof(entry));
			const auto function_rva = image.virtual_address_to_rva(entry);
			if (!function_rva || !image.is_executable(*function_rva))
			{
				report.diagnostics.push_back({
				    std::string(name),
				    failure_kind::invalid_target,
				    0,
				    "VTable entry does not point into an executable PE section",
				});
				return;
			}
			++report.derived_resolved;
			report.derived_addresses.push_back({std::string(name), entry_rva, *function_rva});
		}
	}

	std::optional<pe_image> pe_image::from_loaded_module(HMODULE module, std::string &error)
	{
		if (!module)
		{
			error = "module is not loaded";
			return std::nullopt;
		}
		const auto *base = reinterpret_cast<const uint8_t *>(module);
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
		if (dos->e_magic != IMAGE_DOS_SIGNATURE)
		{
			error = "loaded module has no valid DOS header";
			return std::nullopt;
		}
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			error = "loaded module is not a 64-bit PE image";
			return std::nullopt;
		}

		pe_image image;
		image.m_image = base;
		image.m_size = nt->OptionalHeader.SizeOfImage;
		image.m_timestamp = nt->FileHeader.TimeDateStamp;
		image.m_preferred_base = nt->OptionalHeader.ImageBase;
		image.m_virtual_base = reinterpret_cast<uint64_t>(base);
		const auto *section = IMAGE_FIRST_SECTION(nt);
		for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
		{
			const auto size = std::min<uint64_t>(
			    std::max(section[index].Misc.VirtualSize, section[index].SizeOfRawData),
			    image.m_size > section[index].VirtualAddress ? image.m_size - section[index].VirtualAddress : 0);
			if (!size)
			{
				continue;
			}
			image.m_sections.push_back({
			    std::string(reinterpret_cast<const char *>(section[index].Name),
			                strnlen(reinterpret_cast<const char *>(section[index].Name), IMAGE_SIZEOF_SHORT_NAME)),
			    section[index].VirtualAddress,
			    static_cast<uint32_t>(size),
			    section[index].Characteristics,
			});
		}
		return image;
	}

	std::optional<pe_image> pe_image::from_file(const std::filesystem::path &path, std::string &error)
	{
		std::ifstream stream(path, std::ios::binary | std::ios::ate);
		if (!stream)
		{
			error = "could not open input file";
			return std::nullopt;
		}
		const auto length = stream.tellg();
		if (length <= 0)
		{
			error = "input file is empty";
			return std::nullopt;
		}
		pe_image image;
		image.m_file_storage.resize(static_cast<size_t>(length));
		stream.seekg(0);
		if (!stream.read(reinterpret_cast<char *>(image.m_file_storage.data()), length))
		{
			error = "could not read input file";
			return std::nullopt;
		}
		if (image.m_file_storage.size() < sizeof(IMAGE_DOS_HEADER))
		{
			error = "input is too small for a PE image";
			return std::nullopt;
		}
		const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(image.m_file_storage.data());
		if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0
		    || !checked_range(static_cast<uint64_t>(dos->e_lfanew), sizeof(IMAGE_NT_HEADERS64), image.m_file_storage.size()))
		{
			error = "input has no valid DOS/PE header";
			return std::nullopt;
		}
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(
		    image.m_file_storage.data() + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
		{
			error = "input is not a 64-bit PE image";
			return std::nullopt;
		}
		const auto section_table_offset =
		    static_cast<uint64_t>(dos->e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader)
		    + nt->FileHeader.SizeOfOptionalHeader;
		if (!checked_range(
		        section_table_offset,
		        static_cast<size_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER),
		        image.m_file_storage.size()))
		{
			error = "truncated PE section table";
			return std::nullopt;
		}

		image.m_size = nt->OptionalHeader.SizeOfImage;
		image.m_timestamp = nt->FileHeader.TimeDateStamp;
		image.m_preferred_base = nt->OptionalHeader.ImageBase;
		image.m_virtual_base = image.m_preferred_base;
		image.m_file_image = true;
		image.m_virtual_storage.assign(image.m_size, 0);
		const auto headers_size = std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, image.m_file_storage.size());
		std::memcpy(image.m_virtual_storage.data(), image.m_file_storage.data(), headers_size);

		const auto *sections = reinterpret_cast<const IMAGE_SECTION_HEADER *>(
		    image.m_file_storage.data() + section_table_offset);
		for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index)
		{
			const auto virtual_size = std::max(sections[index].Misc.VirtualSize, sections[index].SizeOfRawData);
			if (!virtual_size || sections[index].VirtualAddress >= image.m_size)
			{
				continue;
			}
			const auto mapped_size = static_cast<uint32_t>(
			    std::min<uint64_t>(virtual_size, image.m_size - sections[index].VirtualAddress));
			if (sections[index].SizeOfRawData)
			{
				if (!checked_range(
				        sections[index].PointerToRawData,
				        sections[index].SizeOfRawData,
				        image.m_file_storage.size()))
				{
					error = "PE section lies outside the input file";
					return std::nullopt;
				}
				const auto copy_size = std::min<uint32_t>(mapped_size, sections[index].SizeOfRawData);
				std::memcpy(
				    image.m_virtual_storage.data() + sections[index].VirtualAddress,
				    image.m_file_storage.data() + sections[index].PointerToRawData,
				    copy_size);
			}
			image.m_sections.push_back({
			    std::string(reinterpret_cast<const char *>(sections[index].Name),
			                strnlen(reinterpret_cast<const char *>(sections[index].Name), IMAGE_SIZEOF_SHORT_NAME)),
			    sections[index].VirtualAddress,
			    mapped_size,
			    sections[index].Characteristics,
			});
		}
		image.m_image = image.m_virtual_storage.data();
		return image;
	}

	pe_image pe_image::from_test_image(
	    std::vector<uint8_t> bytes,
	    std::vector<section_view> sections,
	    uint64_t virtual_base)
	{
		pe_image image;
		image.m_virtual_storage = std::move(bytes);
		image.m_image = image.m_virtual_storage.data();
		image.m_size = image.m_virtual_storage.size();
		image.m_preferred_base = virtual_base;
		image.m_virtual_base = virtual_base;
		image.m_file_image = true;
		image.m_sections = std::move(sections);
		return image;
	}

	const uint8_t *pe_image::data(uint64_t rva) const
	{
		return contains(rva, 0) ? m_image + rva : nullptr;
	}

	size_t pe_image::size() const
	{
		return m_size;
	}

	uint32_t pe_image::timestamp() const
	{
		return m_timestamp;
	}

	uint64_t pe_image::preferred_base() const
	{
		return m_preferred_base;
	}

	uint64_t pe_image::virtual_base() const
	{
		return m_virtual_base;
	}

	bool pe_image::is_file_image() const
	{
		return m_file_image;
	}

	bool pe_image::contains(uint64_t rva, size_t length) const
	{
		return rva <= m_size && length <= m_size - rva;
	}

	bool pe_image::is_executable(uint64_t rva, size_t length) const
	{
		return std::ranges::any_of(m_sections, [&](const auto &section)
		{
			return (section.characteristics & IMAGE_SCN_MEM_EXECUTE)
			    && rva >= section.rva
			    && checked_range(rva - section.rva, length, section.size);
		});
	}

	bool pe_image::is_readable(uint64_t rva, size_t length) const
	{
		return std::ranges::any_of(m_sections, [&](const auto &section)
		{
			return (section.characteristics & readable_section_flags)
			    && rva >= section.rva
			    && checked_range(rva - section.rva, length, section.size);
		});
	}

	std::optional<uint64_t> pe_image::virtual_address_to_rva(uint64_t address) const
	{
		if (address >= m_virtual_base && address - m_virtual_base < m_size)
		{
			return address - m_virtual_base;
		}
		if (address >= m_preferred_base && address - m_preferred_base < m_size)
		{
			return address - m_preferred_base;
		}
		return std::nullopt;
	}

	uint64_t pe_image::rva_to_virtual_address(uint64_t rva) const
	{
		return m_virtual_base + rva;
	}

	const std::vector<section_view> &pe_image::sections() const
	{
		return m_sections;
	}

	const std::vector<uint8_t> &pe_image::file_bytes() const
	{
		return m_file_storage;
	}

	bool resolution_report::success() const
	{
		return diagnostics.empty() && signatures_requested == signatures_resolved
		    && derived_requested == derived_resolved;
	}

	std::optional<uint64_t> resolution_report::match(std::string_view name) const
	{
		const auto found = std::ranges::find(addresses, name, &resolved_address::name);
		return found == addresses.end() ? std::nullopt : std::optional(found->match_rva);
	}

	std::optional<uint64_t> resolution_report::target(std::string_view name) const
	{
		const auto found = std::ranges::find(addresses, name, &resolved_address::name);
		return found == addresses.end() ? std::nullopt : std::optional(found->target_rva);
	}

	std::optional<uint64_t> resolution_report::derived(std::string_view name) const
	{
		const auto found = std::ranges::find(derived_addresses, name, &resolved_address::name);
		return found == derived_addresses.end() ? std::nullopt : std::optional(found->target_rva);
	}

	std::span<const signature_spec> registry()
	{
		return signature_registry;
	}

	std::vector<uint64_t> scan_pattern(
	    const pe_image &image,
	    std::string_view pattern_text,
	    std::string &error)
	{
		std::vector<pattern_byte> pattern;
		if (!parse_pattern(pattern_text, pattern))
		{
			error = "signature contains an invalid token";
			return {};
		}
		return find_matches(image, pattern);
	}

	std::optional<uint64_t> resolve_relative_call(
	    const pe_image &image,
	    uint64_t instruction_rva,
	    std::string &error)
	{
		if (!image.contains(instruction_rva, ZYDIS_MAX_INSTRUCTION_LENGTH))
		{
			error = "relative CALL crosses the PE boundary";
			return std::nullopt;
		}
		ZydisDecoder decoder;
		ZydisDecodedInstruction instruction;
		ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
		if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))
		    || ZYAN_FAILED(ZydisDecoderDecodeFull(
		        &decoder,
		        image.data(instruction_rva),
		        ZYDIS_MAX_INSTRUCTION_LENGTH,
		        &instruction,
		        operands)))
		{
			error = "could not decode relative CALL";
			return std::nullopt;
		}
		if (instruction.mnemonic != ZYDIS_MNEMONIC_CALL || instruction.operand_count_visible != 1
		    || operands[0].type != ZYDIS_OPERAND_TYPE_IMMEDIATE || !operands[0].imm.is_relative)
		{
			error = "instruction is not a direct relative CALL";
			return std::nullopt;
		}
		ZyanU64 absolute{};
		if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(
		        &instruction,
		        &operands[0],
		        image.rva_to_virtual_address(instruction_rva),
		        &absolute)))
		{
			error = "could not calculate the relative CALL target";
			return std::nullopt;
		}
		const auto rva = image.virtual_address_to_rva(absolute);
		if (!rva)
		{
			error = "relative CALL target is outside the PE image";
		}
		return rva;
	}

	std::optional<uint64_t> resolve_rip_relative_memory(
	    const pe_image &image,
	    uint64_t instruction_rva,
	    std::string &error)
	{
		if (!image.contains(instruction_rva, ZYDIS_MAX_INSTRUCTION_LENGTH))
		{
			error = "RIP-relative instruction crosses the PE boundary";
			return std::nullopt;
		}
		ZydisDecoder decoder;
		ZydisDecodedInstruction instruction;
		ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
		if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64))
		    || ZYAN_FAILED(ZydisDecoderDecodeFull(
		        &decoder,
		        image.data(instruction_rva),
		        ZYDIS_MAX_INSTRUCTION_LENGTH,
		        &instruction,
		        operands)))
		{
			error = "could not decode RIP-relative instruction";
			return std::nullopt;
		}
		for (uint8_t index = 0; index < instruction.operand_count_visible; ++index)
		{
			if (operands[index].type != ZYDIS_OPERAND_TYPE_MEMORY
			    || operands[index].mem.base != ZYDIS_REGISTER_RIP)
			{
				continue;
			}
			ZyanU64 absolute{};
			if (ZYAN_FAILED(ZydisCalcAbsoluteAddress(
			        &instruction,
			        &operands[index],
			        image.rva_to_virtual_address(instruction_rva),
			        &absolute)))
			{
				error = "could not calculate RIP-relative target";
				return std::nullopt;
			}
			const auto rva = image.virtual_address_to_rva(absolute);
			if (!rva)
			{
				error = "RIP-relative target is outside the PE image";
			}
			return rva;
		}
		error = "instruction has no RIP-relative memory operand";
		return std::nullopt;
	}

	std::optional<uint64_t> resolve_constructor_vtable_assignment(
	    const pe_image &image,
	    uint64_t constructor_rva,
	    size_t max_bytes,
	    std::string &error)
	{
		return resolve_constructor_vtable(image, constructor_rva, max_bytes, error);
	}

	std::optional<uint64_t> resolve_unique_rip_data_reference(
	    const pe_image &image,
	    uint64_t function_rva,
	    size_t max_bytes,
	    std::string &error)
	{
		return resolve_unique_rip_reference(image, function_rva, max_bytes, error);
	}

	resolution_report resolve_all(const pe_image &image)
	{
		resolution_report report;
		report.signatures_requested = signature_registry.size();
		for (const auto &spec : signature_registry)
		{
			std::vector<pattern_byte> pattern;
			if (!parse_pattern(spec.pattern, pattern))
			{
				report.diagnostics.push_back({
				    std::string(spec.name),
				    failure_kind::invalid_pattern,
				    0,
				    "signature contains an invalid token",
				});
				continue;
			}
			const auto matches = find_matches(image, pattern);
			if (matches.size() != spec.expected_matches)
			{
				report.diagnostics.push_back({
				    std::string(spec.name),
				    matches.empty() ? failure_kind::missing : failure_kind::ambiguous,
				    matches.size(),
				    matches.empty() ? "signature was not found" : "signature did not resolve uniquely",
				});
				continue;
			}
			++report.signatures_resolved;

			++report.derived_requested;
			std::string error;
			std::optional<uint64_t> target;
			switch (spec.resolution)
			{
			case resolution_kind::direct:
				target = matches.front();
				break;
			case resolution_kind::relative_call:
				target = resolve_relative_call(image, matches.front(), error);
				break;
			case resolution_kind::second_relative_call:
				target = resolve_nth_call(image, matches.front(), 1, 0x30, error);
				break;
			case resolution_kind::rip_relative_memory:
				target = resolve_rip_relative_memory(image, matches.front(), error);
				break;
			}
			if (!target || !valid_target(image, *target, spec.target))
			{
				report.diagnostics.push_back({
				    std::string(spec.name) + " target",
				    failure_kind::invalid_target,
				    0,
				    error.empty() ? "resolved target is outside its expected PE section" : std::move(error),
				});
				continue;
			}
			++report.derived_resolved;
			report.addresses.push_back({std::string(spec.name), matches.front(), *target});
		}

		const auto derive_constructor = [&](std::string_view signature_name, std::string_view derived_name, size_t max_bytes)
		{
			std::string error;
			const auto ctor = report.target(signature_name);
			const auto address =
			    ctor ? resolve_constructor_vtable(image, *ctor, max_bytes, error) : std::nullopt;
			add_derived(
			    report,
			    derived_name,
			    address,
			    ctor ? std::move(error) : "constructor signature target is unavailable");
		};
		derive_constructor("CXConsole_ctor", "CXConsole vtable", 0x80);
		derive_constructor("CEntitySystem_ctor", "CEntitySystem vtable", 0x180);
		derive_constructor("CVegetation_ctor", "CVegetation vtable", 0x80);
		derive_constructor("CMergedMeshRenderNode_ctor", "CMergedMeshRenderNode vtable", 0x120);

		const auto derive_g_env = [&]
		{
			std::string error;
			const auto function = report.target("CPlayerStateMovement_Ledge_callback");
			const auto address =
			    function ? resolve_unique_rip_reference(image, *function, 0x90, error) : std::nullopt;
			add_derived(
			    report,
			    "gEnv pGame pointer",
			    address,
			    function ? std::move(error) : "source function is unavailable");
		};
		derive_g_env();

		const auto validate_named_vtable = [&](std::string_view source, std::string_view entry, size_t index, bool derived)
		{
			const auto table = derived ? report.derived(source) : report.target(source);
			if (!table)
			{
				++report.derived_requested;
				report.diagnostics.push_back({
				    std::string(entry),
				    failure_kind::invalid_target,
				    0,
				    "VTable address is unavailable",
				});
				return;
			}
			validate_vtable_entry(image, report, entry, *table, index);
		};
		validate_named_vtable("CXConsole vtable", "CXConsole vtable[35]", 35, true);
		validate_named_vtable("CEntity vtable", "CEntity vtable[0]", 0, false);
		validate_named_vtable("CEntity vtable", "CEntity::SetFlags", 5, false);
		validate_named_vtable("CEntity vtable", "CEntity::GetFlags", 6, false);
		validate_named_vtable("CEntity vtable", "CEntity::SetWorldTM", 31, false);
		validate_named_vtable("CEntity vtable", "CEntity::Activate", 52, false);
		validate_named_vtable("CEntity vtable", "CEntity::IsActive", 53, false);
		validate_named_vtable("CEntity vtable", "CEntity::Hide", 63, false);
		validate_named_vtable("CEntity vtable", "CEntity::IsHidden", 64, false);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::SpawnEntity",
		    12,
		    true);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::RemoveEntity",
		    19,
		    true);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::GetEntityIterator",
		    22,
		    true);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::AddSink",
		    29,
		    true);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::RemoveSink",
		    30,
		    true);
		validate_named_vtable(
		    "CEntitySystem vtable",
		    "CEntitySystem::GetEntityLayerData",
		    71,
		    true);
		validate_named_vtable("CStatObj vtable", "CStatObj vtable[0]", 0, false);
		validate_named_vtable("CGeomCacheRenderNode vtable", "CGeomCacheRenderNode vtable[0]", 0, false);
		validate_named_vtable("CVegetation vtable", "CVegetation vtable[0]", 0, true);
		validate_named_vtable("CMergedMeshRenderNode vtable", "CMergedMeshRenderNode vtable[0]", 0, true);
		validate_named_vtable("CBrush vtable", "CBrush vtable[0]", 0, false);
		validate_named_vtable("CPhysicalEntity vtable", "CPhysicalEntity vtable[0]", 0, false);
		validate_named_vtable("C3DEngine vtable", "C3DEngine vtable[38]", 38, false);

		const auto validate_native_anchor =
		    [&](std::string_view name,
		        uint64_t rva,
		        std::initializer_list<uint8_t> expected)
		{
			++report.derived_requested;
			if (!image.is_executable(rva, expected.size())
			    || !std::equal(
			        expected.begin(),
			        expected.end(),
			        image.data(rva)))
			{
				report.diagnostics.push_back({
				    std::string(name),
				    failure_kind::invalid_target,
				    0,
				    "native multiplayer ABI anchor bytes do not match",
				});
				return;
			}
			++report.derived_resolved;
			report.derived_addresses.push_back(
			    {std::string(name), rva, rva});
		};

		// These fixed-RVA anchors are intentionally audited only after the exact
		// WHGame hash gate in the audit executable. Together with the vtable
		// checks above they pin every native join call chain introduced by the
		// KCSE runtime.
		validate_native_anchor(
		    "IEntity::SetWorldTM ABI",
		    0x7F0DF4,
		    {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08});
		validate_native_anchor(
		    "CEntity::ResolvePhysicsProxy ABI",
		    0x3CE9A8,
		    {0x48, 0x89, 0x5C, 0x24, 0x10, 0x55, 0x56, 0x57,
		        0x41, 0x56, 0x41, 0x57});
		validate_native_anchor(
		    "CCryAction::EndGameContext ABI",
		    0xB6E3FC,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74});
		validate_native_anchor(
		    "IActorSystem::CreateActor ABI",
		    0xB86120,
		    {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08});
		validate_native_anchor(
		    "C_Soul::SetSharedSoulGuid ABI",
		    0x3F124C,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C});
		validate_native_anchor(
		    "C_SoulList::ApplySharedSoul ABI",
		    0x3F4578,
		    {0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57});
		validate_native_anchor(
		    "C_InventoryBase::BuildItemInitParams ABI",
		    0x4533E4,
		    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20});
		validate_native_anchor(
		    "C_InventoryBase::InsertCreatedItem ABI",
		    0x465FC0,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74});
		validate_native_anchor(
		    "C_Item::SetInstanceGuid ABI",
		    0x467A6C,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74});
		validate_native_anchor(
		    "C_Soul absolute RPG setter ABI",
		    0x469BF0,
		    {0x8B, 0xC2, 0x48, 0x8D, 0x0C, 0xC1});
		validate_native_anchor(
		    "C_Human::DrawWeapon ABI",
		    0x2AA24A0,
		    {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20});
		validate_native_anchor(
		    "C_Human::HolsterWeapon ABI",
		    0x8EE994,
		    {0x4C, 0x8B, 0xDC, 0x53, 0x48, 0x83, 0xEC, 0x60});
		validate_native_anchor(
		    "C_Human::IsWeaponDrawn ABI",
		    0x8C69B4,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C});
		validate_native_anchor(
		    "IEntitySystem::AddSink ABI",
		    0xDC006C,
		    {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08});
		validate_native_anchor(
		    "IEntitySystem::RemoveSink ABI",
		    0xDA68BC,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C});
		validate_native_anchor(
		    "IEntitySystem::InitEntity sink dispatch ABI",
		    0x5CB0F8,
		    {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74});

		++report.derived_requested;
		constexpr uint64_t cry_action_vtable = 0x40472D0;
		constexpr size_t end_game_context_slot = 52;
		uint64_t end_game_context_target{};
		if (image.is_readable(
		        cry_action_vtable
		            + end_game_context_slot * sizeof(uint64_t),
		        sizeof(uint64_t)))
		{
			std::memcpy(
			    &end_game_context_target,
			    image.data(
			        cry_action_vtable
			        + end_game_context_slot * sizeof(uint64_t)),
			    sizeof(end_game_context_target));
		}
		const auto end_game_context_rva =
		    image.virtual_address_to_rva(end_game_context_target);
		if (end_game_context_rva && *end_game_context_rva == 0xB6E3FC)
		{
			++report.derived_resolved;
			report.derived_addresses.push_back({
			    "CCryAction::EndGameContext vtable[52]",
			    cry_action_vtable,
			    *end_game_context_rva});
		}
		else
		{
			report.diagnostics.push_back({
			    "CCryAction::EndGameContext vtable[52]",
			    failure_kind::invalid_target,
			    0,
			    "CCryAction slot 52 does not target the audited unload routine",
			});
		}
		return report;
	}

	std::string failure_kind_name(failure_kind kind)
	{
		switch (kind)
		{
		case failure_kind::missing: return "missing";
		case failure_kind::ambiguous: return "ambiguous";
		case failure_kind::invalid_pattern: return "invalid pattern";
		case failure_kind::decode_error: return "decode error";
		case failure_kind::invalid_target: return "invalid target";
		case failure_kind::ambiguous_derivation: return "ambiguous derivation";
		}
		return "unknown";
	}
}
