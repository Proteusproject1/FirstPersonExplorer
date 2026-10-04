-- At 0.15 scale the walk cycle runs fast to keep world speed, so foot-plant text
-- keys fire about 1/0.15 times too often. While shrunk, blank only the audible
-- FootSoundEventName on the client character template, then replay the real
-- footstep sound on every Nth engine foot plant (N = 1/scale ratio), so sounds
-- follow normal-size stride cadence and stop when the animation stops.
-- Footprint/slide effects, AI hearing (FootHearingEffectName) and all weapon,
-- spell and voice audio are untouched. Client templates are not serialized into
-- saves. Never raises: any failure leaves footsteps silent or audible, never breaks FP.
-- Live-validated in 0.8.5: foot-plant events exist only right after the engine's
-- AnimationBlueprint system (read there via one engine hook); the game sets the
-- surface on the lowest skeleton sound emitter, so both feet play there.
return function()
    local F={saved=nil}
    local rows={}
    local PAUSE,DEDUPE=1000,60
    local hookId,hookTried=nil,false
    local function read(fn) local ok,v=pcall(fn); if ok then return v end; return nil end
    local function log(event,detail)
        rows[#rows+1]=string.format("%s\t%s\t%s",tostring(Ext.Utils.MonotonicTime()),event,tostring(detail or ""):gsub("[\r\n\t]"," "))
        if #rows>100 then table.remove(rows,1) end
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/footsteps.tsv",
            "First Person Explorer 0.9.0 footsteps (problems only)\nse_ms\tevent\tdetail\n"..table.concat(rows,"\n").."\n")
    end
    -- Re-resolve the template when restoring: by character UUID, then template id.
    -- The reference captured at mute time is a last resort; if its lifetime has
    -- expired, the guarded Id read fails and the restore is reported instead.
    local function resolve(s)
        local e=s.uuid and read(function() return Ext.Entity.Get(s.uuid) end)
        local candidates={
            e and read(function() return e.ClientCharacter.Template end),
            read(function() return Ext.ClientTemplate.GetTemplate(s.id) end),
            read(function() return Ext.ClientTemplate.GetRootTemplate(s.id) end),
            s.template}
        for i=1,4 do
            local t=candidates[i]
            if t and read(function() return tostring(t.Id) end)==s.id then return t end
        end
    end
    local function once(s,key,event,detail) if not s[key] then s[key]=true; log(event,detail) end end
    -- Is this text-key event entity our character? Compare handles, then UUIDs.
    local function mine(s,key,h)
        if key==s.mineKey then return true end
        if s.mine[key]==nil then
            s.mine[key]=read(function() return tostring(Ext.Entity.Get(h).Uuid.EntityUuid) end)==s.uuid
        end
        return s.mine[key]
    end
    -- Step events live in a per-frame buffer that is already cleared when the Lua tick
    -- runs; collect them right after AnimationBlueprint (hook) and in the tick. Each
    -- event counts once. Hooks may run on worker threads: no logging, audio or world
    -- queries here, only reads of the singleton holders resolved in the tick.
    local function collect(s)
        local now=Ext.Utils.MonotonicTime()
        for _,holder in ipairs(s.holders or {}) do
            local events=read(function() return holder.AnimationTextKeyEventsSingleton.Events end)
            for i=1,(events and read(function() return #events end) or 0) do
                local ev=events[i]
                local h=read(function() return ev.Entity end)
                local key=h~=nil and read(function() return Ext.Utils.HandleToInteger(h) end)
                local props=read(function() return ev.Event.Properties end)
                local footId=props and read(function() return props.FootID end)
                if key and footId~=nil then
                    local sig=tostring(key)..":"..tostring(footId)..":"..tostring(read(function() return ev.Event.Time end))
                    if not s.seen[sig] or now-s.seen[sig]>DEDUPE then
                        s.seen[sig]=now
                        if read(function() return props.PlaySound end)~=false and mine(s,key,h) then s.pending=s.pending+1 end
                    end
                end
            end
        end
    end
    local function onAnimation()
        local s=F.saved
        if not s or tostring(Ext.Utils.GetGameState())~="Running" then return end
        pcall(collect,s)
    end
    local function subscribe()
        if hookId or hookTried then return end
        hookTried=true
        local ok,id=pcall(Ext.Entity.OnSystemPostUpdate,"AnimationBlueprint",onAnimation)
        if ok and id then hookId=id
        else log("HOOK","post:AnimationBlueprint unavailable ("..tostring(id).."); reading in tick only") end
    end
    function F.begin(e)
        F.stop()
        local ok,err=pcall(function()
            local t=read(function() return e.ClientCharacter.Template end)
            local id=t and read(function() return tostring(t.Id) end)
            if not id then log("SKIP","client character template unavailable"); return end
            local sharing=0
            for _,other in pairs(read(function() return Ext.Entity.GetAllEntitiesWithComponent("ClientCharacter") end) or {}) do
                if read(function() return tostring(other.ClientCharacter.Template.Id) end)==id then sharing=sharing+1 end
            end
            if sharing>1 then log("SKIP","template "..id.." shared by "..sharing.." loaded characters"); return end
            local list=t.FootStepInfos
            local s={id=id,uuid=read(function() return tostring(e.Uuid.EntityUuid) end),template=t,entries={},
                acc=0.9999,foot=0,plays=0,plants=0,pending=0,failed=0,mine={},seen={},lastPlant=nil}
            for i=1,#list do
                local info=list[i]
                local sound=tostring(info.FootSoundEventName or "")
                if sound~="" then
                    s.entries[#s.entries+1]={index=i,name=tostring(info.Name),bone=tostring(info.FootBoneName),sound=sound}
                end
            end
            if #s.entries==0 then log("SKIP","template "..id.." has no footstep sound events"); return end
            -- Record ownership before writing so a partial failure can still be restored.
            F.saved=s
            for _,entry in ipairs(s.entries) do list[entry.index].FootSoundEventName="" end
            for _,entry in ipairs(s.entries) do
                if tostring(list[entry.index].FootSoundEventName)~="" then error("footstep write readback failed") end
            end
        end)
        if not ok then log("ERROR","mute: "..tostring(err)); F.stop() end
        subscribe()
    end
    function F.stop()
        local s=F.saved
        if not s then return end
        F.saved=nil
        local ok,err=pcall(function()
            local t=resolve(s)
            if not t then log("RESTORE_FAILED","template "..s.id.." unavailable; footsteps restore on game restart"); return end
            local list=t.FootStepInfos
            local restored,skipped=0,0
            for _,entry in ipairs(s.entries) do
                local info=list[entry.index]
                -- Restore only entries that still carry our blank value; another mod's
                -- change to the same entry wins, like the camera height guard.
                if info and tostring(info.Name)==entry.name and tostring(info.FootBoneName)==entry.bone
                    and tostring(info.FootSoundEventName or "")=="" then
                    info.FootSoundEventName=entry.sound; restored=restored+1
                else skipped=skipped+1 end
            end
        end)
        if not ok then log("ERROR","restore: "..tostring(err)) end
    end
    -- Session load: restore, and drop the engine subscription (it needs the new world).
    function F.reset()
        F.stop()
        if hookId then pcall(Ext.Entity.Unsubscribe,hookId) end
        hookId,hookTried=nil,false
    end
    -- Ext.ClientAudio.PostEvent needs an ENTITY (it uses that entity's Sound component).
    -- The lowest active skeleton emitter is the foot one the game updates with the
    -- surface material; both feet play there. Resolved once per entry.
    local function emitter(s,e)
        if s.emitter~=nil then return s.emitter or nil end
        local bones=read(function() return e.SkeletonSoundObjects end)
        local best=nil
        for i=1,(bones and read(function() return #bones.SoundEntities end) or 0) do
            local h=read(function() return bones.SoundEntities[i] end)
            local id=h and read(function() return Ext.Entity.Get(h).Sound.ActiveData.SoundObjectId end)
            local y=read(function() return bones.Positions[i][2] end)
            if type(id)=="number" and id~=0 and id~=-1 and type(y)=="number" and (not best or y<best.y) then best={handle=h,y=y,index=i} end
        end
        s.emitter=best or false
        if not best then log("SKIP","no sound emitter for footstep playback; footsteps stay silent") end
        return best
    end
    -- Called every Running tick while in first person, after height compensation.
    function F.tick(e,handle,ratio)
        local s=F.saved
        if not s or type(ratio)~="number" or ratio~=ratio or ratio<=0 then return end
        local ok,err=pcall(function()
            s.mineKey=read(function() return Ext.Utils.HandleToInteger(handle) end)
            -- Resolve every singleton holder here (main thread); the hook reuses the list.
            local holders={}
            for _,h in pairs(read(function() return Ext.Entity.GetAllEntitiesWithComponent("AnimationTextKeyEventsSingleton") end) or {}) do holders[#holders+1]=h end
            s.holders=holders
            if #holders==0 then once(s,"noEvents","SKIP","animation text-key events unavailable; footsteps stay silent"); return end
            collect(s)
            local now=Ext.Utils.MonotonicTime()
            for sig,at in pairs(s.seen) do if now-at>500 then s.seen[sig]=nil end end
            if s.pending==0 then return end
            local r=math.max(0.05,math.min(1,ratio))
            local plants=s.pending; s.pending=0
            -- After a pause the first step sounds immediately, like a normal-size start.
            if s.lastPlant and now-s.lastPlant>PAUSE then s.acc=0.9999 end
            s.lastPlant=now
            s.plants=s.plants+plants
            -- Sounds are played here, never inside the engine hook.
            for _=1,plants do
                s.acc=s.acc+r
                if s.acc>=1 then
                    s.acc=s.acc-1
                    s.foot=s.foot%#s.entries+1
                    local target=emitter(s,e)
                    if target then
                        local played,result=pcall(Ext.ClientAudio.PostEvent,target.handle,s.entries[s.foot].sound)
                        s.plays=s.plays+1
                        if not played or result==false then
                            s.failed=s.failed+1
                            once(s,"postFailed","ERROR","PostEvent "..s.entries[s.foot].sound..": "..tostring(result))
                        end
                    end
                end
            end
        end)
        if not ok then once(s,"tickError","ERROR","cadence: "..tostring(err)) end
    end
    return F
end
