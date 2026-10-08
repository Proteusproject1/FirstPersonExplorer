-- True First-Person Camera 3.0: full body first person.
-- While full-body first person is active this module:
--   * sends the camera settings to the native 0.8.0.0 camera hooks through a small mailbox file
--     (a new sequence number every 100 ms is the heartbeat; the DLL stops correcting 400 ms after
--     the last one). It also sends whether you are in combat, the eye fix switch and your
--     character's position (the DLL checks the head it follows against it);
--   * hides the whole body while it faces away from where you look (walking toward the camera),
--     using the movement direction, or at rest the character's rotation checked against movement;
--   * measures how far the camera trails the character along the movement direction (optional
--     safety net hides the body while it does);
--   * with Diagnostic logs on: logs every tick (last 60 s kept) plus combat, facing and "camera
--     behind" events and marks, and asks the DLL for its per-frame camera log.
-- Never raises.
return function(settings)
    local F = {active=false, seq=0, marks=0, nextSend=0, rows={}, events={}, nextFlush=0,
        prev=nil, vx=0, vz=0, speed=0, accel=0, lag=0, lagAlong=0, wholeBody=false, clearSince=nil,
        open=nil, dest=nil, heads={}, handle=nil, charPending=false, feet=nil, head=nil,
        combat=false, safetyHide=false, facingHide=false, faceClearSince=nil, faceDeg=nil, faceSrc="-",
        moveDir=nil, calib=0, calibN=0}
    local MAILBOX = "FirstPersonExplorer/fullbody_camera_0778.txt"
    local TRACE = "FirstPersonExplorer/fullbody_trace.tsv"
    local EVENTS = "FirstPersonExplorer/fullbody_events.tsv"
    local MAX_ROWS, MAX_EVENTS = 3600, 2000
    -- "Behind" compares the real eye (TargetCurrent + Direction * Distance) with the character's
    -- camera root along the movement; the vanilla orbit eye sits ~10 cm behind at rest, so the
    -- threshold is above that.
    local BEHIND, CLEAR, CLEAR_MS = 0.15, 0.11, 300
    -- Facing: hide beyond 120 degrees between body and view, show again below 100 for 300 ms.
    -- Moving faster than 0.5 m/s the movement direction is the body's facing.
    local FACE_HIDE, FACE_SHOW, FACE_SHOW_MS, MOVING = 120, 100, 300, 0.5
    local function read(fn) local ok, v = pcall(fn); if ok then return v end; return nil end
    local function vec(v)
        if v == nil then return nil end
        local x, y, z = read(function() return v[1] end), read(function() return v[2] end), read(function() return v[3] end)
        if type(x) == "number" and type(y) == "number" and type(z) == "number" then return {x, y, z} end
        return nil
    end
    local function quat(v)
        local p = vec(v)
        local w = p and read(function() return v[4] end)
        if type(w) == "number" then return {p[1], p[2], p[3], w} end
        return nil
    end
    local function mm(n) return math.floor(n * 1000 + 0.5) end
    local function cm(n) return string.format("%.1f", n * 100) end
    local function state()
        local fb = settings.fb
        return string.format("anchor=%s lock=%s pivot=%s lead=%s lean=%d fwd=%d up=%d neck=%d bob=%d stop=%d hide=%s safety=%s facing=%s eyefix=%s combat_body=%s combat=%s speed=%.2f behind=%scm",
            settings.headAnchor() and "head" or "pivot", tostring(fb.lock), tostring(fb.pivot), tostring(fb.lead), fb.lean, fb.fwd, fb.up, fb.neck,
            fb.bob, fb.stop, fb.hide, tostring(fb.safety), tostring(fb.facing), tostring(fb.eyefix), settings.combatView(),
            tostring(F.combat), F.speed, cm(F.lagAlong))
    end
    local function event(kind, detail)
        if not settings.fb.logs then return end
        F.events[#F.events + 1] = string.format("%d\t%s\t%s", Ext.Utils.MonotonicTime(), kind, (tostring(detail or "")):gsub("[\r\n\t]", " "))
        if #F.events > MAX_EVENTS then table.remove(F.events, 1) end
        pcall(Ext.IO.SaveFile, EVENTS, "True First-Person Camera 3.0.0 events\nms\tevent\tdetail\n" .. table.concat(F.events, "\n") .. "\n")
    end
    local function flush()
        if not settings.fb.logs then return end
        pcall(Ext.IO.SaveFile, TRACE, "True First-Person Camera 3.0.0 trace (last 60 s)\n"
            .. "ms\tspeed_m_s\taccel_m_s2\teye_offset_cm\teye_behind_cm\twhole_body\tlock\tlead\tlean_cm\tfwd_cm\tup_cm\tfeet_y\tpivot_y\thead_x\thead_y\thead_z"
            .. "\tcombat\tzoom_requested\tfacing_deg\tfacing_from\tfacing_hide\tsafety_hide\n"
            .. table.concat(F.rows, "\n") .. "\n")
    end
    -- "FPC5 seq on lock lead lean fwd up mark pivot neck anchor bob stop x_mm y_mm z_mm
    --  combat eyefix feet_ok feet_x_mm feet_y_mm feet_z_mm logs FPC5" (parsed by the DLL's camera.c)
    local function send(on, force)
        local now = Ext.Utils.MonotonicTime()
        if not force and now < F.nextSend then return end
        F.nextSend = now + 100
        F.seq = F.seq + 1
        local fb, d, p = settings.fb, F.dest or {0, 0, 0}, F.feet or {0, 0, 0}
        local line = string.format("FPC5 %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d FPC5\n", F.seq, on and 1 or 0,
            fb.lock and 1 or 0, fb.lead and 1 or 0, fb.lean, fb.fwd, fb.up, F.marks, fb.pivot and 1 or 0, fb.neck,
            settings.headAnchor() and 1 or 0, fb.bob, fb.stop,
            mm(d[1]), mm(d[2]), mm(d[3]),
            F.combat and 1 or 0, fb.eyefix and 1 or 0, F.feet and 1 or 0, mm(p[1]), mm(p[2]), mm(p[3]), fb.logs and 1 or 0)
        pcall(Ext.IO.SaveFile, MAILBOX, line)
    end
    -- Heights above the feet, for comparing characters (pivot vs live head).
    local function heights()
        if not (F.feet and F.dest) then return "heights unavailable" end
        local s = string.format("pivot_above_feet=%scm", cm(F.dest[2] - F.feet[2]))
        if F.head then s = s .. string.format(" head_above_feet=%scm head_minus_pivot=%scm", cm(F.head[2] - F.feet[2]), cm(F.head[2] - F.dest[2])) end
        return s
    end
    -- Script Extender's reading of the head meshes' world bounds (verifies what the DLL reads):
    -- x/z centre, 16 cm below the top (crown), exactly like the DLL's anchor.
    local function headCenter()
        local mn, mx
        for _, r in ipairs(F.heads) do
            local lo = vec(read(function() return r.WorldBound.Min end))
            local hi = vec(read(function() return r.WorldBound.Max end))
            if lo and hi then
                mn = mn and {math.min(mn[1], lo[1]), math.min(mn[2], lo[2]), math.min(mn[3], lo[3])} or lo
                mx = mx and {math.max(mx[1], hi[1]), math.max(mx[2], hi[2]), math.max(mx[3], hi[3])} or hi
            end
        end
        if not mn then return nil end
        return {(mn[1] + mx[1]) / 2, mx[2] - 0.16, (mn[3] + mx[3]) / 2}
    end
    function F.setHeads(list) F.heads = type(list) == "table" and list or {} end
    local function closeEvent(now)
        if not F.open then return end
        event("BEHIND_END", string.format("duration=%dms peak=%scm", now - F.open.t, cm(F.open.peak)))
        F.open = nil
    end
    local function unit2(x, z)
        local l = math.sqrt(x * x + z * z)
        if l < 0.000001 then return nil end
        return {x / l, z / l}
    end
    -- Body facing on the ground plane: movement when moving; at rest the character's rotation
    -- (+Z forward, its sign checked against movement while running), else the last movement.
    local function bodyFacing(c)
        local q = quat(read(function() return c.e.Transform.Transform.RotationQuat end))
        local qf = q and unit2(2 * (q[1] * q[3] + q[4] * q[2]), 1 - 2 * (q[1] * q[1] + q[2] * q[2]))
        if F.speed > MOVING then
            local m = unit2(F.vx, F.vz)
            F.moveDir = m
            if qf and m and F.speed > 1.0 then
                F.calib = F.calib * 0.95 + (m[1] * qf[1] + m[2] * qf[2]) * 0.05
                F.calibN = F.calibN + 1
            end
            return m, "m"
        end
        if qf and F.calibN >= 30 and math.abs(F.calib) > 0.5 then
            local s = F.calib > 0 and 1 or -1
            return {qf[1] * s, qf[2] * s}, "q"
        end
        if F.moveDir then return F.moveDir, "l" end
        return nil, "-"
    end
    local function facingCheck(c, now)
        local dir = vec(read(function() return c.b.Direction end))
        local look = dir and unit2(-dir[1], -dir[3])
        local body, src = bodyFacing(c)
        F.faceSrc = src
        if not (look and body) then F.faceDeg = nil; return end
        local dot = math.max(-1, math.min(1, look[1] * body[1] + look[2] * body[2]))
        F.faceDeg = math.deg(math.acos(dot))
        if not settings.fb.facing then
            if F.facingHide then F.facingHide = false; event("FACING_SHOW", "facing check switched off") end
            return
        end
        if F.faceDeg > FACE_HIDE then
            F.faceClearSince = nil
            if not F.facingHide then F.facingHide = true; event("FACING_HIDE", string.format("angle=%.0f from=%s %s", F.faceDeg, src, state())) end
        elseif F.faceDeg < FACE_SHOW then
            F.faceClearSince = F.faceClearSince or now
            if F.facingHide and now - F.faceClearSince >= FACE_SHOW_MS then
                F.facingHide = false; event("FACING_SHOW", string.format("angle=%.0f from=%s", F.faceDeg, src))
            end
        else F.faceClearSince = nil end
    end
    -- Called every tick while first person is active, with the camera choice from BootstrapClient;
    -- enabled = full body in use right now (off in combat with "Body in combat: Hide").
    function F.update(c, enabled)
        if not enabled then if F.active then F.stop("full body off") end; return end
        local now = Ext.Utils.MonotonicTime()
        local combat = c.combat == true
        if not F.active then F.active = true; F.combat = combat; event("START", state())
        elseif combat ~= F.combat then F.combat = combat; event(combat and "COMBAT_START" or "COMBAT_END", state()) end
        F.dest = vec(read(function() return c.b.TargetDestination end)) or F.dest
        local cur = vec(read(function() return c.b.TargetCurrent end))
        local pos = vec(read(function() return c.e.Transform.Transform.Translate end))
        if pos and F.prev and now > F.prev.t then
            local dt = (now - F.prev.t) / 1000
            if dt < 0.5 then
                F.vx, F.vz = (pos[1] - F.prev.p[1]) / dt, (pos[3] - F.prev.p[3]) / dt
                local s = math.sqrt(F.vx * F.vx + F.vz * F.vz)
                F.accel = (s - F.speed) / dt; F.speed = s
            end
        end
        if pos then F.prev = {t=now, p=pos} end
        F.feet = pos; F.head = headCenter()
        if c.handle ~= F.handle then F.handle = c.handle; F.charPending = true; F.calib = 0; F.calibN = 0; F.moveDir = nil end
        if F.charPending and F.feet and F.dest and F.head then F.charPending = false; event("CHARACTER", heights()) end
        local dir = vec(read(function() return c.b.Direction end))
        local dist = read(function() return c.b.Distance end)
        if cur and dir and type(dist) == "number" then cur = {cur[1] + dir[1] * dist, cur[2] + dir[2] * dist, cur[3] + dir[3] * dist} end
        if F.dest and cur then
            local dx, dz = F.dest[1] - cur[1], F.dest[3] - cur[3]
            F.lag = math.sqrt(dx * dx + dz * dz)
            F.lagAlong = F.speed > 0.05 and (dx * F.vx + dz * F.vz) / F.speed or 0
        end
        -- Detector: always logs; hides the whole body only with the safety net enabled.
        if F.lagAlong > BEHIND then
            F.clearSince = nil
            if not F.open then F.open = {t=now, peak=F.lagAlong}; event("BEHIND_START", state())
            elseif F.lagAlong > F.open.peak then F.open.peak = F.lagAlong end
            if settings.fb.safety and not F.safetyHide then F.safetyHide = true; event("SAFETY_HIDE", state()) end
        elseif F.lagAlong < CLEAR then
            F.clearSince = F.clearSince or now
            if F.open then closeEvent(now) end
            if F.safetyHide and now - F.clearSince >= CLEAR_MS then F.safetyHide = false; event("SAFETY_SHOW", state()) end
        end
        if F.safetyHide and not settings.fb.safety then F.safetyHide = false; event("SAFETY_SHOW", "safety net switched off") end
        facingCheck(c, now)
        F.wholeBody = F.safetyHide or F.facingHide
        local fb = settings.fb
        local hf = F.head or {0, 0, 0}
        local zoom = read(function() return c.b.DistanceDestination end)
        if fb.logs then F.rows[#F.rows + 1] = string.format("%d\t%.3f\t%.2f\t%s\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%.3f\t%.3f\t%.3f\t%.3f\t%.3f\t%d\t%.3f\t%s\t%s\t%d\t%d", now, F.speed, F.accel,
            cm(F.lag), cm(F.lagAlong), F.wholeBody and 1 or 0, fb.lock and 1 or 0, fb.lead and 1 or 0, fb.lean, fb.fwd, fb.up,
            F.feet and F.feet[2] or 0, F.dest and F.dest[2] or 0, hf[1], hf[2], hf[3],
            F.combat and 1 or 0, type(zoom) == "number" and zoom or -1, F.faceDeg and string.format("%.0f", F.faceDeg) or "-", F.faceSrc,
            F.facingHide and 1 or 0, F.safetyHide and 1 or 0) end
        if #F.rows > MAX_ROWS then table.remove(F.rows, 1) end
        if now >= F.nextFlush then F.nextFlush = now + 2000; flush() end
        send(true, false)
    end
    -- Leaving first person (or full body switched off): the DLL stops at once, not after 400 ms.
    function F.stop(reason)
        if not F.active then return end
        F.active = false; F.wholeBody = false; F.safetyHide = false; F.facingHide = false; F.faceClearSince = nil
        F.prev = nil; F.speed = 0; F.lagAlong = 0; F.handle = nil; F.combat = false
        closeEvent(Ext.Utils.MonotonicTime())
        event("STOP", reason or "")
        send(false, true)
        flush()
    end
    -- "Mark moment" hotkey: stamps both this log and the DLL's camera log.
    function F.mark()
        F.marks = F.marks + 1
        event("MARK", string.format("#%d active=%s %s %s facing=%s from=%s", F.marks, tostring(F.active), state(), heights(),
            F.faceDeg and string.format("%.0f", F.faceDeg) or "-", F.faceSrc))
        send(F.active, true)
        if F.active then flush() end
    end
    function F.wholeBodyHidden() return F.active and F.wholeBody end
    function F.reset() F.stop("reset") end
    return F
end
