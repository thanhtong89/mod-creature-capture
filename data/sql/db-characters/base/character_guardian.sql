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
    `bonus_damage` FLOAT NOT NULL DEFAULT 0 COMMENT 'Leeched + fed bonus damage',
    `bonus_health` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Leeched + fed bonus HP',
    `bonus_mana` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Leeched + fed bonus mana',
    `bonus_armor` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Fed bonus armor',
    `bonus_haste` FLOAT NOT NULL DEFAULT 0 COMMENT 'Fed bonus haste percentage',
    `bonus_res_holy` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus holy resistance',
    `bonus_res_fire` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus fire resistance',
    `bonus_res_nature` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus nature resistance',
    `bonus_res_frost` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus frost resistance',
    `bonus_res_shadow` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus shadow resistance',
    `bonus_res_arcane` INT NOT NULL DEFAULT 0 COMMENT 'Fed bonus arcane resistance',
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

SET @have_bonus_damage = (SELECT COUNT(*) FROM INFORMATION_SCHEMA.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_guardian' AND COLUMN_NAME = 'bonus_damage');
SET @sql = IF(@have_bonus_damage = 0, "ALTER TABLE `character_guardian` ADD COLUMN `bonus_damage` FLOAT NOT NULL DEFAULT 0 AFTER `dismissed`, ADD COLUMN `bonus_health` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bonus_damage`, ADD COLUMN `bonus_mana` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bonus_health`, ADD COLUMN `bonus_armor` INT UNSIGNED NOT NULL DEFAULT 0 AFTER `bonus_mana`, ADD COLUMN `bonus_haste` FLOAT NOT NULL DEFAULT 0 AFTER `bonus_armor`, ADD COLUMN `bonus_res_holy` INT NOT NULL DEFAULT 0 AFTER `bonus_haste`, ADD COLUMN `bonus_res_fire` INT NOT NULL DEFAULT 0 AFTER `bonus_res_holy`, ADD COLUMN `bonus_res_nature` INT NOT NULL DEFAULT 0 AFTER `bonus_res_fire`, ADD COLUMN `bonus_res_frost` INT NOT NULL DEFAULT 0 AFTER `bonus_res_nature`, ADD COLUMN `bonus_res_shadow` INT NOT NULL DEFAULT 0 AFTER `bonus_res_frost`, ADD COLUMN `bonus_res_arcane` INT NOT NULL DEFAULT 0 AFTER `bonus_res_shadow`", 'SELECT 1');
PREPARE stmt FROM @sql; EXECUTE stmt; DEALLOCATE PREPARE stmt;
