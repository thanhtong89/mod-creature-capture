-- Creature Capture Addon - Multi-Guardian Spellbook UI
-- Supports up to 4 guardian slots with target-based spellbar switching
-- Receives data from server via addon messages (LANG_ADDON whispers)

-- SavedVariables
CreatureCaptureDB = CreatureCaptureDB or {}

-- ============================================================================
-- Data Layer (4 guardian slots)
-- ============================================================================

local MAX_SLOTS = 4

local guardians = {}
for i = 0, MAX_SLOTS - 1 do
    guardians[i] = {
        guardianName = "",
        archetype = 0,  -- 0=DPS, 1=Tank, 2=Healer
        spellSlots = {0, 0, 0, 0, 0, 0, 0, 0},
        hasGuardian = false,
        creatureGuid = nil,  -- hex GUID string from server, matches UnitGUID format
    }
end

local selectedSlot = -1  -- -1 = none selected

local ARCHETYPE_NAMES = { [0] = "DPS", [1] = "Tank", [2] = "Healer" }
local ARCHETYPE_COLORS = {
    [0] = {0.8, 0.2, 0.2},   -- DPS = red
    [1] = {0.2, 0.5, 0.8},   -- Tank = blue
    [2] = {0.2, 0.8, 0.3},   -- Healer = green
}

-- ============================================================================
-- Constants
-- ============================================================================

local SLOT_SIZE = 36
local SLOT_GAP = 4
local NUM_SPELL_SLOTS = 8
local TAB_WIDTH = 60
local TAB_HEIGHT = 18
local TAB_GAP = 2
local PANEL_WIDTH = NUM_SPELL_SLOTS * SLOT_SIZE + (NUM_SPELL_SLOTS - 1) * SLOT_GAP + 20
local PANEL_HEIGHT = 95  -- taller to accommodate slot tabs

local BACKDROP_INFO = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = {left = 4, right = 4, top = 4, bottom = 4},
}

local TAB_BACKDROP = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 8, edgeSize = 8,
    insets = {left = 2, right = 2, top = 2, bottom = 2},
}

-- ============================================================================
-- Helpers
-- ============================================================================

local function MakeMovable(frame, key)
    frame:SetMovable(true)
    frame:EnableMouse(true)
    frame:RegisterForDrag("LeftButton")
    frame:SetScript("OnDragStart", function(self) self:StartMoving() end)
    frame:SetScript("OnDragStop", function(self)
        self:StopMovingOrSizing()
        local point, _, relPoint, x, y = self:GetPoint()
        CreatureCaptureDB[key] = {point, relPoint, x, y}
    end)
end

local function RestorePosition(frame, key)
    local pos = CreatureCaptureDB[key]
    if pos then
        frame:ClearAllPoints()
        frame:SetPoint(pos[1], UIParent, pos[2], pos[3], pos[4])
    end
end

-- Helper: find a spell's spellbook index by spell ID
local function FindSpellBookIndex(targetSpellId)
    local i = 1
    while true do
        local name = GetSpellName(i, BOOKTYPE_SPELL)
        if not name then break end
        local link = GetSpellLink(i, BOOKTYPE_SPELL)
        if link then
            local id = tonumber(link:match("spell:(%d+)"))
            if id == targetSpellId then
                return i
            end
        end
        i = i + 1
    end
    return nil
end

-- Helper: get spell ID from cursor info
local function GetSpellIdFromCursor()
    local infoType, id, subType = GetCursorInfo()
    if infoType == "spell" then
        local name = GetSpellName(id, subType)
        if name then
            local link = GetSpellLink(id, subType)
            if link then
                local spellId = tonumber(link:match("spell:(%d+)"))
                return spellId, name
            end
        end
    end
    return nil, nil
end

-- Helper: get currently selected slot data
local function GetSelectedData()
    if selectedSlot >= 0 and selectedSlot < MAX_SLOTS then
        return guardians[selectedSlot]
    end
    return nil
