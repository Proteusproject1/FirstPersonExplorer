-- CameraSwitches[1] is exploration. Its target-height vectors map to the
-- same normal/Alt offset fields used by standalone NCT. Change Y only.
return function()
    local H={saved=nil,baselines={}}
    local function positive(v) return type(v)=="number" and v==v and v>0.001 and v<100 end
    local function scale(e)
        local n=e.GameObjectVisual and e.GameObjectVisual.Scale
        if not positive(n) then error("Height compensation: character scale unavailable") end
        return n
    end
    local function switches()
        local g=Ext.Utils.GetGlobalSwitches()
        local c=g and g.CameraSwitches and g.CameraSwitches[1]
        if not c then error("Height compensation: exploration camera settings unavailable") end
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
        local c=switches()
        local a,b=c.DefaultTargetHeightMod[2],c.ControllerTargetHeightMod[2]
        if not positive(a) or not positive(b) then error("Height compensation: invalid NCT vertical offsets") end
        local id=identity(e)
        local p=H.baselines[id]
        if not p then p={scale=scale(e)}; H.baselines[id]=p end
        H.saved={id=id,scale=p.scale,a=a,b=b,appliedA=a,appliedB=b}
    end
    function H.stop()
        local s=H.saved
        if not s then return end
        local c=switches()
        -- Respect a settings reload from another mod; restore only values we own.
        if math.abs(c.DefaultTargetHeightMod[2]-s.appliedA)<0.0001 then setY(c,"DefaultTargetHeightMod",s.a) end
        if math.abs(c.ControllerTargetHeightMod[2]-s.appliedB)<0.0001 then setY(c,"ControllerTargetHeightMod",s.b) end
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
        local c=switches()
        if math.abs(c.DefaultTargetHeightMod[2]-s.appliedA)>0.0001
            or math.abs(c.ControllerTargetHeightMod[2]-s.appliedB)>0.0001 then
            error("Height compensation: camera settings changed externally")
        end
        local a,b=s.a*correction,s.b*correction
        if math.abs(a-s.appliedA)>0.0001 or math.abs(b-s.appliedB)>0.0001 then
            -- Record intended writes first so cleanup handles a partial setter failure.
            s.appliedA=a; setY(c,"DefaultTargetHeightMod",a)
            s.appliedB=b; setY(c,"ControllerTargetHeightMod",b)
        end
        return ratio,c.DefaultTargetHeightMod[2],c.ControllerTargetHeightMod[2]
    end
    return H
end
