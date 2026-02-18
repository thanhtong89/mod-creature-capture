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
#include "CreatureAISelector.h"
#include "DatabaseEnv.h"
#include "DataMap.h"
#include "ItemScript.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "Unit.h"

#include <algorithm>
#include <typeinfo>
#include <vector>

// Tesseract item constant (uses existing item 44807 from client Item.dbc)
constexpr uint32 ITEM_TESSERACT = 44807;

// Gossip menu actions
enum TesseractGossipActions
{
    TESSERACT_ACTION_SUMMON     = 1,
    TESSERACT_ACTION_DISMISS    = 2,
    TESSERACT_ACTION_INFO       = 3,
    TESSERACT_ACTION_RELEASE    = 4
};

// Constants
constexpr float GUARDIAN_FOLLOW_DIST = 3.0f;
constexpr float GUARDIAN_FOLLOW_ANGLE = M_PI / 2; // 90 degrees to the side

// Data key for storing guardian info on player
class CapturedGuardianData : public DataMap::Base
{
public:
    CapturedGuardianData() : guardianGuid(), guardianEntry(0), guardianLevel(0),
        guardianHealth(0), guardianPower(0), guardianPowerType(0), savedToDb(false) {}
    ObjectGuid guardianGuid;
    // For respawn after cross-map teleport and persistence
    uint32 guardianEntry;
    uint8 guardianLevel;
    uint32 guardianHealth;
    uint32 guardianPower;       // Mana/Rage/Energy value
    uint8 guardianPowerType;    // 0=mana, 1=rage, 2=focus, 3=energy
    bool savedToDb;             // Whether this guardian is persisted in DB
};

/*
 * CapturedGuardianAI
 *
 * Wrapper AI that preserves the creature's original combat behavior (SmartAI,
 * scripted AI, etc.) while adding follow/protect logic for the player owner.
 */
class CapturedGuardianAI : public CreatureAI
{
public:
    explicit CapturedGuardianAI(Creature* creature, CreatureAI* originalAI = nullptr)
        : CreatureAI(creature),
        _originalAI(originalAI),
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

    ~CapturedGuardianAI()
    {
        delete _originalAI;
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

            // DELEGATE COMBAT TO ORIGINAL AI if available
            if (_originalAI)
            {
                _originalAI->UpdateAI(diff);
            }
            else
            {
                // Fallback: basic melee + our spell logic
                DoMeleeAttackIfReady();
                DoCastSpells();
            }
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

        // Periodically check summoned creatures - stop them from attacking the owner
        _summonCheckTimer -= diff;
        if (_summonCheckTimer <= 0 && _owner && !_summonedGuids.empty())
        {
            _summonCheckTimer = 500; // Check twice per second

            // Check each tracked summon
            for (auto it = _summonedGuids.begin(); it != _summonedGuids.end(); )
            {
                Creature* summon = ObjectAccessor::GetCreature(*me, *it);

                // Remove dead/despawned/invalid summons from tracking
                if (!summon || !summon->IsAlive() || !summon->IsInWorld())
                {
                    it = _summonedGuids.erase(it);
                    continue;
                }

                // If it's targeting the owner, stop it!
                if (summon->GetVictim() == _owner)
                {
                    summon->GetThreatMgr().ClearAllThreat();
                    summon->AttackStop();
                    summon->SetFaction(_owner->GetFaction());

                    // Redirect to guardian's target if we have one (with null checks)
                    if (Unit* myVictim = me->GetVictim())
                    {
                        if (CreatureAI* ai = summon->AI())
                            ai->AttackStart(myVictim);
                    }
                }

                ++it;
            }
        }
    }


    void JustSummoned(Creature* summon) override
    {
        if (!summon || !_owner)
            return;

        // 1. Set owner to the player (not the guardian) so it's treated as player's minion
        summon->SetOwnerGUID(_owner->GetGUID());
        summon->SetCreatorGUID(_owner->GetGUID());

        // 2. Force faction to match player
        summon->SetFaction(_owner->GetFaction());

        // 3. Clear any hostile flags and mark as player-controlled
        summon->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
        summon->SetImmuneToPC(false);
        summon->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);

        // 4. Clear all threat so it doesn't attack the player
        summon->GetThreatMgr().ClearAllThreat();

        // 5. Set to defensive react state
        summon->SetReactState(REACT_DEFENSIVE);

