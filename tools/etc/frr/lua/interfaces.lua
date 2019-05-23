--[[ Interface tracking ]]--

local cjson = require('cjson')
local j = cjson.new()

local f = assert(io.popen('ip -j link', 'r'))
local s = assert(f:read('*a'))
f:close()

log.debug("Got interfaces: " .. s)

interfaces = j.decode(s)

ifstates = {}

for i = 1, #interfaces do
	iface = interfaces[i]

	ifstates[#ifstates + 1] =
	{
		["type"] = "interface";
		["name" ] = iface['ifname'];
		["state" ] = iface['operstate'];
	}

end

return ifstates