end

-- ============================================================================
-- Main Panel: Spellbook
-- ============================================================================

local spellbook = CreateFrame("Frame", "CaptureSpellbook", UIParent)
spellbook:SetWidth(PANEL_WIDTH)
spellbook:SetHeight(PANEL_HEIGHT)
spellbook:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, 180)
spellbook:SetBackdrop(BACKDROP_INFO)
spellbook:SetBackdropColor(0.05, 0.1, 0.12, 0.9)
spellbook:SetBackdropBorderColor(0.15, 0.45, 0.45, 1)
spellbook:SetFrameStrata("MEDIUM")
MakeMovable(spellbook, "spellbook")
spellbook:Hide()

-- Title
local titleText = spellbook:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
titleText:SetPoint("TOPLEFT", spellbook, "TOPLEFT", 10, -6)
titleText:SetText("Guardian Spellbook")
titleText:SetTextColor(0.3, 0.7, 0.7)

-- Archetype label (right side of title)
local archLabel = spellbook:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
archLabel:SetPoint("TOPRIGHT", spellbook, "TOPRIGHT", -10, -6)
archLabel:SetText("")
archLabel:SetTextColor(0.6, 0.6, 0.6)

-- ============================================================================
-- Slot Indicator Tabs (above the spellbar)
-- ============================================================================

local slotTabs = {}

for i = 0, MAX_SLOTS - 1 do
    local tab = CreateFrame("Button", "CaptureSlotTab" .. i, spellbook)
    tab:SetWidth(TAB_WIDTH)
    tab:SetHeight(TAB_HEIGHT)
    tab:SetPoint("TOPLEFT", spellbook, "TOPLEFT", 10 + i * (TAB_WIDTH + TAB_GAP), -18)
    tab:SetBackdrop(TAB_BACKDROP)
    tab:SetBackdropColor(0.1, 0.1, 0.1, 0.8)
    tab:SetBackdropBorderColor(0.3, 0.3, 0.3, 0.8)

    local tabText = tab:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    tabText:SetPoint("CENTER")
    tabText:SetText("[" .. (i + 1) .. "]")
    tabText:SetTextColor(0.5, 0.5, 0.5)
    tab.text = tabText

    tab:SetScript("OnClick", function()
        if guardians[i].hasGuardian then
            selectedSlot = i
            RefreshSpellbook()
        end
    end)

    tab:SetScript("OnEnter", function(self)
        if guardians[i].hasGuardian then
            GameTooltip:SetOwner(self, "ANCHOR_TOP")
            local g = guardians[i]
            local archName = ARCHETYPE_NAMES[g.archetype] or "DPS"
            GameTooltip:SetText(g.guardianName .. " (" .. archName .. ")")
            if g.creatureGuid then
                GameTooltip:AddLine("Click or target to select", 0.7, 0.7, 0.7)
            else
                GameTooltip:AddLine("Stored in Tesseract", 0.7, 0.7, 0.7)
            end
            GameTooltip:Show()
        end
    end)

    tab:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)

    slotTabs[i] = tab
end

-- ============================================================================
-- Confirmation Dialogs
-- ============================================================================

StaticPopupDialogs["CCAPTURE_UNLEARN_SLOT"] = {
    text = "Unlearn %s from slot %s?",
    button1 = "Yes",
    button2 = "No",
    OnAccept = function(self, slotData)
        SendChatMessage(".capture unlearn " .. slotData.slot, "SAY")
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
}

StaticPopupDialogs["CCAPTURE_TEACH_SLOT"] = {
    text = "Teach %s to %s?",
    button1 = "Yes",
    button2 = "No",
    OnAccept = function(self, teachData)
        SendChatMessage(".capture teach " .. teachData.slot .. " " .. teachData.spellId, "SAY")
    end,
    timeout = 0,
    whileDead = true,
    hideOnEscape = true,
}

