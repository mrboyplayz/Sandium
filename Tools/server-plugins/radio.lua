---@type Plugin
local plugin = ...
plugin.name = "Radio"
plugin.author = "Sandium"
plugin.description = "Car radio: clients submit YouTube links over TCP 28000, the server downloads the audio (yt-dlp) and broadcasts it to every client via the flags channel."

local RADIO_DIR = "/opt/subrosa/stream/"
local FLAGS_FILE = "/opt/subrosa/stream/flags.txt"
local serial = 0
local listening = false

plugin:addHook("Physics", function()
	if not listening then
		-- guard: only create the listener once
		listening = true
		local ok, server = pcall(function()
			return TCPServer(28000)
		end)
		if not ok then
			listening = false
			plugin:print("Could not open TCP 28000: " .. tostring(server))
			return
		end
		radio_server = server
		plugin:print("radio TCP listening on 28000")
	end

	if not radio_server then return end

	-- drain accepted connections
	for _ = 1, 8 do
		local conn = radio_server:accept()
		if not conn then break end
		local data = conn:receive(16384)
		if data and #data > 4 then
			local vid, url = data:match("^radio%s+(%d+)%s+(%S+)")
			if vid and url and #url < 400 and not url:find("[;|&$`]") then
				serial = serial + 1
				local name = string.format("radio%d.mp3", serial)
				plugin:print("radio: downloading serial " .. serial .. " for vehicle " .. vid)
				-- background download; shell returns at once
				local cmd = string.format(
					"nohup sh -c 'yt-dlp -x --audio-format mp3 --audio-quality 5 -o \"/opt/subrosa/stream/radio%d.%%(ext)s\" \"%s\" && touch \"/opt/subrosa/stream/radio%d.done\"' >/tmp/radio%d.log 2>&1 &",
					serial, url, serial, serial)
				os.execute(cmd)
				-- remember which vehicle this serial belongs to
				radio_jobs = radio_jobs or {}
				radio_jobs[serial] = { name = name, vid = vid }
			else
				plugin:print("radio: rejected bad request")
			end
		end
		conn:close()
	end

	-- poll finished downloads -> publish via flags
	if radio_jobs then
		for ser, job in pairs(radio_jobs) do
			local done = io.open(RADIO_DIR .. "radio" .. ser .. ".done", "r")
			if done then
				done:close()
				radio_jobs[ser] = nil
				local lines = {}
				local f = io.open(FLAGS_FILE, "r")
				if f then
					for line in f:lines() do
						if not line:find("^radio=") then
							table.insert(lines, line)
						end
					end
					f:close()
				end
				table.insert(lines, string.format("radio=%d %s %s", ser, job.name, job.vid))
				local out = io.open(FLAGS_FILE, "w")
				if out then
					out:write(table.concat(lines, "\n") .. "\n")
					out:close()
				end
				plugin:print("radio ready: " .. job.name .. " on vehicle " .. job.vid)
			end
		end
	end
end)
