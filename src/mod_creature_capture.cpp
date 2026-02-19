/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU AGPL v3 license
 *
 * Creature Capture Module
 * Allows players to capture NPCs and turn them into guardian companions.
 * Supports up to 4 guardian slots per player with archetype system (Tank/DPS/Healer).
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
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <vector>

// Tesseract item constant (uses existing item 44807 from client Item.dbc)
constexpr uint32 ITEM_TESSERACT = 44807;

// Addon message prefix
static constexpr char ADDON_PREFIX[] = "CCAPTURE";

// Limits
constexpr uint32 MAX_GUARDIAN_SLOTS  = 4;
constexpr uint32 MAX_GUARDIAN_SPELLS = 8;

// Follow distances
constexpr float GUARDIAN_FOLLOW_DIST = 3.0f;
constexpr float HEALER_FOLLOW_DIST   = 12.0f;

// Follow angles per slot (spread around player)
// Slot 0: front-right (~45deg), Slot 1: back-right (~135deg),
// Slot 2: back-left (~225deg), Slot 3: front-left (~315deg)
static const float GUARDIAN_FOLLOW_ANGLES[MAX_GUARDIAN_SLOTS] = {
    static_cast<float>(M_PI / 4.0),          // 45
    static_cast<float>(3.0 * M_PI / 4.0),    // 135
    static_cast<float>(5.0 * M_PI / 4.0),    // 225
    static_cast<float>(7.0 * M_PI / 4.0),    // 315
};

// Gossip action encoding for Tesseract item:
//   encoded = slot * 10 + action   (actions 1-4)
//   decode:  slot = encoded / 10,  action = encoded % 10
enum TesseractGossipActions
{
    TESSERACT_ACTION_SUMMON  = 1,
    TESSERACT_ACTION_DISMISS = 2,
    TESSERACT_ACTION_INFO    = 3,
    TESSERACT_ACTION_RELEASE = 4,
    TESSERACT_ACTION_CLOSE   = 99
};

// Guardian gossip action encoding:
//   encoded = 100 + slot * 10 + archetype   (archetype 0-2)
//   decode:  slot = (encoded - 100) / 10,   archetype = (encoded - 100) % 10
enum GuardianGossipActions
{
    GUARDIAN_ACTION_BASE  = 100,
    GUARDIAN_ACTION_CLOSE = 199
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

// ============================================================================
// Module Configuration (before data structures so FindEmptySlot can use it)
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
    uint8 maxSlots = 4;

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
        uint8 slots = sConfigMgr->GetOption<uint8>("CreatureCapture.MaxSlots", 4);
        maxSlots = std::max(uint8(1), std::min(uint8(MAX_GUARDIAN_SLOTS), slots));
    }
};

static CreatureCaptureConfig config;

// ============================================================================
// Addon Message Helpers (slot-aware)
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

static void SendGuardianSpells(Player* player, uint8 slot, uint32 const* spells)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tSPELLS:" << (uint32)slot;
    for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        ss << ":" << spells[i];
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianArchetype(Player* player, uint8 slot, uint8 archetype)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tARCH:" << (uint32)slot << ":" << (uint32)archetype;
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianName(Player* player, uint8 slot, std::string const& name)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tNAME:" << (uint32)slot << ":" << name;
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianDismiss(Player* player, uint8 slot)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tDISMISS:" << (uint32)slot;
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianGuid(Player* player, uint8 slot, ObjectGuid guid)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tGUID:" << (uint32)slot << ":";
    // Format as hex matching UnitGUID("target") format: 0x0000000000000000
    ss << "0x" << std::hex << std::uppercase << std::setfill('0')
       << std::setw(16) << guid.GetRawValue();
    SendCaptureAddonMessage(player, ss.str());
}

static void SendGuardianClear(Player* player, uint8 slot)
{
    std::ostringstream ss;
    ss << ADDON_PREFIX << "\tCLEAR:" << (uint32)slot;
    SendCaptureAddonMessage(player, ss.str());
}

// Forward declarations for data types
struct GuardianSlotData;
class CapturedGuardianData;

static void SendFullSlotState(Player* player, uint8 slot, GuardianSlotData const& slotData);
static void SendAllSlotsState(Player* player);

// ============================================================================
// Data Structures
// ============================================================================

struct GuardianSlotData
{
    ObjectGuid guardianGuid;
    uint32 guardianEntry    = 0;
    uint8  guardianLevel    = 0;
    uint32 guardianHealth   = 0;
    uint32 guardianPower    = 0;
    uint8  guardianPowerType = 0;
    uint8  archetype        = ARCHETYPE_DPS;
    uint32 spellSlots[MAX_GUARDIAN_SPELLS] = {};
    uint32 displayId        = 0;
    int8   equipmentId      = 0;
    bool   dismissed        = false;
    bool   savedToDb        = false;

    void Clear()
    {
        guardianGuid.Clear();
        guardianEntry = 0;
        guardianLevel = 0;
        guardianHealth = 0;
        guardianPower = 0;
        guardianPowerType = 0;
        archetype = ARCHETYPE_DPS;
        memset(spellSlots, 0, sizeof(spellSlots));
        displayId = 0;
        equipmentId = 0;
        dismissed = false;
        savedToDb = false;
    }

    bool IsOccupied() const { return guardianEntry != 0; }
    bool IsActive()   const { return !guardianGuid.IsEmpty(); }
};

class CapturedGuardianData : public DataMap::Base
{
public:
    GuardianSlotData slots[MAX_GUARDIAN_SLOTS];

    int8 FindEmptySlot() const
    {
        for (uint8 i = 0; i < config.maxSlots; ++i)
            if (!slots[i].IsOccupied())
                return static_cast<int8>(i);
        return -1;
    }

    int8 FindSlotByGuid(ObjectGuid guid) const
    {
        if (guid.IsEmpty()) return -1;
        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
            if (slots[i].guardianGuid == guid)
                return static_cast<int8>(i);
        return -1;
    }

    int8 FindSlotByEntry(uint32 entry) const
    {
        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
            if (slots[i].guardianEntry == entry)
                return static_cast<int8>(i);
        return -1;
    }
};