-- ============================================================================
-- Spell Slot Buttons
-- ============================================================================

local slotButtons = {}
local xButtons = {}

for i = 1, NUM_SPELL_SLOTS do
    local slotBtn = CreateFrame("Button", "CaptureSpellSlot" .. i, spellbook)
    slotBtn:SetWidth(SLOT_SIZE)
    slotBtn:SetHeight(SLOT_SIZE)
    slotBtn:SetPoint("TOPLEFT", spellbook, "TOPLEFT", 10 + (i - 1) * (SLOT_SIZE + SLOT_GAP), -38)

    -- Border texture
    local border = slotBtn:CreateTexture(nil, "OVERLAY")
    border:SetTexture("Interface\\Buttons\\UI-ActionButton-Border")
    border:SetBlendMode("ADD")
    border:SetAlpha(0.6)
    border:SetWidth(SLOT_SIZE * 1.7)
    border:SetHeight(SLOT_SIZE * 1.7)
    border:SetPoint("CENTER", slotBtn, "CENTER")
    slotBtn.border = border

    -- Icon texture
    local icon = slotBtn:CreateTexture(nil, "ARTWORK")
    icon:SetAllPoints()
    icon:SetTexture("Interface\\Icons\\INV_Misc_QuestionMark")
    slotBtn.icon = icon

    -- Empty slot background
    local emptyBg = slotBtn:CreateTexture(nil, "BACKGROUND")
    emptyBg:SetAllPoints()
    emptyBg:SetTexture("Interface\\PaperDoll\\UI-Backpack-EmptySlot")
    emptyBg:SetAlpha(0.5)
    slotBtn.emptyBg = emptyBg

    -- Slot number label
    local numLabel = slotBtn:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    numLabel:SetPoint("BOTTOMRIGHT", slotBtn, "BOTTOMRIGHT", -2, 2)
    numLabel:SetText(i)
    numLabel:SetTextColor(0.7, 0.7, 0.7, 0.5)

    -- Enable drag-to-teach: accept spells dropped onto the slot
    slotBtn:EnableMouse(true)
    slotBtn:RegisterForDrag("LeftButton")

    -- Drag FROM slot: let player drag spell to action bar
    slotBtn:SetScript("OnDragStart", function(self)
        local d = GetSelectedData()
        if d then
            local spellId = d.spellSlots[i]
            if spellId > 0 and not InCombatLockdown() then
                local idx = FindSpellBookIndex(spellId)
                if idx then
                    PickupSpell(idx, BOOKTYPE_SPELL)
                end
            end
        end
    end)

    -- Drop ON slot: teach a spell
    slotBtn:SetScript("OnReceiveDrag", function(self)
        if InCombatLockdown() then return end
        local d = GetSelectedData()
        if not d then return end
        local spellId, spellName = GetSpellIdFromCursor()
        if spellId and spellName then
            ClearCursor()
            local dialog = StaticPopup_Show("CCAPTURE_TEACH_SLOT",
                spellName .. " (slot " .. i .. ")", d.guardianName)
            if dialog then
                dialog.data = {slot = i, spellId = spellId}
            end
        end
    end)

    -- Also handle OnClick with cursor holding a spell
    slotBtn:SetScript("OnClick", function(self, button)
        if InCombatLockdown() then return end
        local d = GetSelectedData()
        if not d then return end
        local spellId, spellName = GetSpellIdFromCursor()
        if spellId and spellName then
            ClearCursor()
            local dialog = StaticPopup_Show("CCAPTURE_TEACH_SLOT",
                spellName .. " (slot " .. i .. ")", d.guardianName)
            if dialog then
                dialog.data = {slot = i, spellId = spellId}
            end
        end
    end)

    -- Hover: tooltip + X button
    slotBtn:SetScript("OnEnter", function(self)
        local d = GetSelectedData()
        if d then
            local spellId = d.spellSlots[i]
            if spellId > 0 then
                GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
                GameTooltip:SetSpellByID(spellId)
                GameTooltip:Show()
                xButtons[i]:Show()
            end
        end
    end)
    slotBtn:SetScript("OnLeave", function(self)
        GameTooltip:Hide()
        local xBtn = xButtons[i]
        if xBtn then
            xBtn.hideTimer = 0.15
        end
    end)

    slotButtons[i] = slotBtn

    -- X (unlearn) button
    local xBtn = CreateFrame("Button", "CaptureSpellX" .. i, spellbook)
    xBtn:SetWidth(14)
    xBtn:SetHeight(14)
    xBtn:SetPoint("TOPRIGHT", slotBtn, "TOPRIGHT", 3, 3)
    xBtn:SetFrameLevel(slotBtn:GetFrameLevel() + 2)
    xBtn.mouseOver = false

    local xBg = xBtn:CreateTexture(nil, "BACKGROUND")
    xBg:SetAllPoints()
    xBg:SetTexture("Interface\\TargetingFrame\\UI-StatusBar")
    xBg:SetVertexColor(0.6, 0.1, 0.1, 0.9)

    local xText = xBtn:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
    xText:SetPoint("CENTER")
    xText:SetText("X")
    xText:SetTextColor(1, 0.3, 0.3)

    xBtn:SetScript("OnEnter", function(self)
        self.mouseOver = true
        self.hideTimer = nil
        xBg:SetVertexColor(0.8, 0.1, 0.1, 1.0)
    end)
    xBtn:SetScript("OnLeave", function(self)
        self.mouseOver = false
        xBg:SetVertexColor(0.6, 0.1, 0.1, 0.9)
        self:Hide()
    end)
    xBtn:SetScript("OnUpdate", function(self, elapsed)
        if self.hideTimer then
            self.hideTimer = self.hideTimer - elapsed
            if self.hideTimer <= 0 then
                self.hideTimer = nil
                if not self.mouseOver then
                    self:Hide()
                end
            end
        end
    end)
    xBtn:SetScript("OnClick", function()
        local d = GetSelectedData()
        if not d then return end
        local spellId = d.spellSlots[i]
        local spellName = "this spell"
        if spellId > 0 then
            local name = GetSpellInfo(spellId)
            if name then spellName = name end
        end
        local dialog = StaticPopup_Show("CCAPTURE_UNLEARN_SLOT", spellName, tostring(i))
        if dialog then
            dialog.data = {slot = i}
        end
    end)
    xBtn:Hide()

    xButtons[i] = xBtn
