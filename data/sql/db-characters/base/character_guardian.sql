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
    `dismissed` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1=explicitly dismissed, 0=auto-summon on login',
    `save_time` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    UNIQUE KEY `idx_owner_slot` (`owner`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Creature Capture Module - Multi-Slot Guardian Storage';

-- Migration: add power_chosen column for existing installations
-- Harmlessly errors if column already exists (e.g. fresh install)
ALTER TABLE `character_guardian`
    ADD COLUMN `power_chosen` TINYINT UNSIGNED NOT NULL DEFAULT 0
    COMMENT '0=free pick available, 1=must pay gold'
    AFTER `equipment_id`;
