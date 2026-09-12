-- Scale-only experiment. The engine updates scale; no raw physics writes.
local STATUS,CHANNEL="FPE_FP_SCALE_TEST_077","FPE_Scale077"
local leases={}
local lastNote
local function read(fn) local ok,v=pcall(fn); if ok then return v end end
local function note(s)
    if not (s:match("^ready") or s:match("^error:") or s:match("^remove retry:") or s:match("^rejected:")) then return end
    if s==lastNote then return end
    lastNote=s
    pcall(Ext.IO.SaveFile,"FirstPersonExplorer/scale_server.txt","0.7.6.3 Public Release\n"..s)
end
local function remove(peer,reason)
    local p=leases[peer]
    if p then
        local ok,err=pcall(Osi.RemoveStatus,p.uuid,STATUS,p.uuid)
        if ok then leases[peer]=nil; note("removed: "..reason)
        else note("remove retry: "..tostring(err)) end
    end
end
Ext.RegisterNetListener(CHANNEL,function(_,payload,peer)
    local ok,err=pcall(function()
        if type(peer)~="number" or type(payload)~="string" or #payload>80 then return end
        if payload=="off" then remove(peer,"client exit"); return end
        local uuid=payload:match("^on:([%x%-]+)$")
        if not uuid or #uuid~=36 then return end
        local user=(peer & 0xffff0000) | 1
        local current=Osi.GetCurrentCharacter(user)
        -- Authorize the requested character against the sending user's selection.
        if type(current)~="string" or current:sub(-36):lower()~=uuid:lower() then
            remove(peer,"selected character mismatch"); note("rejected: selection mismatch"); return
        end
        local e=Ext.Entity.Get(uuid)
        if not e or e.IsInCombat~=nil or e.ClientTimelineActorControl~=nil then
            remove(peer,"unsupported character state"); return
        end
        local p=leases[peer]
        if p and p.uuid~=uuid then remove(peer,"character changed"); if leases[peer] then return end; p=nil end
        local now=Ext.Utils.MonotonicTime()
        if not p then p={uuid=uuid,refresh=0}; leases[peer]=p end
        p.untilTime=now+1200
        if now>=p.refresh then
            -- Unique overwrite status: refresh duration without adding scale stacks.
            -- Two seconds of game time is a fallback if the Lua lease stops running.
            Osi.ApplyStatus(uuid,STATUS,2.0,1,uuid)
            p.refresh=now+750
            note("requested scale=0.15; status="..tostring(Osi.HasActiveStatus(uuid,STATUS))
                .." visualScale="..tostring(read(function() return e.GameObjectVisual.Scale end))
                .." sizeCategory="..tostring(read(function() return e.ObjectSize.Size end)))
        end
    end)
    if not ok then remove(peer,"error"); note("error: "..tostring(err)) end
end)
Ext.Events.Tick:Subscribe(function()
    local now=Ext.Utils.MonotonicTime()
    for peer,p in pairs(leases) do
        local e=read(function() return Ext.Entity.Get(p.uuid) end)
        if now>p.untilTime or not e or read(function() return e.IsInCombat end)~=nil then remove(peer,"lease/state expired") end
    end
end)
local function cleanup()
    for peer in pairs(leases) do remove(peer,"session transition") end
end
Ext.Events.GameStateChanged:Subscribe(function(e)
    if tostring(e.ToState)~="Running" then cleanup() end
end)
Ext.Events.SessionLoaded:Subscribe(function()
    cleanup()
    -- Remove only this test's status from a save loaded while first person was active.
    for _,e in pairs(read(function() return Ext.Entity.GetAllEntitiesWithComponent("ServerCharacter") end) or {}) do
        local id=read(function() return tostring(e.Uuid.EntityUuid) end)
        if id then pcall(Osi.RemoveStatus,id,STATUS,id) end
    end
end)
Ext.Events.ResetCompleted:Subscribe(cleanup)
note("ready; scale-only status; no ObjectSize, damage, weight or camera changes")