        // 6. Make it attack what the guardian is attacking
        if (Unit* victim = me->GetVictim())
        {
            summon->AI()->AttackStart(victim);
        }

        // Delegate to original AI (but our setup takes priority)
        if (_originalAI)
            _originalAI->JustSummoned(summon);

        // Re-apply faction after delegation in case original AI changed it
        summon->SetFaction(_owner->GetFaction());

        // Track this summon so we can check it periodically
        _summonedGuids.push_back(summon->GetGUID());
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        // Remove from our tracking list
        if (summon)
        {
            auto it = std::find(_summonedGuids.begin(), _summonedGuids.end(), summon->GetGUID());
            if (it != _summonedGuids.end())
                _summonedGuids.erase(it);
        }

        if (_originalAI)
            _originalAI->SummonedCreatureDespawn(summon);
    }

    void AttackStart(Unit* target) override
    {
        if (!target || !me->CanCreatureAttack(target))
            return;

        // Ensure we enter combat state - this is required for SmartAI events to fire
        if (!me->IsInCombat())
        {
            me->SetInCombatWith(target);
            target->SetInCombatWith(me);
        }

        // Delegate to original AI if available
        if (_originalAI)
        {
            _originalAI->AttackStart(target);
        }
        else
        {
            if (me->Attack(target, true))
            {
                me->GetMotionMaster()->MoveChase(target);
            }
        }
    }

    void EnterEvadeMode(EvadeReason why) override
    {
        // Don't evade normally - just return to owner
        // But still notify original AI so it can reset internal state
        if (_originalAI)
            _originalAI->EnterEvadeMode(why);

        me->AttackStop();
        me->GetMotionMaster()->Clear();
        if (_owner)
            me->GetMotionMaster()->MoveFollow(_owner, GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
    }

    void JustEngagedWith(Unit* who) override
    {
        if (_originalAI)
            _originalAI->JustEngagedWith(who);
    }

    void KilledUnit(Unit* victim) override
    {
        // Give loot rights to owner
        if (_owner && victim && victim->IsCreature())
        {
            Creature* killed = victim->ToCreature();
            killed->SetLootRecipient(_owner);
            killed->LowerPlayerDamageReq(killed->GetMaxHealth());
        }

        // Notify original AI
        if (_originalAI)
            _originalAI->KilledUnit(victim);
    }

    void SpellHit(Unit* caster, SpellInfo const* spellInfo) override
    {
        if (_originalAI)
            _originalAI->SpellHit(caster, spellInfo);
    }

    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType damageType, SpellSchoolMask schoolMask) override
    {
        if (_originalAI)
            _originalAI->DamageTaken(attacker, damage, damageType, schoolMask);
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

    void JustDied(Unit* killer) override
    {
        // Notify owner
        if (_owner)
        {
            ChatHandler(_owner->GetSession()).PSendSysMessage("Your captured guardian has died.");
        }

        // Delegate to original AI for cleanup
        if (_originalAI)
            _originalAI->JustDied(killer);
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
            {
                continue;
            }

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
                // Prioritize healing owner if they need it, then self
                Unit* healTarget = nullptr;

                if (_owner && _owner->IsAlive() && _owner->GetHealthPct() < 40.0f)
                    healTarget = _owner;
                else if (me->GetHealthPct() < 40.0f)
                    healTarget = me;

                if (!healTarget)
                    continue;

                me->CastSpell(healTarget, spellId, false);
            }
            else if (isBuffSpell)
            {
                // Check if we already have this buff
                if (me->HasAura(spellId))
                {
                    continue;
                }

                // Cast on self
                me->CastSpell(me, spellId, false);
            }
            else if (isPeriodicSpell)
            {
                // Don't reapply DoTs if target already has them
                if (target->HasAura(spellId, me->GetGUID()))
                {
                    continue;
                }

                // Check range
                if (spellInfo->GetMaxRange(false) > 0 &&
                    !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                {
                    continue;
                }

                // Cast on target
                me->CastSpell(target, spellId, false);
            }
            else
            {
                // Regular offensive spell - check range
                if (spellInfo->GetMaxRange(false) > 0 &&
                    !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                {
                    continue;
                }

                // Cast on target
                me->CastSpell(target, spellId, false);
            }

            // Add cooldown - use spell's recovery time or category cooldown
            uint32 cooldown = spellInfo->RecoveryTime;
            if (isHealingSpell && cooldown < 10000)
                cooldown = 10000; // Min 10s for heals
            else if (isBuffSpell && cooldown < 30000)
                cooldown = 30000; // Min 30s for buffs
			else
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
            {
                me->AddSpellCooldown(spellId, 0, cooldown);
            }

            break; // Only cast one spell per update
        }
    }

    CreatureAI* _originalAI;   // The creature's native AI (SmartAI, CombatAI, etc.)
    Player* _owner;
    int32 _updateTimer;
    int32 _combatCheckTimer;
    int32 _regenTimer;
    int32 _summonCheckTimer = 1000;  // For checking summoned creatures
    std::vector<ObjectGuid> _summonedGuids;  // Track our summons
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

// Database persistence functions
static void SaveGuardianToDb(Player* player, CapturedGuardianData* data)
{
    if (!data || data->guardianEntry == 0)
        return;

    uint32 ownerGuid = player->GetGUID().GetCounter();

    // Use transaction for atomicity
    auto trans = CharacterDatabase.BeginTransaction();

    trans->Append("DELETE FROM character_guardian WHERE owner = {}", ownerGuid);
    trans->Append(
        "INSERT INTO character_guardian (owner, entry, level, slot, cur_health, cur_power, power_type, save_time) "
        "VALUES ({}, {}, {}, 0, {}, {}, {}, UNIX_TIMESTAMP())",
        ownerGuid,
        data->guardianEntry,
        data->guardianLevel,
        data->guardianHealth,
        data->guardianPower,
        data->guardianPowerType
    );

    CharacterDatabase.CommitTransaction(trans);
    data->savedToDb = true;
}

static void LoadGuardianFromDb(Player* player)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();

    QueryResult result = CharacterDatabase.Query(
        "SELECT entry, level, cur_health, cur_power, power_type FROM character_guardian WHERE owner = {} AND slot = 0",
        ownerGuid
    );

    if (!result)
        return;

    Field* fields = result->Fetch();

    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    data->guardianEntry = fields[0].Get<uint32>();
    data->guardianLevel = fields[1].Get<uint8>();
    data->guardianHealth = fields[2].Get<uint32>();
    data->guardianPower = fields[3].Get<uint32>();
    data->guardianPowerType = fields[4].Get<uint8>();
    data->savedToDb = true;
}

