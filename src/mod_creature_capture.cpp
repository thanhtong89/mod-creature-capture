/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3 license
 *
 * Creature Capture Module
 * Allows players to capture NPCs and turn them into guardian companions.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "DataMap.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Unit.h"

// Constants
constexpr float GUARDIAN_FOLLOW_DIST = 3.0f;
constexpr float GUARDIAN_FOLLOW_ANGLE = M_PI / 2; // 90 degrees to the side

// Data key for storing guardian info on player
class CapturedGuardianData : public DataMap::Base
{
public:
    CapturedGuardianData() : guardianGuid(), guardianEntry(0), guardianLevel(0), guardianHealth(0) {}
    ObjectGuid guardianGuid;
    // For respawn after cross-map teleport
    uint32 guardianEntry;
    uint8 guardianLevel;
    uint32 guardianHealth;
};

/*
 * CapturedGuardianAI
 *
 * Custom AI for captured guardians. Simpler than PetAI since we don't need
 * the control bar - just follow and protect behavior.
 */
class CapturedGuardianAI : public CreatureAI
{
public:
    explicit CapturedGuardianAI(Creature* creature) : CreatureAI(creature),
        _owner(nullptr),
        _updateTimer(1000),
        _combatCheckTimer(500),
        _regenTimer(2000)
    {
        // Find our owner
        if (ObjectGuid ownerGuid = me->GetOwnerGUID())
        {
            _owner = ObjectAccessor::GetPlayer(*me, ownerGuid);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!me->IsAlive())
            return;

        // Update owner reference periodically
        _updateTimer -= diff;
        if (_updateTimer <= 0)
        {
            _updateTimer = 1000;
            if (!_owner || !_owner->IsInWorld())
            {
                if (ObjectGuid ownerGuid = me->GetOwnerGUID())
                    _owner = ObjectAccessor::GetPlayer(*me, ownerGuid);

                if (!_owner)
                {
                    // Owner gone, despawn
                    me->DespawnOrUnsummon();
                    return;
                }
            }

            // Check if too far from owner - teleport back
            if (_owner && me->GetDistance(_owner) > 50.0f)
            {
                float x, y, z;
                _owner->GetClosePoint(x, y, z, me->GetCombatReach(), GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
                me->NearTeleportTo(x, y, z, me->GetOrientation());
            }
        }

        // Combat logic
        if (me->GetVictim())
        {
            // Check if we should stop attacking
            if (!me->GetVictim()->IsAlive() ||
                !me->CanCreatureAttack(me->GetVictim()) ||
                (_owner && me->GetDistance(_owner) > 40.0f))
            {
                me->AttackStop();
                me->GetMotionMaster()->Clear();
                if (_owner)
                    me->GetMotionMaster()->MoveFollow(_owner, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
                return;
            }

            // Do melee attack
            DoMeleeAttackIfReady();

            // Try to cast spells
            DoCastSpells();
        }
        else
        {
            // Out of combat - regenerate health
            _regenTimer -= diff;
            if (_regenTimer <= 0)
            {
                _regenTimer = 2000; // Regen tick every 2 seconds

                if (me->GetHealth() < me->GetMaxHealth())
                {
                    // Regenerate ~6% max health per tick (similar to creatures)
                    uint32 regenAmount = me->GetMaxHealth() * 6 / 100;
                    if (regenAmount < 1)
                        regenAmount = 1;

                    me->SetHealth(std::min(me->GetHealth() + regenAmount, me->GetMaxHealth()));
                }

                // Also regenerate mana if applicable
                if (me->GetMaxPower(POWER_MANA) > 0 && me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
                {
                    uint32 manaRegen = me->GetMaxPower(POWER_MANA) * 6 / 100;
                    if (manaRegen < 1)
                        manaRegen = 1;

                    me->SetPower(POWER_MANA, std::min(me->GetPower(POWER_MANA) + static_cast<int32>(manaRegen),
                                                       me->GetMaxPower(POWER_MANA)));
                }
            }

            // Look for threats to owner
            _combatCheckTimer -= diff;
            if (_combatCheckTimer <= 0)
            {
                _combatCheckTimer = 500;

                if (_owner)
                {
                    // Check if owner is being attacked
                    if (Unit* attacker = _owner->getAttackerForHelper())
                    {
                        if (me->CanCreatureAttack(attacker))
                        {
                            AttackStart(attacker);
                            return;
                        }
                    }

                    // Check if owner is attacking something
                    if (Unit* ownerTarget = _owner->GetVictim())
                    {
                        if (me->CanCreatureAttack(ownerTarget))
                        {
                            AttackStart(ownerTarget);
                            return;
                        }
                    }

					if (!me->GetVictim())
					{
						for (Unit* attacker : me->getAttackers())
						{
							if (attacker && attacker->IsAlive() && me->CanCreatureAttack(attacker))
							{
								me->AddThreat(attacker, 100.0f);
								AttackStart(attacker);
								return; // Target found, stop looking
							}
						}
					}
                }
            }

            // Make sure we're following
            if (_owner && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                me->GetMotionMaster()->MoveFollow(_owner, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
            }
        }
    }

    void AttackStart(Unit* target) override
    {
        if (!target || !me->CanCreatureAttack(target))
            return;

        if (me->Attack(target, true))
        {
            me->GetMotionMaster()->MoveChase(target);
        }
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        // Don't evade, just return to owner
        me->AttackStop();
        me->GetMotionMaster()->Clear();
        if (_owner)
            me->GetMotionMaster()->MoveFollow(_owner, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
    }

    // Use this hook inside your Guardian's AI:
    void DamageDealt(Unit* victim, uint32& /*damage*/, DamageEffectType /*damageType*/, SpellSchoolMask /*damageSchoolMask*/) override
    {
        if (victim && victim->IsCreature() && _owner)
        {
            Creature* target = victim->ToCreature();

            // This is the "Magic Bullet" for AzerothCore loot:
            // We force the victim to think the owner has already dealt damage to it.
            target->SetLootRecipient(_owner);

            // This ensures that even if the guardian does 100% of the damage,
            // the "Player Damage Required" is satisfied.
            target->LowerPlayerDamageReq(target->GetHealth());
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        // Notify owner
        if (_owner)
        {
            ChatHandler(_owner->GetSession()).PSendSysMessage("Your captured guardian has died.");
        }
    }

private:
    void DoCastSpells()
    {
        // Don't cast if already casting
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        // Get creature spells from template
        CreatureTemplate const* cInfo = me->GetCreatureTemplate();
        if (!cInfo)
            return;

        Unit* target = me->GetVictim();
        if (!target)
            return;

        // Try each spell slot
        for (uint8 i = 0; i < MAX_CREATURE_SPELLS; ++i)
        {
            uint32 spellId = cInfo->spells[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            // Check cooldown
            if (me->HasSpellCooldown(spellId))
                continue;

            // Check if spell can be used in combat
            if (!spellInfo->CanBeUsedInCombat())
                continue;

            // Determine spell type
            bool isHealingSpell = spellInfo->IsPositive() && spellInfo->HasEffect(SPELL_EFFECT_HEAL);
            bool isBuffSpell = spellInfo->IsPositive() && !isHealingSpell;
            bool isPeriodicSpell = spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE) ||
                                   spellInfo->HasAura(SPELL_AURA_PERIODIC_LEECH) ||
                                   spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);

            // Apply conditions for different spell types
            if (isHealingSpell)
            {
                // Only cast healing spells when health is below 40%
                if (me->GetHealthPct() > 40.0f)
                    continue;

                // Cast on self
                me->CastSpell(me, spellId, false);
            }
            else if (isBuffSpell)
            {
                // Check if we already have this buff
                if (me->HasAura(spellId))
                    continue;

                // Cast on self
                me->CastSpell(me, spellId, false);
            }
            else if (isPeriodicSpell)
            {
                // Don't reapply DoTs if target already has them
                if (target->HasAura(spellId, me->GetGUID()))
                    continue;

                // Check range
                if (spellInfo->GetMaxRange(false) > 0 &&
                    !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                    continue;

                // Cast on target
                me->CastSpell(target, spellId, false);
            }
            else
            {
                // Regular offensive spell - check range
                if (spellInfo->GetMaxRange(false) > 0 &&
                    !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                    continue;

                // Cast on target
                me->CastSpell(target, spellId, false);
            }

            // Add cooldown - use spell's recovery time or category cooldown
            uint32 cooldown = spellInfo->RecoveryTime;
            if (isHealingSpell && cooldown < 10000)
                cooldown = 10000; // Min 10s for heals
            else if (isBuffSpell && cooldown < 30000)
                cooldown = 30000; // Min 30s for buffs
			else if (cooldown == 0)
			{
				if (spellInfo->CategoryRecoveryTime > cooldown)
					cooldown = spellInfo->CategoryRecoveryTime;
				if (spellInfo->StartRecoveryTime > cooldown)
					cooldown = spellInfo->StartRecoveryTime;

				// 2. ENFORCE GLOBAL MINIMUM
				// If the spell is still 0 (very common for NPC spells),
				// force a default 5-10 second cooldown so they don't spam it.
				if (cooldown == 0)
				{
					cooldown = 2000;
				}

				// 3. Add a small "Variance" (Optional but makes it feel real)
				// This prevents the guardian from casting exactly every 8.000 seconds.
				cooldown += urand(500, 1500);
			}
            // force cooldowns for heals/buffs to prevent spam
            // Regular offensive spells cast naturally via GCD/cast time

            if (cooldown > 0)
                me->AddSpellCooldown(spellId, 0, cooldown);

            break; // Only cast one spell per update
        }
    }

    Player* _owner;
    int32 _updateTimer;
    int32 _combatCheckTimer;
    int32 _regenTimer;
};

/*
 * Module Configuration
 */
struct CreatureCaptureConfig
{
    bool enabled = true;
    bool announce = true;
    uint32 guardianDuration = 0;
    bool allowElite = false;
    bool allowRare = true;
    int32 maxLevelDiff = 5;
    uint8 minCreatureLevel = 1;
    uint32 healthPct = 100;
    uint32 damagePct = 100;

    void Load()
    {
        enabled = sConfigMgr->GetOption<bool>("CreatureCapture.Enable", true);
        announce = sConfigMgr->GetOption<bool>("CreatureCapture.Announce", true);
        guardianDuration = sConfigMgr->GetOption<uint32>("CreatureCapture.GuardianDuration", 0);
        allowElite = sConfigMgr->GetOption<bool>("CreatureCapture.AllowElite", false);
        allowRare = sConfigMgr->GetOption<bool>("CreatureCapture.AllowRare", true);
        maxLevelDiff = sConfigMgr->GetOption<int32>("CreatureCapture.MaxLevelDiff", 5);
        minCreatureLevel = sConfigMgr->GetOption<uint8>("CreatureCapture.MinCreatureLevel", 1);
        healthPct = sConfigMgr->GetOption<uint32>("CreatureCapture.HealthPct", 100);
        damagePct = sConfigMgr->GetOption<uint32>("CreatureCapture.DamagePct", 100);
    }
};

static CreatureCaptureConfig config;

/*
 * Helper functions
 */
static void DismissGuardian(Player* player)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    if (data && data->guardianGuid)
    {
        if (Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid))
        {
            guardian->DespawnOrUnsummon();
        }
        data->guardianGuid.Clear();
    }
}

static bool CanCaptureCreature(Player* player, Creature* target, std::string& error)
{
    if (!target)
    {
        error = "No target selected.";
        return false;
    }

    if (!target->IsAlive())
    {
        error = "Target must be alive.";
        return false;
    }

    if (target->IsPet() || target->IsGuardian() || target->IsSummon())
    {
        error = "Cannot capture pets, guardians, or summons.";
        return false;
    }

    if (target->IsPlayer())
    {
        error = "Cannot capture players.";
        return false;
    }

    CreatureTemplate const* cInfo = target->GetCreatureTemplate();
    if (!cInfo)
    {
        error = "Invalid creature.";
        return false;
    }

    // Check creature type - only beasts, humanoids, etc.
    // Exclude mechanical, elemental types that don't make sense
    if (cInfo->type == CREATURE_TYPE_CRITTER)
    {
        error = "Cannot capture critters.";
        return false;
    }

    // Check rank (elite, rare, etc.)
    if (cInfo->rank == CREATURE_ELITE_ELITE ||
        cInfo->rank == CREATURE_ELITE_WORLDBOSS ||
        cInfo->rank == CREATURE_ELITE_RAREELITE)
    {
        if (!config.allowElite)
        {
            error = "Cannot capture elite creatures.";
            return false;
        }
    }

    if (cInfo->rank == CREATURE_ELITE_RARE && !config.allowRare)
    {
        error = "Cannot capture rare creatures.";
        return false;
    }

    // Check level
    if (target->GetLevel() < config.minCreatureLevel)
    {
        error = "Creature level is too low.";
        return false;
    }

    int32 levelDiff = static_cast<int32>(target->GetLevel()) - static_cast<int32>(player->GetLevel());
    if (levelDiff > config.maxLevelDiff)
    {
        error = "Creature level is too high for you to capture.";
        return false;
    }

    // Check if creature is in combat with someone else
    if (target->IsInCombat() && target->GetVictim() != player)
    {
        error = "Creature is in combat with someone else.";
        return false;
    }

    // Check distance
    if (!player->IsWithinDistInMap(target, 30.0f))
    {
        error = "Target is too far away.";
        return false;
    }

    return true;
}

static TempSummon* SummonCapturedGuardian(Player* player, uint32 entry, uint8 level)
{
    // Get summon position near player
    float x, y, z;
    player->GetClosePoint(x, y, z, player->GetCombatReach(), GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);

    // Calculate duration (0 = infinite for TEMPSUMMON_MANUAL_DESPAWN)
    uint32 duration = config.guardianDuration > 0 ? config.guardianDuration * IN_MILLISECONDS : 0;

    // Summon the creature as a guardian
    TempSummon* guardian = player->SummonCreature(
        entry,
        x, y, z, player->GetOrientation(),
        TEMPSUMMON_MANUAL_DESPAWN,
        duration
    );

    if (!guardian)
        return nullptr;

    // Set up as player-controlled guardian
    guardian->SetOwnerGUID(player->GetGUID());
    guardian->SetCreatorGUID(player->GetGUID());
    guardian->SetFaction(player->GetFaction());
    guardian->SetLevel(level);

    // Clear immunity flags that prevent combat
    guardian->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_ATTACKABLE_1);
    // Force Faction to match player
    guardian->SetFaction(player->GetFaction());

    // Set flags for player control
    guardian->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);

    // Set reactive state
    guardian->SetReactState(REACT_DEFENSIVE);

    // Apply stat modifiers from config
    if (config.healthPct != 100)
    {
        uint32 newHealth = guardian->GetMaxHealth() * config.healthPct / 100;
        guardian->SetMaxHealth(newHealth);
        guardian->SetHealth(newHealth);
    }

    // Start following the player
    guardian->GetMotionMaster()->MoveFollow(player, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);

    // Force AI refresh to use our custom AI
    guardian->SetAI(new CapturedGuardianAI(guardian));

    return guardian;
}

/*
 * Command Script
 */
using namespace Acore::ChatCommands;

class CreatureCaptureCommandScript : public CommandScript
{
public:
    CreatureCaptureCommandScript() : CommandScript("CreatureCaptureCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable captureCommandTable =
        {
            { "",           HandleCaptureCommand,    SEC_PLAYER, Console::No },
            { "dismiss",    HandleDismissCommand,    SEC_PLAYER, Console::No },
            { "info",       HandleInfoCommand,       SEC_PLAYER, Console::No },
        };

        static ChatCommandTable commandTable =
        {
            { "capture", captureCommandTable },
        };

        return commandTable;
    }

    static bool HandleCaptureCommand(ChatHandler* handler, Optional<PlayerIdentifier> /*target*/)
    {
        if (!config.enabled)
        {
            handler->PSendSysMessage("Creature capture is disabled.");
            return true;
        }

        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        // Get target creature
        Creature* target = handler->getSelectedCreature();
        std::string error;

        if (!CanCaptureCreature(player, target, error))
        {
            handler->PSendSysMessage("Cannot capture: {}", error);
            return true;
        }

        // Dismiss existing guardian first
        DismissGuardian(player);

        // Store creature info before despawning
        uint32 entry = target->GetEntry();
        uint8 level = target->GetLevel();
        std::string name = target->GetName();

        // Despawn the original creature
        target->DespawnOrUnsummon();

        // Summon new guardian
        TempSummon* guardian = SummonCapturedGuardian(player, entry, level);
        if (!guardian)
        {
            handler->PSendSysMessage("Failed to summon guardian.");
            return true;
        }

        // Store guardian GUID
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        data->guardianGuid = guardian->GetGUID();

        handler->PSendSysMessage("You have captured {} (Level {})! It will now follow and protect you!",
            name, level);

        return true;
    }

    static bool HandleDismissCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
        {
            handler->PSendSysMessage("You don't have a captured guardian.");
            return true;
        }

        DismissGuardian(player);
        handler->PSendSysMessage("Your guardian has been dismissed.");

        return true;
    }

    static bool HandleInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
        {
            handler->PSendSysMessage("You don't have a captured guardian.");
            return true;
        }

        Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
        if (!guardian)
        {
            handler->PSendSysMessage("Your guardian could not be found.");
            data->guardianGuid.Clear();
            return true;
        }

        handler->PSendSysMessage("=== Captured Guardian Info ===");
        handler->PSendSysMessage("Name: {}", guardian->GetName());
        handler->PSendSysMessage("Level: {}", guardian->GetLevel());
        handler->PSendSysMessage("Health: {} / {}", guardian->GetHealth(), guardian->GetMaxHealth());
        handler->PSendSysMessage("Entry: {}", guardian->GetEntry());

        return true;
    }
};

/*
 * Player Script - Handle teleport, logout, etc.
 */
class CreatureCapturePlayerScript : public PlayerScript
{
public:
    CreatureCapturePlayerScript() : PlayerScript("CreatureCapturePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_LOGOUT,
        PLAYERHOOK_ON_BEFORE_TELEPORT,
        PLAYERHOOK_ON_MAP_CHANGED
    }) {}

