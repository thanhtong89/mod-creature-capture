/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3 license
 *
 * Creature Capture Module
 * Allows players to capture NPCs and turn them into guardian companions.
 * Guardians use an archetype system (Tank/DPS/Healer) instead of delegating
 * to the creature's native AI.
 */

#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
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
#include "WorldPacket.h"

#include <algorithm>
#include <sstream>
#include <vector>

// Tesseract item constant (uses existing item 44807 from client Item.dbc)
constexpr uint32 ITEM_TESSERACT = 44807;

// Addon message prefix
static constexpr char ADDON_PREFIX[] = "CCAPTURE";

// Gossip menu actions — Tesseract item
enum TesseractGossipActions
{
    TESSERACT_ACTION_SUMMON     = 1,
    TESSERACT_ACTION_DISMISS    = 2,
    TESSERACT_ACTION_INFO       = 3,
    TESSERACT_ACTION_RELEASE    = 4
};

// Gossip menu actions — Guardian NPC (archetype selection)
enum GuardianGossipActions
{
    GUARDIAN_ACTION_ARCHETYPE_DPS    = 100,
    GUARDIAN_ACTION_ARCHETYPE_TANK   = 101,
    GUARDIAN_ACTION_ARCHETYPE_HEALER = 102,
    GUARDIAN_ACTION_CLOSE            = 199
};

// Guardian archetypes
enum GuardianArchetype : uint8
{
    ARCHETYPE_DPS    = 0,
    ARCHETYPE_TANK   = 1,
    ARCHETYPE_HEALER = 2
};

static const char* ArchetypeName(uint8 arch)
{
    switch (arch)
    {
        case ARCHETYPE_TANK:   return "Tank";
        case ARCHETYPE_HEALER: return "Healer";
        default:               return "DPS";
    }
}

// Constants
constexpr uint32 MAX_GUARDIAN_SPELLS = 8;
constexpr float GUARDIAN_FOLLOW_DIST = 3.0f;
constexpr float GUARDIAN_FOLLOW_ANGLE = M_PI / 2; // 90 degrees to the side
constexpr float HEALER_FOLLOW_DIST = 12.0f;
constexpr float HEALER_FOLLOW_ANGLE = M_PI; // Behind owner

// ============================================================================
// Addon Message Helper
// ============================================================================

static void SendCaptureAddonMessage(Player* player, std::string const& msg)
{
    WorldPacket data;
    std::size_t len = msg.length();
    data.Initialize(SMSG_MESSAGECHAT, 1 + 4 + 8 + 4 + 8 + 4 + 1 + len + 1);
    data << uint8(CHAT_MSG_WHISPER);
    data << uint32(LANG_ADDON);
    data << uint64(0);
    data << uint32(0);
    data << uint64(0);
    data << uint32(len + 1);
    data << msg;
    data << uint8(0);
    player->GetSession()->SendPacket(&data);
}

static void SendGuardianSpells(Player* player, uint32 const* spells)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tSPELLS";
    for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        ss << ":" << spells[i];
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianArchetype(Player* player, uint8 archetype)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tARCH:" << (uint32)archetype;
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianName(Player* player, std::string const& name)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tNAME:" << name;
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianDismiss(Player* player)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tDISMISS";
    SendCaptureAddonMessage(player, ss.str());
}

// Send all guardian state to the addon
static void SendFullGuardianState(Player* player, std::string const& name, uint8 archetype, uint32 const* spells)
{
    SendGuardianName(player, name);
    SendGuardianArchetype(player, archetype);
    SendGuardianSpells(player, spells);
}

// ============================================================================
// Data: Stored on Player via DataMap
// ============================================================================

class CapturedGuardianData : public DataMap::Base
{
public:
    CapturedGuardianData() : guardianGuid(), guardianEntry(0), guardianLevel(0),
        guardianHealth(0), guardianPower(0), guardianPowerType(0),
        archetype(ARCHETYPE_DPS), savedToDb(false)
    {
        memset(spellSlots, 0, sizeof(spellSlots));
    }

    ObjectGuid guardianGuid;
    // For respawn after cross-map teleport and persistence
    uint32 guardianEntry;
    uint8 guardianLevel;
    uint32 guardianHealth;
    uint32 guardianPower;
    uint8 guardianPowerType;
    uint8 archetype;
    uint32 spellSlots[MAX_GUARDIAN_SPELLS];
    bool savedToDb;
};

// ============================================================================
// CapturedGuardianAI — Archetype-driven combat AI
// ============================================================================

class CapturedGuardianAI : public CreatureAI
{
public:
    explicit CapturedGuardianAI(Creature* creature, uint8 archetype, uint32 const* spells)
        : CreatureAI(creature),
        _owner(nullptr),
        _archetype(archetype),
        _updateTimer(1000),
        _combatCheckTimer(500),
        _regenTimer(2000)
    {
        if (spells)
            memcpy(_spellSlots, spells, sizeof(_spellSlots));
        else
            memset(_spellSlots, 0, sizeof(_spellSlots));

        if (ObjectGuid ownerGuid = me->GetOwnerGUID())
            _owner = ObjectAccessor::GetPlayer(*me, ownerGuid);
    }