static void DeleteGuardianFromDb(Player* player)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();
    CharacterDatabase.Execute("DELETE FROM character_guardian WHERE owner = {}", ownerGuid);
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

    // DEBUG: Log what AI type was created and creature's AIName
    // ChatHandler(player->GetSession()).PSendSysMessage("[DEBUG] Guardian AIName from template: '{}'",
    //     guardian->GetAIName().empty() ? "(none)" : guardian->GetAIName());
    // ChatHandler(player->GetSession()).PSendSysMessage("[DEBUG] Guardian current AI type: {}",
    //     guardian->GetAI() ? typeid(*guardian->GetAI()).name() : "(null)");

    // EXPERIMENTAL: Try NOT replacing the AI - let the creature use its native SmartAI/CombatAI
    // and just modify its behavior through ownership/faction settings.
    // Comment out SetAI to test if native AI casts spells properly.
    //
    // If spells work without SetAI, the issue is in our AI wrapping approach.
    // If spells still don't work, the issue is something else (ownership, flags, etc.)

    // For now, let's test WITH our wrapper to preserve follow/protect behavior:
    CreatureAI* originalAI = FactorySelector::SelectAI(guardian);
    if (originalAI)
    {
        originalAI->InitializeAI();
    }
    guardian->SetAI(new CapturedGuardianAI(guardian, originalAI));

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
            { "",           HandleCaptureCommand,    SEC_PLAYER,        Console::No },
            { "dismiss",    HandleDismissCommand,    SEC_PLAYER,        Console::No },
            { "info",       HandleInfoCommand,       SEC_PLAYER,        Console::No },
            { "spawn",      HandleSpawnCommand,      SEC_GAMEMASTER,    Console::No },
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

    static bool HandleSpawnCommand(ChatHandler* handler, uint32 creatureEntry)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        // Check if creature entry is valid
        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureEntry);
        if (!cInfo)
        {
            handler->PSendSysMessage("Creature entry {} does not exist.", creatureEntry);
            return true;
        }

        // Check if player already has a guardian
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (data->guardianGuid || data->guardianEntry != 0)
        {
            handler->PSendSysMessage("You already have a guardian. Dismiss or release it first.");
            return true;
        }

        // Summon as guardian at the player's level
        uint8 level = player->GetLevel();
        TempSummon* guardian = SummonCapturedGuardian(player, creatureEntry, level);
        if (!guardian)
        {
            handler->PSendSysMessage("Failed to summon guardian.");
            return true;
        }

        data->guardianGuid = guardian->GetGUID();
        data->guardianEntry = creatureEntry;
        data->guardianLevel = level;
        data->guardianHealth = guardian->GetHealth();
        data->guardianPowerType = guardian->getPowerType();
        data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));

        SaveGuardianToDb(player, data);

        handler->PSendSysMessage("GM captured {} (Entry {}) at level {}.",
            cInfo->Name, creatureEntry, level);
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
        if (!config.enabled)
            return;

        // Load guardian data from database
        LoadGuardianFromDb(player);

        // Give player a Tesseract if they don't have one
        if (!player->HasItemCount(ITEM_TESSERACT, 1))
        {
            if (player->AddItem(ITEM_TESSERACT, 1))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ff00[Creature Capture]|r You have received a Tesseract! Use it to capture and summon guardians.");
            }
        }

        // Notify if they have a stored guardian
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (data->guardianEntry != 0)
        {
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data->guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Creature Capture]|r Your guardian {} is stored in the Tesseract. Use it to summon!", name);
        }
        else if (config.announce)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Creature Capture]|r Target a creature and use your Tesseract to capture it!");
        }
    }

    void OnPlayerLogout(Player* player) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        // If guardian is active, store its current state
        if (data->guardianGuid)
        {
            if (Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid))
            {
                data->guardianEntry = guardian->GetEntry();
                data->guardianLevel = guardian->GetLevel();
                data->guardianHealth = guardian->GetHealth();
                data->guardianPowerType = guardian->getPowerType();
                data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));
            }
        }

        // Save to database if we have a guardian
        if (data->guardianEntry != 0)
        {
            SaveGuardianToDb(player, data);
        }

        // Dismiss the active guardian
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

