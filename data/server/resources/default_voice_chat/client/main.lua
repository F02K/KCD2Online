features.enable("voice_chat")

-- Product policy lives in this resource. The native voice service only owns
-- capture, processing, Opus, transport, spatial playback, and device access.
local push_to_talk_key = 0x56 -- V

input.bind("push_to_talk", "Voice: push to talk", push_to_talk_key, true)
input.on("push_to_talk", function(event)
    local range = "normal"
    if event.control then
        range = "whisper"
    elseif event.shift then
        range = "shout"
    end
    voice.set_transmitting(event.pressed == true, range)
end)

-- This invokes a native rendering primitive, but placement, dimensions and
-- colors are owned by the plugin and can be changed without rebuilding C++.
ui.show("voice_indicator", {
    kind = "voice_indicator",
    width = 68,
    height = 28,
    offset_x = 38,
    offset_y = 46,
    normal_color = { 0.76, 0.61, 0.34, 1.0 },
    whisper_color = { 0.55, 0.68, 0.52, 1.0 },
    shout_color = { 0.86, 0.48, 0.27, 1.0 }
})
