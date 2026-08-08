---@meta game_data

---@class (exact) rom.game_data

-- The passed function will be called before the game parses a .cfg file.
--is_early_main setup/check required for some of the files.
---@param function function signature (string file_name, string file_content) return new_file_content or nil
function game_data.on_xml_parse(function) end

-- When the game opens a CryFile (Which can be a lot of different kind of files: .pak, .xml, .cfg and so on),
--you can tell KCD2Online to instead load a certain new file instead.
--is_early_main setup/check required for some of the files.
---@param old_filename string old filename that should get replaced
---@param new_filename string new filename that will replace the old one.
function game_data.on_cryfile_open(old_filename, new_filename) end

-- To be used with `on_pak_openable`.
--is_early_main setup/check required.
---@param root string root used for the pak file, game uses `data`, `localization`...
---@param path string file path to the .pak file to be opened.
function game_data.open_pak(root, path) end

-- The passed function will be called when the game is loading mods .pak files
--is_early_main setup/check is required.
---@param function function signature () return void / nil
function game_data.on_pak_openable(function) end