/*
 * Tesseract Item Script
 * Handles the Tesseract item for summoning/dismissing guardians via gossip menu
 */
class TesseractItemScript : public ItemScript
{
public:
    TesseractItemScript() : ItemScript("item_tesseract") {}

    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !item)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        // Check if player has an active guardian
        bool hasActiveGuardian = false;
        Creature* guardian = nullptr;

        if (data->guardianGuid)
        {
            guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
            if (guardian && guardian->IsAlive())
                hasActiveGuardian = true;
        }

        // Check if player has a stored guardian
        bool hasStoredGuardian = (data->guardianEntry != 0);

        // Check if player has a valid capture target
        Creature* target = ObjectAccessor::GetCreatureOrPetOrVehicle(*player, player->GetTarget());

        // DIRECT CAPTURE: If targeting a creature and don't have a guardian yet, capture directly!
        if (target && !hasActiveGuardian && !hasStoredGuardian)
        {
            std::string error;
            if (CanCaptureCreature(player, target, error))
            {
                // Perform capture directly - no gossip menu needed
                uint32 entry = target->GetEntry();
                uint8 level = target->GetLevel();

                // Despawn the original creature
                target->DespawnOrUnsummon();

                // Summon as guardian
                TempSummon* newGuardian = SummonCapturedGuardian(player, entry, level);
                if (newGuardian)
                {
                    data->guardianGuid = newGuardian->GetGUID();
                    data->guardianEntry = entry;
                    data->guardianLevel = level;
                    data->guardianHealth = newGuardian->GetHealth();
                    data->guardianPowerType = newGuardian->getPowerType();
                    data->guardianPower = newGuardian->GetPower(Powers(data->guardianPowerType));

                    // Save to database
                    SaveGuardianToDb(player, data);

                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff00ff00[Tesseract]|r {} has been captured!", newGuardian->GetName());
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff0000[Tesseract]|r Failed to capture creature.");
                }
                return true;
            }
            else
            {
                // Can't capture - show error
                ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000[Tesseract]|r {}", error);
                return true;
            }
        }

        // Otherwise, open gossip menu for summon/dismiss/info
        ClearGossipMenuFor(player);

        if (hasActiveGuardian)
        {
            // Guardian is out - offer to dismiss or release
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Dismiss " + std::string(guardian->GetName()),
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_DISMISS);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Guardian Info",
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_INFO);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                "Release Guardian (permanent)",
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_RELEASE);
        }
        else if (hasStoredGuardian)
        {
            // Guardian is stored - offer to summon or release
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data->guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Summon " + name,
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_SUMMON);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE,
                "Release Guardian (permanent)",
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_RELEASE);
        }
        else
        {
            // No guardian and no valid target
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Target a creature and use the Tesseract to capture it!",
                GOSSIP_SENDER_MAIN, 0);
        }

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, item->GetGUID());
        return true;
    }

    void OnGossipSelect(Player* player, Item* /*item*/, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        switch (action)
        {
            case TESSERACT_ACTION_SUMMON:
            {
                if (data->guardianEntry == 0)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("No guardian stored in Tesseract.");
                    return;
                }

                // Summon the guardian
                TempSummon* guardian = SummonCapturedGuardian(player, data->guardianEntry, data->guardianLevel);
                if (guardian)
                {
                    // Restore health and power
                    if (data->guardianHealth > 0 && data->guardianHealth <= guardian->GetMaxHealth())
                        guardian->SetHealth(data->guardianHealth);
                    if (data->guardianPower > 0)
                        guardian->SetPower(Powers(data->guardianPowerType), data->guardianPower);

                    data->guardianGuid = guardian->GetGUID();

                    ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[Tesseract]|r {} has been summoned!",
                        guardian->GetName());
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000[Tesseract]|r Failed to summon guardian.");
                }
                break;
            }
            case TESSERACT_ACTION_DISMISS:
            {
                if (!data->guardianGuid)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("No active guardian to dismiss.");
                    return;
                }

                Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
                if (guardian && guardian->IsAlive())
                {
                    // Store guardian state before dismissing
                    data->guardianEntry = guardian->GetEntry();
                    data->guardianLevel = guardian->GetLevel();
                    data->guardianHealth = guardian->GetHealth();
                    data->guardianPowerType = guardian->getPowerType();
                    data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));

                    std::string name = guardian->GetName();

                    // Despawn
                    guardian->DespawnOrUnsummon();
                    data->guardianGuid.Clear();

                    ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[Tesseract]|r {} has been stored.",
                        name);
                }
                else
                {
                    data->guardianGuid.Clear();
                    ChatHandler(player->GetSession()).PSendSysMessage("Guardian not found.");
                }
                break;
            }
            case TESSERACT_ACTION_INFO:
            {
                if (!data->guardianGuid)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("No active guardian.");
                    return;
                }

                Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
                if (guardian)
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("=== Guardian Info ===");
                    ChatHandler(player->GetSession()).PSendSysMessage("Name: {}", guardian->GetName());
                    ChatHandler(player->GetSession()).PSendSysMessage("Level: {}", guardian->GetLevel());
                    ChatHandler(player->GetSession()).PSendSysMessage("Health: {} / {}",
                        guardian->GetHealth(), guardian->GetMaxHealth());
                    ChatHandler(player->GetSession()).PSendSysMessage("Entry: {}", guardian->GetEntry());
                }
                break;
            }
            case TESSERACT_ACTION_RELEASE:
            {
                // Get name before we clear everything
                std::string name = "Guardian";
                if (data->guardianGuid)
                {
                    if (Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid))
                    {
                        name = guardian->GetName();
                        guardian->DespawnOrUnsummon();
                    }
                }
                else if (data->guardianEntry)
                {
                    if (CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(data->guardianEntry))
                        name = cInfo->Name;
                }

                // Clear all guardian data
                data->guardianGuid.Clear();
                data->guardianEntry = 0;
                data->guardianLevel = 0;
                data->guardianHealth = 0;
                data->guardianPower = 0;
                data->guardianPowerType = 0;
                data->savedToDb = false;

                // Delete from database
                DeleteGuardianFromDb(player);

                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff6600[Tesseract]|r {} has been released into the wild.", name);
                break;
            }
            default:
                break;
        }
    }
};


// Register scripts
void AddSC_mod_creature_capture()
{
    new CreatureCaptureCommandScript();
    new CreatureCapturePlayerScript();
    new CreatureCaptureWorldScript();
    new TesseractItemScript();
}