    void SetArchetype(uint8 arch)
    {
        _archetype = arch;
        // Adjust follow distance based on archetype
        if (_owner && !me->GetVictim())
        {
            me->GetMotionMaster()->Clear();
            float dist = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
            float angle = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_ANGLE : GUARDIAN_FOLLOW_ANGLE;
            me->GetMotionMaster()->MoveFollow(_owner, dist, angle);
        }
    }

    uint8 GetArchetype() const { return _archetype; }

    void SetSpell(uint32 slot, uint32 spellId)
    {
        if (slot < MAX_GUARDIAN_SPELLS)
            _spellSlots[slot] = spellId;
    }

    uint32 GetSpell(uint32 slot) const
    {
        return (slot < MAX_GUARDIAN_SPELLS) ? _spellSlots[slot] : 0;
    }

    uint32 const* GetSpells() const { return _spellSlots; }

    void UpdateAI(uint32 diff) override
    {
        if (!me->IsAlive())
            return;

        // Update owner reference
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
                    me->DespawnOrUnsummon();
                    return;
                }
            }

            // Teleport back if too far from owner
            if (_owner && me->GetDistance(_owner) > 50.0f)
            {
                float x, y, z;
                _owner->GetClosePoint(x, y, z, me->GetCombatReach(), GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);
                me->NearTeleportTo(x, y, z, me->GetOrientation());
            }
        }

        // In-combat behavior
        if (me->GetVictim())
        {
            // Check if should stop attacking
            if (!me->GetVictim()->IsAlive() ||
                !me->CanCreatureAttack(me->GetVictim()) ||
                (_owner && me->GetDistance(_owner) > 40.0f))
            {
                me->AttackStop();
                me->GetMotionMaster()->Clear();
                if (_owner)
                {
                    float dist = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
                    float angle = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_ANGLE : GUARDIAN_FOLLOW_ANGLE;
                    me->GetMotionMaster()->MoveFollow(_owner, dist, angle);
                }
                return;
            }

            // Archetype-specific combat
            switch (_archetype)
            {
                case ARCHETYPE_TANK:
                    UpdateTankAI(diff);
                    break;
                case ARCHETYPE_HEALER:
                    UpdateHealerAI(diff);
                    break;
                default:
                    UpdateDpsAI(diff);
                    break;
            }
        }
        else
        {
            // Out of combat — regenerate
            _regenTimer -= diff;
            if (_regenTimer <= 0)
            {
                _regenTimer = 2000;

                if (me->GetHealth() < me->GetMaxHealth())
                {
                    uint32 regenAmount = me->GetMaxHealth() * 6 / 100;
                    if (regenAmount < 1) regenAmount = 1;
                    me->SetHealth(std::min(me->GetHealth() + regenAmount, me->GetMaxHealth()));
                }

                if (me->GetMaxPower(POWER_MANA) > 0 && me->GetPower(POWER_MANA) < me->GetMaxPower(POWER_MANA))
                {
                    uint32 manaRegen = me->GetMaxPower(POWER_MANA) * 6 / 100;
                    if (manaRegen < 1) manaRegen = 1;
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
                    // Tank: prioritize owner's attackers
                    if (_archetype == ARCHETYPE_TANK)
                    {
                        if (Unit* attacker = _owner->getAttackerForHelper())
                        {
                            if (me->CanCreatureAttack(attacker))
                            {
                                me->AddThreat(attacker, 200.0f);
                                AttackStart(attacker);
                                return;
                            }
                        }
                    }

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

                    // Check own attackers
                    if (!me->GetVictim())
                    {
                        for (Unit* attacker : me->getAttackers())
                        {
                            if (attacker && attacker->IsAlive() && me->CanCreatureAttack(attacker))
                            {
                                me->AddThreat(attacker, 100.0f);
                                AttackStart(attacker);
                                return;
                            }
                        }
                    }

                    // Healer: also check if owner needs healing while out of combat
                    if (_archetype == ARCHETYPE_HEALER && _owner->IsAlive() && _owner->GetHealthPct() < 80.0f)
                    {
                        DoCastHealingSpells();
                    }
                }
            }

            // Follow owner
            if (_owner && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
            {
                float dist = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
                float angle = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_ANGLE : GUARDIAN_FOLLOW_ANGLE;
                me->GetMotionMaster()->MoveFollow(_owner, dist, angle);
            }
        }

        // Check summoned creatures — stop them from attacking the owner
        _summonCheckTimer -= diff;
        if (_summonCheckTimer <= 0 && _owner && !_summonedGuids.empty())
        {
            _summonCheckTimer = 500;

            for (auto it = _summonedGuids.begin(); it != _summonedGuids.end(); )
            {
                Creature* summon = ObjectAccessor::GetCreature(*me, *it);

                if (!summon || !summon->IsAlive() || !summon->IsInWorld())
                {
                    it = _summonedGuids.erase(it);
                    continue;
                }

                if (summon->GetVictim() == _owner)
                {
                    summon->GetThreatMgr().ClearAllThreat();
                    summon->AttackStop();
                    summon->SetFaction(_owner->GetFaction());

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

        summon->SetOwnerGUID(_owner->GetGUID());
        summon->SetCreatorGUID(_owner->GetGUID());
        summon->SetFaction(_owner->GetFaction());
        summon->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_PC);
        summon->SetImmuneToPC(false);
        summon->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
        summon->GetThreatMgr().ClearAllThreat();
        summon->SetReactState(REACT_DEFENSIVE);

        if (Unit* victim = me->GetVictim())
            summon->AI()->AttackStart(victim);

        _summonedGuids.push_back(summon->GetGUID());
    }

    void SummonedCreatureDespawn(Creature* summon) override
    {
        if (summon)
        {
            auto it = std::find(_summonedGuids.begin(), _summonedGuids.end(), summon->GetGUID());
            if (it != _summonedGuids.end())
                _summonedGuids.erase(it);
        }
    }

    void AttackStart(Unit* target) override
    {
        if (!target || !me->CanCreatureAttack(target))
            return;

        if (!me->IsInCombat())
        {
            me->SetInCombatWith(target);
            target->SetInCombatWith(me);
        }

        if (me->Attack(target, true))
        {
            // Healer stays at range
            if (_archetype == ARCHETYPE_HEALER)
            {
                // Don't chase — just face target and stay near owner
                me->SetFacingToObject(target);
            }
            else
            {
                me->GetMotionMaster()->MoveChase(target);
            }
        }
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        me->AttackStop();
        me->GetMotionMaster()->Clear();
        if (_owner)
        {
            float dist = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
            float angle = (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_ANGLE : GUARDIAN_FOLLOW_ANGLE;
            me->GetMotionMaster()->MoveFollow(_owner, dist, angle);
        }
    }

    void JustEngagedWith(Unit* /*who*/) override { }

    void KilledUnit(Unit* victim) override
    {
        if (_owner && victim && victim->IsCreature())
        {
            Creature* killed = victim->ToCreature();
            killed->SetLootRecipient(_owner);
            killed->LowerPlayerDamageReq(killed->GetMaxHealth());
        }
    }

    void SpellHit(Unit* /*caster*/, SpellInfo const* /*spellInfo*/) override { }

    void DamageTaken(Unit* /*attacker*/, uint32& /*damage*/, DamageEffectType /*damageType*/, SpellSchoolMask /*schoolMask*/) override { }

    void DamageDealt(Unit* victim, uint32& /*damage*/, DamageEffectType /*damageType*/, SpellSchoolMask /*damageSchoolMask*/) override
    {
        if (victim && victim->IsCreature() && _owner)
        {
            Creature* target = victim->ToCreature();
            target->SetLootRecipient(_owner);
            target->LowerPlayerDamageReq(target->GetHealth());
        }
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (_owner)
            ChatHandler(_owner->GetSession()).PSendSysMessage("Your captured guardian has died.");
    }

private:
    // ---- DPS archetype ----
    void UpdateDpsAI(uint32 /*diff*/)
    {
        DoMeleeAttackIfReady();
        DoCastOffensiveSpells();
    }

    // ---- Tank archetype ----
    void UpdateTankAI(uint32 /*diff*/)
    {
        DoMeleeAttackIfReady();

        // Generate extra threat on all nearby enemies to protect owner
        if (_owner)
        {
            for (auto const& ref : me->GetThreatMgr().GetThreatList())
            {
                if (Unit* target = ref->getTarget())
                {
                    if (target->GetVictim() == _owner)
                    {
                        me->AddThreat(target, 50.0f);
                    }
                }
            }
        }

        // Cast defensive spells on self, then offensive on target
        DoCastSelfBuffs();
        DoCastOffensiveSpells();
    }

    // ---- Healer archetype ----
    void UpdateHealerAI(uint32 /*diff*/)
    {
        // Priority: heal owner, then self, then attack
        bool healed = DoCastHealingSpells();
        if (!healed)
        {
            DoCastSelfBuffs();
            DoCastOffensiveSpells();
        }
    }

    // ---- Spell casting helpers ----

    void DoCastOffensiveSpells()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        Unit* target = me->GetVictim();
        if (!target)
            return;

        for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        {
            uint32 spellId = _spellSlots[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->CanBeUsedInCombat())
                continue;

            // Skip positive spells (heals/buffs) in offensive routine
            if (spellInfo->IsPositive())
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            // Check range
            if (spellInfo->GetMaxRange(false) > 0 &&
                !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                continue;

            // Handle periodic spells — don't reapply
            bool isPeriodic = spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE) ||
                              spellInfo->HasAura(SPELL_AURA_PERIODIC_LEECH) ||
                              spellInfo->HasAura(SPELL_AURA_PERIODIC_DAMAGE_PERCENT);
            if (isPeriodic && target->HasAura(spellId, me->GetGUID()))
                continue;

            me->CastSpell(target, spellId, false);
            ApplySpellCooldown(spellId, spellInfo, false);
            break;
        }
    }

    bool DoCastHealingSpells()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return false;

        // Find heal target: owner first, then self
        Unit* healTarget = nullptr;
        if (_owner && _owner->IsAlive() && _owner->GetHealthPct() < 80.0f)
            healTarget = _owner;
        else if (me->GetHealthPct() < 60.0f)
            healTarget = me;

        if (!healTarget)
            return false;

        for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        {
            uint32 spellId = _spellSlots[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            bool isHeal = spellInfo->IsPositive() && spellInfo->HasEffect(SPELL_EFFECT_HEAL);
            if (!isHeal)
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            me->CastSpell(healTarget, spellId, false);
            ApplySpellCooldown(spellId, spellInfo, true);
            return true;
        }

        return false;
    }

    void DoCastSelfBuffs()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        {
            uint32 spellId = _spellSlots[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            bool isHeal = spellInfo->HasEffect(SPELL_EFFECT_HEAL);
            if (!spellInfo->IsPositive() || isHeal)
                continue;

            if (me->HasAura(spellId))
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            me->CastSpell(me, spellId, false);
            ApplySpellCooldown(spellId, spellInfo, false);
            break;
        }
    }

    void ApplySpellCooldown(uint32 spellId, SpellInfo const* spellInfo, bool isHeal)
    {
        uint32 cooldown = spellInfo->RecoveryTime;

        if (isHeal && cooldown < 10000)
            cooldown = 10000;
        else
        {
            if (spellInfo->CategoryRecoveryTime > cooldown)
                cooldown = spellInfo->CategoryRecoveryTime;
            if (spellInfo->StartRecoveryTime > cooldown)
                cooldown = spellInfo->StartRecoveryTime;
            if (cooldown == 0)
                cooldown = 2000;

            cooldown += urand(500, 1500);
        }

        if (cooldown > 0)
            me->AddSpellCooldown(spellId, 0, cooldown);
    }

    Player* _owner;
    uint8 _archetype;
    uint32 _spellSlots[MAX_GUARDIAN_SPELLS];
    int32 _updateTimer;
    int32 _combatCheckTimer;
    int32 _regenTimer;
    int32 _summonCheckTimer = 1000;
    std::vector<ObjectGuid> _summonedGuids;
};

// ============================================================================
// Module Configuration
// ============================================================================

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

// ============================================================================
// Helper Functions
// ============================================================================

static void DismissGuardian(Player* player)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    if (data && data->guardianGuid)
    {
        if (Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid))
            guardian->DespawnOrUnsummon();
        data->guardianGuid.Clear();
    }
}

