-- Tesseract item for summoning/dismissing captured guardians

DELETE FROM `item_template` WHERE `entry` = 90000;
INSERT INTO `item_template` (
    `entry`, `class`, `subclass`, `name`, `displayid`, `Quality`,
    `Flags`, `BuyPrice`, `SellPrice`, `InventoryType`,
    `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
    `maxcount`, `stackable`, `bonding`, `description`,
    `spellid_1`, `spelltrigger_1`, `spellcooldown_1`, `spellcategory_1`, `spellcategorycooldown_1`,
    `ScriptName`
) VALUES (
    90000,          -- entry
    15,             -- class (Miscellaneous)
    0,              -- subclass
    'Tesseract',    -- name
    3567,           -- displayid (Soulstone - purple orb)
    3,              -- Quality (Rare/Blue)
    64,             -- Flags (ITEM_FLAG_NO_DESTROY)
    10000,          -- BuyPrice (1g)
    2500,           -- SellPrice (25s)
    0,              -- InventoryType (Non-equip)
    -1,             -- AllowableClass (All)
    -1,             -- AllowableRace (All)
    1,              -- ItemLevel
    1,              -- RequiredLevel
    1,              -- maxcount (only 1 per player)
    1,              -- stackable
    1,              -- bonding (Bind on Pickup)
    'A multidimensional storage device capable of containing any creature as your guardian.',
    36330,           -- spellid_1 (Opening - generic use spell)
    0,              -- spelltrigger_1 (ON_USE)
    1000,             -- spellcooldown_1
    0,              -- spellcategory_1
    -1,             -- spellcategorycooldown_1
    'item_tesseract' -- ScriptName
);
