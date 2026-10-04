-- Server-side observation only. UUIDs and full server handles correlate with
-- the native trace; no component, status, camera, or scale writes occur here.
return function()
    local D={rows={},pending={},sequence=0,nextSample=0,nextFlush=0,dirty=false}
    local PATH="FirstPersonExplorer/scale_transition_diag_07610.tsv"
    local HEADER="FPE exploration 0.7.6.10 EXPERIMENTAL INSTANT SCALE\n"
        .."seq\tse_ms\tevent\tuuid\tserver_entity_hex\tvisual_scale\thas_scale_change\thas_fpe_status\tscale_boosts\treason\n"
    local function read(fn) local ok,v=pcall(fn); if ok then return v end end
    local function safe(v) return tostring(v):gsub("[\t\r\n]"," ") end
    local function scaleBoosts(e)
        local list=read(function() return e.BoostsContainer.Boosts end)
        if not list then return "unavailable" end
        local values={}
        for _,entry in pairs(list) do
            if tostring(entry.Type)=="ScaleMultiplier" then
                for _,h in pairs(entry.Boosts or {}) do
                    local b=read(function() return Ext.Entity.Get(h) end)
                    local value=read(function() return b.ScaleMultiplierBoost.Multiplier end)
                    values[#values+1]=safe(value)
                end
            end
        end
        table.sort(values)
        return #values>0 and table.concat(values,",") or "none"
    end
    local function row(uuid,event,reason)
        local e=read(function() return Ext.Entity.Get(uuid) end)
        local handle=read(function()
            return string.format("%016x",Ext.Utils.HandleToInteger(Ext.Entity.UuidToHandle(uuid)))
        end) or "unavailable"
        local visual=read(function() return e.GameObjectVisual.Scale end)
        local transition=read(function() return e:HasRawComponent("esv::ScaleChangeComponent") end)
        local status=read(function() return Osi.HasActiveStatus(uuid,"FPE_FP_SCALE_TEST_077") end)
        D.sequence=D.sequence+1
        D.rows[#D.rows+1]=table.concat({D.sequence,Ext.Utils.MonotonicTime(),safe(event),safe(uuid),handle,
            safe(visual),safe(transition),safe(status),scaleBoosts(e),safe(reason or "")},"\t")
        if #D.rows>4000 then table.remove(D.rows,1) end
        D.dirty=true
    end
    function D.flush()
        if D.dirty then
            local ok=pcall(Ext.IO.SaveFile,PATH,HEADER..table.concat(D.rows,"\n").."\n")
            if ok then D.dirty=false end
        end
    end
    function D.track(uuid,event,reason)
        local now=Ext.Utils.MonotonicTime()
        local count=0
        for _ in pairs(D.pending) do count=count+1 end
        if D.pending[uuid] or count<8 then D.pending[uuid]={untilTime=now+6000} end
        row(uuid,event,reason)
        D.flush()
    end
    function D.tick()
        local now=Ext.Utils.MonotonicTime()
        if now>=D.nextSample then
            D.nextSample=now+50
            for uuid,p in pairs(D.pending) do
                row(uuid,"SAMPLE")
                if now>=p.untilTime then
                    if read(function() return Osi.HasActiveStatus(uuid,"FPE_FP_SCALE_TEST_077") end)==1 then
                        p.untilTime=now+6000
                    else D.pending[uuid]=nil end
                end
            end
        end
        if now>=D.nextFlush then D.nextFlush=now+250; D.flush() end
    end
    function D.reset()
        D.pending={}; D.nextSample=0; D.nextFlush=0
        row("-","SESSION_RESET"); D.flush()
    end
    D.reset()
    return D
end