// ============================================================================
// CapturedGuardianAI — Archetype-driven combat AI
// ============================================================================

class CapturedGuardianAI : public CreatureAI
{
public:
    explicit CapturedGuardianAI(Creature* creature, uint8 archetype, uint32 const* spells, uint8 slotIndex)
        : CreatureAI(creature),
        _owner(nullptr),
        _archetype(archetype),
        _slotIndex(slotIndex),
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

    float GetFollowDist() const
    {
        return (_archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
    }

    float GetFollowAngle() const
    {
        return GUARDIAN_FOLLOW_ANGLES[_slotIndex % MAX_GUARDIAN_SLOTS];
    }

    void SetArchetype(uint8 arch)
    {
        _archetype = arch;
        if (_owner && !me->GetVictim())
        {
            me->GetMotionMaster()->Clear();
            me->GetMotionMaster()->MoveFollow(_owner, GetFollowDist(), GetFollowAngle());
        }
    }

    uint8 GetArchetype() const { return _archetype; }
    uint8 GetSlotIndex() const { return _slotIndex; }

    void SetSpell(uint32 slot, uint32 spellId)
    {
        if (slot < MAX_GUARDIAN_SPELLS)
            _spellSlots[slot] = spellId;

        if (spellId)
            EquipFallbackWeaponForSpell(spellId);
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
                _owner->GetClosePoint(x, y, z, me->GetCombatReach(), GetFollowDist(), GetFollowAngle());
                me->NearTeleportTo(x, y, z, me->GetOrientation());
            }
        }

        // In-combat behavior
        if (me->GetVictim())
        {
            if (!me->GetVictim()->IsAlive() ||
                !me->CanCreatureAttack(me->GetVictim()) ||
                (_owner && me->GetDistance(_owner) > 40.0f))
            {
                me->AttackStop();
                me->GetMotionMaster()->Clear();
                if (_owner)
                    me->GetMotionMaster()->MoveFollow(_owner, GetFollowDist(), GetFollowAngle());
                return;
            }

            switch (_archetype)
            {
                case ARCHETYPE_TANK:   UpdateTankAI(diff);   break;
                case ARCHETYPE_HEALER: UpdateHealerAI(diff); break;
                default:               UpdateDpsAI(diff);    break;
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

                    if (Unit* attacker = _owner->getAttackerForHelper())
                    {
                        if (me->CanCreatureAttack(attacker))
                        {
                            AttackStart(attacker);
                            return;
                        }
                    }

                    if (Unit* ownerTarget = _owner->GetVictim())
                    {
                        if (me->CanCreatureAttack(ownerTarget))
                        {
                            AttackStart(ownerTarget);
                            return;
                        }
                    }

                    // Defend fellow guardians being attacked
                    if (!me->GetVictim())
                    {
                        Unit* allyAttacker = FindAllyAttacker();
                        if (allyAttacker)
                        {
                            AttackStart(allyAttacker);
                            return;
                        }
                    }

                    // Defend self from attackers
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

                    if (_archetype == ARCHETYPE_HEALER && _owner->IsAlive() && _owner->GetHealthPct() < 80.0f)
                        DoCastHealingSpells();
                }
            }

            // Follow owner
            if (_owner && me->GetMotionMaster()->GetCurrentMovementGeneratorType() != FOLLOW_MOTION_TYPE)
                me->GetMotionMaster()->MoveFollow(_owner, GetFollowDist(), GetFollowAngle());
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
            me->GetMotionMaster()->MoveChase(target);
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        me->AttackStop();
        me->GetMotionMaster()->Clear();
        if (_owner)
            me->GetMotionMaster()->MoveFollow(_owner, GetFollowDist(), GetFollowAngle());
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
    // Find an enemy attacking any fellow guardian
    Unit* FindAllyAttacker()
    {
        if (!_owner)
            return nullptr;

        CapturedGuardianData* data = _owner->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsActive() || s.guardianGuid == me->GetGUID())
                continue;

            Creature* ally = ObjectAccessor::GetCreature(*me, s.guardianGuid);
            if (!ally || !ally->IsAlive())
                continue;

