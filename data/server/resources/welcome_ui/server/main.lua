local open = {}

local function show_dashboard(player)
    open[player.id] = true
    ui.show(player.id, "welcome", {
        title = "KCD2Online Server",
        size = { 440, 260 },
        widgets = {
            { type = "text", text = "Willkommen, " .. player.name .. "!" },
            { type = "separator", text = "Server Resource UI" },
            { type = "text", text = "Dieses Fenster wurde nur durch server/main.lua erzeugt." },
            { type = "progress", text = "Beispiel", value = 0.72 },
            { type = "button", id = "hello", text = "Hallo sagen" },
            { type = "button", id = "close", text = "Schliessen", same_line = true }
        }
    })
    input.register(player.id, "toggle_welcome", "Willkommensfenster", 0x75) -- F6
end

server.on("player_joined", function(player)
    show_dashboard(player)
end)

server.on("ui", function(player_id, document, control, event, payload)
    if document ~= "welcome" or event ~= "click" then return end
    if control == "hello" then
        server.say("Hallo aus Lua!", player_id)
    elseif control == "close" then
        open[player_id] = false
        ui.close(player_id, "welcome")
    end
end)

input.on("toggle_welcome", function(player_id, payload)
    if open[player_id] then
        open[player_id] = false
        ui.close(player_id, "welcome")
        return
    end
    for _, player in ipairs(server.players()) do
        if player.id == player_id then show_dashboard(player) end
    end
end)
