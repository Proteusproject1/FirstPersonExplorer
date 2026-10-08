local DIAGNOSTICS=false
local lastNote
-- Scale-only experiment. The engine updates scale; no raw physics writes.
local STATUS,CHANNEL="FPE_FP_SCALE_TEST_077","FPE_Scale077"
local leases={}
local phase="Running"
local selections={}
local combatMode
local diagnostics=Ext.Require("ScaleDiagnostics.lua")()
local instant=Ext.Require("InstantScale.lua")()
local function diag(uuid,event,reason) pcall(diagnostics.track,uuid,event,reason) end

local saveRows={}
local function saveNote(message)
    if not DIAGNOSTICS then return end
    saveRows[#saveRows+1]="t="..Ext.Utils.MonotonicTime().." "..message
    if #saveRows>80 then table.remove(saveRows,1) end
    pcall(Ext.IO.SaveFile,"FirstPersonExplorer/save_server.txt","0.9.0\n"..table.concat(saveRows,"\n"))
end

local function read(fn) local ok,v=pcall(fn); if ok then return v end end
local function note(s)
    if not (s:match("^ready") or s:match("^error:") or s:match("^remove retry:") or s:match("^rejected:")) then return end
    if s==lastNote then return end
    lastNote=s
    pcall(Ext.IO.SaveFile,"FirstPersonExplorer/scale_server.txt","0.9.0\n"..s)
end
local function remove(peer,reason)
    local p=leases[peer]
    if p then
        diag(p.uuid,"REMOVE_REQUEST",reason)
        if phase=="Running" then pcall(instant.request,p.uuid,"O") else pcall(instant.reset) end
        local ok,err=pcall(Osi.RemoveStatus,p.uuid,STATUS,p.uuid)
        if ok then diag(p.uuid,"RESTORE_REQUEST",reason); leases[peer]=nil; note("removed: "..reason); saveNote("scale removed; "..reason)
        else note("remove retry: "..tostring(err)) end
    end
end
combatMode=Ext.Require("CombatModeServer.lua")(function() return phase=="Running" end,function(user)
    for peer,p in pairs(leases) do
        if ((peer & 0xffff0000) | 1)==user then
            local e=read(function() return Ext.Entity.Get(p.uuid) end)
            if e and e.IsInCombat~=nil then remove(peer,"combat toggle disabled") end
        end
    end
end)
local function selection(peer)
    local current=Osi.GetCurrentCharacter((peer & 0xffff0000) | 1)
    local id=type(current)=="string" and current:sub(-36):lower() or nil
    local e=id and Ext.Entity.Get(id)
    if not combatMode.enabled(peer) then selections[peer]=nil; return id,e end
    if e then
        local previous=selections[peer]
        local old=previous and Ext.Entity.Get(previous)
        if previous~=id and old and old.IsInCombat~=nil and e.IsInCombat~=nil
            and e.TurnBased and e.TurnBased.IsActiveCombatTurn==false then return previous,old end
        selections[peer]=id; return id,e
    end
    id=selections[peer]; e=id and Ext.Entity.Get(id)
    if e and e.IsInCombat~=nil then return id,e end
    selections[peer]=nil
end
Ext.RegisterNetListener("FPE_CombatSelection",function(_,payload,peer)
    if phase~="Running" or payload~="get" or type(peer)~="number" then return end
    local ok,err=pcall(function()
        local id,e=selection(peer)
        Ext.ServerNet.PostMessageToUser((peer & 0xffff0000) | 1,"FPE_CombatSelection",
            id and (id..":"..(e.IsInCombat~=nil and "1" or "0")) or "none")
    end)
    if not ok then note("error: selection "..tostring(err)) end
end)
Ext.RegisterNetListener(CHANNEL,function(_,payload,peer)
    if phase~="Running" then return end
    local ok,err=pcall(function()
        if type(peer)~="number" or type(payload)~="string" or #payload>80 then return end
        if payload=="off" then remove(peer,"client exit"); return end
        local uuid=payload:match("^on:([%x%-]+)$")
        if not uuid or #uuid~=36 then return end
        uuid=uuid:lower()
        local current,e=selection(peer)
        -- Authorize the requested character against the sending user's selection.
        if type(current)~="string" or current:sub(-36):lower()~=uuid:lower() then
            if not combatMode.enabled(peer) then remove(peer,"selected character mismatch") end
            note("rejected: selection mismatch"); return
        end
        if not e or (e.IsInCombat~=nil and not combatMode.enabled(peer)) or e.ClientTimelineActorControl~=nil then
            remove(peer,"unsupported character state"); return
        end
        local p=leases[peer]
        if p and p.uuid~=uuid then remove(peer,"character changed"); if leases[peer] then return end; p=nil end
        local now=Ext.Utils.MonotonicTime()
        if not p then
            -- Apply once per entry. Heartbeats renew only our ownership lease,
            -- never the engine status or its duration.
            p={uuid=uuid}; leases[peer]=p
            diag(uuid,"SHRINK_REQUEST","authorized entry")
            pcall(instant.request,uuid,"I")
            Osi.ApplyStatus(uuid,STATUS,-1.0,1,uuid)
            diag(uuid,"SHRINK_APPLIED","status applied once")
            saveNote("scale applied once; duration=-1; peer="..peer)
        end
        p.untilTime=now+1200
        if DIAGNOSTICS and (not p.nextNote or now>=p.nextNote) then
            p.nextNote=now+1000
            note("persistent scale=0.15; status="..tostring(Osi.HasActiveStatus(uuid,STATUS))
                .." visualScale="..tostring(read(function() return e.GameObjectVisual.Scale end)))
        end
    end)
    if not ok then remove(peer,"error"); note("error: "..tostring(err)) end
end)
Ext.Events.Tick:Subscribe(function()
    if phase~="Running" then return end
    pcall(diagnostics.tick)
    pcall(instant.tick)
    pcall(combatMode.tick)
    local now=Ext.Utils.MonotonicTime()
    for peer,p in pairs(leases) do
        local e=read(function() return Ext.Entity.Get(p.uuid) end)
        if now>p.untilTime or not e or (read(function() return e.IsInCombat end)~=nil and not combatMode.enabled(peer)) then remove(peer,"lease/state expired") end
    end
end)
local function cleanup()
    for peer in pairs(leases) do remove(peer,"session transition") end
    selections={}
end
Ext.Events.GameStateChanged:Subscribe(function(e)
    local previous=phase
    phase=tostring(e.ToState)
    saveNote("state "..tostring(e.FromState).." -> "..phase)
    if phase=="Save" then
        pcall(instant.cancel)
        saveNote("holding status writes and lease cleanup")
    elseif phase=="Running" and previous=="Save" then
        local now=Ext.Utils.MonotonicTime()
        -- Let the client heartbeat resume before considering wall-clock expiry.
        for _,p in pairs(leases) do p.untilTime=now+1200 end
        saveNote("resume; heartbeat grace=1200ms")
    elseif phase~="Running" then cleanup(); pcall(instant.reset) end
end)
Ext.Events.SessionLoaded:Subscribe(function()
    phase="Running"
    cleanup()
    pcall(diagnostics.reset)
    pcall(instant.reset)
    combatMode.load()
    -- Remove only this test's status from a save loaded while first person was active.
    for _,e in pairs(read(function() return Ext.Entity.GetAllEntitiesWithComponent("ServerCharacter") end) or {}) do
        local id=read(function() return tostring(e.Uuid.EntityUuid) end)
        if id then
            diag(id,"LOAD_CLEANUP","remove saved FPE status")
            pcall(Osi.RemoveStatus,id,STATUS,id)
        end
    end
end)
Ext.Events.ResetCompleted:Subscribe(function() cleanup(); pcall(diagnostics.reset); pcall(instant.reset) end)
note("ready; scale-only status; no ObjectSize, damage, weight or camera changes")