end

-- ============================================================================
-- Refresh UI
-- ============================================================================

function RefreshSpellbook()
    -- Update slot tabs
    local anyGuardian = false
    for i = 0, MAX_SLOTS - 1 do
        local tab = slotTabs[i]
        local g = guardians[i]

        if g.hasGuardian then
            anyGuardian = true
            local archColor = ARCHETYPE_COLORS[g.archetype] or ARCHETYPE_COLORS[0]
            local shortName = g.guardianName ~= "" and g.guardianName:sub(1, 6) or ("[" .. (i + 1) .. "]")
            tab.text:SetText("[" .. (i + 1) .. "] " .. shortName)

            if i == selectedSlot then
                -- Active tab
                tab:SetBackdropColor(archColor[1] * 0.3, archColor[2] * 0.3, archColor[3] * 0.3, 0.9)
                tab:SetBackdropBorderColor(archColor[1], archColor[2], archColor[3], 1)
                tab.text:SetTextColor(1, 1, 1)
            else
                -- Inactive but occupied
                tab:SetBackdropColor(0.1, 0.1, 0.1, 0.8)
                tab:SetBackdropBorderColor(archColor[1] * 0.5, archColor[2] * 0.5, archColor[3] * 0.5, 0.8)
                tab.text:SetTextColor(0.6, 0.6, 0.6)
            end
        else
            tab.text:SetText("[" .. (i + 1) .. "]")
            tab:SetBackdropColor(0.05, 0.05, 0.05, 0.5)
            tab:SetBackdropBorderColor(0.2, 0.2, 0.2, 0.5)
            tab.text:SetTextColor(0.3, 0.3, 0.3)
        end
    end

    if not anyGuardian then
        spellbook:Hide()
        return
    end

    local d = GetSelectedData()
    if not d or not d.hasGuardian then
        -- Auto-select first occupied slot
        for i = 0, MAX_SLOTS - 1 do
            if guardians[i].hasGuardian then
                selectedSlot = i
                d = guardians[i]
                break
            end
        end
        if not d or not d.hasGuardian then
            spellbook:Hide()
            return
        end
        -- Re-update tabs for the new selection
        for i = 0, MAX_SLOTS - 1 do
            local tab = slotTabs[i]
            local g = guardians[i]
            if g.hasGuardian then
                local archColor = ARCHETYPE_COLORS[g.archetype] or ARCHETYPE_COLORS[0]
                if i == selectedSlot then
                    tab:SetBackdropColor(archColor[1] * 0.3, archColor[2] * 0.3, archColor[3] * 0.3, 0.9)
                    tab:SetBackdropBorderColor(archColor[1], archColor[2], archColor[3], 1)
                    tab.text:SetTextColor(1, 1, 1)
                else
                    tab:SetBackdropColor(0.1, 0.1, 0.1, 0.8)
                    tab:SetBackdropBorderColor(archColor[1] * 0.5, archColor[2] * 0.5, archColor[3] * 0.5, 0.8)
                    tab.text:SetTextColor(0.6, 0.6, 0.6)
                end
            end
        end
    end

    -- Update title
    titleText:SetText((d.guardianName ~= "" and d.guardianName or "Guardian") .. " Spellbook")

    -- Update archetype label
    local archName = ARCHETYPE_NAMES[d.archetype] or "DPS"
    local archColor = ARCHETYPE_COLORS[d.archetype] or ARCHETYPE_COLORS[0]
    archLabel:SetText(archName)
    archLabel:SetTextColor(archColor[1], archColor[2], archColor[3])

    -- Update spell slots
    for i = 1, NUM_SPELL_SLOTS do
        local spellId = d.spellSlots[i]
        local btn = slotButtons[i]

        if spellId > 0 then
            local _, _, icon = GetSpellInfo(spellId)
            btn.icon:SetTexture(icon or "Interface\\Icons\\INV_Misc_QuestionMark")
            btn.icon:Show()
            btn.emptyBg:Hide()
        else
            btn.icon:SetTexture(nil)
            btn.icon:Hide()
            btn.emptyBg:Show()
        end
    end

    -- Only show if a guardian is actively selected (targeted)
    if selectedSlot >= 0 then
        spellbook:Show()
    end
