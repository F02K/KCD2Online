-- early_main is required for some data modifcations, 
-- like when fmod audio getevent is parsed at game start for sound replacement,
-- or when some xml files are parsed by the game.
-- Generally the lua documentation files will tell you if some game events require a is_early_main check
-- for them to do anything meaningful.
if rom.core.is_early_main then
	rom.log.warning("hello from early main")
	
	-- rom.audio.on_fmod_getevent(function(event_name)
		-- rom.log.info(event_name)

		-- return the new event_name if you ever modify it
		-- return event_name
	-- end)
	
	-- rom.game_data.on_xml_parse(function(filename, file_content)
		-- rom.log.info(filename)

		-- return the new file_content if you ever modify it
		-- return file_content
	-- end)
	
	-- local old_voices_file_path = "data/libs/gameaudio/voices.xml"
	-- local new_voices_file_path = rom.path.combine(_PLUGIN.plugins_mod_folder_path, "voices.xml")
	-- rom.game_data.on_cryfile_open(old_voices_file_path, new_voices_file_path)
	
	--rom.game_data.on_pak_openable(function()
	--	rom.game_data.open_pak("data", "C:/Users/User/SomePath/Data/SomePakFile.pak")
	--	rom.game_data.open_pak("localization", "C:/Users/User/SomePath/Localization/english_xml.pak")
	--end)
	

	return
end

-- print(rom.game.player.inventory:CreateItem("some-item-guid", 1, 1))

-- local player_inventory = rom.game.player.inventory

-- for k,v in pairs(player_inventory) do

	-- print(k,v)

-- end

-- for k,v in pairs(player_inventory:GetInventoryTable()) do

	-- local item = rom.game.ItemManager.GetItem(v)
	-- local item_name = rom.game.ItemManager.GetItemName(item.class)
	-- print(item_name, item.class, item.amount, item.entity)
	
	-- for kk,vv in pairs(rom.game.ItemManager.GetItem(v)) do
		-- print(kk)
	-- end

-- end

-- playerTeleportTo = { x = 2968, y = 894 , z = 65 }
-- rom.game.player:SetWorldPos(playerTeleportTo)

-- rom.game.System.GetEntityByName("ksta_additive_woman_7"):SetWorldScale(1)

-- UI Showcase

local example_bool = false
rom.gui.add_to_menu_bar(function()
    local new_value, clicked = rom.ImGui.Checkbox("Example Bool", example_bool)
    if clicked then
        example_bool = new_value
		rom.log.info(example_bool)
	end
end)

rom.gui.add_imgui(function()
    -- rom.ImGui.PushStyleColor(rom.ImGuiCol.WindowBg, 1, 1, 1, 1)
    
   if rom.ImGui.Begin("My Custom Window") then
       if rom.ImGui.Button("Label") then
         rom.log.info("hi")
       end
	
   end
   rom.ImGui.End()
   
   -- rom.ImGui.PopStyleColor()
end)