// Serialize spell slots to comma-separated string
static std::string SerializeSpells(uint32 const* spells)
{
    std::ostringstream ss;
    for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
    {
        if (i > 0) ss << ",";
        ss << spells[i];
    }
    return ss.str();
}

// Deserialize comma-separated spell IDs into array
static void DeserializeSpells(std::string const& str, uint32* spells)
{
    memset(spells, 0, sizeof(uint32) * MAX_GUARDIAN_SPELLS);
    if (str.empty())
        return;

    std::istringstream iss(str);
    std::string token;
    uint32 i = 0;
    while (std::getline(iss, token, ',') && i < MAX_GUARDIAN_SPELLS)
    {
        spells[i++] = std::strtoul(token.c_str(), nullptr, 10);
    }
}

// Populate initial spells from creature template
static void PopulateDefaultSpells(uint32 creatureEntry, uint32* spells)
{
    memset(spells, 0, sizeof(uint32) * MAX_GUARDIAN_SPELLS);
    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureEntry);
    if (!cInfo)
        return;

    uint32 slot = 0;
    for (uint8 i = 0; i < MAX_CREATURE_SPELLS && slot < MAX_GUARDIAN_SPELLS; ++i)
    {
        if (cInfo->spells[i])
            spells[slot++] = cInfo->spells[i];
    }
}