end

-- ============================================================================
-- Target-based spellbar switching
-- ============================================================================

local function OnTargetChanged()
    local targetGuid = UnitGUID("target")
    if not targetGuid then
        selectedSlot = -1
        spellbook:Hide()
        return
    end

    for i = 0, MAX_SLOTS - 1 do
        local g = guardians[i]
        if g.hasGuardian and g.creatureGuid and g.creatureGuid == targetGuid then
            if selectedSlot ~= i then
                selectedSlot = i
                RefreshSpellbook()
            end
            spellbook:Show()
            return
        end
    end
    -- Targeting something that isn't a guardian: hide spellbar
    selectedSlot = -1
    spellbook:Hide()
end

-- ============================================================================
-- Message Parsers (slot-aware)
-- ============================================================================

local function ParseSpells(payload)
    -- SPELLS:<slot>:<id1>:<id2>:...:<id8>
    local parts = {strsplit(":", payload)}
    -- parts[1] = "SPELLS", parts[2] = slot, parts[3..10] = spell IDs
    local slot = tonumber(parts[2])
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    local g = guardians[slot]
    for i = 1, NUM_SPELL_SLOTS do
        g.spellSlots[i] = tonumber(parts[i + 2]) or 0
    end
    g.hasGuardian = true

    RefreshSpellbook()
