-- First Person Explorer 0.7.6.3: experimental immediate first-person visual scaling.
-- Scale boost experiment with latched body hiding. No camera transform writes.
local VERSION = "0.7.6.3 Public Release"
local DIAGNOSTICS = false
local TARGET_SCALE = 0.01
local ENTER, EXIT = 0.5, 0.8
local HIDE_COMMAND = {0.000011,0.000013,0.000017}
local SHOW_COMMAND = {0.000017,0.000013,0.000011}
local PROBE_COMMAND = {0.000019,0.000023,0.000029}
local active, owner, blocked, lastStatus = false, nil, false, ""
local hidden = {} -- Entity handle -> registered renderable identity (no retained proxies)
local lastWaiting = ""
local requireInwardScroll, outwardUntil = false, 0
local retryAt, retryError = 0, ""
local scaleClient = Ext.Require("ScaleClient.lua")()
local controllerZoomUntil, controllerAxis = 0, 0
local controllerZoomHeld=false -- NCT default: hold LeftStick to zoom, RightY to pitch otherwise.
local controllerLastZoom
local targetingReference
local lastAccessoryWarning = ""
local traceRows, traceAt, traceReason = {}, 0, ""
local function cameraTrace(c, reason, drift)
    if not DIAGNOSTICS then return end
    local now = Ext.Utils.MonotonicTime()
    if now < traceAt and reason == traceReason then return end
    traceAt, traceReason = now+250, reason
    local function vector(p)
        if not p then return "nil" end
        return tostring(p[1]) .. "," .. tostring(p[2]) .. "," .. tostring(p[3])
    end
    local ok, row = pcall(function()
        return "t=" .. now .. " reason=" .. reason .. " active=" .. tostring(active)
            .. " handle=" .. tostring(c.handle) .. " select=" .. tostring(c.b.SelectMode)
            .. " requested=" .. tostring(c.b.DistanceDestination) .. " actual=" .. tostring(c.b.DistanceCurrent)
            .. " focus=" .. vector(c.b.TargetCurrent) .. " reference=" .. vector(targetingReference)
            .. " focus_drift=" .. tostring(drift) .. " trigger=" .. tostring(c.b.Trigger)
    end)
    if not ok then return end
    traceRows[#traceRows+1]=row
    if #traceRows>80 then table.remove(traceRows,1) end
    pcall(Ext.IO.SaveFile,"FirstPersonExplorer/camera_trace.txt",VERSION .. "\n" .. table.concat(traceRows,"\n"))
end
local function read(fn) local ok, v = pcall(fn); if ok then return v end; return nil end
local function log(message)
    if not DIAGNOSTICS and not (message:match("^Loaded") or message:find("ERROR",1,true)
        or message:match("^DISABLED") or message:match("^RETRYING")) then return end
    Ext.Utils.Print("[FirstPersonExplorer] " .. message)
    pcall(Ext.IO.SaveFile, "FirstPersonExplorer/status.txt", VERSION .. "\n" .. message .. "\n")
end
local function waiting(reason)
    if not DIAGNOSTICS then return end
    if reason ~= lastWaiting then
        lastWaiting = reason
        log("WAITING: " .. reason)
        pcall(function()
            local lines = {VERSION, reason}
            for i, c in pairs(Ext.Entity.GetAllEntitiesWithComponent("GameCameraBehavior")) do
                local b = c.GameCameraBehavior
                lines[#lines + 1] = "Camera " .. tostring(i)
                lines[#lines + 1] = "Camera.Active=" .. tostring(read(function() return c.Camera.Active end))
                lines[#lines + 1] = "Camera.AcceptsInput=" .. tostring(read(function() return c.Camera.AcceptsInput end))
                for _, field in ipairs({"IsPaused", "SelectMode", "TacticalMode", "PlayerInControl", "DistanceDestination", "DistanceCurrent", "Distance", "Zoom", "Target", "Trigger", "TrackTarget"}) do
                    lines[#lines + 1] = field .. "=" .. tostring(read(function() return b[field] end))
                end
                for _, field in ipairs({"Target", "Trigger", "TrackTarget"}) do
                    local e = read(function() return Ext.Entity.Get(b[field]) end)
                    lines[#lines + 1] = field .. " HasVisual=" .. tostring(read(function() return e.Visual.Visual ~= nil end))
                        .. " HasClientCharacter=" .. tostring(read(function() return e.ClientCharacter ~= nil end))
                        .. " Combat=" .. tostring(read(function() return e.IsInCombat ~= nil end))
                        .. " DialogId=" .. tostring(read(function() return e.DialogState.DialogId end))
                end
            end
            Ext.IO.SaveFile("FirstPersonExplorer/activation_probe.txt", table.concat(lines, "\n"))
        end)
    end
end
local function entity(handle)
    return read(function() return Ext.Entity.Get(Ext.Utils.IntegerToHandle(handle)) end)
end
local function root(e) return read(function() return e.Visual.Visual end) end
local function scale(r)
    local s = r.WorldTransform.Scale
    return {s[1], s[2], s[3]}
end
local function nativeCommand(r, command)
    local before = scale(r)
    r:SetWorldScale(command)
    local after = scale(r)
    -- The native bridge consumes the command without changing transforms.
    -- If it is missing or this object type is unsupported, undo the probe immediately.
    if before[1] ~= after[1] or before[2] ~= after[2] or before[3] ~= after[3] then
        r:SetWorldScale(before)
        error("Native rendering bridge unavailable for " .. tostring(r) .. "; original scale restored. Check the native DLL log and launch DirectX 11.")
    end
end
local function sameScale(a,b)
    for i=1,3 do if math.abs(a[i]-b[i]) > math.max(0.0000001,math.abs(b[i])*0.00001) then return false end end
    return true
end
local function setScaleChecked(r, value)
    r:SetWorldScale(value)
    if not sameScale(scale(r),value) then error("Targeting scale readback failed: " .. tostring(r)) end
end
local function restoreScale(r, saved)
    if saved.original then
        setScaleChecked(r,saved.original)
        saved.original, saved.applied = nil, nil
    end
end
local function describe(v, attachment)
    local resource = read(function() return v.VisualResource end)
    local parts = {tostring(read(function() return resource.SourceFile end) or "")}
    local tags = read(function() return resource.Tags end) or {}
    for _, tag in pairs(tags) do parts[#parts + 1] = tostring(tag) end
    if read(function() return attachment.Flags.Hair end) == true then parts[#parts + 1] = "hair" end
    return string.lower(table.concat(parts, "|")):gsub("\\", "/")
end
local function walk(e, callback)
    local start = root(e)
    if not start then return end
    local visited = {}
    local function visit(v, path, inheritedAccessory, attachment, depth)
        if depth > 16 then error("Visual tree exceeds depth limit") end
        local id = tostring(v)
        if visited[id] then return end
        visited[id] = true
        -- BG3SE exposes Effect as a distinct polymorphic Visual subclass.
        -- Its entire subtree belongs to the effect, not the character mesh.
        -- Do not infer effects from missing assets or generic RenderableObject names.
        local visualType = read(function() return Ext.Types.GetObjectType(v) end)
        if visualType == "Effect" or id:match("^Effect %(") then return end
        local desc = describe(v, attachment)
        local target = true
        -- Only positively identified attached weapon/instrument subtrees may
        -- degrade independently. Never classify by generic RenderableObject type.
        local accessory = inheritedAccessory or (depth > 0 and
            (desc:find("/assets/weapons/",1,true) ~= nil
             or desc:find("/assets/musicalinstruments/",1,true) ~= nil))
        do
            for index, object in pairs(v.ObjectDescs or {}) do
                local r = object.Renderable
                -- Identity must survive attachment reordering while scaled.
                if r then callback(r, tostring(r), target, desc .. " visualpath=" .. path .. "/" .. tostring(index), accessory) end
            end
        end
        for i, a in pairs(v.Attachments or {}) do
            if a.Visual then visit(a.Visual, path .. "/" .. tostring(i), accessory, a, depth + 1) end
        end
    end
    visit(start, "root", false, nil, 0)
end
local function restore()
    local failures = 0
    for handle, entries in pairs(hidden) do
        local e = entity(handle)
        if e and root(e) then
            local ok = pcall(walk, e, function(r, key)
                local saved = entries[key]
                if saved then
                    local success = pcall(function()
                        -- Restore size before making the renderable visible.
                        restoreScale(r,saved)
                        nativeCommand(r, SHOW_COMMAND)
                    end)
                    if success then entries[key] = nil else failures = failures + 1 end
                end
            end)
            if not ok then failures = failures + 1 end
        end
        -- Missing renderables were destroyed/rebuilt; never write through old proxies.
        if failures == 0 then hidden[handle] = nil end
    end
    return failures == 0
end
local function leave(reason)
    local scaleOK,scaleError=pcall(scaleClient.stop)
    local changed = active or next(hidden) ~= nil
    active, owner, lastStatus, outwardUntil = false, nil, "", 0
    if not restore() then blocked = true; log("RESTORE ERROR: reload the save before continuing.")
    elseif changed then log("FP OFF: " .. reason)
    else waiting(reason) end
    if not scaleOK then blocked=true; log("HEIGHT RESTORE ERROR: "..tostring(scaleError).."; restart game before continuing") end
end
local function suppress(handle, e)
    -- Audit the complete current tree before registering anything. A missing class
    -- now reports every unsupported attachment in one run, not just the first one.
    local unsupported, skipped, accessoryWarnings = {}, {}, {}
    walk(e, function(r, key, target, desc, accessory)
        if target then
            local ok, err = pcall(nativeCommand, r, PROBE_COMMAND)
            if not ok then
                local detail = key .. " asset=" .. desc
                    .. " parent=" .. tostring(read(function() return r.Parent end)) .. " error=" .. tostring(err)
                -- Only the explicit, restored probe rejection is eligible. Setter
                -- exceptions and failures involving an already-hidden object still
                -- require full cleanup because its restoration may be incomplete.
                local existing = hidden[handle] and hidden[handle][key]
                if accessory and not existing and tostring(err):find("Native rendering bridge unavailable for ",1,true) then
                    skipped[key] = true
                    accessoryWarnings[#accessoryWarnings + 1] = detail
                else unsupported[#unsupported + 1] = detail end
            end
        end
    end)
    if #unsupported > 0 then
        pcall(Ext.IO.SaveFile, "FirstPersonExplorer/unsupported_renderables.txt", VERSION .. "\n" .. table.concat(unsupported, "\n"))
        error(tostring(#unsupported) .. " unsupported renderables; full list in unsupported_renderables.txt. No partial suppression applied this tick.")
    end
    hidden[handle] = hidden[handle] or {}
    local entries, count, lines, scaledCount, overwritten = hidden[handle], 0, {}, 0, 0
    walk(e, function(r, key, target, desc)
        if not target or skipped[key] then return end
        local saved = entries[key] or {}
        entries[key] = saved
        nativeCommand(r, HIDE_COMMAND)
        do
            if not saved.original then
                saved.original = scale(r)
                saved.applied = {saved.original[1]*TARGET_SCALE,saved.original[2]*TARGET_SCALE,saved.original[3]*TARGET_SCALE}
            elseif not sameScale(scale(r),saved.applied) then overwritten=overwritten+1 end
            setScaleChecked(r,saved.applied)
            scaledCount=scaledCount+1
        end
        count = count + 1
        if DIAGNOSTICS then lines[#lines + 1] = key .. " scale=" .. table.concat(scale(r), ",") .. " asset=" .. desc end
    end)
    if count == 0 then error("No suppressible character renderables found; no working first-person suppression") end
    local warning = table.concat(accessoryWarnings,"\n")
    if warning ~= lastAccessoryWarning then
        lastAccessoryWarning = warning
        pcall(Ext.IO.SaveFile, "FirstPersonExplorer/equipment_fallback.txt", VERSION .. "\n"
            .. (warning ~= "" and warning or "No unsupported equipment skipped."))
    end
    local status = "FP ON handle=" .. tostring(handle) .. " renderables=" .. count .. " fp_scaled=" .. scaledCount
        .. " equipment_skipped=" .. #accessoryWarnings
        .. " animation_scale_resets=" .. overwritten
    if status ~= lastStatus then
        lastStatus = status
        log(status)
        if DIAGNOSTICS then pcall(Ext.IO.SaveFile, "FirstPersonExplorer/renderables.txt", VERSION .. "\n" .. table.concat(lines, "\n")) end
    end
end
local function number(v) return type(v) == "number" and v == v and v > 0.001 and v < math.huge end
local function distance(b)
    -- Never substitute actual follow/collision distance for requested zoom.
    local requested = read(function() return b.DistanceDestination end)
    if number(requested) then return requested end
    return nil
end
local function camera()
    local choices = {}
    for _, c in pairs(Ext.Entity.GetAllEntitiesWithComponent("GameCameraBehavior")) do
        local b = c.GameCameraBehavior
        local cameraActive = read(function() return c.Camera.Active end)
        local acceptsInput = read(function() return c.Camera.AcceptsInput end)
        if b and cameraActive ~= false and (acceptsInput ~= false or (active and cameraActive == true)) and not b.IsPaused and b.TacticalMode ~= true then
            local candidates = {}
            for _, field in ipairs({"Trigger", "Target", "FollowTarget", "TrackTarget"}) do
                local h = read(function() return b[field] end)
                if h then candidates[#candidates + 1] = h end
            end
            for _, h in pairs(read(function() return b.Targets end) or {}) do candidates[#candidates + 1] = h end
            for _, h in ipairs(candidates) do
                local e = read(function() return Ext.Entity.Get(h) end)
                if e and root(e) and read(function() return e.ClientCharacter end) then
                    local handle = Ext.Utils.HandleToInteger(h)
                    local score = read(function() return e.ClientControl ~= nil end) and 100 or 0
                    if b.PlayerInControl then score = score + 1 end
                    if cameraActive == true then score = score + 10 end
                    choices[#choices + 1] = {b=b, e=e, handle=handle, score=score,
                        explorationInput=cameraActive == true and acceptsInput == true}
                    break
                end
            end
        end
    end
    table.sort(choices, function(a, b) return a.score > b.score end)
    if #choices > 1 and choices[1].score == choices[2].score and choices[1].handle ~= choices[2].handle then return nil end
    return choices[1]
end
local function tick()
    if blocked then restore(); return end
    if Ext.Utils.MonotonicTime() < retryAt then return end
    local gameState = tostring(Ext.Utils.GetGameState())
    if gameState ~= "Running" then leave("game state=" .. gameState); return end
    local c = camera()
    if not c then leave("no unambiguous exploration camera"); return end
    scaleClient.observe(c.e)
    if read(function() return c.e.IsInCombat end) ~= nil then leave("combat"); return end
    local dialog = read(function() return c.e.DialogState.DialogId end)
    if read(function() return c.e.ClientTimelineActorControl end) ~= nil then leave("cinematic actor control"); return end
    -- A positive ID alone did block the user's exploration test in 0.3.1.
    -- Only override it with affirmative evidence of an active, input-taking camera.
    if type(dialog) == "number" and dialog > 0 and not c.explorationInput then
        leave("dialogue ID=" .. tostring(dialog) .. "; exploration camera input unconfirmed"); return
    end
    local d = distance(c.b)
    if not d then leave("missing requested camera distance"); return end
    if active and outwardUntil > 0 and Ext.Utils.MonotonicTime() <= outwardUntil and d >= EXIT then
        cameraTrace(c,"outward-scroll",nil)
        requireInwardScroll = true
        leave("outward scroll reached requested distance " .. tostring(d))
        return
    end
    -- Movement, jump/cast focus drift and automatic zoom do not release the latch.
    -- Controller: only a zoom change while the right-stick Y axis is active counts.
    local now=Ext.Utils.MonotonicTime()
    if controllerLastZoom and controllerZoomHeld and (math.abs(controllerAxis)>0.2 or now<controllerZoomUntil) then
        if d>controllerLastZoom+0.015 and active and d>=EXIT then
            requireInwardScroll=true; controllerLastZoom=d; leave("controller outward zoom"); return
        elseif d<controllerLastZoom-0.015 then requireInwardScroll=false end
    end
    controllerLastZoom=d
    if c.b.SelectMode then
        local trigger=read(function() return Ext.Utils.HandleToInteger(c.b.Trigger) end)
        if not active or owner~=c.handle or trigger~=c.handle then leave("targeting without established first person"); return end
        cameraTrace(c,"targeting-latched",nil)
    else cameraTrace(c,"exploration",nil) end
    if owner and owner ~= c.handle then leave("character switch") end
    if not active and requireInwardScroll then waiting("scroll inward to re-enter first person"); return end
    if not active and d > ENTER then waiting("camera distance above entry threshold"); return end
    lastWaiting = ""
    active, owner = true, c.handle
    suppress(c.handle, c.e)
    scaleClient.update(c)
    if not c.b.SelectMode then
        targetingReference = read(function()
            local p = c.b.TargetCurrent
            return {p[1],p[2],p[3],handle=c.handle}
        end)
    end
    retryError = ""
end
Ext.Events.ControllerAxisInput:Subscribe(function(e)
    if tostring(e.Axis)=="RightY" or e.Axis==3 then
        controllerAxis=type(e.Value)=="number" and e.Value or 0
        if math.abs(controllerAxis)>0.2 then controllerZoomUntil=Ext.Utils.MonotonicTime()+200 end
    end
end)
Ext.Events.ControllerButtonInput:Subscribe(function(e)
    if tostring(e.Button)=="LeftStick" or e.Button==7 then controllerZoomHeld=e.Pressed==true end
end)
Ext.Events.MouseWheelInput:Subscribe(function(e)
    -- SDL positive Y is wheel up/inward with the user's standard NCT mouse setup.
    -- Do not consume the event: BG3/NCT must still apply the requested zoom change.
    if type(e.ScrollY) ~= "number" then return end
    if e.ScrollY > 0 then requireInwardScroll, outwardUntil = false, 0 end
    if active and e.ScrollY < 0 then
        outwardUntil = Ext.Utils.MonotonicTime() + 750
    end
end)
Ext.Events.Tick:Subscribe(function()
    local ok, err = pcall(tick)
    if not ok then
        -- A transient visual or unsupported attachment must not poison the session.
        -- Restore everything before a bounded retry; restoration failures stay fatal.
        leave("error")
        retryAt = Ext.Utils.MonotonicTime() + 1000
        if tostring(err):find("Height compensation:",1,true) then
            blocked=true -- A persistent height fault must never create a shrink/grow loop.
        end
        if blocked then log("DISABLED: height or restoration fault; reload required")
        elseif retryError ~= tostring(err) then log("RETRYING: " .. tostring(err)) end
        retryError = tostring(err)
        pcall(Ext.IO.SaveFile, "FirstPersonExplorer/last_error.txt", VERSION .. "\n" .. tostring(err))
    end
end)
Ext.Events.GameStateChanged:Subscribe(function(e)
    if tostring(e.ToState) ~= "Running" then targetingReference=nil; leave("state transition") end
end)
Ext.Events.SessionLoaded:Subscribe(function() targetingReference=nil; leave("session loaded"); scaleClient.reset(); controllerLastZoom=nil; controllerAxis=0; blocked = next(hidden) ~= nil; lastStatus = ""; retryAt = 0; retryError = "" end)
Ext.Events.ResetCompleted:Subscribe(function() leave("extender reset") end)
log("Loaded " .. VERSION .. "; native draw bridge required (DX11); enter=0.5 exit=0.8; scale boost=0.15; targeting latch; no camera writes.")
