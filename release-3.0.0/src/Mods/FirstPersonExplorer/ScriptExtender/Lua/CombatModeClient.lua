-- Reports this player's MCM "first-person combat" choice to the server every 500 ms;
-- the server's reply is the applied value (the client never assumes it). A missing
-- reply for 2 s means OFF. On the first run with MCM, the choice saved by 0.8.x (the
-- retired passive) is carried into MCM once, then a marker file prevents repeats.
return function(settings)
    local M={enabled=false,received=0,nextPoll=0,migration=nil}
    local MARKER="FirstPersonExplorer/mcm_carryover.txt"
    local function migrated() return type(Ext.IO.LoadFile(MARKER))=="string" end
    local function mark(detail) pcall(Ext.IO.SaveFile,MARKER,"First Person Explorer 0.9.0 carry-over\n"..detail.."\n") end
    Ext.RegisterNetListener("FPE_CombatMode",function(_,payload)
        if payload=="0" or payload=="1" then
            M.enabled=payload=="1"; M.received=Ext.Utils.MonotonicTime()
        elseif (payload=="legacy:1" or payload=="legacy:0" or payload=="legacy:none") and M.migration=="asked" then
            M.migration="done"
            if payload=="legacy:1" and not settings.combat then settings.set("combat",true) end
            mark(payload)
        end
    end)
    function M.poll()
        local now=Ext.Utils.MonotonicTime()
        if now>=M.nextPoll then
            M.nextPoll=now+500
            if M.migration==nil and settings.ready then
                local ok,done=pcall(migrated)
                M.migration=(ok and done) and "done" or "asked"
            end
            if M.migration=="asked" then Ext.ClientNet.PostMessageToServer("FPE_CombatMode","legacy") end
            Ext.ClientNet.PostMessageToServer("FPE_CombatMode",settings.combat and "set:1" or "set:0")
        end
        if now-M.received>2000 then M.enabled=false end
    end
    function M.reset() M.enabled=false; M.received=0; M.nextPoll=0 end
    function M.resume() M.received=Ext.Utils.MonotonicTime(); M.nextPoll=0 end
    return M
end
