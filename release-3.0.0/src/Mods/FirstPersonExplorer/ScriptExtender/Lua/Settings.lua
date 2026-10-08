-- Player settings from Mod Configuration Menu (MCM_blueprint.json next to meta.lsx).
-- Client side: each player's own client reads its own values. Without MCM, or before
-- MCM is ready, the defaults below apply. Values are re-read every second and on MCM's
-- save/reset/profile events. A view or held-items change is flagged so first person is
-- re-entered with it; combat on/off is sent to the server by CombatModeClient. Never raises.
-- 3.0: one "View" choice (Full body / Hidden body / Shrunken body) replaces the Full body switch
-- and the Body style; first-person combat is on by default; "In combat" (Hands and weapons by
-- default; also Weapons only, Full body, Nothing) applies to Full body; the camera stays on the head. Fixed camera mechanics: camera lock, eye at the head, eye at
-- pivot, eye distance fix, speed lead, no lean, stop push 15 cm; head, hair and headwear always
-- hidden in Full body. Four camera values apply only with "Fine-tune camera" on. S.resetAll()
-- puts every setting back to the recommended values (the MCM "Reset" button).
return function()
    local IDS={combat="first_person_combat",view="view",held="render_held_items"}
    local FULL,HIDDEN,SHRUNKEN="Full body","Hidden body","Shrunken body"
    local VIEWS={[FULL]=true,[HIDDEN]=true,[SHRUNKEN]=true}
    local DEFAULTS={combat=true,view=FULL,held=true}
    local FB_IDS={combatBody="fb_combat_body",facing="fb_facing",tune="fb_tune",
        up="fb_eye_height_cm",fwd="fb_eye_forward_cm",neck="fb_neck_cm",bob="fb_bob_pct",logs="fb_logs"}
    local COMBAT_HANDS,COMBAT_WEAPONS,COMBAT_FULL,COMBAT_NOTHING="Hands and weapons","Weapons only","Full body","Nothing"
    local COMBAT_BODY={[COMBAT_HANDS]=true,[COMBAT_WEAPONS]=true,[COMBAT_FULL]=true,[COMBAT_NOTHING]=true}
    local COMBAT_VIEW={[COMBAT_HANDS]="hands",[COMBAT_WEAPONS]="weapons",[COMBAT_FULL]="show",[COMBAT_NOTHING]="nothing"}
    -- Fixed mechanics (not settings).
    local FIXED={lock=true,anchor=true,pivot=true,eyefix=true,lead=true,safety=false,lean=0,stop=15,hide="Head + hair + headwear"}
    local FB_DEFAULTS={combatBody=COMBAT_HANDS,facing=true,tune=false,up=10,fwd=5,neck=13,bob=25,logs=false}
    local FB_RANGES={up={-30,30},fwd={-10,30},neck={0,25},bob={0,100}}
    local function styleOf(view) return view==SHRUNKEN and "Shrunken" or "Full size" end
    local S={combat=DEFAULTS.combat,view=DEFAULTS.view,style=styleOf(DEFAULTS.view),held=DEFAULTS.held,ready=false,changed={},ids=IDS,fbIds=FB_IDS,fb={}}
    for k,v in pairs(FB_DEFAULTS) do S.fb[k]=v end
    for k,v in pairs(FIXED) do S.fb[k]=v end
    S.fb.enabled=S.view==FULL
    local nextPoll=0
    local function get(id)
        if type(MCM)~="table" or type(MCM.Get)~="function" then return nil end
        local ok,v=pcall(MCM.Get,id)
        if ok then return v end
        return nil
    end
    local function note()
        local fb=S.fb
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/settings.txt",string.format(
            "True First-Person Camera 3.0.0\nsource=%s\nview=%s\nfirst_person_combat=%s\nbody_style=%s\nrender_held_items=%s\n"
            .."fb_combat_body=%s\nfb_facing=%s\nfb_tune=%s\nfb_eye_height_cm=%d\nfb_eye_forward_cm=%d\nfb_neck_cm=%d\nfb_bob_pct=%d\nfb_logs=%s\n",
            S.ready and "MCM" or "defaults (MCM not available yet)",S.view,tostring(S.combat),S.style,tostring(S.held),
            fb.combatBody,tostring(fb.facing),tostring(fb.tune),fb.up,fb.fwd,fb.neck,fb.bob,tostring(fb.logs)))
    end
    local function int(v,range,default)
        if type(v)~="number" or v~=v or v%1~=0 or v<range[1] or v>range[2] then return default end
        return math.floor(v)
    end
    function S.refresh()
        local combat,view,held=get(IDS.combat),get(IDS.view),get(IDS.held)
        local ready=combat~=nil or view~=nil or held~=nil
        -- Missing or invalid values fall back to the default for that setting only.
        local new={combat=DEFAULTS.combat,view=DEFAULTS.view,held=DEFAULTS.held}
        if type(combat)=="boolean" then new.combat=combat end
        if VIEWS[view] then new.view=view end
        if type(held)=="boolean" then new.held=held end
        new.style=styleOf(new.view)
        local fb={}
        for k,v in pairs(FIXED) do fb[k]=v end
        fb.enabled=new.view==FULL
        for _,k in ipairs({"facing","tune","logs"}) do
            local v=get(FB_IDS[k])
            if type(v)=="boolean" then fb[k]=v else fb[k]=FB_DEFAULTS[k] end
        end
        -- Camera values: the player's own only with Fine-tune on; otherwise the recommended ones.
        for k,range in pairs(FB_RANGES) do fb[k]=fb.tune and int(get(FB_IDS[k]),range,FB_DEFAULTS[k]) or FB_DEFAULTS[k] end
        local cb=get(FB_IDS.combatBody); fb.combatBody=COMBAT_BODY[cb] and cb or FB_DEFAULTS.combatBody
        local fbChanged=false
        for k,v in pairs(fb) do if S.fb[k]~=v then fbChanged=true end end
        if new.combat~=S.combat or new.view~=S.view or new.held~=S.held or ready~=S.ready or fbChanged then
            -- Record which settings changed: view/held need a first-person re-entry,
            -- combat is applied by the existing combat logic once the server confirms it.
            if new.style~=S.style then S.changed.style=true end
            if new.held~=S.held then S.changed.held=true end
            if new.combat~=S.combat then S.changed.combat=true end
            if fb.enabled~=S.fb.enabled then S.changed.fullbody=true end
            S.combat,S.view,S.style,S.held,S.ready=new.combat,new.view,new.style,new.held,ready
            S.fb=fb
            note()
        end
    end
    -- View: Full body (see your body, head hidden) / Hidden body / Shrunken body (classic views).
    function S.fullBody() return S.view==FULL end
    function S.headAnchor() return true end
    -- In combat (Full body view, camera always on the head): "hands" (what you hold + gloves, gauntlets,
    -- bracers), "weapons" (what you hold), "show" (full body) or "nothing".
    function S.combatView() return COMBAT_VIEW[S.fb.combatBody] or "hands" end
    -- Set by BootstrapClient when the DLL cannot provide the full-body camera (older native):
    -- then the classic hidden view is used, exactly like 0.9.0.
    S.noCamera=false
    function S.fullBodyInUse() return S.fullBody() and not S.noCamera end
    function S.shrunken() return S.style=="Shrunken" end
    -- Held items only exist in the full-size styles; shrunken hides everything as before.
    function S.showHeld() return S.held and not S.shrunken() end
    function S.poll(now)
        if now<nextPoll then return end
        nextPoll=now+1000
        S.refresh()
    end
    -- True when the view or held-items changed since the last call.
    function S.consumeRestyle()
        local c=S.changed.style==true or S.changed.held==true or S.changed.fullbody==true
        S.changed={}
        return c
    end
    function S.set(key,value)
        if type(MCM)~="table" or type(MCM.Set)~="function" then return false end
        local ok=pcall(MCM.Set,IDS[key],value)
        if ok then S.refresh() end
        return ok
    end
    -- "Reset to recommended settings": every setting of this mod back to its recommended value.
    -- Returns how many settings were written (0 without MCM).
    function S.resetAll()
        if type(MCM)~="table" or type(MCM.Set)~="function" then return 0 end
        local n=0
        local function put(id,value) if pcall(MCM.Set,id,value) then n=n+1 end end
        put(IDS.view,DEFAULTS.view); put(IDS.combat,DEFAULTS.combat); put(IDS.held,DEFAULTS.held)
        for k,id in pairs(FB_IDS) do put(id,FB_DEFAULTS[k]) end
        S.refresh()
        return n
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
