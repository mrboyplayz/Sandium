---@type Plugin
local plugin = ...
plugin.name = 'Discord Status'
plugin.author = 'Sandium'
plugin.description = 'Reports live players and appearance to the local master server.'

local json = require 'main.json'
local lastPost = 0

plugin:addHook('PostServerSend', function()
    local now = os.realClock()
    if now - lastPost < 10 then return end
    lastPost = now

    local body = { players = {} }
    for _, ply in pairs(players.getNonBots()) do
        table.insert(body.players, {
            name = ply.name,
            phoneNumber = ply.phoneNumber,
            gender = ply.gender,
            head = ply.head,
            skinColor = ply.skinColor,
            hairColor = ply.hairColor,
            hair = ply.hair,
            eyeColor = ply.eyeColor,
        })
    end
    http.post('http://127.0.0.1', '/api/server-snapshot', {},
              json.encode(body), 'application/json', function(response)
        if not response or response.status ~= 200 then
            plugin:warn('Master snapshot update failed')
        end
    end)
end)