// ============================================================================
// Database Persistence
// ============================================================================

static void SaveGuardianToDb(Player* player, CapturedGuardianData* data)
{
    if (!data || data->guardianEntry == 0)
        return;

    uint32 ownerGuid = player->GetGUID().GetCounter();
    std::string spellStr = SerializeSpells(data->spellSlots);

    auto trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM character_guardian WHERE owner = {}", ownerGuid);
    trans->Append(
        "INSERT INTO character_guardian (owner, entry, level, slot, cur_health, cur_power, power_type, archetype, spells, save_time) "
        "VALUES ({}, {}, {}, 0, {}, {}, {}, {}, '{}', UNIX_TIMESTAMP())",
        ownerGuid,
        data->guardianEntry,
        data->guardianLevel,
        data->guardianHealth,
        data->guardianPower,
        data->guardianPowerType,
        data->archetype,
        spellStr
    );
    CharacterDatabase.CommitTransaction(trans);
    data->savedToDb = true;
}

static void LoadGuardianFromDb(Player* player)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();

    QueryResult result = CharacterDatabase.Query(
        "SELECT entry, level, cur_health, cur_power, power_type, archetype, spells FROM character_guardian WHERE owner = {} AND slot = 0",
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
    data->archetype = fields[5].Get<uint8>();
    DeserializeSpells(fields[6].Get<std::string>(), data->spellSlots);
    data->savedToDb = true;
}

