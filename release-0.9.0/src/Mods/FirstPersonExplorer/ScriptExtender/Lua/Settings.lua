-- Player settings from Mod Configuration Menu (MCM_blueprint.json next to meta.lsx).
-- Client side: each player's own client reads its own values. Without MCM, or before
-- MCM is ready, the defaults below apply. Values are re-read every second and on MCM's
-- save/reset/profile events. A body-style or held-items change is flagged so first
-- person is re-entered with it; combat on/off is sent to the server by
-- CombatModeClient. Never raises.
return function()
    local IDS={combat="first_person_combat",style="body_style",held="render_held_items"}
    local DEFAULTS={combat=false,style="Full size",held=true}
    local STYLES={["Full size"]=true,["Shrunken"]=true}
    local S={combat=DEFAULTS.combat,style=DEFAULTS.style,held=DEFAULTS.held,ready=false,changed={},ids=IDS}
    local nextPoll=0
    local function get(id)
        if type(MCM)~="table" or type(MCM.Get)~="function" then return nil end
        local ok,v=pcall(MCM.Get,id)
        if ok then return v end
        return nil
    end
    local function note()
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/settings.txt",string.format(
            "First Person Explorer 0.9.0\nsource=%s\nfirst_person_combat=%s\nbody_style=%s\nrender_held_items=%s\n",
            S.ready and "MCM" or "defaults (MCM not available yet)",tostring(S.combat),S.style,tostring(S.held)))
    end
    function S.refresh()
        local combat,style,held=get(IDS.combat),get(IDS.style),get(IDS.held)
        local ready=combat~=nil or style~=nil or held~=nil
        -- Missing or invalid values fall back to the default for that setting only.
        local new={combat=DEFAULTS.combat,style=DEFAULTS.style,held=DEFAULTS.held}
        if type(combat)=="boolean" then new.combat=combat end
        if STYLES[style] then new.style=style end
        if type(held)=="boolean" then new.held=held end
        if new.combat~=S.combat or new.style~=S.style or new.held~=S.held or ready~=S.ready then
            -- Record which settings changed: style/held need a first-person re-entry,
            -- combat is applied by the existing combat logic once the server confirms it.
            if new.style~=S.style then S.changed.style=true end
            if new.held~=S.held then S.changed.held=true end
            if new.combat~=S.combat then S.changed.combat=true end
            S.combat,S.style,S.held,S.ready=new.combat,new.style,new.held,ready
            note()
        end
    end
    function S.shrunken() return S.style=="Shrunken" end
    -- Held items only exist in the full-size style; shrunken hides everything as before.
    function S.showHeld() return S.held and not S.shrunken() end
    function S.poll(now)
        if now<nextPoll then return end
        nextPoll=now+1000
        S.refresh()
    end
    -- True when the body style or held-items setting changed since the last call.
    function S.consumeRestyle()
        local c=S.changed.style==true or S.changed.held==true
        S.changed={}
        return c
    end
    function S.set(key,value)
        if type(MCM)~="table" or type(MCM.Set)~="function" then return false end
        local ok=pcall(MCM.Set,IDS[key],value)
        if ok then S.refresh() end
        return ok
    end
    pcall(function()
        local events=Ext.ModEvents and Ext.ModEvents.BG3MCM
        if not events then return end
        for _,name in ipairs({"MCM_Setting_Saved","MCM_Setting_Reset","MCM_Profile_Activated"}) do
            pcall(function()
                events[name]:Subscribe(function(payload)
                    if name=="MCM_Profile_Activated" or (type(payload)=="table" and payload.modUUID==ModuleUUID) then S.refresh() end
                end)
            end)
        end
    end)
    S.refresh()
    S.changed={}
    note()
    return S
end
