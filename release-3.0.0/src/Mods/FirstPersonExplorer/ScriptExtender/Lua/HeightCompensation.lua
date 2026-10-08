-- CameraSwitches[1]/[2] are the exploration/combat definitions. Compensate
-- both while shrunk, so entering combat does not briefly drop eye height.
return function()
    local H={saved=nil,baselines={}}
    local function positive(v) return type(v)=="number" and v==v and v>0.001 and v<100 end
    local function scale(e)
        local n=e.GameObjectVisual and e.GameObjectVisual.Scale
        if not positive(n) then error("Height compensation: character scale unavailable") end
        return n
    end
    local function switches(index)
        local g=Ext.Utils.GetGlobalSwitches()
        local c=g and g.CameraSwitches and g.CameraSwitches[index]
        if not c then error("Height compensation: camera settings unavailable index="..index) end
        return c
    end
    local function setY(c,field,y)
        -- BG3SE marshals vec3 as a fresh Lua table. Assign the complete property.
        local v=c[field]
        c[field]={v[1],y,v[3]}
        if math.abs(c[field][2]-y)>0.0001 then
            error("Height compensation: vector write readback failed for "..field)
        end
    end
    local function identity(e) return tostring(e.Uuid.EntityUuid) end
    -- Keep numeric baselines only, never entity proxies across ticks. After release,
    -- wait for the asynchronous engine transition and a stable observed scale.
    function H.observe(e)
        local id=identity(e)
        local p=H.baselines[id]
        if not p or (H.saved and H.saved.id==id) then return end
        local ok,n=pcall(scale,e)
        if not ok then p.observed=nil; p.stableSince=nil; return end
        local now=Ext.Utils.MonotonicTime()
        if not p.observed or math.abs(n-p.observed)>0.0001 then
            p.observed=n; p.stableSince=now
        elseif now>=p.releasedAt+3000 and now-p.stableSince>=750 then
            -- No FPE request is active: BG3 owns the resulting size, including
            -- any other ability's growth/reduction. Rebaseline to that size.
            p.scale=n
        end
    end
    function H.reset() H.stop(); H.baselines={} end
    function H.begin(e)
        if H.saved then error("Height compensation already active") end
        local cameras={}
        local g=Ext.Utils.GetGlobalSwitches()
        for i=1,(g and g.CameraSwitches and g.CameraSwitches[2] and 2 or 1) do
            local c=switches(i)
            local a,b=c.DefaultTargetHeightMod[2],c.ControllerTargetHeightMod[2]
            if not positive(a) or not positive(b) then error("Height compensation: invalid NCT vertical offsets") end
            cameras[i]={a=a,b=b,appliedA=a,appliedB=b}
        end
        local id=identity(e)
        local p=H.baselines[id]
        if not p then p={scale=scale(e)}; H.baselines[id]=p end
        H.saved={id=id,scale=p.scale,cameras=cameras}
    end
    function H.stop()
        local s=H.saved
        if not s then return end
        for i,p in ipairs(s.cameras) do
            local c=switches(i)
            -- Respect a settings reload from another mod; restore only our values.
            if math.abs(c.DefaultTargetHeightMod[2]-p.appliedA)<0.0001 then setY(c,"DefaultTargetHeightMod",p.a) end
            if math.abs(c.ControllerTargetHeightMod[2]-p.appliedB)<0.0001 then setY(c,"ControllerTargetHeightMod",p.b) end
        end
        local p=H.baselines[s.id]
        p.releasedAt=Ext.Utils.MonotonicTime(); p.observed=nil; p.stableSince=nil
        H.saved=nil
    end
    function H.update(e)
        local s=H.saved
        if not s then error("Height compensation not initialized") end
        local ratio=scale(e)/s.scale
        -- Legitimate growth and reduction can cross the old 0.1..1.1 limits.
        -- Keep finite positive scale validation, but cap camera correction rather
        -- than disabling hiding. This never writes character size or other boosts.
        local correction=math.max(0.1,math.min(10,1/ratio))
        for i,p in ipairs(s.cameras) do
            local c=switches(i)
            if math.abs(c.DefaultTargetHeightMod[2]-p.appliedA)>0.0001
                or math.abs(c.ControllerTargetHeightMod[2]-p.appliedB)>0.0001 then
                error("Height compensation: camera settings changed externally index="..i)
            end
            local a,b=p.a*correction,p.b*correction
            if math.abs(a-p.appliedA)>0.0001 or math.abs(b-p.appliedB)>0.0001 then
                p.appliedA=a; setY(c,"DefaultTargetHeightMod",a)
                p.appliedB=b; setY(c,"ControllerTargetHeightMod",b)
            end
        end
        return ratio,s.cameras[1].appliedA,(s.cameras[2] or s.cameras[1]).appliedA
    end
    return H
end