static void DeleteGuardianFromDb(Player* player)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();
    CharacterDatabase.Execute("DELETE FROM character_guardian WHERE owner = {}", ownerGuid);
}

// ============================================================================
// Capture Validation
// ============================================================================

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

    if (cInfo->type == CREATURE_TYPE_CRITTER)
    {
        error = "Cannot capture critters.";
        return false;
    }

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

    if (target->IsInCombat() && target->GetVictim() != player)
    {
        error = "Creature is in combat with someone else.";
        return false;
    }

    if (!player->IsWithinDistInMap(target, 30.0f))
    {
        error = "Target is too far away.";
        return false;
    }

    return true;
}

// ============================================================================
// Summon Guardian
// ============================================================================

static TempSummon* SummonCapturedGuardian(Player* player, uint32 entry, uint8 level, uint8 archetype, uint32* spells)
{
    float x, y, z;
    player->GetClosePoint(x, y, z, player->GetCombatReach(), GUARDIAN_FOLLOW_DIST, GUARDIAN_FOLLOW_ANGLE);

    uint32 duration = config.guardianDuration > 0 ? config.guardianDuration * IN_MILLISECONDS : 0;

    TempSummon* guardian = player->SummonCreature(
        entry, x, y, z, player->GetOrientation(),
        TEMPSUMMON_MANUAL_DESPAWN, duration
    );

    if (!guardian)
        return nullptr;

    guardian->SetOwnerGUID(player->GetGUID());
    guardian->SetCreatorGUID(player->GetGUID());
    guardian->SetFaction(player->GetFaction());
    guardian->SetLevel(level);

    guardian->RemoveUnitFlag(UNIT_FLAG_IMMUNE_TO_NPC | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_ATTACKABLE_1);
    guardian->SetFaction(player->GetFaction());
    guardian->SetFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PLAYER_CONTROLLED);
    guardian->SetReactState(REACT_DEFENSIVE);

    if (config.healthPct != 100)
    {
        uint32 newHealth = guardian->GetMaxHealth() * config.healthPct / 100;
        guardian->SetMaxHealth(newHealth);
        guardian->SetHealth(newHealth);
    }

    // Set archetype-appropriate follow distance
    float followDist = (archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
    float followAngle = (archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_ANGLE : GUARDIAN_FOLLOW_ANGLE;
    guardian->GetMotionMaster()->MoveFollow(player, followDist, followAngle);

    // Install our archetype-driven AI (no SmartAI delegation)
    guardian->SetAI(new CapturedGuardianAI(guardian, archetype, spells));

    return guardian;
}

// ============================================================================
// Command Script
// ============================================================================

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
            { "teach",      HandleTeachCommand,      SEC_PLAYER,        Console::No },
            { "unlearn",    HandleUnlearnCommand,    SEC_PLAYER,        Console::No },
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

        Creature* target = handler->getSelectedCreature();
        std::string error;

        if (!CanCaptureCreature(player, target, error))
        {
            handler->PSendSysMessage("Cannot capture: {}", error);
            return true;
        }

        DismissGuardian(player);

        uint32 entry = target->GetEntry();
        uint8 level = target->GetLevel();
        std::string name = target->GetName();

        // Populate default spells from creature template
        uint32 spells[MAX_GUARDIAN_SPELLS];
        PopulateDefaultSpells(entry, spells);

        target->DespawnOrUnsummon();

        TempSummon* guardian = SummonCapturedGuardian(player, entry, level, ARCHETYPE_DPS, spells);
        if (!guardian)
        {
            handler->PSendSysMessage("Failed to summon guardian.");
            return true;
        }

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        data->guardianGuid = guardian->GetGUID();
        data->guardianEntry = entry;
        data->guardianLevel = level;
        data->guardianHealth = guardian->GetHealth();
        data->guardianPowerType = guardian->getPowerType();
        data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));
        data->archetype = ARCHETYPE_DPS;
        memcpy(data->spellSlots, spells, sizeof(spells));

        SaveGuardianToDb(player, data);

        handler->PSendSysMessage("You have captured {} (Level {})! It will now follow and protect you!", name, level);

        // Notify addon
        SendFullGuardianState(player, name, ARCHETYPE_DPS, spells);

        return true;
    }

    static bool HandleSpawnCommand(ChatHandler* handler, uint32 creatureEntry)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(creatureEntry);
        if (!cInfo)
        {
            handler->PSendSysMessage("Creature entry {} does not exist.", creatureEntry);
            return true;
        }

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (data->guardianGuid || data->guardianEntry != 0)
        {
            handler->PSendSysMessage("You already have a guardian. Dismiss or release it first.");
            return true;
        }

        uint8 level = player->GetLevel();
        uint32 spells[MAX_GUARDIAN_SPELLS];
        PopulateDefaultSpells(creatureEntry, spells);

        TempSummon* guardian = SummonCapturedGuardian(player, creatureEntry, level, ARCHETYPE_DPS, spells);
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
        data->archetype = ARCHETYPE_DPS;
        memcpy(data->spellSlots, spells, sizeof(spells));

        SaveGuardianToDb(player, data);

        handler->PSendSysMessage("GM captured {} (Entry {}) at level {}.", cInfo->Name, creatureEntry, level);

        SendFullGuardianState(player, cInfo->Name, ARCHETYPE_DPS, spells);

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
        SendGuardianDismiss(player);

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
        handler->PSendSysMessage("Archetype: {}", ArchetypeName(data->archetype));

        return true;
    }

    static bool HandleTeachCommand(ChatHandler* handler, uint32 slot, uint32 spellId)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r No active guardian.");
            return true;
        }

        if (slot < 1 || slot > MAX_GUARDIAN_SPELLS)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Invalid slot (1-{}).", MAX_GUARDIAN_SPELLS);
            return true;
        }

        Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
        if (!guardian)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Guardian not found.");
            return true;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Spell does not exist.");
            return true;
        }

        // Check power type compatibility
        // Allow: spells with no cost, spells costing health, and spells matching guardian's power type
        if (spellInfo->PowerType != POWER_HEALTH &&
            spellInfo->ManaCost > 0 &&
            static_cast<uint8>(spellInfo->PowerType) != guardian->getPowerType())
        {
            std::string powerName;
            switch (spellInfo->PowerType)
            {
                case POWER_MANA:   powerName = "Mana"; break;
                case POWER_RAGE:   powerName = "Rage"; break;
                case POWER_ENERGY: powerName = "Energy"; break;
                case POWER_FOCUS:  powerName = "Focus"; break;
                default:           powerName = "an unknown resource"; break;
            }
            handler->PSendSysMessage("|cffff0000[Guardian]|r This guardian cannot use {} spells.", powerName);
            return true;
        }

        // Also check ManaCostPercentage-based costs
        if (spellInfo->PowerType != POWER_HEALTH &&
            spellInfo->ManaCostPercentage > 0 &&
            static_cast<uint8>(spellInfo->PowerType) != guardian->getPowerType())
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r This guardian lacks the required resource for this spell.");
            return true;
        }

        uint32 slotIdx = slot - 1; // Convert to 0-based

        // Update AI
        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI());
        if (ai)
            ai->SetSpell(slotIdx, spellId);

        // Update data
        data->spellSlots[slotIdx] = spellId;
        SaveGuardianToDb(player, data);

        handler->PSendSysMessage("|cff00ff00[Guardian]|r Learned {} in slot {}.",
            spellInfo->SpellName[0], slot);

        // Notify addon with updated spells
        SendGuardianSpells(player, data->spellSlots);

        return true;
    }

    static bool HandleUnlearnCommand(ChatHandler* handler, uint32 slot)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r No active guardian.");
            return true;
        }

        if (slot < 1 || slot > MAX_GUARDIAN_SPELLS)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Invalid slot (1-{}).", MAX_GUARDIAN_SPELLS);
            return true;
        }

        Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
        if (!guardian)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Guardian not found.");
            return true;
        }

        uint32 slotIdx = slot - 1;
        uint32 oldSpellId = data->spellSlots[slotIdx];

        if (oldSpellId == 0)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Slot {} is already empty.", slot);
            return true;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(oldSpellId);
        std::string spellName = spellInfo ? spellInfo->SpellName[0] : "Unknown";

        // Update AI
        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI());
        if (ai)
            ai->SetSpell(slotIdx, 0);

        // Update data
        data->spellSlots[slotIdx] = 0;
        SaveGuardianToDb(player, data);

        handler->PSendSysMessage("|cff00ff00[Guardian]|r Unlearned {} from slot {}.", spellName, slot);

        SendGuardianSpells(player, data->spellSlots);

        return true;
    }
};

