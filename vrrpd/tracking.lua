-- Custom object tracking handler for VRRP
--
-- Available objects:
--    obj: the tracked object
--     vr: the virtual router tracking the object
--
-- Available data:
--    OBJ_UP   - object is in UP state
--    OBJ_DOWN - object is in DOWN state

if (obj.state == OBJ_DOWN) then
	vr:set_priority(vr.priority - 1)
else
	vr:set_priority(100)
end

--[[
if (obj.state == OBJ_UP) then
	vr:set_priority(vr.priority + 40)
elseif obj.state == OBJ_DOWN then
	vr:set_priority(vr.priority - 10)
end
--]]