            for (Unit* attacker : ally->getAttackers())
            {
                if (attacker && attacker->IsAlive() && me->CanCreatureAttack(attacker))
                    return attacker;
            }
        }
        return nullptr;
    }

    // Equip a fallback weapon if the spell requires one and the creature lacks it
    void EquipFallbackWeaponForSpell(uint32 spellId)
    {
        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return;

        // Check for ranged weapon need: DmgClass ranged, or uses ranged slot, or auto-repeat
        bool needsRanged = (spellInfo->DmgClass == SPELL_DAMAGE_CLASS_RANGED &&
                            spellInfo->IsRangedWeaponSpell()) ||
                           spellInfo->HasAttribute(SPELL_ATTR0_USES_RANGED_SLOT);

        if (needsRanged && !me->HasWeapon(RANGED_ATTACK))
        {
            // 2504 = Worn Shortbow (common item in all AzerothCore DBs)
            me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID + 2, 2504);
            return;
        }

        // Check for melee weapon need: DmgClass melee with weapon damage effects
        bool needsMelee = spellInfo->DmgClass == SPELL_DAMAGE_CLASS_MELEE &&
                          (spellInfo->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE) ||
                           spellInfo->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL) ||
                           spellInfo->HasEffect(SPELL_EFFECT_NORMALIZED_WEAPON_DMG) ||
                           spellInfo->HasAttribute(SPELL_ATTR3_REQUIRES_MAIN_HAND_WEAPON));

        if (needsMelee && !me->HasWeapon(BASE_ATTACK))
        {
            // 25 = Worn Shortsword (common item in all AzerothCore DBs)
            me->SetUInt32Value(UNIT_VIRTUAL_ITEM_SLOT_ID, 25);
        }
    }

    void UpdateDpsAI(uint32 /*diff*/)
    {
        DoMeleeAttackIfReady();
        DoCastOffensiveSpells();
    }

    void UpdateTankAI(uint32 /*diff*/)
    {
        DoMeleeAttackIfReady();

        if (_owner)
        {
            // Collect healer guardians to protect
            std::vector<ObjectGuid> healerGuids;
            CapturedGuardianData* data = _owner->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
            for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
            {
                GuardianSlotData& s = data->slots[i];
                if (s.IsActive() && s.guardianGuid != me->GetGUID() && s.archetype == ARCHETYPE_HEALER)
                    healerGuids.push_back(s.guardianGuid);
            }

            for (auto const& ref : me->GetThreatMgr().GetThreatList())
            {
                if (Unit* target = ref->getTarget())
                {
                    Unit* targetVictim = target->GetVictim();
                    if (!targetVictim)
                        continue;

                    // Protect owner
                    if (targetVictim == _owner)
                    {
                        me->AddThreat(target, 50.0f);
                        continue;
                    }

                    // Protect healer guardians
                    for (ObjectGuid const& healerGuid : healerGuids)
                    {
                        if (targetVictim->GetGUID() == healerGuid)
                        {
                            me->AddThreat(target, 80.0f);
                            break;
                        }
                    }
                }
            }
        }

        DoCastSelfBuffs();
        DoCastOffensiveSpells();
    }

    void UpdateHealerAI(uint32 /*diff*/)
    {
        // Priority 1: Heal owner/self if needed
        if (DoCastHealingSpells())
            return;

        // Priority 2: Maintain self buffs
        if (DoCastSelfBuffs())
            return;

        // Priority 3: Buff allies (owner + other guardians)
        if (DoCastAllyBuffs())
            return;

        // Priority 4: Debuff current target
        if (DoCastDebuffSpells())
            return;

        // Priority 5: Offensive spells + melee
        DoMeleeAttackIfReady();
        DoCastOffensiveSpells();
    }

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

            if (spellInfo->IsPositive())
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            if (spellInfo->GetMaxRange(false) > 0 &&
                !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                continue;

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

        // Build prioritized list of heal targets
        Unit* healTarget = nullptr;

        // Owner at 50% or below is top priority
        if (_owner && _owner->IsAlive() && _owner->GetHealthPct() < 50.0f)
            healTarget = _owner;
        // Self at 50%
        else if (me->GetHealthPct() < 50.0f)
            healTarget = me;
        // Check other guardians belonging to owner
        else if (_owner)
        {
            CapturedGuardianData* data = _owner->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
            for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
            {
                GuardianSlotData& s = data->slots[i];
                if (!s.IsActive() || s.guardianGuid == me->GetGUID())
                    continue;

                Creature* ally = ObjectAccessor::GetCreature(*me, s.guardianGuid);
                if (ally && ally->IsAlive() && ally->GetHealthPct() < 50.0f)
                {
                    healTarget = ally;
                    break;
                }
            }
        }

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

    bool DoCastSelfBuffs()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return false;

        bool cast = false;
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
            cast = true;
            break;
        }
        return cast;
    }

    bool DoCastAllyBuffs()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING) || !_owner)
            return false;

        // Collect allies: owner + other active guardians
        std::vector<Unit*> allies;
        if (_owner->IsAlive())
            allies.push_back(_owner);

        CapturedGuardianData* data = _owner->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsActive() || s.guardianGuid == me->GetGUID())
                continue;

            Creature* ally = ObjectAccessor::GetCreature(*me, s.guardianGuid);
            if (ally && ally->IsAlive() && me->IsWithinDistInMap(ally, 30.0f))
                allies.push_back(ally);
        }

        for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        {
            uint32 spellId = _spellSlots[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || !spellInfo->IsPositive())
                continue;

            if (spellInfo->HasEffect(SPELL_EFFECT_HEAL))
                continue;

            // Must have an aura component to be a buff
            bool hasBuff = false;
            for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
            {
                if (spellInfo->Effects[eff].IsAura())
                {
                    hasBuff = true;
                    break;
                }
            }
            if (!hasBuff)
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            // Find an ally missing this buff
            for (Unit* ally : allies)
            {
                if (ally->HasAura(spellId))
                    continue;

                me->CastSpell(ally, spellId, false);
                ApplySpellCooldown(spellId, spellInfo, false);
                return true;
            }
        }
        return false;
    }

    bool DoCastDebuffSpells()
    {
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return false;

        Unit* target = me->GetVictim();
        if (!target)
            return false;

        for (uint32 i = 0; i < MAX_GUARDIAN_SPELLS; ++i)
        {
            uint32 spellId = _spellSlots[i];
            if (!spellId)
                continue;

            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo || spellInfo->IsPositive())
                continue;

            // Skip direct damage spells — those are handled by DoCastOffensiveSpells
            if (spellInfo->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE) ||
                spellInfo->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE) ||
                spellInfo->HasEffect(SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL) ||
                spellInfo->HasEffect(SPELL_EFFECT_NORMALIZED_WEAPON_DMG))
                continue;

            // Must have an aura component (debuff)
            bool hasDebuff = false;
            for (uint8 eff = 0; eff < MAX_SPELL_EFFECTS; ++eff)
            {
                if (spellInfo->Effects[eff].IsAura())
                {
                    hasDebuff = true;
                    break;
                }
            }
            if (!hasDebuff)
                continue;

            if (target->HasAura(spellId, me->GetGUID()))
                continue;

            if (me->HasSpellCooldown(spellId))
                continue;

            if (spellInfo->GetMaxRange(false) > 0 &&
                !me->IsWithinDistInMap(target, spellInfo->GetMaxRange(false)))
                continue;

            me->CastSpell(target, spellId, false);
            ApplySpellCooldown(spellId, spellInfo, false);
            return true;
        }
        return false;
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
    uint8 _slotIndex;
    uint32 _spellSlots[MAX_GUARDIAN_SPELLS];
    int32 _updateTimer;
    int32 _combatCheckTimer;
    int32 _regenTimer;
    int32 _summonCheckTimer = 1000;
    std::vector<ObjectGuid> _summonedGuids;
};

// ============================================================================
// Addon Message — Full state helpers (defined after data structures)
// ============================================================================