// ============================================================================
// Player Script — Handle teleport, logout, login
// ============================================================================

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

        LoadGuardianFromDb(player);

        if (!player->HasItemCount(ITEM_TESSERACT, 1))
        {
            if (player->AddItem(ITEM_TESSERACT, 1))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ff00[Creature Capture]|r You have received a Tesseract! Use it to capture and summon guardians.");
            }
        }

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

        if (data->guardianGuid)
        {
            if (Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid))
            {
                data->guardianEntry = guardian->GetEntry();
                data->guardianLevel = guardian->GetLevel();
                data->guardianHealth = guardian->GetHealth();
                data->guardianPowerType = guardian->getPowerType();
                data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));

                // Sync spells from AI
                if (CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI()))
                    memcpy(data->spellSlots, ai->GetSpells(), sizeof(data->spellSlots));
            }
        }

        if (data->guardianEntry != 0)
            SaveGuardianToDb(player, data);

        DismissGuardian(player);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid)
            return true;

        Creature* guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
        if (!guardian)
        {
            data->guardianGuid.Clear();
            return true;
        }

        bool sameMap = (player->GetMapId() == mapId);

        if (sameMap)
        {
            guardian->NearTeleportTo(x, y, z, guardian->GetOrientation());
        }
        else
        {
            data->guardianEntry = guardian->GetEntry();
            data->guardianLevel = guardian->GetLevel();
            data->guardianHealth = guardian->GetHealth();
            data->guardianPowerType = guardian->getPowerType();
            data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));

            // Preserve spells from AI
            if (CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI()))
                memcpy(data->spellSlots, ai->GetSpells(), sizeof(data->spellSlots));

            guardian->DespawnOrUnsummon();
            data->guardianGuid.Clear();

            SendGuardianDismiss(player);
        }

        return true;
    }

    void OnPlayerMapChanged(Player* player) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        if (data->guardianEntry && !data->guardianGuid)
        {
            TempSummon* guardian = SummonCapturedGuardian(player, data->guardianEntry, data->guardianLevel, data->archetype, data->spellSlots);
            if (guardian)
            {
                if (data->guardianHealth > 0 && data->guardianHealth <= guardian->GetMaxHealth())
                    guardian->SetHealth(data->guardianHealth);
                if (data->guardianPower > 0)
                    guardian->SetPower(Powers(data->guardianPowerType), data->guardianPower);

                data->guardianGuid = guardian->GetGUID();

                SendFullGuardianState(player, guardian->GetName(), data->archetype, data->spellSlots);
            }
        }
    }
};

