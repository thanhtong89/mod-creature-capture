-- Character Guardian persistence table
-- Stores captured guardians so they persist across logout/login

DROP TABLE IF EXISTS `character_guardian`;
CREATE TABLE `character_guardian` (
    `id` INT UNSIGNED NOT NULL AUTO_INCREMENT,
    `owner` INT UNSIGNED NOT NULL COMMENT 'Player GUID',
    `entry` INT UNSIGNED NOT NULL COMMENT 'Creature template entry',
    `name` VARCHAR(50) DEFAULT NULL COMMENT 'Custom name (if renamed)',
    `level` TINYINT UNSIGNED NOT NULL DEFAULT 1,
    `slot` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=active, 1-5=stored in tesseract',
    `cur_health` INT UNSIGNED NOT NULL DEFAULT 1,
    `cur_power` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'Mana/Rage/Energy value',
    `power_type` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=mana, 1=rage, 2=focus, 3=energy',
    `archetype` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0=DPS, 1=Tank, 2=Healer',
    `spells` VARCHAR(200) DEFAULT '' COMMENT 'Comma-separated spell IDs for 8 slots',
    `save_time` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`id`),
    KEY `idx_owner` (`owner`),
    KEY `idx_owner_slot` (`owner`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='Creature Capture Module - Guardian Storage';