static void SendFullSlotState(Player* player, uint8 slot, GuardianSlotData const& slotData)
{
    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(slotData.guardianEntry);
    std::string name = cInfo ? cInfo->Name : "Guardian";
    SendGuardianName(player, slot, name);
    SendGuardianArchetype(player, slot, slotData.archetype);
    SendGuardianSpells(player, slot, slotData.spellSlots);
    if (slotData.IsActive())
        SendGuardianGuid(player, slot, slotData.guardianGuid);
}

static void SendAllSlotsState(Player* player)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    for (uint8 i = 0; i < config.maxSlots; ++i)
    {
        if (data->slots[i].IsOccupied())
            SendFullSlotState(player, i, data->slots[i]);
    }
}

// ============================================================================
// Helper Functions
// ============================================================================

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
// Database Persistence (per-slot)
// ============================================================================

static void SaveGuardianSlotToDb(Player* player, GuardianSlotData* slotData, uint8 slotIndex)
{
    if (!slotData || slotData->guardianEntry == 0)
        return;

    uint32 ownerGuid = player->GetGUID().GetCounter();
    std::string spellStr = SerializeSpells(slotData->spellSlots);

    auto trans = CharacterDatabase.BeginTransaction();
    trans->Append("DELETE FROM character_guardian WHERE owner = {} AND slot = {}", ownerGuid, slotIndex);
    trans->Append(
        "INSERT INTO character_guardian (owner, entry, level, slot, cur_health, cur_power, power_type, archetype, spells, display_id, equipment_id, dismissed, save_time) "
        "VALUES ({}, {}, {}, {}, {}, {}, {}, {}, '{}', {}, {}, {}, UNIX_TIMESTAMP())",
        ownerGuid,
        slotData->guardianEntry,
        slotData->guardianLevel,
        slotIndex,
        slotData->guardianHealth,
        slotData->guardianPower,
        slotData->guardianPowerType,
        slotData->archetype,
        spellStr,
        slotData->displayId,
        slotData->equipmentId,
        slotData->dismissed ? 1 : 0
    );
    CharacterDatabase.CommitTransaction(trans);
    slotData->savedToDb = true;
}

static void SaveAllGuardiansToDb(Player* player)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
    {
        if (data->slots[i].IsOccupied())
            SaveGuardianSlotToDb(player, &data->slots[i], i);
    }
}

static void LoadGuardiansFromDb(Player* player)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();

    QueryResult result = CharacterDatabase.Query(
        "SELECT slot, entry, level, cur_health, cur_power, power_type, archetype, spells, display_id, equipment_id, dismissed "
        "FROM character_guardian WHERE owner = {}",
        ownerGuid
    );

    if (!result)
        return;

    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

    do
    {
        Field* fields = result->Fetch();
        uint8 slot = fields[0].Get<uint8>();
        if (slot >= MAX_GUARDIAN_SLOTS)
            continue;

        GuardianSlotData& s = data->slots[slot];
        s.guardianEntry     = fields[1].Get<uint32>();
        s.guardianLevel     = fields[2].Get<uint8>();
        s.guardianHealth    = fields[3].Get<uint32>();
        s.guardianPower     = fields[4].Get<uint32>();
        s.guardianPowerType = fields[5].Get<uint8>();
        s.archetype         = fields[6].Get<uint8>();
        DeserializeSpells(fields[7].Get<std::string>(), s.spellSlots);
        s.displayId         = fields[8].Get<uint32>();
        s.equipmentId       = fields[9].Get<int8>();
        s.dismissed         = fields[10].Get<uint8>() != 0;
        s.savedToDb         = true;
    }
    while (result->NextRow());
}

static void DeleteGuardianSlotFromDb(Player* player, uint8 slotIndex)
{
    uint32 ownerGuid = player->GetGUID().GetCounter();
    CharacterDatabase.Execute("DELETE FROM character_guardian WHERE owner = {} AND slot = {}", ownerGuid, slotIndex);
}

// ============================================================================
// Dismiss / Snapshot Helpers
// ============================================================================

static void SnapshotGuardianSlot(Player* player, uint8 slotIndex)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    GuardianSlotData& s = data->slots[slotIndex];

    if (!s.IsActive())
        return;

    Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid);
    if (!guardian)
        return;

    s.guardianEntry     = guardian->GetEntry();
    s.guardianLevel     = guardian->GetLevel();
    s.guardianHealth    = guardian->GetHealth();
    s.guardianPowerType = guardian->getPowerType();
    s.guardianPower     = guardian->GetPower(Powers(s.guardianPowerType));

    if (CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI()))
        memcpy(s.spellSlots, ai->GetSpells(), sizeof(s.spellSlots));
}

static void DismissGuardianSlot(Player* player, uint8 slotIndex)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    GuardianSlotData& s = data->slots[slotIndex];

    if (s.IsActive())
    {
        if (Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid))
            guardian->DespawnOrUnsummon();
        s.guardianGuid.Clear();
    }
}