// ============================================================================
// World Script — Load config
// ============================================================================

class CreatureCaptureWorldScript : public WorldScript
{
public:
    CreatureCaptureWorldScript() : WorldScript("CreatureCaptureWorldScript") {}

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        config.Load();
    }
};

// ============================================================================
// Tesseract Item Script
// ============================================================================

class TesseractItemScript : public ItemScript
{
public:
    TesseractItemScript() : ItemScript("item_tesseract") {}

    bool OnUse(Player* player, Item* item, SpellCastTargets const& /*targets*/) override
    {
        if (!player || !item)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        bool hasActiveGuardian = false;
        Creature* guardian = nullptr;

        if (data->guardianGuid)
        {
            guardian = ObjectAccessor::GetCreature(*player, data->guardianGuid);
            if (guardian && guardian->IsAlive())
                hasActiveGuardian = true;
        }

        bool hasStoredGuardian = (data->guardianEntry != 0);

        Creature* target = ObjectAccessor::GetCreatureOrPetOrVehicle(*player, player->GetTarget());

        // Direct capture if targeting a creature and no guardian
        if (target && !hasActiveGuardian && !hasStoredGuardian)
        {
            std::string error;
            if (CanCaptureCreature(player, target, error))
            {
                uint32 entry = target->GetEntry();
                uint8 level = target->GetLevel();

                uint32 spells[MAX_GUARDIAN_SPELLS];
                PopulateDefaultSpells(entry, spells);

                target->DespawnOrUnsummon();

                TempSummon* newGuardian = SummonCapturedGuardian(player, entry, level, ARCHETYPE_DPS, spells);
                if (newGuardian)
                {
                    data->guardianGuid = newGuardian->GetGUID();
                    data->guardianEntry = entry;
                    data->guardianLevel = level;
                    data->guardianHealth = newGuardian->GetHealth();
                    data->guardianPowerType = newGuardian->getPowerType();
                    data->guardianPower = newGuardian->GetPower(Powers(data->guardianPowerType));
                    data->archetype = ARCHETYPE_DPS;
                    memcpy(data->spellSlots, spells, sizeof(spells));

                    SaveGuardianToDb(player, data);

                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff00ff00[Tesseract]|r {} has been captured!", newGuardian->GetName());

                    SendFullGuardianState(player, newGuardian->GetName(), ARCHETYPE_DPS, spells);
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
                ChatHandler(player->GetSession()).PSendSysMessage("|cffff0000[Tesseract]|r {}", error);
                return true;
            }
        }

        // Gossip menu for summon/dismiss/info
        ClearGossipMenuFor(player);

        if (hasActiveGuardian)
        {
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

                TempSummon* guardian = SummonCapturedGuardian(player, data->guardianEntry, data->guardianLevel, data->archetype, data->spellSlots);
                if (guardian)
                {
                    if (data->guardianHealth > 0 && data->guardianHealth <= guardian->GetMaxHealth())
                        guardian->SetHealth(data->guardianHealth);
                    if (data->guardianPower > 0)
                        guardian->SetPower(Powers(data->guardianPowerType), data->guardianPower);

                    data->guardianGuid = guardian->GetGUID();

                    ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[Tesseract]|r {} has been summoned!",
                        guardian->GetName());

                    SendFullGuardianState(player, guardian->GetName(), data->archetype, data->spellSlots);
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
                    data->guardianEntry = guardian->GetEntry();
                    data->guardianLevel = guardian->GetLevel();
                    data->guardianHealth = guardian->GetHealth();
                    data->guardianPowerType = guardian->getPowerType();
                    data->guardianPower = guardian->GetPower(Powers(data->guardianPowerType));

                    if (CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI()))
                        memcpy(data->spellSlots, ai->GetSpells(), sizeof(data->spellSlots));

                    std::string name = guardian->GetName();

                    guardian->DespawnOrUnsummon();
                    data->guardianGuid.Clear();

                    SaveGuardianToDb(player, data);

                    ChatHandler(player->GetSession()).PSendSysMessage("|cff00ff00[Tesseract]|r {} has been stored.", name);
                    SendGuardianDismiss(player);
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
                    ChatHandler(player->GetSession()).PSendSysMessage("Archetype: {}", ArchetypeName(data->archetype));
                }
                break;
            }
            case TESSERACT_ACTION_RELEASE:
            {
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

                data->guardianGuid.Clear();
                data->guardianEntry = 0;
                data->guardianLevel = 0;
                data->guardianHealth = 0;
                data->guardianPower = 0;
                data->guardianPowerType = 0;
                data->archetype = ARCHETYPE_DPS;
                memset(data->spellSlots, 0, sizeof(data->spellSlots));
                data->savedToDb = false;

                DeleteGuardianFromDb(player);

                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff6600[Tesseract]|r {} has been released into the wild.", name);

                SendGuardianDismiss(player);
                break;
            }
            default:
                break;
        }
    }
};