    void OnPlayerLogin(Player* player) override
    {
        if (config.enabled && config.announce)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Creature Capture]|r Use .capture to capture a targeted NPC as your guardian!");
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        // Dismiss guardian on logout
        DismissGuardian(player);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
            return true; // No guardian, allow teleport

        Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
        if (!guardian)
        {
            data->guardianGuid.Clear();
            return true;
        }

        bool sameMap = (player->GetMapId() == mapId);

        if (sameMap)
        {
            // Same map - just teleport the guardian to the destination
            guardian->NearTeleportTo(x, y, z, guardian->GetOrientation());
        }
        else
        {
            // Cross-map teleport - store guardian info for respawn
            data->guardianEntry = guardian->GetEntry();
            data->guardianLevel = guardian->GetLevel();
            data->guardianHealth = guardian->GetHealth();

            // Despawn the guardian
            guardian->DespawnOrUnsummon();
            data->guardianGuid.Clear();
        }

        return true; // Allow teleport
    }

    void OnPlayerMapChanged(Player* player) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        // Check if we have a guardian to respawn after cross-map teleport
        if (data->guardianEntry && !data->guardianGuid)
        {
            // Respawn the guardian
            TempSummon* guardian = SummonCapturedGuardian(player, data->guardianEntry, data->guardianLevel);
            if (guardian)
            {
                // Restore health
                if (data->guardianHealth > 0 && data->guardianHealth <= guardian->GetMaxHealth())
                    guardian->SetHealth(data->guardianHealth);

                data->guardianGuid = guardian->GetGUID();
            }

            // Clear the stored info
            data->guardianEntry = 0;
            data->guardianLevel = 0;
            data->guardianHealth = 0;
        }
    }
};

/*
 * World Script - Load config on startup
 */
class CreatureCaptureWorldScript : public WorldScript
{
public:
    CreatureCaptureWorldScript() : WorldScript("CreatureCaptureWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.Load();
    }
};

// Register scripts
void AddSC_mod_creature_capture()
{
    new CreatureCaptureCommandScript();
    new CreatureCapturePlayerScript();
    new CreatureCaptureWorldScript();
}
