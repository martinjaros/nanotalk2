local nanotalk_proto = Proto("nanotalk", "Nanotalk distributed multimedia service")

nanotalk_proto.dissector = function(buffer, info, tree)
    if buffer:len() > 1 then
        local msgtype = buffer(0, 1):uint()

        if msgtype == 0xC0 then
            if buffer:len() == 41 then
                srcid = tostring(buffer(1, 20))
                dstid = tostring(buffer(21, 20))
                info.cols.protocol = "NANOTALK"
                info.cols.info = "Lookup request " .. srcid:sub(0,16) .. " -> " .. dstid:sub(0,16)

                local subtree = tree:add(nanotalk_proto, buffer())
                subtree:add(buffer(0, 1), "Type: Lookup request (0xC0)")
                subtree:add(buffer(1, 20), "Source ID: " .. srcid)
                subtree:add(buffer(21, 20), "Destination ID: " .. dstid)
            end
        elseif msgtype == 0xC1 then
            if buffer:len() >= 41 and (buffer:len() - 41) % 26 == 0 then
                srcid = tostring(buffer(1, 20))
                dstid = tostring(buffer(21, 20))
                info.cols.protocol = "NANOTALK"
                info.cols.info = "Lookup response " .. srcid:sub(0,16) .. " -> " .. dstid:sub(0,16)

                local subtree = tree:add(nanotalk_proto, buffer())
                subtree:add(buffer(0, 1), "Type: Lookup response (0xC1)")
                subtree:add(buffer(1, 20), "Source ID: " .. srcid)
                subtree:add(buffer(21, 20), "Destination ID: " .. dstid)

                for i = 41, buffer:len() - 1, 26 do
                    local list = subtree:add(buffer(i, 26), "Node")
                    list:add(buffer(i, 20), "ID: " .. tostring(buffer(i, 20)))
                    list:add(buffer(i + 20, 2), "Port: " .. buffer(i + 20, 2):uint())
                    list:add(buffer(i + 22, 4), "Address: " .. tostring(buffer(i + 22, 4):ipv4()))
                end
            end
        elseif msgtype == 0xC2 then
            if buffer:len() == 53 then
                info.cols.protocol = "NANOTALK"
                info.cols.info = "Connection request"

                local subtree = tree:add(nanotalk_proto, buffer())
                subtree:add(buffer(0, 1), "Type: Connection request (0xC2)")
                subtree:add(buffer(1, 20), "Source ID: " .. tostring(buffer(1, 20)))
                subtree:add(buffer(21, 32), "Nonce: " .. tostring(buffer(21, 32)))
            end
        elseif msgtype == 0xC3 then
            if buffer:len() == 97 then
                info.cols.protocol = "NANOTALK"
                info.cols.info = "Connection response"

                local subtree = tree:add(nanotalk_proto, buffer())
                subtree:add(buffer(0, 1), "Type: Connection response (0xC3)")
                subtree:add(buffer(1, 32), "Public key: " .. tostring(buffer(1, 32)))
                subtree:add(buffer(33, 32), "Source nonce: " .. tostring(buffer(33, 32)))
                subtree:add(buffer(65, 32), "Peer nonce: " .. tostring(buffer(65, 32)))
            end
        end
    end
end

local dissector_table = DissectorTable.get("udp.port")
dissector_table:add(5004, nanotalk_proto)
