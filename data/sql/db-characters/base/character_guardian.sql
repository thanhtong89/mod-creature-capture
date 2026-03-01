-- Character Guardian persistence table
-- Stores captured guardians so they persist across logout/login
-- Supports up to 4 guardian slots per player (slots 0-3)

CREATE TABLE IF NOT EXISTS `character_guardian` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `owner` INT UNSIGNED NOT NULL COMMENT 'Player GUID',
    `entry` INT UNSIGNED NOT NULL COMMENT 'Creature template entry',
    `level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `slot` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Guardian slot 0-3',
    `cur_health` INT UNSIGNED NOT NULL DEFAULT 1,
    `cur_power` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Mana/Rage/Energy value',
    `power_type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=mana, 1=rage, 2=focus, 3=energy',
    `archetype` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=DPS, 1=Tank, 2=Healer',
    `spells` VARCHAR(200) DEFAULT '' COMMENT 'Comma-separated spell IDs for 8 slots',
    `display_id` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Creature display ID for model restoration',
    `equipment_id` TINYINT NOT NULL DEFAULT 0 COMMENT 'Equipment template ID for weapon restoration',
    `power_chosen` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=free pick available, 1=must pay gold',
    `ranged_dps` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=melee DPS stance, 1=ranged DPS stance',
    `dismissed` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1=explicitly dismissed, 0=auto-summon on login',
    `bonus_strength` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Strength',
    `bonus_agility` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Agility',
    `bonus_intellect` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Intellect',
    `bonus_stamina` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Stamina',
    `bonus_attack_power` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Attack Power',
    `bonus_spell_power` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Spell Power',
    `bonus_crit_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Crit Rating',
    `bonus_dodge_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Dodge Rating',
    `bonus_parry_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Parry Rating',
    `bonus_haste_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Haste Rating',
    `bonus_hit_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Hit Rating',
    `bonus_arpen_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Armor Penetration Rating',
    `bonus_expertise_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Expertise Rating',
    `bonus_block_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Block Rating',
    `bonus_block_value` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Block Value from shields',
    `bonus_armor` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Armor',
    `bonus_weapon_dmg` FLOAT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus weapon damage from fed weapons',
    `bonus_res_holy` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Holy resistance',
    `bonus_res_fire` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Fire resistance',
    `bonus_res_nature` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Nature resistance',
    `bonus_res_frost` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Frost resistance',
    `bonus_res_shadow` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Shadow resistance',
    `bonus_res_arcane` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Arcane resistance',
    `save_time` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_owner_slot` (`owner`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Creature Capture Module - Multi-Slot Guardian Storage';

-- Migrations: add columns for existing installations (safe to re-run)
SET @have_power_chosen = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'power_chosen');
SET @sql = IF(@have_power_chosen = 0, "ALTER TABLE `character_guardian` ADD COLUMN `power_chosen` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=free pick available, 1=must pay gold' AFTER `equipment_id`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

SET @have_ranged_dps = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'ranged_dps');
SET @sql = IF(@have_ranged_dps = 0, "ALTER TABLE `character_guardian` ADD COLUMN `ranged_dps` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=melee DPS stance, 1=ranged DPS stance' AFTER `power_chosen`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Migration: add new stat columns (replaces old bonus_damage/bonus_health/bonus_mana/bonus_haste)
SET @have_bonus_strength = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'bonus_strength');
SET @sql = IF(@have_bonus_strength = 0, "ALTER TABLE `character_guardian` ADD COLUMN `bonus_strength` INT NOT NULL DEFAULT 0 AFTER `dismissed`, ADD COLUMN `bonus_agility` INT NOT NULL DEFAULT 0 AFTER `bonus_strength`, ADD COLUMN `bonus_intellect` INT NOT NULL DEFAULT 0 AFTER `bonus_agility`, ADD COLUMN `bonus_stamina` INT NOT NULL DEFAULT 0 AFTER `bonus_intellect`, ADD COLUMN `bonus_attack_power` INT NOT NULL DEFAULT 0 AFTER `bonus_stamina`, ADD COLUMN `bonus_spell_power` INT NOT NULL DEFAULT 0 AFTER `bonus_attack_power`, ADD COLUMN `bonus_crit_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_spell_power`, ADD COLUMN `bonus_dodge_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_crit_rating`, ADD COLUMN `bonus_parry_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_dodge_rating`, ADD COLUMN `bonus_haste_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_parry_rating`, ADD COLUMN `bonus_hit_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_haste_rating`, ADD COLUMN `bonus_arpen_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_hit_rating`, ADD COLUMN `bonus_expertise_rating` INT NOT NULL DEFAULT 0 AFTER `bonus_arpen_rating`, ADD COLUMN `bonus_weapon_dmg` FLOAT NOT NULL DEFAULT 0 AFTER `bonus_armor`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Migration: add bonus_armor and resistance columns if missing (from older schema)
SET @have_bonus_armor = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'bonus_armor');
SET @sql = IF(@have_bonus_armor = 0, "ALTER TABLE `character_guardian` ADD COLUMN `bonus_armor` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bonus_expertise_rating`, ADD COLUMN `bonus_weapon_dmg` FLOAT NOT NULL DEFAULT 0 AFTER `bonus_armor`, ADD COLUMN `bonus_res_holy` INT NOT NULL DEFAULT 0 AFTER `bonus_weapon_dmg`, ADD COLUMN `bonus_res_fire` INT NOT NULL DEFAULT 0 AFTER `bonus_res_holy`, ADD COLUMN `bonus_res_nature` INT NOT NULL DEFAULT 0 AFTER `bonus_res_fire`, ADD COLUMN `bonus_res_frost` INT NOT NULL DEFAULT 0 AFTER `bonus_res_nature`, ADD COLUMN `bonus_res_shadow` INT NOT NULL DEFAULT 0 AFTER `bonus_res_frost`, ADD COLUMN `bonus_res_arcane` INT NOT NULL DEFAULT 0 AFTER `bonus_res_shadow`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Migration: convert old flat bonuses to new stat accumulators
-- bonus_damage -> bonus_strength (damage / 0.5 = STR that would produce equivalent AP)
-- bonus_health -> bonus_stamina (health / 10)
-- bonus_mana -> bonus_intellect (mana / 15)
-- bonus_haste -> bonus_haste_rating (haste_pct * 32.79)
SET @have_old_bonus_damage = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'bonus_damage');
SET @sql = IF(@have_old_bonus_damage > 0, "UPDATE `character_guardian` SET `bonus_strength` = GREATEST(`bonus_strength`, ROUND(`bonus_damage` / 0.5)), `bonus_stamina` = GREATEST(`bonus_stamina`, ROUND(`bonus_health` / 10)), `bonus_intellect` = GREATEST(`bonus_intellect`, ROUND(`bonus_mana` / 15)), `bonus_haste_rating` = GREATEST(`bonus_haste_rating`, ROUND(`bonus_haste` * 32.79)) WHERE `bonus_damage` > 0 OR `bonus_health` > 0 OR `bonus_mana` > 0 OR `bonus_haste` > 0", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Drop old columns after migration
SET @sql = IF(@have_old_bonus_damage > 0, "ALTER TABLE `character_guardian` DROP COLUMN `bonus_damage`, DROP COLUMN `bonus_health`, DROP COLUMN `bonus_mana`, DROP COLUMN `bonus_haste`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;

-- Migration: add block rating and block value columns
SET @have_block_rating = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'bonus_block_rating');
SET @sql = IF(@have_block_rating = 0, "ALTER TABLE `character_guardian` ADD COLUMN `bonus_block_rating` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Block Rating' AFTER `bonus_expertise_rating`, ADD COLUMN `bonus_block_value` INT NOT NULL DEFAULT 0 COMMENT 'Accumulated bonus Block Value from shields' AFTER `bonus_block_rating`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
