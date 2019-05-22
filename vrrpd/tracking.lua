-- Custom object tracking handler for VRRP
--
-- Available objects:
--    obj: the tracked object
--     vr: the virtual router tracking the object

if (obj.state == "DOWN") then
	vr:set_priority(vr.priority - 5)
else
	vr:set_priority(100)
end
