local M = {}

local valid_widths = {
    ["20"] = true,
    ["40"] = true,
    ["80"] = true,
    ["160"] = true,
    ["320"] = true
}

function M.apply(config, dats)
    local htmode = config.htmode
    local width = type(htmode) == "string" and
        string.match(htmode, "%d+") or nil

    if not width or not valid_widths[width] then
        return nil, "invalid bandwidth"
    end

    local is_eht = string.sub(htmode, 1, 3) == "EHT"
    if width == "320" and (config.band ~= "6g" or not is_eht) then
        return nil, "320 MHz requires 6 GHz EHT mode"
    end

    dats.HT_BW = 0
    dats.VHT_BW = 0

    if is_eht then
        dats.EHT_ApBw = 0
    end

    if width == "40" then
        dats.HT_BW = 1
        if is_eht then
            dats.EHT_ApBw = 1
        end
        dats.HT_BSSCoexistence = config.noscan == "1" and 0 or 1
    elseif width == "80" then
        dats.HT_BW = 1
        dats.VHT_BW = 1
        if is_eht then
            dats.EHT_ApBw = 2
        end
    elseif width == "160" then
        dats.HT_BW = 1
        dats.VHT_BW = 2
        if is_eht then
            dats.EHT_ApBw = 3
        end
    elseif width == "320" then
        dats.HT_BW = 1
        dats.VHT_BW = 2
        dats.EHT_ApBw = 4
        dats.HT_EXTCHA = 0
    end

    return true
end

return M
