---@type Plugin
local plugin = ...
plugin.name = "Radio"
plugin.author = "Sandium"
plugin.description = "Car radio: clients submit YouTube links over TCP 28000, the server downloads the audio (yt-dlp) and broadcasts it to every client via the flags channel."

local RADIO_DIR = "/opt/subrosa/stream/"
local FLAGS_FILE = "/opt/subrosa/stream/flags.txt"
-- Use a process-independent base so a server restart cannot reuse radio1.mp3
-- and accidentally publish an old .done marker or an unchanged client flag.
local serial = os.time()
local radioServer = nil
local retryTicks = 0

local function isYouTubeUrl(url)
	local host = url:match("^https://([^/%?#]+)")
	if not host then return false end
	host = host:lower():gsub(":%d+$", "")
	return host == "youtu.be" or host == "youtube.com" or
		host:sub(-#".youtube.com") == ".youtube.com"
end

local function shellQuote(value)
	return "'" .. value:gsub("'", "'\\''") .. "'"
end

-- RosaServer can reload plugins without restarting the process. Release the
-- listening socket before the replacement instance tries to bind the port.
plugin:addDisableHandler(function()
	if radioServer then
		pcall(function()
			if radioServer.isOpen then
				radioServer:close()
			end
		end)
		radioServer = nil
	end
end)

plugin:addHook("Physics", function()
	if not radioServer then
		if retryTicks > 0 then
			retryTicks = retryTicks - 1
		else
			local ok, server = pcall(TCPServer.new, 28000)
			if not ok then
				-- Physics runs about 60 times per second. Wait roughly five
				-- seconds before retrying so a bind failure cannot flood logs.
				retryTicks = 300
				plugin:print("Could not open TCP 28000: " .. tostring(server))
			else
				radioServer = server
				plugin:print("radio TCP listening on 28000")
			end
		end
	end

	if not radioServer then return end

	-- drain accepted connections
	for _ = 1, 8 do
		local conn = radioServer:accept()
		if not conn then break end
		local data = conn:receive(16384)
		if data and #data > 4 then
			local vid, url = data:match("^radio%s+(%d+)%s+(%S+)")
			if vid and url and #url < 400 and isYouTubeUrl(url) then
				serial = serial + 1
				local name = string.format("radio%d.mp3", serial)
				plugin:print("radio: downloading serial " .. serial .. " for vehicle " .. vid)
				-- Quote every value before passing it through either shell. This
				-- allows normal query parameters such as '&list=' safely.
				local output = RADIO_DIR .. "radio" .. serial .. ".%(ext)s"
				local donePath = RADIO_DIR .. "radio" .. serial .. ".done"
				local logPath = "/tmp/radio" .. serial .. ".log"
				os.remove(donePath)
				os.remove(RADIO_DIR .. name)
				local download = "yt-dlp -x --audio-format mp3 --audio-quality 5 -o " ..
					shellQuote(output) .. " " .. shellQuote(url) .. " && touch " .. shellQuote(donePath)
				local cmd = "nohup sh -c " .. shellQuote(download) .. " >" .. shellQuote(logPath) .. " 2>&1 &"
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