static void DismissAllGuardians(Player* player)
{
    CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
    for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        DismissGuardianSlot(player, i);
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
// Summon Guardian (slot-aware)
// ============================================================================

static TempSummon* SummonCapturedGuardian(Player* player, uint32 entry, uint8 level, uint8 archetype,
    uint32* spells, uint8 slotIndex, uint32 displayId = 0, int8 equipmentId = 0)
{
    float angle = GUARDIAN_FOLLOW_ANGLES[slotIndex % MAX_GUARDIAN_SLOTS];
    float dist  = (archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;

    float x, y, z;
    player->GetClosePoint(x, y, z, player->GetCombatReach(), dist, angle);

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
    guardian->SetFlag(UNIT_NPC_FLAGS, UNIT_NPC_FLAG_GOSSIP);
    guardian->SetReactState(REACT_DEFENSIVE);

    // Clear any inherited threat/combat state from the creature template
    guardian->GetThreatMgr().ClearAllThreat();
    guardian->CombatStop(true);
    guardian->GetMotionMaster()->Clear();

    if (config.healthPct != 100)
    {
        uint32 newHealth = guardian->GetMaxHealth() * config.healthPct / 100;
        guardian->SetMaxHealth(newHealth);
        guardian->SetHealth(newHealth);
    }

    // Restore display model if captured with a specific one
    if (displayId != 0)
        guardian->SetDisplayId(displayId);

    // Restore equipment if captured with weapons
    if (equipmentId > 0)
        guardian->LoadEquipment(equipmentId, true);

    guardian->GetMotionMaster()->MoveFollow(player, dist, angle);

    // Install archetype-driven AI with slot index
    guardian->SetAI(new CapturedGuardianAI(guardian, archetype, spells, slotIndex));

    return guardian;
}

// ============================================================================
// Target-based slot resolution helper (for commands)
// ============================================================================

static int8 FindTargetedGuardianSlot(Player* player, CapturedGuardianData* data)
{
    Unit* selected = player->GetSelectedUnit();
    if (!selected || !selected->IsCreature())
        return -1;

    return data->FindSlotByGuid(selected->GetGUID());
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

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 emptySlot = data->FindEmptySlot();
        if (emptySlot < 0)
        {
            handler->PSendSysMessage("All guardian slots are full. Release a guardian first.");
            return true;
        }

        uint32 entry = target->GetEntry();
        uint8 level = target->GetLevel();
        std::string name = target->GetName();
        uint32 capturedDisplayId = target->GetDisplayId();
        int8 capturedEquipmentId = static_cast<int8>(target->GetCurrentEquipmentId());

        uint32 spells[MAX_GUARDIAN_SPELLS];
        PopulateDefaultSpells(entry, spells);

        target->DespawnOrUnsummon();

        TempSummon* guardian = SummonCapturedGuardian(player, entry, level, ARCHETYPE_DPS, spells,
            static_cast<uint8>(emptySlot), capturedDisplayId, capturedEquipmentId);
        if (!guardian)
        {
            handler->PSendSysMessage("Failed to summon guardian.");
            return true;
        }

        GuardianSlotData& s = data->slots[emptySlot];
        s.guardianGuid      = guardian->GetGUID();
        s.guardianEntry     = entry;
        s.guardianLevel     = level;
        s.guardianHealth    = guardian->GetHealth();
        s.guardianPowerType = guardian->getPowerType();
        s.guardianPower     = guardian->GetPower(Powers(s.guardianPowerType));
        s.archetype         = ARCHETYPE_DPS;
        s.displayId         = capturedDisplayId;
        s.equipmentId       = capturedEquipmentId;
        memcpy(s.spellSlots, spells, sizeof(spells));

        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(emptySlot));

        handler->PSendSysMessage("You have captured {} (Level {}) in slot {}!", name, level, emptySlot + 1);

        SendFullSlotState(player, static_cast<uint8>(emptySlot), s);

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
        int8 emptySlot = data->FindEmptySlot();
        if (emptySlot < 0)
        {
            handler->PSendSysMessage("All guardian slots are full. Release a guardian first.");
            return true;
        }

        uint8 level = player->GetLevel();
        uint32 spells[MAX_GUARDIAN_SPELLS];
        PopulateDefaultSpells(creatureEntry, spells);

        TempSummon* guardian = SummonCapturedGuardian(player, creatureEntry, level, ARCHETYPE_DPS, spells,
            static_cast<uint8>(emptySlot));
        if (!guardian)
        {
            handler->PSendSysMessage("Failed to summon guardian.");
            return true;
        }

        GuardianSlotData& s = data->slots[emptySlot];
        s.guardianGuid      = guardian->GetGUID();
        s.guardianEntry     = creatureEntry;
        s.guardianLevel     = level;
        s.guardianHealth    = guardian->GetHealth();
        s.guardianPowerType = guardian->getPowerType();
        s.guardianPower     = guardian->GetPower(Powers(s.guardianPowerType));
        s.archetype         = ARCHETYPE_DPS;
        s.displayId         = guardian->GetDisplayId();
        s.equipmentId       = 0;
        memcpy(s.spellSlots, spells, sizeof(spells));

        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(emptySlot));

        handler->PSendSysMessage("GM captured {} (Entry {}) in slot {} at level {}.",
            cInfo->Name, creatureEntry, emptySlot + 1, level);

        SendFullSlotState(player, static_cast<uint8>(emptySlot), s);

        return true;
    }

    static bool HandleDismissCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 slot = FindTargetedGuardianSlot(player, data);

        if (slot < 0)
        {
            handler->PSendSysMessage("Target one of your guardians to dismiss it.");
            return true;
        }

        GuardianSlotData& s = data->slots[slot];
        if (!s.IsActive())
        {
            handler->PSendSysMessage("That guardian is not currently summoned.");
            return true;
        }

        SnapshotGuardianSlot(player, static_cast<uint8>(slot));
        DismissGuardianSlot(player, static_cast<uint8>(slot));
        s.dismissed = true;
        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(slot));

        handler->PSendSysMessage("Guardian in slot {} has been dismissed.", slot + 1);
        SendGuardianDismiss(player, static_cast<uint8>(slot));

        return true;
    }

    static bool HandleInfoCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 slot = FindTargetedGuardianSlot(player, data);

        if (slot >= 0)
        {
            // Show info for targeted guardian
            GuardianSlotData& s = data->slots[slot];
            Creature* guardian = s.IsActive() ? ObjectAccessor::GetCreature(*player, s.guardianGuid) : nullptr;
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";

            handler->PSendSysMessage("=== Guardian Slot {} ===", slot + 1);
            handler->PSendSysMessage("Name: {}", name);
            handler->PSendSysMessage("Level: {}", s.guardianLevel);
            if (guardian)
                handler->PSendSysMessage("Health: {} / {}", guardian->GetHealth(), guardian->GetMaxHealth());
            handler->PSendSysMessage("Entry: {}", s.guardianEntry);
            handler->PSendSysMessage("Archetype: {}", ArchetypeName(s.archetype));
            handler->PSendSysMessage("Status: {}", s.IsActive() ? "Active" : "Stored");
        }
        else
        {
            // Show summary of all slots
            handler->PSendSysMessage("=== Guardian Slots ===");
            bool anyOccupied = false;
            for (uint8 i = 0; i < config.maxSlots; ++i)
            {
                GuardianSlotData& s = data->slots[i];
                if (s.IsOccupied())
                {
                    CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry);
                    std::string name = cInfo ? cInfo->Name : "Guardian";
                    handler->PSendSysMessage("[{}] {} ({}) - {}",
                        i + 1, name, ArchetypeName(s.archetype), s.IsActive() ? "Active" : "Stored");
                    anyOccupied = true;
                }
                else
                {
                    handler->PSendSysMessage("[{}] Empty", i + 1);
                }
            }
            if (!anyOccupied)
                handler->PSendSysMessage("No guardians captured. Target a creature and use .capture!");
        }

        return true;
    }

    static bool HandleTeachCommand(ChatHandler* handler, uint32 slot, uint32 spellId)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 guardianSlot = FindTargetedGuardianSlot(player, data);

        if (guardianSlot < 0)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Target one of your guardians first.");
            return true;
        }

        GuardianSlotData& s = data->slots[guardianSlot];
        if (!s.IsActive())
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r That guardian is not currently summoned.");
            return true;
        }

        if (slot < 1 || slot > MAX_GUARDIAN_SPELLS)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Invalid slot (1-{}).", MAX_GUARDIAN_SPELLS);
            return true;
        }

        Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid);
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

        if (spellInfo->PowerType != POWER_HEALTH &&
            spellInfo->ManaCostPercentage > 0 &&
            static_cast<uint8>(spellInfo->PowerType) != guardian->getPowerType())
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r This guardian lacks the required resource for this spell.");
            return true;
        }

        uint32 slotIdx = slot - 1;

        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI());
        if (ai)
            ai->SetSpell(slotIdx, spellId);

        s.spellSlots[slotIdx] = spellId;
        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(guardianSlot));

        handler->PSendSysMessage("|cff00ff00[Guardian]|r Learned {} in slot {}.",
            spellInfo->SpellName[0], slot);

        SendGuardianSpells(player, static_cast<uint8>(guardianSlot), s.spellSlots);

        return true;
    }

    static bool HandleUnlearnCommand(ChatHandler* handler, uint32 slot)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 guardianSlot = FindTargetedGuardianSlot(player, data);

        if (guardianSlot < 0)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Target one of your guardians first.");
            return true;
        }

        GuardianSlotData& s = data->slots[guardianSlot];
        if (!s.IsActive())
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r That guardian is not currently summoned.");
            return true;
        }

        if (slot < 1 || slot > MAX_GUARDIAN_SPELLS)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Invalid slot (1-{}).", MAX_GUARDIAN_SPELLS);
            return true;
        }

        Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid);
        if (!guardian)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Guardian not found.");
            return true;
        }

        uint32 slotIdx = slot - 1;
        uint32 oldSpellId = s.spellSlots[slotIdx];

        if (oldSpellId == 0)
        {
            handler->PSendSysMessage("|cffff0000[Guardian]|r Slot {} is already empty.", slot);
            return true;
        }

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(oldSpellId);
        std::string spellName = spellInfo ? spellInfo->SpellName[0] : "Unknown";

        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(guardian->AI());
        if (ai)
            ai->SetSpell(slotIdx, 0);

        s.spellSlots[slotIdx] = 0;
        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(guardianSlot));

        handler->PSendSysMessage("|cff00ff00[Guardian]|r Unlearned {} from slot {}.", spellName, slot);

        SendGuardianSpells(player, static_cast<uint8>(guardianSlot), s.spellSlots);

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

        LoadGuardiansFromDb(player);

        if (!player->HasItemCount(ITEM_TESSERACT, 1))
        {
            if (player->AddItem(ITEM_TESSERACT, 1))
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ff00[Creature Capture]|r You have received a Tesseract! Use it to capture and summon guardians.");
            }
        }

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        bool anyOccupied = false;
        for (uint8 i = 0; i < config.maxSlots; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsOccupied())
                continue;

            anyOccupied = true;
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";

            if (!s.dismissed)
            {
                // Auto-summon guardians that were not explicitly dismissed
                TempSummon* guardian = SummonCapturedGuardian(player, s.guardianEntry, s.guardianLevel,
                    s.archetype, s.spellSlots, i, s.displayId, s.equipmentId);
                if (guardian)
                {
                    if (s.guardianHealth > 0 && s.guardianHealth <= guardian->GetMaxHealth())
                        guardian->SetHealth(s.guardianHealth);
                    if (s.guardianPower > 0)
                        guardian->SetPower(Powers(s.guardianPowerType), s.guardianPower);

                    s.guardianGuid = guardian->GetGUID();

                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff00ff00[Creature Capture]|r Slot {}: {} ({}) summoned.",
                        i + 1, name, ArchetypeName(s.archetype));
                }
            }
            else
            {
                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cff00ff00[Creature Capture]|r Slot {}: {} ({}) stored in Tesseract.",
                    i + 1, name, ArchetypeName(s.archetype));
            }
        }

        if (anyOccupied)
        {
            SendAllSlotsState(player);
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

        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        {
            if (data->slots[i].IsActive())
                SnapshotGuardianSlot(player, i);
        }

        SaveAllGuardiansToDb(player);
        DismissAllGuardians(player);
    }

    bool OnPlayerBeforeTeleport(Player* player, uint32 mapId, float x, float y, float z, float /*orientation*/, uint32 /*options*/, Unit* /*target*/) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        bool sameMap = (player->GetMapId() == mapId);

        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsActive())
                continue;

            Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid);
            if (!guardian)
            {
                s.guardianGuid.Clear();
                continue;
            }

            if (sameMap)
            {
                // Same map: teleport guardian to new position with staggered angle
                float angle = GUARDIAN_FOLLOW_ANGLES[i % MAX_GUARDIAN_SLOTS];
                float dist  = (s.archetype == ARCHETYPE_HEALER) ? HEALER_FOLLOW_DIST : GUARDIAN_FOLLOW_DIST;
                float gx = x + dist * std::cos(angle);
                float gy = y + dist * std::sin(angle);
                guardian->NearTeleportTo(gx, gy, z, guardian->GetOrientation());
            }
            else
            {
                // Cross-map: snapshot, despawn, clear GUID
                SnapshotGuardianSlot(player, i);
                guardian->DespawnOrUnsummon();
                s.guardianGuid.Clear();

                SendGuardianDismiss(player, i);
            }
        }

        return true;
    }

    void OnPlayerMapChanged(Player* player) override
    {
        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        for (uint8 i = 0; i < MAX_GUARDIAN_SLOTS; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsOccupied() || s.IsActive())
                continue;

            // Re-summon with full state
            TempSummon* guardian = SummonCapturedGuardian(player, s.guardianEntry, s.guardianLevel,
                s.archetype, s.spellSlots, i, s.displayId, s.equipmentId);
            if (guardian)
            {
                if (s.guardianHealth > 0 && s.guardianHealth <= guardian->GetMaxHealth())
                    guardian->SetHealth(s.guardianHealth);
                if (s.guardianPower > 0)
                    guardian->SetPower(Powers(s.guardianPowerType), s.guardianPower);

                s.guardianGuid = guardian->GetGUID();

                SendFullSlotState(player, i, s);
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
// Tesseract Item Script (multi-slot gossip)
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

        // Check if targeting a capturable creature
        Creature* target = ObjectAccessor::GetCreatureOrPetOrVehicle(*player, player->GetTarget());
        if (target)
        {
            std::string error;
            if (CanCaptureCreature(player, target, error))
            {
                int8 emptySlot = data->FindEmptySlot();
                if (emptySlot >= 0)
                {
                    // Auto-capture into first empty slot
                    uint32 entry = target->GetEntry();
                    uint8 level = target->GetLevel();
                    uint32 capturedDisplayId = target->GetDisplayId();
                    int8 capturedEquipmentId = static_cast<int8>(target->GetCurrentEquipmentId());

                    uint32 spells[MAX_GUARDIAN_SPELLS];
                    PopulateDefaultSpells(entry, spells);

                    target->DespawnOrUnsummon();

                    TempSummon* guardian = SummonCapturedGuardian(player, entry, level, ARCHETYPE_DPS, spells,
                        static_cast<uint8>(emptySlot), capturedDisplayId, capturedEquipmentId);
                    if (guardian)
                    {
                        GuardianSlotData& s = data->slots[emptySlot];
                        s.guardianGuid      = guardian->GetGUID();
                        s.guardianEntry     = entry;
                        s.guardianLevel     = level;
                        s.guardianHealth    = guardian->GetHealth();
                        s.guardianPowerType = guardian->getPowerType();
                        s.guardianPower     = guardian->GetPower(Powers(s.guardianPowerType));
                        s.archetype         = ARCHETYPE_DPS;
                        s.displayId         = capturedDisplayId;
                        s.equipmentId       = capturedEquipmentId;
                        memcpy(s.spellSlots, spells, sizeof(spells));

                        SaveGuardianSlotToDb(player, &s, static_cast<uint8>(emptySlot));

                        ChatHandler(player->GetSession()).PSendSysMessage(
                            "|cff00ff00[Tesseract]|r {} captured in slot {}!", guardian->GetName(), emptySlot + 1);

                        SendFullSlotState(player, static_cast<uint8>(emptySlot), s);
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
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff0000[Tesseract]|r All guardian slots are full. Release a guardian first.");
                    // Fall through to show gossip so they can release
                }
            }
            // If not capturable (own guardian, etc.), fall through to gossip
        }

        // Build multi-slot gossip menu
        ClearGossipMenuFor(player);

        bool anyOccupied = false;
        for (uint8 i = 0; i < config.maxSlots; ++i)
        {
            GuardianSlotData& s = data->slots[i];

            if (!s.IsOccupied())
                continue;

            anyOccupied = true;
            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";

            if (s.IsActive())
            {
                // Active guardian — clicking dismisses
                std::string label = "[" + std::to_string(i + 1) + "] Dismiss " + name + " (" + ArchetypeName(s.archetype) + ")";
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, label,
                    GOSSIP_SENDER_MAIN, i * 10 + TESSERACT_ACTION_DISMISS);
            }
            else
            {
                // Stored guardian — clicking summons
                std::string label = "[" + std::to_string(i + 1) + "] Summon " + name + " (" + ArchetypeName(s.archetype) + ")";
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, label,
                    GOSSIP_SENDER_MAIN, i * 10 + TESSERACT_ACTION_SUMMON);
            }
        }

        // Release options (separate, with danger icon)
        for (uint8 i = 0; i < config.maxSlots; ++i)
        {
            GuardianSlotData& s = data->slots[i];
            if (!s.IsOccupied())
                continue;

            CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry);
            std::string name = cInfo ? cInfo->Name : "Guardian";
            std::string label = "Release [" + std::to_string(i + 1) + "] " + name + " (permanent)";
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, label,
                GOSSIP_SENDER_MAIN, i * 10 + TESSERACT_ACTION_RELEASE);
        }

        if (!anyOccupied)
        {
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Target a creature and use the Tesseract to capture it!",
                GOSSIP_SENDER_MAIN, TESSERACT_ACTION_CLOSE);
        }

        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind",
            GOSSIP_SENDER_MAIN, TESSERACT_ACTION_CLOSE);

        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, item->GetGUID());
        return true;
    }

    void OnGossipSelect(Player* player, Item* /*item*/, uint32 /*sender*/, uint32 action) override
    {
        CloseGossipMenuFor(player);

        if (action == TESSERACT_ACTION_CLOSE || action == 0)
            return;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");

        // Decode slot and action
        uint8 slot = action / 10;
        uint8 localAction = action % 10;

        if (slot >= MAX_GUARDIAN_SLOTS)
            return;

        GuardianSlotData& s = data->slots[slot];

        switch (localAction)
        {
            case TESSERACT_ACTION_SUMMON:
            {
                if (!s.IsOccupied())
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("No guardian in slot {}.", slot + 1);
                    return;
                }

                if (s.IsActive())
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("Guardian in slot {} is already summoned.", slot + 1);
                    return;
                }

                TempSummon* guardian = SummonCapturedGuardian(player, s.guardianEntry, s.guardianLevel,
                    s.archetype, s.spellSlots, slot, s.displayId, s.equipmentId);
                if (guardian)
                {
                    if (s.guardianHealth > 0 && s.guardianHealth <= guardian->GetMaxHealth())
                        guardian->SetHealth(s.guardianHealth);
                    if (s.guardianPower > 0)
                        guardian->SetPower(Powers(s.guardianPowerType), s.guardianPower);

                    s.guardianGuid = guardian->GetGUID();
                    s.dismissed = false;

                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff00ff00[Tesseract]|r {} summoned from slot {}!", guardian->GetName(), slot + 1);

                    SaveGuardianSlotToDb(player, &s, slot);
                    SendFullSlotState(player, slot, s);
                }
                else
                {
                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cffff0000[Tesseract]|r Failed to summon guardian.");
                }
                break;
            }
            case TESSERACT_ACTION_DISMISS:
            {
                if (!s.IsActive())
                {
                    ChatHandler(player->GetSession()).PSendSysMessage("No active guardian in slot {}.", slot + 1);
                    return;
                }

                Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid);
                if (guardian && guardian->IsAlive())
                {
                    SnapshotGuardianSlot(player, slot);
                    std::string name = guardian->GetName();
                    guardian->DespawnOrUnsummon();
                    s.guardianGuid.Clear();
                    s.dismissed = true;

                    SaveGuardianSlotToDb(player, &s, slot);

                    ChatHandler(player->GetSession()).PSendSysMessage(
                        "|cff00ff00[Tesseract]|r {} stored from slot {}.", name, slot + 1);
                    SendGuardianDismiss(player, slot);
                }
                else
                {
                    s.guardianGuid.Clear();
                    s.dismissed = true;
                    ChatHandler(player->GetSession()).PSendSysMessage("Guardian not found.");
                }
                break;
            }
            case TESSERACT_ACTION_RELEASE:
            {
                std::string name = "Guardian";
                if (s.IsActive())
                {
                    if (Creature* guardian = ObjectAccessor::GetCreature(*player, s.guardianGuid))
                    {
                        name = guardian->GetName();
                        guardian->DespawnOrUnsummon();
                    }
                }
                else if (s.IsOccupied())
                {
                    if (CreatureTemplate const* cInfo = sObjectMgr->GetCreatureTemplate(s.guardianEntry))
                        name = cInfo->Name;
                }

                s.Clear();
                DeleteGuardianSlotFromDb(player, slot);

                ChatHandler(player->GetSession()).PSendSysMessage(
                    "|cffff6600[Tesseract]|r {} released from slot {}.", name, slot + 1);

                SendGuardianClear(player, slot);
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

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        int8 slot = data->FindSlotByGuid(creature->GetGUID());
        if (slot < 0)
            return false;

        if (creature->GetOwnerGUID() != player->GetGUID())
            return false;

        // Build the normal gossip menu first
        player->PrepareGossipMenu(creature, creature->GetGossipMenuId(), true);

        GuardianSlotData& s = data->slots[slot];

        // Append archetype selection with slot-encoded actions
        std::string dpsLabel   = std::string("[DPS] Switch to DPS")        + (s.archetype == ARCHETYPE_DPS    ? " (active)" : "");
        std::string tankLabel  = std::string("[Tank] Switch to Tank")      + (s.archetype == ARCHETYPE_TANK   ? " (active)" : "");
        std::string healLabel  = std::string("[Healer] Switch to Healer")  + (s.archetype == ARCHETYPE_HEALER ? " (active)" : "");

        // Encode: 100 + slot*10 + archetype
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, dpsLabel,  GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_BASE + slot * 10 + ARCHETYPE_DPS);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, tankLabel, GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_BASE + slot * 10 + ARCHETYPE_TANK);
        AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, healLabel, GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_BASE + slot * 10 + ARCHETYPE_HEALER);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Nevermind.", GOSSIP_SENDER_MAIN, GUARDIAN_ACTION_CLOSE);

        SendGossipMenuFor(player, player->GetGossipTextId(creature), creature->GetGUID());
        return true;
    }

    bool CanCreatureGossipSelect(Player* player, Creature* creature, uint32 /*sender*/, uint32 action) override
    {
        if (!config.enabled || !player || !creature)
            return false;

        // Only handle our action range
        if (action < GUARDIAN_ACTION_BASE || action > GUARDIAN_ACTION_CLOSE)
            return false;

        CloseGossipMenuFor(player);

        if (action == GUARDIAN_ACTION_CLOSE)
            return true;

        // Decode: slot = (action - 100) / 10, archetype = (action - 100) % 10
        uint8 slot = (action - GUARDIAN_ACTION_BASE) / 10;
        uint8 newArchetype = (action - GUARDIAN_ACTION_BASE) % 10;

        if (slot >= MAX_GUARDIAN_SLOTS || newArchetype > ARCHETYPE_HEALER)
            return false;

        CapturedGuardianData* data = player->CustomData.GetDefault<CapturedGuardianData>("CapturedGuardian");
        GuardianSlotData& s = data->slots[slot];

        if (!s.IsActive() || s.guardianGuid != creature->GetGUID())
            return false;

        if (s.archetype == newArchetype)
        {
            ChatHandler(player->GetSession()).PSendSysMessage(
                "|cff00ff00[Guardian]|r Already set to {} archetype.", ArchetypeName(newArchetype));
            return true;
        }

        s.archetype = newArchetype;

        CapturedGuardianAI* ai = dynamic_cast<CapturedGuardianAI*>(creature->AI());
        if (ai)
            ai->SetArchetype(newArchetype);

        SaveGuardianSlotToDb(player, &s, slot);

        ChatHandler(player->GetSession()).PSendSysMessage(
            "|cff00ff00[Guardian]|r Slot {} switched to {} archetype.", slot + 1, ArchetypeName(newArchetype));

        SendGuardianArchetype(player, slot, newArchetype);

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
