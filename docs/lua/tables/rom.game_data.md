# Table: rom.game_data

## Functions (4)

### `on_xml_parse(function)`

The passed function will be called before the game parses a .cfg file.
is_early_main setup/check required for some of the files.

- **Parameters:**
  - `function` (function): signature (string file_name, string file_content) return new_file_content or nil

**Example Usage:**
```lua
rom.game_data.on_xml_parse(function)
```

### `on_cryfile_open(old_filename, new_filename)`

When the game opens a CryFile (Which can be a lot of different kind of files: .pak, .xml, .cfg and so on),
you can tell KCD2Online to instead load a certain new file instead.
is_early_main setup/check required for some of the files.

- **Parameters:**
  - `old_filename` (string): old filename that should get replaced
  - `new_filename` (string): new filename that will replace the old one.

**Example Usage:**
```lua
rom.game_data.on_cryfile_open(old_filename, new_filename)
```

### `open_pak(root, path)`

To be used with `on_pak_openable`.
is_early_main setup/check required.

- **Parameters:**
  - `root` (string): root used for the pak file, game uses `data`, `localization`...
  - `path` (string): file path to the .pak file to be opened.

**Example Usage:**
```lua
rom.game_data.open_pak(root, path)
```

### `on_pak_openable(function)`

The passed function will be called when the game is loading mods .pak files
is_early_main setup/check is required.

- **Parameters:**
  - `function` (function): signature () return void / nil

**Example Usage:**
```lua
rom.game_data.on_pak_openable(function)
```


