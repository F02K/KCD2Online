#include "kcse/native_keybinds.hpp"

#include <atomic>
#include <crysystem/CCryAction.h>
#include <crysystem/SSystemGlobalEnvironment.h>
#include <cstring>
#include <KCSE/KCSEAPI.h>
#include <Offsets/vtables/IActionListener.h>
#include <Offsets/vtables/IActionMap.h>
#include <Offsets/vtables/IActionMapManager.h>
#include <Offsets/vtables/ISystem.h>
#include <Offsets/vtables/IXmlNode.h>
#include <playermodule/C_Keybinds.h>
#include <REL/Relocation.h>
#include <string_view>

namespace kcd2o::kcse::native_keybinds
{
	namespace
	{
		using Offsets::IXmlNode;
		using parse_keybinds = void (*)(wh::playermodule::C_Keybinds *, IXmlNode **);

		constexpr std::string_view chat_action       = "kcd2o_chat";
		constexpr std::string_view voice_action      = "kcd2o_voice";
		constexpr std::string_view emote_action      = "kcd2o_emote";
		constexpr std::string_view player_hub_action = "kcd2o_player_hub";
		constexpr std::string_view social_action     = "kcd2o_social";
		constexpr std::string_view staff_action      = "kcd2o_staff";

		std::atomic_bool g_available{};
		std::atomic_bool g_voice_held{};
		std::atomic_bool g_emote_held{};
		std::atomic<std::uint32_t> g_chat_generation{};
		std::atomic<std::uint32_t> g_player_hub_generation{};
		std::atomic<std::uint32_t> g_social_generation{};
		std::atomic<std::uint32_t> g_staff_generation{};
		std::atomic_bool g_listener_registered{};
		std::atomic_bool g_actions_added{};
		REL::Relocation<parse_keybinds> g_original_parse;

		Offsets::IActionMapManager *action_map_manager() noexcept
		{
			auto *framework = CCryAction::GetInstance();
			return framework ? reinterpret_cast<Offsets::IActionMapManager *>(framework->m_pActionMapManager) : nullptr;
		}

		const char *action_name(const Offsets::SActionId &action) noexcept
		{
			const auto *name = *reinterpret_cast<const char *const *>(&action);
			return name ? name : "";
		}

		class action_listener final : public Offsets::IActionListener
		{
		public:
			void OnAction(const Offsets::SActionId &action, int activation_mode, float) override
			{
				const std::string_view name{action_name(action)};
				if (name == chat_action && (activation_mode & 1) != 0)
				{
					g_chat_generation.fetch_add(1, std::memory_order_acq_rel);
				}
				else if (name == player_hub_action && (activation_mode & 1) != 0)
				{
					g_player_hub_generation.fetch_add(1, std::memory_order_acq_rel);
				}
				else if (name == social_action && (activation_mode & 1) != 0)
				{
					g_social_generation.fetch_add(1, std::memory_order_acq_rel);
				}
				else if (name == staff_action && (activation_mode & 1) != 0)
				{
					g_staff_generation.fetch_add(1, std::memory_order_acq_rel);
				}
				else if (name == voice_action)
				{
					if ((activation_mode & 1) != 0)
					{
						g_voice_held.store(true, std::memory_order_release);
					}
					if ((activation_mode & 2) != 0)
					{
						g_voice_held.store(false, std::memory_order_release);
					}
				}
				else if (name == emote_action)
				{
					if ((activation_mode & 1) != 0)
					{
						g_emote_held.store(true, std::memory_order_release);
					}
					if ((activation_mode & 2) != 0)
					{
						g_emote_held.store(false, std::memory_order_release);
					}
				}
			}

			void AfterAction() override
			{
			}
		};

		action_listener g_listener;

		IXmlNode *add_child(IXmlNode *parent, const char *tag)
		{
			IXmlNode *child{};
			parent->newChild(&child, tag);
			return child;
		}

		void set_attribute(IXmlNode *node, const char *name, const char *value)
		{
			node->setAttr(name, value);
		}

		IXmlNode *find_child(IXmlNode *parent, const char *tag, const char *attribute, const char *value)
		{
			for (int index{}; index < parent->getChildCount(); ++index)
			{
				IXmlNode *child{};
				parent->getChild(&child, index);
				if (!child)
				{
					continue;
				}
				if (std::strcmp(child->getTag(), tag) == 0 && std::strcmp(child->getAttr(attribute), value) == 0)
				{
					return child;
				}
				child->Release();
			}
			return nullptr;
		}

		void add_action_reference(IXmlNode *superaction, std::string_view action)
		{
			auto *node = add_child(superaction, "action");
			set_attribute(node, "name", action.data());
			set_attribute(node, "map", "player");
			node->Release();
		}

		void add_default_control(IXmlNode *superaction, const char *input)
		{
			auto *node = add_child(superaction, "control");
			set_attribute(node, "input", input);
			set_attribute(node, "controller", "keyboard");
			node->Release();
		}

		void add_superaction(IXmlNode *root, std::string_view name, const char *label, const char *input)
		{
			if (auto *existing = find_child(root, "superaction", "name", name.data()))
			{
				existing->Release();
				return;
			}
			auto *node = add_child(root, "superaction");
			set_attribute(node, "name", name.data());
			set_attribute(node, "ui_group", "kcd2online");
			set_attribute(node, "ui_name", label);
			set_attribute(node, "ui_tooltip", "");
			set_attribute(node, "keyboard", "writeable");
			add_action_reference(node, name);
			add_default_control(node, input);
			node->Release();
		}