// ============================================================================
// AllCreatureScript — Gossip injection for archetype selection on guardian NPC
// ============================================================================

class CaptureGuardianGossipScript : public AllCreatureScript
{
public:
    CaptureGuardianGossipScript() : AllCreatureScript("CaptureGuardianGossipScript") { }

    bool CanCreatureGossipHello(Player* player, Creature* creature) override
    {
        if (!config.enabled || !player || !creature)
            return false;

        // Check if this creature is the player's active guardian
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid || data->guardianGuid != creature->GetGUID())
            return false;

        if (creature->GetOwnerGUID() != player->GetGUID())
            return false;

        // Build the normal gossip menu first (preserves vendor/trainer/quest options)
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        // Append archetype selection
        std::string dpsLabel   = std::string("[DPS] Switch to DPS")    + (data->archetype == ARCHETYPE_DPS    ? " (active)" : "");
        std::string tankLabel  = std::string("[Tank] Switch to Tank")  + (data->archetype == ARCHETYPE_TANK   ? " (active)" : "");
        std::string healLabel  = std::string("[Healer] Switch to Healer") + (data->archetype == ARCHETYPE_HEALER ? " (active)" : "");

        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, dpsLabel,  GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_ARCHETYPE_DPS);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, tankLabel, GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_ARCHETYPE_TANK);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, healLabel, GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_ARCHETYPE_HEALER);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind.", GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_CLOSE);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!config.enabled || !player || !creature)
            return false;

        // Only handle our actions
        if (action < GUARDIAN_ACTION_ARCHETYPE_DPS || action > GUARDIAN_ACTION_CLOSE)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        if (!data->guardianGuid || data->guardianGuid != creature->GetGUID())
            return false;

        CloseGossipMenuFor(player);

        if (action == GUARDIAN_ACTION_CLOSE)
            return true;

        uint8 newArchetype;
        switch (action)
        {
            case GUARDIAN_ACTION_ARCHETYPE_TANK:   newArchetype = ARCHETYPE_TANK;   break;
            case GUARDIAN_ACTION_ARCHETYPE_HEALER: newArchetype = ARCHETYPE_HEALER; break;
            default:                               newArchetype = ARCHETYPE_DPS;    break;
        }

        if (data->archetype == newArchetype)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Guardian]|r Already set to {} archetype.", ArchetypeName(newArchetype));
            return true;
        }

        data->archetype = newArchetype;

        // Update the AI
        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(creature->AI());
        if (ai)
            ai->SetArchetype(newArchetype);

        SaveGuardianToDb(player, data);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff00ff00[Guardian]|r Switched to {} archetype.", ArchetypeName(newArchetype));

        SendGuardianArchetype(player, newArchetype);

        return true;
    }
};

// ============================================================================
// Registration
// ============================================================================

void AddSC_mod_creature_capture()
{
    new CreatureCaptureCommandScript();
    new CreatureCapturePlayerScript();
    new CreatureCaptureWorldScript();
    new TesseractItemScript();
    new CaptureGuardianGossipScript();
}
