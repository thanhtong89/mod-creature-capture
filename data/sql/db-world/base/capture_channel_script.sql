-- Add an AuraScript on spell 1515 (Tame Beast aura applied to creature).
-- 1515 already has 'spell_hun_tame_beast' (SpellScript/CheckCast only) -- a
-- separate AuraScript on the same spell ID is fine; they don't conflict.
DELETE FROM `spell_script_names` WHERE `spell_id` = 1515 AND `ScriptName` = 'spell_creature_capture_channel';
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES (1515, 'spell_creature_capture_channel');
