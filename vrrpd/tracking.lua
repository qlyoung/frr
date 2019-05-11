-- Builtin object tracking handlers for VRRP
--
-- All functions receive two arguments:
-- <name>: the tracked object
--     vr: the virtual router tracking obj

function vrrp_ot_interface (vr, interface)
	if interface.flags & IFF_DOWN then
		vr:set_priority(10)
	elseif interface.flags & IFF_UP then
		vr:set_priority(110)
	end
end

function vrrp_ot_ipsla (vr)
end

function vrrp_ot_route (vr, route)
	if not route.reachable then
		vr:set_priority(10)
	elseif route.reachable then
		vr:set_priority(110)
	end
end
