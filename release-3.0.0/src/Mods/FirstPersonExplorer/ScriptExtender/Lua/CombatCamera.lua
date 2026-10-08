-- Keep engine follow interpolation; never copy animated head/world transforms.
-- Capture plain values and reacquire the camera entity on release.
return function()
    local C={saved=nil}
    local fields={DefaultDistanceMin=0.1,DefaultDistanceMax=0.2,
        ControllerDistanceMin=0.1,ControllerDistanceMax=0.2,
        TacticalDistanceMin=0.1,TacticalDistanceMax=0.2}
    local function switches()
        local g=Ext.Utils.GetGlobalSwitches()
        local s=g and g.CameraSwitches and g.CameraSwitches[2]
        if not s then error("Combat camera: combat settings unavailable") end
        return s
    end
    local function handle(v) return v and Ext.Utils.HandleToInteger(v) or nil end
    local function write(b,field,value)
        b[field]=value and Ext.Utils.IntegerToHandle(value) or nil
        if handle(b[field])~=value then error("Combat camera: rejected "..field) end
    end
    function C.active() return C.saved~=nil end
    function C.stop(outward)
        local p=C.saved
        if not p then return end
        local s=switches()
        for k,v in pairs(p.limits) do
            if math.abs(s[k]-fields[k])<0.0001 then s[k]=v end
        end
        local e=Ext.Entity.Get(Ext.Utils.IntegerToHandle(p.camera))
        local b=e and e.GameCameraBehavior
        if b then
            for _,k in ipairs({"Target","TrackTarget","FollowTarget","Trigger"}) do
                if handle(b[k])==p.owner then write(b,k,p[k]) end
            end
            if b.BlockPanning==true then b.BlockPanning=p.BlockPanning end
            if p.CameraMode~=nil and b.CameraMode~=p.CameraMode then b.CameraMode=p.CameraMode end
            if p.PlayerInControl~=nil and b.PlayerInControl~=p.PlayerInControl then b.PlayerInControl=p.PlayerInControl end
            if outward then b.DistanceDestination=3.0 end
        end
        C.saved=nil
    end
    function C.update(c)
        if not c.combat then C.stop(); return end
        if C.saved and C.saved.camera~=c.cameraHandle then C.stop() end
        if not C.saved then
            local s=switches()
            local p={camera=c.cameraHandle,owner=c.handle,limits={},BlockPanning=c.b.BlockPanning}
            for k in pairs(fields) do
                local n=s[k]
                if type(n)~="number" or n~=n then error("Combat camera: invalid "..k) end
                p.limits[k]=n
            end
            for _,k in ipairs({"Target","TrackTarget","FollowTarget","Trigger"}) do p[k]=handle(c.b[k]) end
            p.CameraMode=c.b.CameraMode
            p.PlayerInControl=c.b.PlayerInControl
            C.saved=p -- cleanup can recover a partial write
            for k,v in pairs(fields) do
                s[k]=v
                if math.abs(s[k]-v)>0.0001 then error("Combat camera: rejected "..k) end
            end
        end
        local p=C.saved
        p.owner=c.handle
        -- Pin the trigger entity as well: scripted cuts (charge attacks, target cycling)
        -- reframe the camera by swapping the camera's trigger, which bypasses the zoom limits.
        for _,k in ipairs({"Target","TrackTarget","FollowTarget","Trigger"}) do
            if handle(c.b[k])~=c.handle then write(c.b,k,c.handle) end
        end
        -- Suppress the L3 selection-cursor camera mode: it transitions the camera away from
        -- the body by design, handing camera control to the player (PlayerInControl=true).
        if c.b.SelectMode==true then c.b.SelectMode=false end
        if c.b.PlayerInControl==true then c.b.PlayerInControl=false end
        if p.CameraMode~=nil and c.b.CameraMode~=p.CameraMode then c.b.CameraMode=p.CameraMode end
        c.b.BlockPanning=true
        -- Transition requests can set the desired zoom outside the clamped range.
        -- Correct the destination only; the engine still interpolates actual zoom.
        if c.b.DistanceDestination>0.2 or c.b.DistanceDestination<0.1 then
            c.b.DistanceDestination=0.15
        end
    end
    return C
end
