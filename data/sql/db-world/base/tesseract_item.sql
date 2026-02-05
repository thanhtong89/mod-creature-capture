-- Tesseract item for summoning/dismissing captured guardians
-- Uses existing item 44807 (Indalamar's Holy Hand Grenade) which exists in client Item.dbc
-- This avoids the need for client-side DBC patches

DELETE FROM `item_template` WHERE `entry` = 44807;
INSERT INTO `item_template` (
    `entry`, `class`, `subclass`, `name`, `displayid`, `Quality`,
    `Flags`, `BuyPrice`, `SellPrice`, `InventoryType`,
    `AllowableClass`, `AllowableRace`, `ItemLevel`, `RequiredLevel`,
    `maxcount`, `stackable`, `bonding`, `description`, `Material`,
    `spellid_1`, `spelltrigger_1`, `spellcooldown_1`, `spellcategory_1`, `spellcategorycooldown_1`,
    `ScriptName`
) VALUES (
    44807,          -- entry (repurposed from Indalamar's Holy Hand Grenade)
    0,             -- class
    0,              -- subclass
    'Tesseract',    -- name
    6009,           -- displayid (Soulstone - purple orb)
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
    4,              -- Material (jewelry)
    36330,           -- spellid_1 (Opening - generic use spell)
    0,              -- spelltrigger_1 (ON_USE)
    1000,             -- spellcooldown_1
    4,              -- spellcategory_1
    -1,             -- spellcategorycooldown_1
    'item_tesseract' -- ScriptName
);
