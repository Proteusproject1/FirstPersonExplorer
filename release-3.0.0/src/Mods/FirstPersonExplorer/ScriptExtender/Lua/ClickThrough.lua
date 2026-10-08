-- Full-size style: replaces the first-person shrink. The shrink existed only to keep the (natively
-- hidden) body out of the cursor/targeting ray. Instead, while in first person, set
-- CanClickThrough and clear PointerBlocker in the character's physics group, on both
-- the Physics component and its physics object (the ray tests the object). On exit
-- only those two bits are put back, so any other flag the engine changed is kept.
-- Sight (CanSeeThrough) and projectiles (CanShootThrough) are untouched. Client only.
-- 0.8.6 live: the engine clears the bit on most frames. The bit is re-applied every
-- tick AND right before the engine's PickingHelper (targeting) system runs, so it is
-- always set when targeting reads it. The hook may run on a worker thread: it only
-- reads/writes the two flag fields of an already-resolved entity. Never raises.
return function()
    local C={}
    local active=nil
    local hookId,hookTried=nil,false
    local rows={}
    local function read(fn) local ok,v=pcall(fn); if ok then return v end; return nil end
    local function log(event,detail)
        rows[#rows+1]=string.format("%s\t%s\t%s",tostring(Ext.Utils.MonotonicTime()),event,tostring(detail or ""):gsub("[\r\n\t]"," "))
        if #rows>200 then table.remove(rows,1) end
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/clickthrough.tsv",
            "First Person Explorer 0.9.0 click-through (problems only)\nse_ms\tevent\tdetail\n"..table.concat(rows,"\n").."\n")
    end
    -- The two places the physics group lives; resolved from the entity each time.
    local function holders(e)
        return {{name="component",h=read(function() return e.Physics end)},
            {name="object",h=read(function() return e.Physics.Physics end)}}
    end
    local function group(h) return h and read(function() return h.PhysicsGroup end) end
    local function describe(g) return g and tostring(read(function() return g.__Value end))..(read(function() return g.CanClickThrough end) and " click-through" or "")..(read(function() return g.PointerBlocker end) and " pointer-blocker" or "") or "unavailable" end
    local function ours(g) return g and read(function() return g.CanClickThrough end)==true and read(function() return g.PointerBlocker end)~=true end
    local function apply(h)
        local g=group(h)
        if not g then return false end
        if ours(g) then return true end
        local ok=pcall(function()
            local want=g|"CanClickThrough"
            if want.PointerBlocker then want=want~"PointerBlocker" end
            h.PhysicsGroup=want.__Value
        end)
        return ok and ours(group(h))
    end
    -- Re-apply wherever the bit was cleared; returns the number of re-applied holders.
    local function enforce(rec)
        local n=0
        for _,t in ipairs(holders(rec.entity)) do
            if rec.targets[t.name] and t.h and not ours(group(t.h)) then
                if apply(t.h) then n=n+1 else rec.failed=(rec.failed or 0)+1 end
            end
        end
        return n
    end
    local function onPicking()
        local rec=active
        if not rec or tostring(Ext.Utils.GetGameState())~="Running" then return end
        pcall(function() rec.hookRuns=rec.hookRuns+1; rec.hookReapplied=rec.hookReapplied+enforce(rec) end)
    end
    local function subscribe()
        if hookId or hookTried then return end
        hookTried=true
        local ok,id=pcall(Ext.Entity.OnSystemUpdate,"PickingHelper",onPicking)
        if ok and id then hookId=id
        else log("HOOK","pre:PickingHelper unavailable ("..tostring(id).."); tick re-apply only") end
    end
    function C.begin(e)
        C.stop()
        pcall(function()
            local rec={uuid=read(function() return tostring(e.Uuid.EntityUuid) end),entity=e,targets={},ticks=0,
                tickReapplied=0,hookRuns=0,hookReapplied=0}
            local parts,rejected={},false
            for _,t in ipairs(holders(e)) do
                local g=group(t.h)
                if g then
                    local before=describe(g)
                    rec.targets[t.name]={hadClick=read(function() return g.CanClickThrough end)==true,hadBlocker=read(function() return g.PointerBlocker end)==true}
                    local ok=apply(t.h)
                    if not ok then rejected=true end
                    parts[#parts+1]=t.name.." before="..before.." after="..describe(group(t.h))..(ok and "" or " WRITE_REJECTED")
                else parts[#parts+1]=t.name.." unavailable" end
            end
            active=rec
            if rejected or #parts==0 then log("BEGIN",(rec.uuid or "?").." "..table.concat(parts,"; ")) end
        end)
        subscribe()
    end
    local function summary(rec,event)
        log(event,"ticks="..rec.ticks.." tickReapplied="..rec.tickReapplied.." pickingRuns="..rec.hookRuns
            .." pickingReapplied="..rec.hookReapplied.." failed="..tostring(rec.failed or 0))
    end
    -- Called every Running first-person tick with a fresh entity.
    function C.tick(e)
        local rec=active
        if not rec then return end
        pcall(function()
            rec.entity=e
            rec.ticks=rec.ticks+1
            rec.tickReapplied=rec.tickReapplied+enforce(rec)
        end)
    end
    function C.stop()
        local rec=active
        if not rec then return end
        active=nil
        pcall(function()
            local e=rec.uuid and read(function() return Ext.Entity.Get(rec.uuid) end)
            local parts={}
            for _,t in ipairs(e and holders(e) or {}) do
                local saved=rec.targets[t.name]
                local g=saved and group(t.h)
                if g then
                    local ok=pcall(function()
                        local restore=g
                        if not saved.hadClick and restore.CanClickThrough then restore=restore~"CanClickThrough" end
                        if saved.hadBlocker and not restore.PointerBlocker then restore=restore|"PointerBlocker" end
                        if restore.__Value~=g.__Value then t.h.PhysicsGroup=restore.__Value end
                    end)
                    parts[#parts+1]=t.name.." "..describe(group(t.h))..(ok and "" or " RESTORE_FAILED")
                end
            end
            local failed=(rec.failed or 0)>0 or #parts==0
            for _,p in ipairs(parts) do if p:find("RESTORE_FAILED",1,true) then failed=true end end
            if failed then summary(rec,"TOTAL"); log("RESTORE",(rec.uuid or "?").." "..(#parts>0 and table.concat(parts,"; ") or "entity unavailable")) end
        end)
    end
    -- Session load: drop the engine subscription (it needs the new world).
    function C.reset()
        C.stop()
        if hookId then pcall(Ext.Entity.Unsubscribe,hookId) end
        hookId,hookTried=nil,false
    end
    return C
end
