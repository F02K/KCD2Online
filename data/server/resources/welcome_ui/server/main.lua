local open = {}

local function find_player(player_id)
    for _, player in ipairs(server.players()) do
        if player.id == player_id then
            return player
        end
    end
    return nil
end

local function show_dashboard(player)
    open[player.id] = true

    ui.show(player.id, "welcome", {
        title = "KCD2Online Server",
        size = { 440, 260 },
        widgets = {
            { type = "text", text = "Welcome, " .. player.name .. "!" },
            { type = "separator", text = "Server Resource UI" },
            { type = "text", text = "This window is controlled by server Lua." },
            { type = "progress", text = "Example", value = 0.72 },
            { type = "button", id = "hello", text = "Say hello" },
            { type = "button", id = "close", text = "Close", same_line = true }
        }
    })
end

server.on("player_joined", function(player)
    input.register(player.id, "toggle_welcome", "Welcome window", 0x75) -- F6
    show_dashboard(player)
end)

server.on("player_left", function(player, reason)
    open[player.id] = nil
end)

server.on("ui", function(player_id, document_id, control_id, event, payload)
    if document_id ~= "welcome" or event ~= "click" then
        return
    end

    if control_id == "hello" then
        server.say("Hello from Lua!", player_id)
    elseif control_id == "close" then
        open[player_id] = false
        ui.close(player_id, "welcome")
    end
end)

input.on("toggle_welcome", function(player_id, payload)
    if payload.pressed ~= true then
        return
    end

    if open[player_id] then
        open[player_id] = false
        ui.close(player_id, "welcome")
        return
    end

    local player = find_player(player_id)
    if player then
        show_dashboard(player)
    end
end)
