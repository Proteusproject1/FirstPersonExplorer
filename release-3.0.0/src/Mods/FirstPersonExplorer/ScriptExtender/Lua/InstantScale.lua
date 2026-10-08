-- Best-effort local mailbox. A failed request never changes status lifecycle.
return function()
    local M={}
    local STATUS="FPE_FP_SCALE_TEST_077"
    local ticketPath="FirstPersonExplorer/instant_ticket_0769.txt"
    local requestPath="FirstPersonExplorer/instant_request_0769.txt"
    local pending,bases={},{}
    local sequence,nonce=0,nil
    local trace={}
    local function log(uuid,event,detail)
        trace[#trace+1]=string.format("%s\t%s\t%s\t%s",Ext.Utils.MonotonicTime(),uuid,event,tostring(detail or ""):gsub("[\r\n\t]"," "))
        if #trace>300 then table.remove(trace,1) end
        pcall(Ext.IO.SaveFile,"FirstPersonExplorer/instant_requests_07610.tsv",
            "FPE PAK 0.7.6.10; native protocol 0769\nse_ms\tuuid\tevent\tdetail\n"..table.concat(trace,"\n").."\n")
    end
    local function ticket()
        local text=Ext.IO.LoadFile(ticketPath)
        if type(text)~="string" or #text~=33 or not text:match("^[0-9a-f]+\n$") then return end
        local n,t=text:sub(1,16),text:sub(17,32)
        if nonce~=n then pending={}; bases={}; nonce=n; sequence=0 end
        return n,t,tonumber(t,16)
    end
    local function boostSafe(e,own,transitioning)
        -- Restrict this first experiment to no other ScaleMultiplier boosts.
        -- Unknown component layouts are deliberately not considered safe.
        local count=0
        for _,entry in pairs(e.BoostsContainer.Boosts) do
            if tostring(entry.Type)=="ScaleMultiplier" then
                for _,h in pairs(entry.Boosts) do
                    local value=Ext.Entity.Get(h).ScaleMultiplierBoost.Multiplier
                    if (not own and not transitioning) or math.abs(value-0.15)>0.000001 then return false end
                    count=count+1
                end
            end
        end
        -- During the already-authorized request, our status and its boost may
        -- update on different ticks. Allow zero OR one own-sized boost, but
        -- still reject any other multiplier and duplicate multipliers.
        return transitioning and count<=1 or count==(own and 1 or 0)
    end
    local function publish(n,t,now)
        sequence=sequence+1
        if sequence>=0xffffffff then pending={}; return end
        local rows={}
        for uuid,r in pairs(pending) do
            if now<r.born or now-r.born>350 then pending[uuid]=nil
            else rows[#rows+1]=r.text end
        end
        table.sort(rows)
        local header="FPE9"..n..t..string.format("%08x%x\n",sequence,#rows)
        Ext.IO.SaveFile(requestPath,header..table.concat(rows)..header)
    end
    function M.request(uuid,direction)
        local n,t,now=ticket()
        if not n then log(uuid,"SKIP","native ticket unavailable"); return end
        pending[uuid]=nil
        local ok,result=pcall(function()
            local e=Ext.Entity.Get(uuid)
            local own=Osi.HasActiveStatus(uuid,STATUS)==1
            if (direction=="I" and own) or (direction=="O" and not own) or not boostSafe(e,own) then
                bases[uuid]=nil; log(uuid,"SKIP","status or additional scale boost"); return
            end
            local raw=e:HasRawComponent("esv::ScaleChangeComponent")
            local base=bases[uuid]
            if direction=="I" and not raw then
                local scale=e.GameObjectVisual.Scale
                if type(scale)~="number" or scale~=scale or scale<0.01 or scale>100 then return end
                base={scale=scale,seen=now}; bases[uuid]=base
            end
            if not base or now<base.seen or (direction=="I" and now-base.seen>6000) then log(uuid,"SKIP","baseline unavailable or stale"); return end
            base.seen=now
            local handle=string.format("%016x",Ext.Utils.HandleToInteger(Ext.Entity.UuidToHandle(uuid)))
            local count=0; for _ in pairs(pending) do count=count+1 end
            if count>=8 then return end
            local id=sequence+1
            return {born=now,direction=direction,text=handle..t..string.format("%08x%08x%s\n",id,math.floor(base.scale*1000000+0.5),direction)}
        end)
        if ok and result then
            pending[uuid]=result; log(uuid,"AUTHORIZE",result.text)
        elseif not ok then log(uuid,"ERROR",result) end
        publish(n,t,now)
        log(uuid,"PUBLISHED",direction.." packet="..sequence)
    end
    function M.tick()
        if next(pending)==nil then return end
        local n,t,now=ticket(); if not n then return end
        local changed=false
        for uuid,r in pairs(pending) do
            local ok,safe=pcall(function()
                local own=Osi.HasActiveStatus(uuid,STATUS)==1
                return boostSafe(Ext.Entity.Get(uuid),own,true)
            end)
            if now<r.born or now-r.born>350 or not ok or not safe then
                pending[uuid]=nil; changed=true
                log(uuid,"CANCEL",not ok and tostring(safe) or (not safe and "additional scale boost" or "expired"))
            end
        end
        if changed then publish(n,t,now) end
    end
    function M.cancel()
        pending={}
        local n,t,now=ticket(); if n then publish(n,t,now) end
    end
    function M.reset() bases={}; M.cancel() end
    return M
end
