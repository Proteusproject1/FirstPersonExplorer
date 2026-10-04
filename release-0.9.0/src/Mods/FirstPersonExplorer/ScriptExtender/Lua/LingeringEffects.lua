-- Full-size style: hide the visuals of lingering buffs on the first-person character.
-- Uses the engine's own per-character set (StatusVisualDisabled); 0.8.9 live: Bless
-- disappeared, the set was never reset by the engine, the statuses keep working.
-- Only statuses applied by this character or a party member are hidden; statuses from
-- enemies, the world or an unknown cause stay visible as danger cues (Garrote,
-- burning, darkness). The set is restored exactly on exit / character switch.
-- Client only. Never raises.
return function()
    local L={}
    local rows={}
    local active=nil
    local function read(fn) local ok,v=pcall(fn); if ok then return v end; return nil end
    local function log(event,detail)
        rows[#rows+1]=string.format("%s\t%s\t%s",tostring(Ext.Utils.MonotonicTime()),event,tostring(detail or ""):gsub("[\r\n\t]"," "))
        if #rows>150 then table.remove(rows,1) end
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/buffs.tsv",
            "First Person Explorer 0.9.0 buff visuals (problems only)\nse_ms\tevent\tdetail\n"..table.concat(rows,"\n").."\n")
    end
    local function names(set)
        local out,seen={},{}
        pcall(function()
            for k,v in pairs(set) do
                for _,x in ipairs({k,v}) do
                    if type(x)=="string" and not seen[x] then seen[x]=true; out[#out+1]=x end
                end
            end
        end)
        table.sort(out)
        return out
    end
    -- self / party / other / none, from the status entity's StatusCause.
    local function causeOf(rec,statusHandle)
        local cause=read(function() return Ext.Entity.Get(statusHandle).StatusCause.Cause end)
        if cause==nil then return "none" end
        local ce=read(function() return Ext.Entity.Get(cause) end)
        if not ce then return "none" end
        if read(function() return tostring(ce.Uuid.EntityUuid) end)==rec.uuid then return "self" end
        if read(function() return ce.PartyMember end)~=nil then return "party" end
        return "other"
    end
    -- Friendly statuses currently on the character, and a readable summary of all.
    local function friendly(rec,e)
        local list=read(function() return e.StatusContainer.Statuses end)
        local hide,summary,seen={},{},{}
        pcall(function()
            for h,name in pairs(list or {}) do
                local s=tostring(name)
                local c=causeOf(rec,h)
                summary[#summary+1]=s.."("..c..")"
                if (c=="self" or c=="party") and not seen[s] then seen[s]=true; hide[#hide+1]=s end
            end
        end)
        table.sort(hide); table.sort(summary)
        return hide,summary
    end
    local function apply(rec,e)
        local hide,summary=friendly(rec,e)
        local comp=read(function() return e.StatusVisualDisabled end)
        if not comp and #hide>0 and not rec.createTried then
            rec.createTried=true
            local ok=pcall(function() e:CreateComponent("StatusVisualDisabled") end)
            comp=read(function() return e.StatusVisualDisabled end)
            rec.created=ok and comp~=nil
            if not rec.created then log("ERROR","StatusVisualDisabled could not be created") end
        end
        if not comp then return end
        if rec.original==nil then rec.original=names(read(function() return comp.Visuals end) or {}) end
        local want,seen={},{}
        for _,n in ipairs(rec.original) do if not seen[n] then seen[n]=true; want[#want+1]=n end end
        for _,n in ipairs(hide) do if not seen[n] then seen[n]=true; want[#want+1]=n end end
        table.sort(want)
        local current=names(read(function() return comp.Visuals end) or {})
        if table.concat(current,",")==table.concat(want,",") then return end
        local ok,err=pcall(function() comp.Visuals=want end)
        rec.writes=rec.writes+1
        if not ok then log("ERROR","hide write failed: "..tostring(err).." statuses="..table.concat(summary,",")) end
    end
    function L.begin(e)
        L.stop()
        pcall(function() active={uuid=read(function() return tostring(e.Uuid.EntityUuid) end),writes=0} end)
    end
    function L.tick(e)
        local rec=active
        if not rec then return end
        pcall(apply,rec,e)
    end
    function L.stop()
        local rec=active
        if not rec then return end
        active=nil
        pcall(function()
            local e=rec.uuid and Ext.Entity.Get(rec.uuid)
            if not e then return end
            if rec.created then e:RemoveComponent("StatusVisualDisabled")
            elseif rec.original~=nil then e.StatusVisualDisabled.Visuals=rec.original end
        end)
    end
    function L.reset() L.stop() end
    return L
end
