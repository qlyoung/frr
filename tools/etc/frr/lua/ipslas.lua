-- Check nexthop reachability

ips = {'192.168.0.1'}
ipslas = {}

for ipidx = 1, #ips do
	ip = ips[ipidx]
	success = os.execute('ping -c 1 -w 1 ' .. ip)
	if success then
		state = "supercalifragilistic"
	else
		state = "UNREACHABLE"
	end

	ipslas[#ipslas + 1] =
		{
			["type"] = "nexthop";
			["name"] = ip;
			["state"] = state;
		}
end

return ipslas
