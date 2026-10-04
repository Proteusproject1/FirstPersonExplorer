-- First-person combat choice. Each player's own client reads its MCM setting and
-- reports it ("set:0"/"set:1"); the server applies it to that player's characters only
-- (a peer can never change another player's choice). Replies echo the applied value.
-- 0.8.x stored the choice per profile in combat_preferences.json and exposed a toggle
-- passive: the file is now only read (reply to "legacy") so each client can carry its
-- old choice into MCM once, and the passive and the older castable action are removed
-- from characters as they are selected. Their stat entries stay in the PAK so saves
-- that contain them still load.
return function(isRunning,onDisabled)
    local M={}
    local CHANNEL="FPE_CombatMode"
    local PASSIVE="FPE_CombatFirstPerson"
    local LEGACY_SPELL="Shout_FPE_ToggleCombat"
    local PATH="FirstPersonExplorer/combat_preferences.json"
    local legacy,session,users,retired={},{},{},{}
    local nextCheck=0
    local function read(fn) local ok,v=pcall(fn); if ok then return v end; return nil end
    local function uuid(v) return type(v)=="string" and v:sub(-36):lower() or nil end
    local function userId(peer) return (peer & 0xffff0000) | 1 end
    local function profile(user)
        local id=read(function() return Osi.GetUserProfileID(user) end)
        if type(id)=="string" and #id>0 and #id<=128 and id~="00000000-0000-0000-0000-000000000000" then return id end
        return nil
    end
    function M.load()
        legacy={}; session={}; users={}; retired={}; nextCheck=0
        local text=read(function() return Ext.IO.LoadFile(PATH) end)
        if not text then return end
        local parsed=read(function() return Ext.Json.Parse(text) end)
        if type(parsed)=="table" and parsed.schema==1 and type(parsed.players)=="table" then
            for k,v in pairs(parsed.players) do
                if type(k)=="string" and #k<=128 and type(v)=="boolean" then legacy[k]=v end
            end
        end
    end
    local function enabledUser(user) return session[user]==true end
    function M.enabled(peer) return enabledUser(userId(peer)) end
    local function reply(user,payload) Ext.ServerNet.PostMessageToUser(user,CHANNEL,payload) end
    -- Remove the retired passive and castable action, at most once every 2 s per character.
    local function retire(id)
        if not id or read(function() return Osi.IsPartyMember(id,0) end)~=1 then return end
        local now=Ext.Utils.MonotonicTime()
        if retired[id] and now-retired[id]<2000 then return end
        retired[id]=now
        if read(function() return Osi.HasSpell(id,LEGACY_SPELL) end)==1 then pcall(Osi.RemoveSpell,id,LEGACY_SPELL,1) end
        if read(function() return Osi.HasPassive(id,PASSIVE) end)==1 then pcall(Osi.RemovePassive,id,PASSIVE) end
    end
    function M.tick()
        local now=Ext.Utils.MonotonicTime()
        if not isRunning() or now<nextCheck then return end
        nextCheck=now+500
        for user in pairs(users) do
            local id=uuid(read(function() return Osi.GetCurrentCharacter(user) end))
            if id then pcall(retire,id) end
        end
    end
    Ext.RegisterNetListener(CHANNEL,function(_,payload,peer)
        if not isRunning() or type(peer)~="number" or type(payload)~="string" then return end
        local user=userId(peer); users[user]=true
        local ok,err=pcall(function()
            if payload=="set:1" or payload=="set:0" then
                local value=payload=="set:1"
                if session[user]~=value then
                    session[user]=value
                    if not value then onDisabled(user) end
                end
                reply(user,value and "1" or "0")
            elseif payload=="get" then
                reply(user,enabledUser(user) and "1" or "0")
            elseif payload=="legacy" then
                local key=profile(user)
                local v=key and legacy[key]
                reply(user,"legacy:"..(v==true and "1" or (v==false and "0" or "none")))
            end
        end)
        if not ok then Ext.Utils.Print("[FirstPersonExplorer] Combat mode message failed: "..tostring(err)) end
    end)
    M.load()
    return M
end