		bool add_player_actions()
		{
			if (g_actions_added.load(std::memory_order_acquire))
			{
				return true;
			}

			auto *manager     = action_map_manager();
			auto *environment = SSystemGlobalEnvironment::GetInstance();
			if (!manager || !environment || !environment->pSystem)
			{
				return false;
			}
			auto *player = static_cast<Offsets::IActionMap *>(manager->GetActionMap("player"));
			if (!player)
			{
				return false;
			}

			constexpr char actions[] =
			    "<actionmap>"
			    "<action name=\"kcd2o_chat\" onPress=\"1\" keyboard=\"_keybinds_ref_\" noModifiers=\"1\"/>"
			    "<action name=\"kcd2o_voice\" onPress=\"1\" onRelease=\"1\" keyboard=\"_keybinds_ref_\"/>"
			    "<action name=\"kcd2o_emote\" onPress=\"1\" onRelease=\"1\" keyboard=\"_keybinds_ref_\" "
			    "noModifiers=\"1\"/>"
			    "<action name=\"kcd2o_player_hub\" onPress=\"1\" keyboard=\"_keybinds_ref_\" noModifiers=\"1\"/>"
			    "<action name=\"kcd2o_social\" onPress=\"1\" keyboard=\"_keybinds_ref_\" noModifiers=\"1\"/>"
			    "<action name=\"kcd2o_staff\" onPress=\"1\" keyboard=\"_keybinds_ref_\" noModifiers=\"1\"/>"
			    "</actionmap>";
			IXmlNode *root{};
			environment->pSystem->LoadXmlFromBuffer(&root, actions, sizeof(actions) - 1, 0, 1);
			if (!root)
			{
				return false;
			}
			const auto loaded = player->LoadFromXMLNode(&root);
			root->Release();
			if (loaded)
			{
				g_actions_added.store(true, std::memory_order_release);
			}
			return loaded;
		}

		bool register_listener()
		{
			if (g_listener_registered.load(std::memory_order_acquire))
			{
				return true;
			}
			auto *manager = action_map_manager();
			if (!manager)
			{
				return false;
			}
			manager->AddExtraActionListener(&g_listener, "player");
			g_listener_registered.store(true, std::memory_order_release);
			return true;
		}

		void inject_keybinds(wh::playermodule::C_Keybinds *keybinds, IXmlNode **root)
		{
			if (!root || !*root)
			{
				g_original_parse(keybinds, root);
				return;
			}
			IXmlNode *patched{};
			(*root)->clone(&patched, false);
			if (!patched)
			{
				g_original_parse(keybinds, root);
				return;
			}

			if (auto *group = find_child(patched, "ui_group", "name", "kcd2online"))
			{
				group->Release();
			}
			else
			{
				group = add_child(patched, "ui_group");
				set_attribute(group, "name", "kcd2online");
				set_attribute(group, "ui_label", "kcd2o_keybind_group");
				group->Release();
			}

			add_superaction(patched, chat_action, "kcd2o_keybind_chat", "enter");
			add_superaction(patched, voice_action, "kcd2o_keybind_voice", "v");
			add_superaction(patched, emote_action, "kcd2o_keybind_emote", "g");
			add_superaction(patched, player_hub_action, "kcd2o_keybind_player_hub", "f2");
			add_superaction(patched, social_action, "kcd2o_keybind_social", "f3");
			add_superaction(patched, staff_action, "kcd2o_keybind_staff", "f7");

			if (auto *conflict = find_child(patched, "conflict", "name", "general"))
			{
				for (const auto name : {chat_action, voice_action, emote_action, player_hub_action, social_action, staff_action})
				{
					auto *entry = add_child(conflict, "superaction");
					set_attribute(entry, "name", name.data());
					entry->Release();
				}
				conflict->Release();
			}

			(*root)->Release();
			*root                    = patched;
			const auto actions_ready = add_player_actions();
			g_original_parse(keybinds, root);
			g_available.store(actions_ready && register_listener(), std::memory_order_release);
		}
	} // namespace

	bool install()
	{
		g_original_parse = REL::Relocation<>{REL::ID(33'537), 0x73}.write_call<5>(inject_keybinds);
		return g_original_parse.address() != 0;
	}

	bool available() noexcept
	{
		return g_available.load(std::memory_order_acquire);
	}

	bool voice_held() noexcept
	{
		return g_voice_held.load(std::memory_order_acquire);
	}

	input_state state() noexcept
	{
		return {available(),
		        g_chat_generation.load(std::memory_order_acquire),
		        g_player_hub_generation.load(std::memory_order_acquire),
		        g_social_generation.load(std::memory_order_acquire),
		        g_staff_generation.load(std::memory_order_acquire),
		        g_emote_held.load(std::memory_order_acquire)};
	}

	void reset_transient() noexcept
	{
		g_voice_held.store(false, std::memory_order_release);
		g_emote_held.store(false, std::memory_order_release);
	}
} // namespace kcd2o::kcse::native_keybinds
