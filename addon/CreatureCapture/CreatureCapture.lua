-- Creature Capture Addon - Guardian Spellbook UI
-- Receives data from server via addon messages (LANG_ADDON whispers)
-- Displays an 8-slot spellbook for managing guardian abilities

-- SavedVariables
CreatureCaptureDB = CreatureCaptureDB or {}

-- ============================================================================
-- Data Layer
-- ============================================================================

local data = {
    guardianName = "",
    archetype = 0,  -- 0=DPS, 1=Tank, 2=Healer
    spellSlots = {0, 0, 0, 0, 0, 0, 0, 0},
    hasGuardian = false,
}

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
local NUM_SLOTS = 8
local PANEL_WIDTH = NUM_SLOTS * SLOT_SIZE + (NUM_SLOTS - 1) * SLOT_GAP + 20  -- 8 slots + padding
local PANEL_HEIGHT = 75

local BACKDROP_INFO = {
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 16, edgeSize = 16,
    insets = {left = 4, right = 4, top = 4, bottom = 4},
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
        -- id is the spellbook index, subType is the book type
        local name = GetSpellName(id, subType)
        if name then
            -- Get spell link to extract the spell ID
            local link = GetSpellLink(id, subType)
            if link then
                local spellId = tonumber(link:match("spell:(%d+)"))
                return spellId, name
            end
        end
    end
    return nil, nil
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

for i = 1, NUM_SLOTS do
    local slotBtn = CreateFrame("Button", "CaptureSpellSlot" .. i, spellbook)
    slotBtn:SetWidth(SLOT_SIZE)
    slotBtn:SetHeight(SLOT_SIZE)
    slotBtn:SetPoint("TOPLEFT", spellbook, "TOPLEFT", 10 + (i - 1) * (SLOT_SIZE + SLOT_GAP), -20)

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
        local spellId = data.spellSlots[i]
        if spellId > 0 and not InCombatLockdown() then
            local idx = FindSpellBookIndex(spellId)
            if idx then
                PickupSpell(idx, BOOKTYPE_SPELL)
            end
        end
    end)

    -- Drop ON slot: teach a spell
    slotBtn:SetScript("OnReceiveDrag", function(self)
        if InCombatLockdown() then return end
        local spellId, spellName = GetSpellIdFromCursor()
        if spellId and spellName then
            ClearCursor()
            local dialog = StaticPopup_Show("CCAPTURE_TEACH_SLOT",
                spellName .. " (slot " .. i .. ")", data.guardianName)
            if dialog then
                dialog.data = {slot = i, spellId = spellId}
            end
        end
    end)

    -- Also handle OnClick with cursor holding a spell
    slotBtn:SetScript("OnClick", function(self, button)
        if InCombatLockdown() then return end
        local spellId, spellName = GetSpellIdFromCursor()
        if spellId and spellName then
            ClearCursor()
            local dialog = StaticPopup_Show("CCAPTURE_TEACH_SLOT",
                spellName .. " (slot " .. i .. ")", data.guardianName)
            if dialog then
                dialog.data = {slot = i, spellId = spellId}
            end
        end
    end)

    -- Hover: tooltip + X button
    slotBtn:SetScript("OnEnter", function(self)
        local spellId = data.spellSlots[i]
        if spellId > 0 then
            GameTooltip:SetOwner(self, "ANCHOR_RIGHT")
            GameTooltip:SetSpellByID(spellId)
            GameTooltip:Show()
            xButtons[i]:Show()
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
        local spellId = data.spellSlots[i]
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

local function RefreshSpellbook()
    if not data.hasGuardian then
        spellbook:Hide()
        return
    end

    -- Update title
    titleText:SetText((data.guardianName ~= "" and data.guardianName or "Guardian") .. " Spellbook")

    -- Update archetype label
    local archName = ARCHETYPE_NAMES[data.archetype] or "DPS"
    local archColor = ARCHETYPE_COLORS[data.archetype] or ARCHETYPE_COLORS[0]
    archLabel:SetText(archName)
    archLabel:SetTextColor(archColor[1], archColor[2], archColor[3])

    -- Update spell slots
    for i = 1, NUM_SLOTS do
        local spellId = data.spellSlots[i]
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

    spellbook:Show()
end

-- ============================================================================
-- Message Parsers
-- ============================================================================

local function ParseSpells(payload)
    -- SPELLS:id1:id2:...:id8
    local spells = {strsplit(":", payload)}
    -- First element is "SPELLS", skip it
    for i = 1, NUM_SLOTS do
        data.spellSlots[i] = tonumber(spells[i + 1]) or 0
    end
    data.hasGuardian = true
    RefreshSpellbook()
end

local function ParseArchetype(payload)
    -- ARCH:0|1|2
    local _, value = strsplit(":", payload)
    data.archetype = tonumber(value) or 0
    RefreshSpellbook()
end

local function ParseName(payload)
    -- NAME:creatureName
    local _, name = strsplit(":", payload, 2)
    data.guardianName = name or ""
    RefreshSpellbook()
end

local function ParseDismiss()
    data.hasGuardian = false
    data.guardianName = ""
    data.archetype = 0
    data.spellSlots = {0, 0, 0, 0, 0, 0, 0, 0}
    RefreshSpellbook()
end

-- ============================================================================
-- Event Handling
-- ============================================================================

local eventFrame = CreateFrame("Frame")
eventFrame:RegisterEvent("CHAT_MSG_ADDON")
eventFrame:RegisterEvent("PLAYER_LOGIN")

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
        elseif msg:find("^DISMISS") then
            ParseDismiss()
        end

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
        if data.hasGuardian then
            spellbook:Show()
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00[Creature Capture]|r No active guardian.")
        end
    end
end