end

local function ParseArchetype(payload)
    -- ARCH:<slot>:<archetype>
    local parts = {strsplit(":", payload)}
    local slot = tonumber(parts[2])
    local archetype = tonumber(parts[3])
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    guardians[slot].archetype = archetype or 0
    RefreshSpellbook()
end

local function ParseName(payload)
    -- NAME:<slot>:<name>
    local _, slotStr, name = strsplit(":", payload, 3)
    local slot = tonumber(slotStr)
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    guardians[slot].guardianName = name or ""
    guardians[slot].hasGuardian = true

    RefreshSpellbook()
end

local function ParseGuid(payload)
    -- GUID:<slot>:<hexGuidString>
    local _, slotStr, guidStr = strsplit(":", payload, 3)
    local slot = tonumber(slotStr)
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    guardians[slot].creatureGuid = guidStr or nil
    guardians[slot].hasGuardian = true
end

local function ParseDismiss(payload)
    -- DISMISS:<slot>
    local _, slotStr = strsplit(":", payload)
    local slot = tonumber(slotStr)
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    -- Clear GUID but keep data (stored in tesseract)
    guardians[slot].creatureGuid = nil

    RefreshSpellbook()
end

local function ParseClear(payload)
    -- CLEAR:<slot>
    local _, slotStr = strsplit(":", payload)
    local slot = tonumber(slotStr)
    if not slot or slot < 0 or slot >= MAX_SLOTS then return end

    -- Fully reset slot
    guardians[slot] = {
        guardianName = "",
        archetype = 0,
        spellSlots = {0, 0, 0, 0, 0, 0, 0, 0},
        hasGuardian = false,
        creatureGuid = nil,
    }

    -- If this was the selected slot, pick another
    if selectedSlot == slot then
        selectedSlot = -1
        for i = 0, MAX_SLOTS - 1 do
            if guardians[i].hasGuardian then
                selectedSlot = i
                break
            end
        end
    end

    RefreshSpellbook()
end

-- ============================================================================
-- Event Handling
-- ============================================================================

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:RegisterEvent("PLAYER_LOGIN")
eventFrame:RegisterEvent("PLAYER_TARGET_CHANGED")

eventFrame:SetScript("OnEvent", function(self, event, arg1, arg2, ...)
    if event == "CHAT_MSG_ADDON" and arg1 == "CCAPTURE" then
        local msg = arg2
        if not msg then return end

        if msg:find("^SPELLS") then
            ParseSpells(msg)
        elseif msg:find("^ARCH") then
            ParseArchetype(msg)
        elseif msg:find("^NAME") then
            ParseName(msg)
        elseif msg:find("^GUID") then
            ParseGuid(msg)
        elseif msg:find("^DISMISS") then
            ParseDismiss(msg)
        elseif msg:find("^CLEAR") then
            ParseClear(msg)
        end

    elseif event == "PLAYER_TARGET_CHANGED" then
        OnTargetChanged()

    elseif event == "PLAYER_LOGIN" then
        RestorePosition(spellbook, "spellbook")
    end
end)

-- ============================================================================
-- Slash Command: Toggle spellbook visibility
-- ============================================================================

SLASH_CCAPTURE1 = "/guardian"
SLASH_CCAPTURE2 = "/ccapture"
SlashCmdList["CCAPTURE"] = function(msg)
    if spellbook:IsShown() then
        spellbook:Hide()
    else
        local anyGuardian = false
        for i = 0, MAX_SLOTS - 1 do
            if guardians[i].hasGuardian then
                anyGuardian = true
                break
            end
        end
        if anyGuardian then
            RefreshSpellbook()
            spellbook:Show()
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[Creature Capture]|r No guardians captured.")
        end
    end
end
