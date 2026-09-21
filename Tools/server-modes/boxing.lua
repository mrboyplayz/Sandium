---@type Plugin
local mode = ...

mode.name = "Boxing"
mode.author = "Sandium"
mode.description = "Free-roam boxing at the configured arena spawn."

mode.defaultConfig = {
	spawnX = 1617.59,
	spawnY = 25.16,
	spawnZ = 1199.76,
	spawnSpacing = 1.75,
	autoSpawn = true,
}

-- Keep simultaneous spawns from occupying the same physics volume while
-- keeping every player centered on the requested arena location.
local spawnOffsets = {
	{  0, 0 },
	{  1, 0 }, { -1, 0 }, { 0,  1 }, { 0, -1 },
	{  1, 1 }, { -1, 1 }, { 1, -1 }, { -1, -1 },
	{  2, 0 }, { -2, 0 }, { 0,  2 }, { 0, -2 },
}

local nextSpawnSlot = 1

local function assignExistingTeams()
	local team = 0
	for _, ply in ipairs(players.getAll()) do
		ply.team = team
		ply.suitColor = (team % 5) + 1
		ply:update()
		team = team + 1
	end
end

local function getArenaCoordinates()
	return tonumber(mode.config.spawnX) or 1617.59,
		tonumber(mode.config.spawnY) or 25.16,
		tonumber(mode.config.spawnZ) or 1199.76
end

local function getSpawnPosition()
	local x, y, z = getArenaCoordinates()
	local offset = spawnOffsets[nextSpawnSlot]
	local spacing = tonumber(mode.config.spawnSpacing) or 1.75

	nextSpawnSlot = (nextSpawnSlot % #spawnOffsets) + 1

	return Vector(
		x + offset[1] * spacing,
		y,
		z + offset[2] * spacing
	)
end

mode:addEnableHandler(function(isReload)
	server.type = TYPE_ROUND
	server.roundTeamDamage = 0
	nextSpawnSlot = 1

	if isReload then
		assignExistingTeams()
	else
		server:reset()
	end
end)

mode:addHook("PostResetGame", function()
	nextSpawnSlot = 1
	server.roundTeamDamage = 0
	server.state = STATE_GAME
	server.time = 60 * 60 * server.TPS
end)

-- As in the Duels mode, replace the stock round simulation so its team and
-- win-state logic cannot end or repopulate the boxing match.
mode:addHook("LogicRound", function()
	server.state = STATE_GAME
	server.time = server.time - 1

	if server.time < 1 then
		server.time = 60 * 60 * server.TPS
	end

	for _, ply in ipairs(players.getAll()) do
		local man = ply.human
		if man and not man.isAlive then
			man:remove()
		end
	end

	return hook.override
end)

local function spawnPlayer(ply)
	if ply.human then
		return
	end

	ply.model = 0
	-- Give each arena slot its own team. Round's team-damage reflection was
	-- hurting attackers (including vehicle drivers) when fighters shared one.
	ply.team = nextSpawnSlot - 1
	ply.suitColor = (ply.team % 5) + 1
	ply.tieColor = 0

	-- PostHumanCreate below performs the final arena placement and enables
	-- boxing. Creating at the arena center avoids one frame at a city spawn.
	local x, y, z = getArenaCoordinates()
	if humans.create(Vector(x, y, z), orientations.n, ply) then
		ply:update()
	end
end

mode:addHook("PlayerActions", function(ply)
	if mode.config.autoSpawn and not ply.human then
		spawnPlayer(ply)
	end
end)

-- Boxing creates players directly, so the stock Round lobby is not needed.
mode:addHook("ServerSend", function()
	for _, ply in ipairs(players.getNonBots()) do
		ply.menuTab = 0
	end
end)

mode:addHook("PostHumanCreate", function(man)
	if not man.player then
		return
	end

	man:teleport(getSpawnPosition())

	-- The boxing2 plugin normally initializes this table first. Keeping the
	-- fallback here also makes the mode safe if hook load order changes.
	man.data.BoxingData = man.data.BoxingData or {
		KBMulti = 1,
		MaxDMG = 95,
	}
	man.data.BoxingData.MaxDMG = 95
	man.data.isPunching = true
	man.player.data.CanPunch = true
	man.player:sendMessage("Boxing enabled: left click to punch, right click to block.")
end)
