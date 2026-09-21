---@type Plugin
local plugin = ...

plugin.name = "Password Gate"
plugin.author = "Sandium"
plugin.description = "Rejects direct joins that did not authenticate with the master gate."

local pending = {}

local function isAuthorized(ply)
	local connection = ply.connection
	if not connection then
		return false
	end

	local address = connection.address
	if not address or not address:match("^%d+%.%d+%.%d+%.%d+$") then
		return false
	end

	local response = http.getSync(
		"http://127.0.0.1",
		"/gate/check?ip=" .. address,
		{}
	)
	return response and response.status == 200
		and response.body:find('"authorized":true', 1, true) ~= nil
end

plugin:addHook("PostPlayerCreate", function(ply)
	pending[ply.index] = {
		player = ply,
		checkAt = os.realClock() + 1,
	}
end)

plugin:addHook("PostPlayerDelete", function(ply)
	pending[ply.index] = nil
end)

plugin:addHook("Logic", function()
	local current = os.realClock()
	for index, entry in pairs(pending) do
		if current >= entry.checkAt then
			pending[index] = nil
			local ply = entry.player
			if isActive(ply) and not ply.isBot and not isAuthorized(ply) then
				plugin:print(string.format("Rejected unauthorized join from %s", ply.connection and ply.connection.address or "unknown"))
				ply:sendMessage("Password authentication required. Launch through SandiumLauncher.")
				ply:remove()
			end
		end
	end
end)

