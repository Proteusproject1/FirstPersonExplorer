local DIAGNOSTICS=false
-- Body style is chosen per first-person entry from the player's settings:
--   Shrunken:  scale status + camera-height compensation + footstep cadence fix
--   Full size: no scale change; click-through targeting + friendly buff visuals hidden
-- A settings change drops out of first person, so one entry never mixes styles.
return function(settings)
    local S={owner=nil,nextSend=0,rows={},nextLog=0,mode=nil}
    local height=Ext.Require("HeightCompensation.lua")()
    local footsteps=Ext.Require("FootstepMute.lua")()
    local click=Ext.Require("ClickThrough.lua")()
    local effects=Ext.Require("LingeringEffects.lua")()
    local function read(fn) local ok,v=pcall(fn); if ok then return v end end
    local function vector(p) return read(function() return table.concat({p[1],p[2],p[3]},",") end) or "unavailable" end
    local function send(p) Ext.ClientNet.PostMessageToServer("FPE_Scale077",p) end
    function S.stop()
        if S.owner then
            if S.mode=="Shrunken" then pcall(send,"off") end
            S.owner=nil; S.nextSend=0
        end
        -- These never raise; stop them before the height restore, which can.
        footsteps.stop(); click.stop(); effects.stop()
        S.mode=nil
        height.stop()
    end
    function S.observe(e) height.observe(e) end
    function S.reset() footsteps.reset(); click.reset(); effects.reset(); S.stop(); height.reset() end
    function S.update(c)
        local id=read(function() return tostring(c.e.Uuid.EntityUuid) end)
        if not id or #id~=36 then error("Scale test: character UUID unavailable") end
        local now=Ext.Utils.MonotonicTime()
        if S.owner~=id then
            S.stop()
            S.mode=settings.shrunken() and "Shrunken" or "Full size"
            if S.mode=="Shrunken" then height.begin(c.e); footsteps.begin(c.e)
            else click.begin(c.e); effects.begin(c.e) end
            S.owner=id; S.nextSend=0
        end
        if S.mode~="Shrunken" then click.tick(c.e); effects.tick(c.e); return end
        local ratio,normalY,controllerY=height.update(c.e)
        footsteps.tick(c.e,c.handle,ratio)
        if now>=S.nextSend then send("on:"..id); S.nextSend=now+250 end
        if DIAGNOSTICS and now>=S.nextLog then
            S.nextLog=now+250
            local row="t="..now.." select="..tostring(c.b.SelectMode)
                .." baseline="..height.saved.scale.." scaleRatio="..ratio.." normalHeightMult="..normalY.." controllerHeightMult="..controllerY
                .." height="..tostring(read(function() return c.b.ControllerHeight end))
                .." visualScale="..tostring(read(function() return c.e.GameObjectVisual.Scale end))
                .." physicsScale="..vector(read(function() return c.e.Physics.Physics.Scale end))
                .." transformScale="..vector(read(function() return c.e.Transform.Transform.Scale end))
                .." category="..tostring(read(function() return c.e.ObjectSize.Size end))
                .." requested="..tostring(c.b.DistanceDestination).." actual="..tostring(c.b.DistanceCurrent)
            S.rows[#S.rows+1]=row; if #S.rows>120 then table.remove(S.rows,1) end
            pcall(Ext.IO.SaveFile,"FirstPersonExplorer/scale_trace.txt","0.9.0\n"..table.concat(S.rows,"\n"))
        end
    end
    return S
end
