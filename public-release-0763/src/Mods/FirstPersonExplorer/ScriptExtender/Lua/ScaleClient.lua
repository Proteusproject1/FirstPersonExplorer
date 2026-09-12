local DIAGNOSTICS=false
return function()
    local S={owner=nil,nextSend=0,rows={},nextLog=0}
    local height=Ext.Require("HeightCompensation.lua")()
    local function read(fn) local ok,v=pcall(fn); if ok then return v end end
    local function vector(p) return read(function() return table.concat({p[1],p[2],p[3]},",") end) or "unavailable" end
    local function send(p) Ext.ClientNet.PostMessageToServer("FPE_Scale077",p) end
    function S.stop()
        if S.owner then pcall(send,"off"); S.owner=nil; S.nextSend=0 end
        height.stop()
    end
    function S.observe(e) height.observe(e) end
    function S.reset() S.stop(); height.reset() end
    function S.update(c)
        local id=read(function() return tostring(c.e.Uuid.EntityUuid) end)
        if not id or #id~=36 then error("Scale test: character UUID unavailable") end
        local now=Ext.Utils.MonotonicTime()
        if S.owner~=id then S.stop(); height.begin(c.e); S.owner=id; S.nextSend=0 end
        local ratio,normalY,controllerY=height.update(c.e)
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
            pcall(Ext.IO.SaveFile,"FirstPersonExplorer/scale_trace.txt","0.7.6.3\n"..table.concat(S.rows,"\n"))
        end
    end
    return S
end
