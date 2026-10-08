-- The camera's Target/Trigger may be a victim or spell focus. Only the server's
-- selected player character may become our owner. Store UUIDs, never proxies.
return function()
    local S={uuid=nil,combat=false,received=0,nextPoll=0}
    Ext.RegisterNetListener("FPE_CombatSelection",function(_,payload)
        if type(payload)~="string" then return end
        local id,combat=payload:match("^([%x%-]+):([01])$")
        if id and #id==36 then
            S.uuid,S.combat,S.received=id,combat=="1",Ext.Utils.MonotonicTime()
        elseif payload=="none" then S.uuid=nil end
    end)
    function S.poll()
        local now=Ext.Utils.MonotonicTime()
        if now>=S.nextPoll then
            S.nextPoll=now+150
            Ext.ClientNet.PostMessageToServer("FPE_CombatSelection","get")
        end
    end
    function S.get()
        if not S.uuid or Ext.Utils.MonotonicTime()-S.received>1500 then return nil end
        local e=Ext.Entity.Get(S.uuid)
        if not e or not e.ClientCharacter then return nil end
        return e,S.combat
    end
    function S.reset() S.uuid=nil; S.combat=false; S.received=0; S.nextPoll=0 end
    function S.resume()
        -- Saving pauses server replies too. Allow one fresh reply interval before
        -- treating the pre-save snapshot as stale; do not cycle scale on resume.
        if S.uuid then S.received=Ext.Utils.MonotonicTime() end
        S.nextPoll=0
    end
    return S
end
