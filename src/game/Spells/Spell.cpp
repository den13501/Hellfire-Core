/*
 * Copyright (C) 2005-2008 MaNGOS <http://getmangos.com/>
 * Copyright (C) 2009-2017 MaNGOSOne <https://github.com/mangos/one>
 * Copyright (C) 2017 Hellfire <https://hellfire-core.github.io/>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Opcodes.h"
#include "Log.h"
#include "UpdateMask.h"
#include "World.h"
#include "ObjectMgr.h"
#include "SpellMgr.h"
#include "ScriptMgr.h"
#include "Player.h"
#include "Pet.h"
#include "Unit.h"
#include "Spell.h"
#include "DynamicObject.h"
#include "SpellAuras.h"
#include "Group.h"
#include "UpdateData.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "CellImpl.h"
#include "SharedDefines.h"
#include "Tools.h"
#include "LootMgr.h"
#include "VMapFactory.h"
#include "BattleGround.h"
#include "Util.h"
#include "TemporarySummon.h"
#include "PetAI.h"
#include "MovementGenerator.h"
#include "InstanceData.h"

#define SPELL_CHANNEL_UPDATE_INTERVAL 1000

extern pEffect SpellEffects[TOTAL_SPELL_EFFECTS];

bool IsQuestTameSpell(uint32 spellId)
{
    SpellEntry const *spellproto = sSpellStore.LookupEntry(spellId);
    if (!spellproto) return false;

    return spellproto->Effect[0] == SPELL_EFFECT_THREAT
        && spellproto->Effect[1] == SPELL_EFFECT_APPLY_AURA && spellproto->EffectApplyAuraName[1] == SPELL_AURA_DUMMY;
}

SpellCastTargets::SpellCastTargets()
{
    m_unitTarget = NULL;
    m_itemTarget = NULL;
    m_GOTarget = NULL;

    m_unitTargetGUID = 0;
    m_GOTargetGUID = 0;
    m_CorpseTargetGUID = 0;
    m_itemTargetGUID = 0;
    m_itemTargetEntry = 0;

    m_srcX = m_srcY = m_srcZ = m_destX = m_destY = m_destZ = 0;
    m_orientation = -1;
    m_strTarget = "";
    m_targetMask = 0;
}

SpellCastTargets::~SpellCastTargets()
{
}

void SpellCastTargets::setUnitTarget(Unit *target)
{
    if (!target)
        return;

    m_unitTarget = target;
    m_unitTargetGUID = target->GetGUID();
    m_targetMask |= TARGET_FLAG_UNIT;
}

void SpellCastTargets::setSrc(float x, float y, float z)
{
    m_srcX = x;
    m_srcY = y;
    m_srcZ = z;
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setSrc(WorldObject *target)
{
    if (!target)
        return;

    target->GetPosition(m_srcX, m_srcY, m_srcZ);
    m_targetMask |= TARGET_FLAG_SOURCE_LOCATION;
}

void SpellCastTargets::setDestination(float x, float y, float z, float orientation, int32 mapId)
{
    m_destX = x;
    m_destY = y;
    m_destZ = z;
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
    if (mapId >= 0)
        m_mapId = mapId;
    if (orientation >= 0)
        m_orientation = orientation;
}

void SpellCastTargets::setDestination(WorldObject *target)
{
    if (!target)
        return;

    target->GetPosition(m_destX, m_destY, m_destZ);
    m_targetMask |= TARGET_FLAG_DEST_LOCATION;
}

void SpellCastTargets::setGOTarget(GameObject *target)
{
    m_GOTarget = target;
    m_GOTargetGUID = target->GetGUID();
    m_targetMask |= TARGET_FLAG_GAMEOBJECT;
}

void SpellCastTargets::setItemTarget(Item* item)
{
    if (!item)
        return;

    m_itemTarget = item;
    m_itemTargetGUID = item->GetGUID();
    m_itemTargetEntry = item->GetEntry();
    m_targetMask |= TARGET_FLAG_ITEM;
}

void SpellCastTargets::setCorpseTarget(Corpse* corpse)
{
    m_CorpseTargetGUID = corpse->GetGUID();
}

void SpellCastTargets::Update(Unit* caster)
{
    m_GOTarget = !m_GOTargetGUID.IsEmpty() ? caster->GetMap()->GetGameObject(m_GOTargetGUID.GetRawValue()) : NULL;
    m_unitTarget = !m_unitTargetGUID.IsEmpty() ?
        (m_unitTargetGUID.GetRawValue() == caster->GetGUID() ? caster : caster->GetMap()->GetUnit(m_unitTargetGUID.GetRawValue())) :
        NULL;

    m_itemTarget = NULL;
    if (caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (m_targetMask & TARGET_FLAG_ITEM)
            m_itemTarget = ((Player*)caster)->GetItemByGuid(m_itemTargetGUID.GetRawValue());
        else if (m_targetMask & TARGET_FLAG_TRADE_ITEM)
        {
            // here it is not guid but slot
            Player* pTrader = ((Player*)caster)->GetTrader();
            if (pTrader && m_itemTargetGUID.GetRawValue() < TRADE_SLOT_COUNT)
                m_itemTarget = pTrader->GetItemByPos(pTrader->GetItemPosByTradeSlot(m_itemTargetGUID.GetRawValue()));
        }
        if (m_itemTarget)
            m_itemTargetEntry = m_itemTarget->GetEntry();
    }
}

void SpellCastTargets::read(ByteBuffer& data, Unit *caster)
{
    data >> m_targetMask;

    if (m_targetMask == TARGET_FLAG_SELF)
        return;

    // TARGET_FLAG_UNK2 is used for non-combat pets, maybe other?
    if (m_targetMask & (TARGET_FLAG_UNIT | TARGET_FLAG_UNK2))
        data >> m_unitTargetGUID.ReadAsPacked();

    if (m_targetMask & (TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_OBJECT_UNK))
        data >> m_GOTargetGUID.ReadAsPacked();

    if ((m_targetMask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM)) && caster->GetTypeId() == TYPEID_PLAYER)
        data >> m_itemTargetGUID.ReadAsPacked();

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
    {
        data >> m_srcX >> m_srcY >> m_srcZ;

        if (!MaNGOS::IsValidMapCoord(m_srcX, m_srcY, m_srcZ))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        data >> m_destX >> m_destY >> m_destZ;

        if (!MaNGOS::IsValidMapCoord(m_destX, m_destY, m_destZ))
            throw ByteBufferException(false, data.rpos(), 0, data.size());
    }

    if (m_targetMask & TARGET_FLAG_STRING)
        data >> m_strTarget;

    if (m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE))
        data >> m_CorpseTargetGUID.ReadAsPacked();

    // find real units/GOs
    Update(caster);
}

void SpellCastTargets::write(ByteBuffer& data) const
{
    data << uint32(m_targetMask);

    if (m_targetMask & (TARGET_FLAG_UNIT | TARGET_FLAG_PVP_CORPSE | TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_CORPSE | TARGET_FLAG_UNK2))
    {
        if (m_targetMask & TARGET_FLAG_UNIT)
        {
            if (m_unitTarget)
                data << m_unitTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if (m_targetMask & (TARGET_FLAG_GAMEOBJECT | TARGET_FLAG_OBJECT_UNK))
        {
            if (m_GOTarget)
                data << m_GOTarget->GetPackGUID();
            else
                data << uint8(0);
        }
        else if (m_targetMask & (TARGET_FLAG_CORPSE | TARGET_FLAG_PVP_CORPSE))
            data << m_CorpseTargetGUID.WriteAsPacked();
        else
            data << uint8(0);
    }

    if (m_targetMask & (TARGET_FLAG_ITEM | TARGET_FLAG_TRADE_ITEM))
    {
        if (m_itemTarget)
            data << m_itemTarget->GetPackGUID();
        else
            data << uint8(0);
    }

    if (m_targetMask & TARGET_FLAG_SOURCE_LOCATION)
        data << m_srcX << m_srcY << m_srcZ;

    if (m_targetMask & TARGET_FLAG_DEST_LOCATION)
        data << m_destX << m_destY << m_destZ;

    if (m_targetMask & TARGET_FLAG_STRING)
        data << m_strTarget;
}

Spell::Spell(Unit* Caster, SpellEntry const *info, bool triggered, uint64 originalCasterGUID, Spell** triggeringContainer, bool skipCheck)
    : m_spellInfo(info), m_spellValue(new SpellValue(info))
    , m_caster(Caster), _path(PathFinder(m_caster))
{
    m_spellState = SPELL_STATE_NULL;
    m_skipCheck = skipCheck;
    m_selfContainer = NULL;
    m_triggeringContainer = triggeringContainer;
    m_referencedFromCurrentSpell = false;
    m_executedCurrently = false;
    m_delayStart = 0;
    m_delayAtDamageCount = 0;
    m_destroyed = false;

    m_applyMultiplierMask = 0;
    m_procCastEnd = 0;
    m_spellSchoolMask = SpellMgr::GetSpellSchoolMask(info);           // Can be override for some spell (wand shoot for example)

    // Get data for type of attack
    switch (GetSpellEntry()->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            if (GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_REQ_OFFHAND)
                m_attackType = OFF_ATTACK;
            else
                m_attackType = BASE_ATTACK;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            m_attackType = RANGED_ATTACK;
            break;
        default:
            // Wands
            if (GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG)
            {
                m_attackType = RANGED_ATTACK;
                if (m_caster->getClassMask() & CLASSMASK_WAND_USERS && m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if (Item* pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK))
                        m_spellSchoolMask = SpellSchoolMask(1 << pItem->GetProto()->Damage[0].DamageType);
                }
            }
            else
                m_attackType = BASE_ATTACK;
            break;
    }

    // Set health leech amount to zero
    m_healthLeech = 0;

    if (originalCasterGUID)
    {
        m_originalCasterGUID = originalCasterGUID;
        m_originalCaster = m_caster->GetMap()->GetUnit(m_originalCasterGUID);
        if (m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }
    else
    {
        m_originalCasterGUID = m_caster->GetGUID();
        m_originalCaster = m_caster;
    }

    for (int i = 0; i < 3; ++i)
        m_currentBasePoints[i] = m_spellValue->EffectBasePoints[i];

    m_TriggerSpells.clear();
    m_IsTriggeredSpell = GetSpellEntry()->AttributesEx4 & SPELL_ATTR_EX4_FORCE_TRIGGERED || triggered;
    //m_AreaAura = false;
    m_CastItem = NULL;
    m_castItemGUID = 0;

    unitTarget = NULL;
    itemTarget = NULL;
    gameObjTarget = NULL;
    focusObject = NULL;
    m_cast_count = 0;
    m_triggeredByAuraSpell = NULL;

    //Auto Shot & Shoot
    if (GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_AUTOREPEAT_FLAG && !triggered)
        m_autoRepeat = true;
    else
        m_autoRepeat = false;

    m_powerCost = 0;                                        // setup to correct value in Spell::prepare, don't must be used before.
    m_casttime = 0;                                         // setup to correct value in Spell::prepare, don't must be used before.
    m_timer = 0;                                            // will set to castime in prepare
    m_autocastDelayTimer = 0;
    m_extraCrit = 0.0f;
    m_needAliveTargetMask = 0;

    // determine reflection
    m_canReflect = GetSpellEntry()->DmgClass == SPELL_DAMAGE_CLASS_MAGIC && !(GetSpellEntry()->Attributes & SPELL_ATTR_ABILITY)
        && !(GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_CANT_BE_REFLECTED) && !(GetSpellEntry()->Attributes & SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY)
        && !SpellMgr::IsPassiveSpell(GetSpellEntry());

    CleanupTargetList();
}

Spell::~Spell()
{
    m_destroyed = true;
    delete m_spellValue;
}

void Spell::FillTargetMap()
{
    for (uint32 i = 0; i < 3; ++i)
    {
        // not call for empty effect.
        // Also some spells use not used effect targets for store targets for dummy effect in triggered spells
        if (!GetSpellEntry()->Effect[i])
            continue;

        uint32 effectTargetType = sSpellMgr.EffectTargetType[GetSpellEntry()->Effect[i]];

        // is it possible that areaaura is not applied to caster?
        if (effectTargetType == SPELL_REQUIRE_NONE)
            continue;

        uint32 targetA = GetSpellEntry()->EffectImplicitTargetA[i];
        uint32 targetB = GetSpellEntry()->EffectImplicitTargetB[i];

        if (targetA)
            SetTargetMap(i, targetA);

        if (targetB) // In very rare case !A && B
            SetTargetMap(i, targetB);

        if (effectTargetType != SPELL_REQUIRE_UNIT)
        {
            if (effectTargetType == SPELL_REQUIRE_CASTER)
                AddUnitTarget(m_caster, i);
            else if (effectTargetType == SPELL_REQUIRE_ITEM)
            {
                if (m_targets.getItemTarget())
                    AddItemTarget(m_targets.getItemTarget(), i);
            }
            continue;
        }

        if (!targetA && !targetB)
        {
            // add here custom effects that need default target.
            // FOR EVERY TARGET TYPE THERE IS A DIFFERENT FILL!!
            switch (GetSpellEntry()->Effect[i])
            {
                case SPELL_EFFECT_DUMMY:
                {
                    switch (GetSpellEntry()->Id)
                    {
                        case 20577:                         // Cannibalize
                        {
                            // non-standard target selection
                            SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex);
                            float max_range = SpellMgr::GetSpellMaxRange(srange);
                            WorldObject* result = NULL;

                            MaNGOS::CannibalizeObjectCheck u_check(m_caster, max_range);
                            MaNGOS::UnitSearcher<MaNGOS::CannibalizeObjectCheck > unit_searcher((Unit*&)result, u_check);
                            Cell::VisitGridObjects(m_caster, unit_searcher, max_range);

                            // little workaround after generalization :p
                            for (uint8 c = 0; c < 2; ++c)
                            {
                                if (result)
                                    break;

                                switch (c)
                                {
                                    case 0:
                                        Cell::VisitWorldObjects(m_caster, unit_searcher, max_range);
                                        break;
                                    case 1:
                                        MaNGOS::ObjectSearcher<Corpse, MaNGOS::CannibalizeObjectCheck > corpse_searcher((Corpse*&)result, u_check);
                                        Cell::VisitWorldObjects(m_caster, corpse_searcher, max_range);
                                        break;
                                }
                            }

                            if (result)
                            {
                                switch (result->GetTypeId())
                                {
                                    case TYPEID_UNIT:
                                    case TYPEID_PLAYER:
                                        AddUnitTarget((Unit*)result, i);
                                        break;
                                    case TYPEID_CORPSE:
                                        m_targets.setCorpseTarget((Corpse*)result);
                                        if (Player* owner = ObjectAccessor::FindPlayer(((Corpse*)result)->GetOwnerGUID()))
                                            AddUnitTarget(owner, i);
                                        break;
                                }
                            }
                            else
                            {
                                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                                    ((Player*)m_caster)->RemoveSpellCooldown(GetSpellEntry()->Id, true);

                                SendCastResult(SPELL_FAILED_NO_EDIBLE_CORPSES);
                                finish(false);
                            }
                            break;
                        }
                        default:
                            if (m_targets.getUnitTarget())
                                AddUnitTarget(m_targets.getUnitTarget(), i);
                            break;
                    }
                    break;
                }
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_TRIGGER_SPELL:
                case SPELL_EFFECT_SKILL_STEP:
                case SPELL_EFFECT_SELF_RESURRECT:
                case SPELL_EFFECT_REPUTATION:
                case SPELL_EFFECT_LEARN_SPELL:
                    if (m_targets.getUnitTarget())
                        AddUnitTarget(m_targets.getUnitTarget(), i);
                    else
                        AddUnitTarget(m_caster, i);
                    break;
                case SPELL_EFFECT_SEND_TAXI:
                case SPELL_EFFECT_SUMMON_PLAYER:
                    if (m_targets.getUnitTarget())
                        AddUnitTarget(m_targets.getUnitTarget(), i);
                    break;
                case SPELL_EFFECT_RESURRECT:
                case SPELL_EFFECT_RESURRECT_NEW:
                    if (m_targets.getUnitTarget())
                        AddUnitTarget(m_targets.getUnitTarget(), i);
                    if (m_targets.getCorpseTargetGUID())
                        if (Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster, m_targets.getCorpseTargetGUID()))
                            if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID()))
                                AddUnitTarget(owner, i);
                    break;
                case SPELL_EFFECT_SUMMON_CHANGE_ITEM:
                case SPELL_EFFECT_ADD_FARSIGHT:
                case SPELL_EFFECT_STUCK:
                case SPELL_EFFECT_DESTROY_ALL_TOTEMS:
                case SPELL_EFFECT_FRIEND_SUMMON:
                    AddUnitTarget(m_caster, i);
                    break;
                case SPELL_EFFECT_LEARN_PET_SPELL:
                    if (Pet* pet = m_caster->GetPet())
                        AddUnitTarget(pet, i);
                    break;
                    /*case SPELL_EFFECT_ENCHANT_ITEM:
                    case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
                    case SPELL_EFFECT_DISENCHANT:
                    case SPELL_EFFECT_FEED_PET:
                    case SPELL_EFFECT_PROSPECTING:
                    if (m_targets.getItemTarget())
                    AddItemTarget(m_targets.getItemTarget(), i);
                    break;*/
                case SPELL_EFFECT_APPLY_AURA:
                    switch (GetSpellEntry()->EffectApplyAuraName[i])
                    {
                        case SPELL_AURA_ADD_FLAT_MODIFIER:  // some spell mods auras have 0 target modes instead expected TARGET_UNIT_CASTER(1) (and present for other ranks for same spell for example)
                        case SPELL_AURA_ADD_PCT_MODIFIER:
                            AddUnitTarget(m_caster, i);
                            break;
                        default:                            // apply to target in other case
                            break;
                    }
                    break;
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                    // AreaAura
                    if (GetSpellEntry()->Attributes == 0x9050000 || GetSpellEntry()->Attributes == 0x10000)
                        SetTargetMap(i, TARGET_UNIT_FRIEND_AND_PARTY);
                    break;
                case SPELL_EFFECT_SKIN_PLAYER_CORPSE:
                    if (m_targets.getUnitTarget())
                        AddUnitTarget(m_targets.getUnitTarget(), i);
                    else if (m_targets.getCorpseTargetGUID())
                        if (Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster, m_targets.getCorpseTargetGUID()))
                            if (Player* owner = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID()))
                                AddUnitTarget(owner, i);
                    break;
                default:
                    break;
            }
        }

        if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
        {
            uint8 mask = (1 << i);
            for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->deleted)
                    continue;

                if (ihit->effectMask & mask)
                {
                    m_needAliveTargetMask |= mask;
                    break;
                }
            }
        }
    }

    if (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION)
    {
        if (GetSpellEntry()->speed > 0.0f && m_targets.HasDst())
        {
            float dist = m_caster->GetDistance(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ);
            if (dist < 5.0f) dist = 5.0f;
            m_delayMoment = (uint64)floor(dist / GetSpellEntry()->speed * 1000.0f);
        }
        else if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_FAKE_DELAY)
            m_delayMoment = SPELL_FAKE_DELAY;
    }
}

void Spell::prepareDataForTriggerSystem()
{
    //==========================================================================================
    // Create base triggers flags for Attacker and Victim (m_procAttacker and  m_procVictim)
    // can spell trigger another or not (m_canTrigger)
    // Create base triggers flags for Attacker and Victim (m_procAttacker and m_procVictim)
    //==========================================================================================
    // SPELL_CAN_TRIGGER_OR_NOT
    // Fill flag can spell trigger or not
    if (!IsTriggeredSpell())
        m_canTrigger = true;          // Normal cast - can trigger
    else if (m_triggeredByAuraSpell)
        m_canTrigger = false;         // Triggered spells can`t trigger another
    else
        m_canTrigger = true;          // Triggered from SPELL_EFFECT_TRIGGER_SPELL - can trigger
        

    if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_WARLOCK &&
        GetSpellEntry()->SpellFamilyFlags & 0x0000008000000060LL)
        // For Hellfire Effect / Rain of Fire triggers need do it
    {
        m_procAttacker = PROC_FLAG_ON_DO_PERIODIC;
        m_procVictim = PROC_FLAG_ON_TAKE_PERIODIC;
        m_canTrigger = true;
        return;
    }

    if (GetSpellEntry()->Id == 45055) // HACK: shadow bolt from Timbal's focusing crystal considered a dot
    {
        m_procAttacker = PROC_FLAG_ON_DO_PERIODIC;
        m_procVictim = PROC_FLAG_ON_TAKE_PERIODIC;
        m_canTrigger = true;
        return;
    }

    if (m_CastItem && GetSpellEntry()->SpellFamilyName != SPELLFAMILY_POTION)
        m_canTrigger = false;         // Do not trigger from item cast spell(except potions)

    // Get data for type of attack and fill base info for trigger
    switch (GetSpellEntry()->DmgClass)
    {
        case SPELL_DAMAGE_CLASS_MELEE:
            m_procAttacker = PROC_FLAG_SUCCESSFUL_MELEE_SPELL_HIT;
            if (m_attackType == OFF_ATTACK)
                m_procAttacker |= PROC_FLAG_SUCCESSFUL_OFFHAND_HIT;

            m_procVictim = PROC_FLAG_TAKEN_MELEE_SPELL_HIT;
            if (IsNextMeleeSwingSpell())
            {
                //m_procAttacker |= PROC_FLAG_SUCCESSFUL_MELEE_HIT;
                m_procVictim |= PROC_FLAG_TAKEN_MELEE_HIT;
            }

            if (IsDelayedSpell())
                m_procCastEnd = PROC_FLAG_DELAYED_MELEE_SPELL_CAST_END;
            break;
        case SPELL_DAMAGE_CLASS_RANGED:
            m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
            m_procVictim = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;

            if (IsDelayedSpell())
                m_procCastEnd = PROC_FLAG_DELAYED_RANGED_SPELL_CAST_END;
            break;
        default:
            if (SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))          // Check for positive spell
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_POSITIVE_SPELL;
                m_procVictim = PROC_FLAG_TAKEN_POSITIVE_SPELL;

                if (IsDelayedSpell())
                    m_procCastEnd = PROC_FLAG_DELAYED_POSITIVE_SPELL_CAST_END;
            }
            else if (GetSpellEntry()->Id == 5019) // Wands
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_RANGED_SPELL_HIT;
                m_procVictim = PROC_FLAG_TAKEN_RANGED_SPELL_HIT;
            }
            else
            {
                m_procAttacker = PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
                m_procVictim = PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;

                if (IsDelayedSpell())
                    m_procCastEnd = PROC_FLAG_DELAYED_NEGATIVE_SPELL_CAST_END;
            }
            break;
    }

    if (IsTriggeredSpell() && GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_TRIGGERED_CAN_TRIGGER)
        m_canTrigger = true;

    if (GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_CANT_TRIGGER_PROC)
        m_canTrigger = false;

    // Hunter traps spells (for Entrapment trigger)
    // Gives your Immolation Trap, Frost Trap, Explosive Trap, and Snake Trap ....
    if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_HUNTER && GetSpellEntry()->SpellFamilyFlags & 0x0000200000000014LL)
        m_procAttacker |= PROC_FLAG_ON_TRAP_ACTIVATION;
}

void Spell::CleanupTargetList()
{
    //m_UniqueTargetInfo.clear();
    //m_UniqueGOTargetInfo.clear();
    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        ihit->deleted = true;

    for (std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
        ihit->deleted = true;

    m_UniqueItemInfo.clear();
    m_countOfHit = 0;
    m_countOfMiss = 0;
    m_delayMoment = 0;
}

void Spell::AddUnitTarget(Unit* pVictim, uint32 effIndex, bool redirected)
{
    if (GetSpellEntry()->Effect[effIndex] == 0)
        return;

    if (!redirected && !CheckTarget(pVictim, effIndex))
        return;

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= 1 << effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him

    // Get spell hit result on target
    TargetInfo target;
    target.targetGUID = targetGUID;                         // Store target GUID
    target.effectMask = 1 << effIndex;                        // Store index of effect
    target.processed = false;                              // Effects not apply on target
    target.damage = 0;
    target.crit = false;
    target.deleted = false;

    // Calculate hit result
    if (m_originalCaster)
    {
        bool canMiss = m_triggeredByAuraSpell || !IsTriggeredSpell() || IsAutoShootSpell();
        target.missCondition = m_originalCaster->SpellHitResult(pVictim, GetSpellEntry(), canMiss);
        if (m_skipCheck && target.missCondition != SPELL_MISS_IMMUNE)
            target.missCondition = SPELL_MISS_NONE;
    }
    else
        target.missCondition = SPELL_MISS_EVADE; //SPELL_MISS_NONE;

    if (target.missCondition == SPELL_MISS_NONE)
        ++m_countOfHit;
    else
        ++m_countOfMiss;

    // Spell have speed - need calculate incoming time
    if (GetSpellEntry()->speed > 0.0f)
    {
        // calculate spell incoming interval
        // TODO: this is a hack
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());

        if (dist < 5.0f)
            dist = 5.0f;

        target.timeDelay = (uint64)floor(dist / GetSpellEntry()->speed * 1000.0f);

        // Calculate minimum incoming time
        if (m_delayMoment == 0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_FAKE_DELAY)
    {
        target.timeDelay = SPELL_FAKE_DELAY;
        m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = 0LL;

    target.reflectResult = SPELL_MISS_NONE;

    // Add target to list
    m_UniqueTargetInfo.push_back(target);
}

void Spell::AddUnitTarget(uint64 unitGUID, uint32 effIndex)
{
    Unit* unit = m_caster->GetGUID() == unitGUID ? m_caster : m_caster->GetMap()->GetUnit(unitGUID);
    if (unit)
        AddUnitTarget(unit, effIndex);
}

void Spell::AddGOTarget(GameObject* pVictim, uint32 effIndex)
{
    if (GetSpellEntry()->Effect[effIndex] == 0)
        return;

    uint64 targetGUID = pVictim->GetGUID();

    // Lookup target in already in list
    for (std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (targetGUID == ihit->targetGUID)                 // Found in list
        {
            ihit->effectMask |= 1 << effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target calculate data for him
    GOTargetInfo target;
    target.targetGUID = targetGUID;
    target.effectMask = 1 << effIndex;
    target.processed = false;                              // Effects not apply on target
    target.deleted = false;

    // Spell have speed - need calculate incoming time
    if (GetSpellEntry()->speed > 0.0f)
    {
        // calculate spell incoming interval
        float dist = m_caster->GetDistance(pVictim->GetPositionX(), pVictim->GetPositionY(), pVictim->GetPositionZ());
        if (dist < 5.0f) dist = 5.0f;
        target.timeDelay = (uint64)floor(dist / GetSpellEntry()->speed * 1000.0f);
        if (m_delayMoment == 0 || m_delayMoment>target.timeDelay)
            m_delayMoment = target.timeDelay;
    }
    else if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_FAKE_DELAY)
    {
        target.timeDelay = SPELL_FAKE_DELAY;
        m_delayMoment = target.timeDelay;
    }
    else
        target.timeDelay = 0LL;

    ++m_countOfHit;

    // Add target to list
    m_UniqueGOTargetInfo.push_back(target);
}

void Spell::AddGOTarget(uint64 goGUID, uint32 effIndex)
{
    GameObject* go = m_caster->GetMap()->GetGameObject(goGUID);
    if (go)
        AddGOTarget(go, effIndex);
}

void Spell::AddItemTarget(Item* pitem, uint32 effIndex)
{
    if (GetSpellEntry()->Effect[effIndex] == 0)
        return;

    // Lookup target in already in list
    for (std::list<ItemTargetInfo>::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
    {
        if (pitem == ihit->item)                            // Found in list
        {
            ihit->effectMask |= 1 << effIndex;                // Add only effect mask
            return;
        }
    }

    // This is new target add data

    ItemTargetInfo target;
    target.item = pitem;
    target.effectMask = 1 << effIndex;
    m_UniqueItemInfo.push_back(target);
}

void Spell::DoAllEffectOnTarget(TargetInfo *target)
{
    // useless? nie da rady wycastowac spella ktory nie jest w spell.dbc
    if (GetSpellEntry()->Id > MAX_SPELL_ID)
        return;

    if (!target || target == (TargetInfo*)0x10 || target->processed)    // Check target WTF?
        return;

    target->processed = true;                               // Target checked in apply effects procedure

    // Get mask of effects for target
    uint32 mask = target->effectMask;
    if (mask == 0)                                          // No effects
        return;

    Unit* unit = m_caster->GetGUID() == target->targetGUID ? m_caster : m_caster->GetMap()->GetUnit(target->targetGUID);
    if (!unit)
        return;

    // Get original caster (if exist) and calculate damage/healing from him data
    Unit *caster = m_originalCasterGUID ? m_originalCaster : m_caster;

    // Skip if m_originalCaster not avaiable
    if (!caster)
        return;

    SpellMissInfo missInfo = target->missCondition;
    
    // Need init unitTarget by default unit (can changed in code on reflect)
    // Or on missInfo!=SPELL_MISS_NONE unitTarget undefined (but need in trigger subsystem)
    unitTarget = unit;

    // Reset damage/healing counter
    m_damage = target->damage;
    m_healing = -target->damage;

    // Fill base trigger info
    uint32 procAttacker = m_procAttacker;
    uint32 procVictim = m_procVictim;
    uint32 procEx = PROC_EX_NONE;

    if (missInfo == SPELL_MISS_NONE)                          // In case spell hit target, do all effect on that target
        DoSpellHitOnUnit(unit, mask);
    else if (missInfo == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
    {
        // drop reflect aura charges on spell_hit
        caster->ProcDamageAndSpell(unit, PROC_FLAG_NONE, PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT, PROC_EX_REFLECT, 1, BASE_ATTACK, GetSpellEntry());
        if (target->reflectResult == SPELL_MISS_NONE && CheckTargetCreatureType(m_caster))       // If reflected spell hit caster -> do all effect on him, also check type again
            DoSpellHitOnUnit(m_caster, mask);
    }

    if (unitTarget->IsFriendlyTo(caster))
    {
        procVictim &= ~PROC_FLAG_TAKEN_NEGATIVE_SPELL_HIT;
        procAttacker &= ~PROC_FLAG_SUCCESSFUL_NEGATIVE_SPELL_HIT;
    }

    // All calculated do it!
    // Do healing and triggers
    if (m_healing > 0)
    {
        bool crit = caster->isSpellCrit(NULL, GetSpellEntry(), m_spellSchoolMask, BASE_ATTACK, m_extraCrit);
        uint32 addhealth = m_healing;
        if (crit)
        {
            procEx |= PROC_EX_CRITICAL_HIT;
            addhealth = caster->SpellCriticalBonus(GetSpellEntry(), addhealth, NULL);
        }
        else
            procEx |= PROC_EX_NORMAL_HIT;
        procEx |= PROC_EX_DAMAGE_OR_HEAL;

        caster->SendHealSpellLog(unitTarget, GetSpellEntry()->Id, addhealth, crit);

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, addhealth, m_attackType, GetSpellEntry(), m_canTrigger);

        int32 gain = unitTarget->ModifyHealth(int32(addhealth));

        float threat = gain * 0.5f;
        SpellMgr::ApplySpellThreatModifiers(GetSpellEntry(), threat);
        unitTarget->getHostileRefManager().threatAssist(caster, threat, GetSpellEntry());

        if (caster->GetTypeId() == TYPEID_PLAYER)
        {
            if (BattleGround *bg = ((Player*)caster)->GetBattleGround())
                bg->UpdatePlayerScore(((Player*)caster), SCORE_HEALING_DONE, gain);

            if (caster->GetMap() && caster->GetMap()->IsDungeon() && ((InstanceMap*)caster->GetMap())->GetInstanceData())
                ((InstanceMap*)caster->GetMap())->GetInstanceData()->OnPlayerHealDamage(caster->ToPlayer(), gain);
        }
    }
    // Do damage and triggers
    else if (m_damage > 0)
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellDamageLog damageInfo(GetSpellEntry()->Id, caster, unitTarget, m_spellSchoolMask);

        // Add bonuses and fill damageInfo struct
        caster->CalculateSpellDamageTaken(&damageInfo, m_damage, GetSpellEntry(), m_attackType, target->crit, target->missCondition == SPELL_MISS_BLOCK);

        // Send log damage message to client
        // caster->SendSpellNonMeleeDamageLog(&damageInfo);

        procEx = createProcExtendMask(&damageInfo, missInfo);
        procEx |= PROC_EX_DAMAGE_OR_HEAL;

        if (damageInfo.damage)
            procVictim |= PROC_FLAG_TAKEN_ANY_DAMAGE;

        if (damageInfo.absorb)
            procEx &= ~PROC_EX_DIRECT_DAMAGE;
        else
            procEx |= PROC_EX_DIRECT_DAMAGE;

        if (missInfo == SPELL_MISS_REFLECT)
            damageInfo.threatTarget = unit->GetGUID();

        caster->DealSpellDamage(&damageInfo, true);

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (missInfo != SPELL_MISS_REFLECT)
        {
            caster->ProcDamageAndSpell(unitTarget, procAttacker, procVictim, procEx, damageInfo.damage, m_attackType, GetSpellEntry(), m_canTrigger);
            if (caster->GetTypeId() == TYPEID_PLAYER)
                ((Player *)caster)->CastItemCombatSpell(unitTarget, m_attackType, procVictim, procEx, GetSpellEntry());
        }

        // Shadow Word: Death - deals damage equal to damage done to caster if victim is not killed
        if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_PRIEST && GetSpellEntry()->SpellFamilyFlags & 0x0000000200000000LL &&
            caster != unitTarget && unitTarget->IsAlive())
        {
            // Redirect damage to caster if victim alive
            int32 truedmg = damageInfo.damage;
            m_caster->CastCustomSpell(m_caster, 32409, &truedmg, NULL, NULL, true);
        }
        // Judgement of Blood
        else if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_PALADIN && GetSpellEntry()->SpellFamilyFlags & 0x0000000800000000LL && GetSpellEntry()->SpellIconID == 153)
        {
            int32 damagePoint = damageInfo.damage * 33 / 100;
            m_caster->CastCustomSpell(m_caster, 32220, &damagePoint, NULL, NULL, true);
        }
        // Bloodthirst
        else if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_WARRIOR && GetSpellEntry()->SpellFamilyFlags & 0x40000000000LL)
        {
            uint32 BTAura = 0;
            switch (GetSpellEntry()->Id)
            {
                case 23881: BTAura = 23885; break;
                case 23892: BTAura = 23886; break;
                case 23893: BTAura = 23887; break;
                case 23894: BTAura = 23888; break;
                case 25251: BTAura = 25252; break;
                case 30335: BTAura = 30339; break;
                default:
                    sLog.outLog(LOG_DEFAULT, "ERROR: Spell::EffectSchoolDMG: Spell %u not handled in BTAura", GetSpellEntry()->Id);
                    break;
            }
            if (BTAura)
                m_caster->CastSpell(m_caster, BTAura, true);
        }
    }
    // Passive spell hits/misses or active spells only misses (only triggers)
    else
    {
        // Fill base damage struct (unitTarget - is real spell target)
        SpellDamageLog damageInfo(GetSpellEntry()->Id, caster, unitTarget, m_spellSchoolMask);
        procEx = createProcExtendMask(&damageInfo, missInfo);
        bool isDamageOrHealSpell = false;
        for (int i = 0; i < 3; i++)
            switch (GetSpellEntry()->EffectApplyAuraName[i])
        {
                case SPELL_AURA_PERIODIC_DAMAGE: case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_HEAL:   case SPELL_AURA_PERIODIC_LEECH:
                    isDamageOrHealSpell = true;
        }

        if (isDamageOrHealSpell)
            procEx |= PROC_EX_DAMAGE_OR_HEAL;

        // Do triggers for unit (reflect triggers passed on hit phase for correct drop charge)
        if (missInfo != SPELL_MISS_REFLECT)
            caster->ProcDamageAndSpell(unit, procAttacker, procVictim, procEx, 0, m_attackType, GetSpellEntry(), m_canTrigger);
    }

    // Call scripted function for AI if this spell is cast upon a creature (except pets)
    if (unit->GetTypeId() == TYPEID_UNIT)
    {
        // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
        // ignore pets or autorepeat/melee casts for speed (not exist quest for spells (hm... )
        if (!((Creature*)unit)->isPet() && !IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive())
            if (Player* p = GetPlayerForCastQuestCond())
                p->CastCreatureOrGO(unit->GetEntry(), unit->GetGUID(), GetSpellEntry()->Id);
    }

    if (!m_caster->IsFriendlyTo(unit) && !SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER || !((Player const*)m_caster)->IsGameMaster())
            m_caster->CombatStart(unit, !(GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO));
    }

    // if target is flagged for pvp also flag caster if a player
    if (unit->IsPvP() && m_caster->GetTypeId() == TYPEID_PLAYER && m_caster != unit)
    {
        if (!((Player*)m_caster)->duel || ((Player*)m_caster)->duel->opponent != unit->GetCharmerOrOwnerPlayerOrPlayerItself())
            ((Player*)m_caster)->UpdatePvP(true);
    }

    if (unit->GetTypeId() == TYPEID_UNIT && ((Creature*)unit)->IsAIEnabled)
        ((Creature*)unit)->AI()->SpellHit(m_caster, GetSpellEntry());

    if (m_caster->GetTypeId() == TYPEID_UNIT && ((Creature*)m_caster)->IsAIEnabled)
        ((Creature*)m_caster)->AI()->SpellHitTarget(unit, GetSpellEntry());
}

void Spell::DoSpellHitOnUnit(Unit *unit, const uint32 effectMask)
{
    if (!unit || !effectMask)
        return;

    if (unit->IsImmunedToSpellEffect(SPELL_EFFECT_ATTACK_ME, MECHANIC_NONE) && SpellMgr::IsTauntSpell(GetSpellEntry()))
    {
        m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_IMMUNE);
        return;
    }

    // Recheck immune (only for delayed spells)
    if (IsDelayedSpell() &&
        !(GetSpellEntry()->Attributes & SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY) &&
        (unit->IsImmunedToDamage(SpellMgr::GetSpellSchoolMask(GetSpellEntry()), true) ||
        unit->IsImmunedToSpell(GetSpellEntry(), true))
        )
    {
        m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_IMMUNE);
        m_damage = 0;
        return;
    }
    
    if (m_caster != unit)
    {
        if (unit->GetCharmerOrOwnerGUID() != m_caster->GetGUID() && !CanIgnoreNotAttackableFlags())
        {
            if (unit->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING))
            {
                m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_EVADE);
                m_damage = 0;
                return;
            }
        }
        if ((m_originalCaster && !m_originalCaster->IsFriendlyTo(unit)) || !m_caster->IsFriendlyTo(unit))
        {
            // reset damage to 0 if target has Invisibility or Vanish aura (_only_ vanish, not stealth) and isn't visible for caster
            bool isVisibleForHit = ((unit->HasAuraType(SPELL_AURA_MOD_INVISIBILITY) || unit->HasAuraTypeWithFamilyFlags(SPELL_AURA_MOD_STEALTH, SPELLFAMILY_ROGUE, SPELLFAMILYFLAG_ROGUE_VANISH)) && !unit->isVisibleForOrDetect(m_caster, m_caster, true)) ? false : true;

            // for delayed spells ignore not visible explicit target
            if (IsDelayedSpell() && unit == m_targets.getUnitTarget() && !isVisibleForHit)
            {
                // that was causing CombatLog errors
                //m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_EVADE);
                m_damage = 0;
                return;
            }
            unit->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_HITBYSPELL);
        }
        else
        {
            // for delayed spells ignore negative spells (after duel end) for friendly targets
            // this cause soul transfer, and curse of boundless agony bugged, for a moment exception added
            if (GetSpellEntry()->Id != 45034 && GetSpellEntry()->Id != 30531 && IsDelayedSpell() && unit->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
            {
                m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_EVADE);
                m_damage = 0;
                return;
            }

            // assisting case, healing and resurrection
            if (unit->HasUnitState(UNIT_STAT_ATTACK_PLAYER))
            {
                m_caster->SetContestedPvP();
                //m_caster->UpdatePvP(true);
            }

            if (unit->IsInCombat() && !(GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_NO_INITIAL_AGGRO))
            {
                m_caster->SetInCombatState(unit->GetCombatTimer() > 0, unit);
                unit->getHostileRefManager().threatAssist(m_caster, 0.0f);
            }
        }
    }

    // Get Data Needed for Diminishing Returns, some effects may have multiple auras, so this must be done on spell hit, not aura add
    if (m_diminishGroup = SpellMgr::GetDiminishingReturnsGroupForSpell(GetSpellEntry(), m_triggeredByAuraSpell))
    {
        m_diminishLevel = unit->GetDiminishing(m_diminishGroup);
        // send immunity message if target is immune
        if (m_diminishLevel == DIMINISHING_LEVEL_IMMUNE)
        {
            m_caster->SendSpellMiss(unitTarget, GetSpellEntry()->Id, SPELL_MISS_IMMUNE);
            if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
                SendChannelStart(0);
            return;
        }

        DiminishingReturnsType type = SpellMgr::GetDiminishingReturnsGroupType(m_diminishGroup);
        // Increase Diminishing on unit, current informations for actually casts will use values above
        if ((type == DRTYPE_PLAYER && unit->isCharmedOwnedByPlayerOrPlayer()) || type == DRTYPE_ALL)
        {                                                   // Freezing trap exception, since it is cast by GO ?
            if (m_caster->isCharmedOwnedByPlayerOrPlayer() || (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_HUNTER && GetSpellEntry()->SpellFamilyFlags & 0x00000000008LL))
                unit->IncrDiminishing(m_diminishGroup);
        }
    }
    
    for (uint32 effectNumber = 0; effectNumber < 3; effectNumber++)
    {
        if (!(effectMask & (1 << effectNumber)))
            continue;

        if (unit->IsImmunedToSpellEffect(GetSpellEntry()->Effect[effectNumber], GetSpellEntry()->EffectMechanic[effectNumber]))
            continue;

        if (int32 effResChance = unit->GetEffectMechanicResistChance(GetSpellEntry(), effectNumber))
        {
            if (effResChance > urand(0,99)) // resistchance 0 ? then 0 cant be > 0; resistchance 100 ? then 100 is always > 99
            {
                m_caster->SendSpellMiss(unit, GetSpellEntry()->Id, SPELL_MISS_RESIST);
                continue;
            }
        }
        
        HandleEffects(unit, NULL, NULL, effectNumber/*,m_damageMultipliers[effectNumber]*/);
    }

    // trigger only for first effect targets
    if (m_ChanceTriggerSpells.size() && (effectMask & 0x1))
    {
        int _duration = 0;
        for (ChanceTriggerSpells::const_iterator i = m_ChanceTriggerSpells.begin(); i != m_ChanceTriggerSpells.end(); ++i)
        {
            if (roll_chance_i(i->second))
            {
                m_caster->CastSpell(unit, i->first, true);

                // SPELL_AURA_ADD_TARGET_TRIGGER auras shouldn't trigger auras without duration
                // set duration equal to triggering spell
                if (SpellMgr::GetSpellDuration(i->first) == -1)
                {
                    // get duration from aura-only once
                    if (!_duration)
                    {
                        Aura * aur = unit->GetAuraByCasterSpell(GetSpellEntry()->Id, m_caster->GetGUID());
                        _duration = aur ? aur->GetAuraDuration() : -1;
                    }
                    unit->SetAurasDurationByCasterSpell(i->first->Id, m_caster->GetGUID(), _duration);
                }
            }
        }
    }

    if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_LINK_HIT)
    {
        if (const std::vector<int32> *spell_triggered = sSpellMgr.GetSpellLinked(GetSpellEntry()->Id + SPELL_LINK_HIT))
            for (std::vector<int32>::const_iterator i = spell_triggered->begin(); i != spell_triggered->end(); ++i)
                if (*i < 0)
                    unit->RemoveAurasDueToSpell(-(*i));
                else
                    unit->CastSpell(unit, *i, true, 0, 0, m_caster->GetGUID());
    }
}

void Spell::DoAllEffectOnTarget(GOTargetInfo *target)
{
    if (this->GetSpellEntry()->Id <= 0 || this->GetSpellEntry()->Id > MAX_SPELL_ID || GetSpellEntry()->Id == 32 || GetSpellEntry()->Id == 80)
        return;

    if (!target || target == (GOTargetInfo*)0x10 || target->processed)    // Check target
        return;

    target->processed = true;                               // Target checked in apply effects procedure

    uint32 effectMask = target->effectMask;
    if (!effectMask)
        return;

    GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
    if (!go)
        return;

    for (uint32 effectNumber = 0; effectNumber < 3; effectNumber++)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, NULL, go, effectNumber);

    // cast at creature (or GO) quest objectives update at successful cast finished (+channel finished)
    // ignore autorepeat/melee casts for speed (not exist quest for spells (hm...)
    if (!IsAutoRepeat() && !IsNextMeleeSwingSpell() && !IsChannelActive())
        if (Player* p = GetPlayerForCastQuestCond())
            p->CastCreatureOrGO(go->GetEntry(), go->GetGUID(), GetSpellEntry()->Id);
}

void Spell::DoAllEffectOnTarget(ItemTargetInfo *target)
{
    uint32 effectMask = target->effectMask;
    if (!target->item || !effectMask)
        return;

    for (uint32 effectNumber = 0; effectNumber < 3; effectNumber++)
        if (effectMask & (1 << effectNumber))
            HandleEffects(NULL, target->item, NULL, effectNumber);
}

bool Spell::IsAliveUnitPresentInTargetList()
{
    // Not need check return true
    if (m_needAliveTargetMask == 0)
        return true;

    uint8 needAliveTargetMask = m_needAliveTargetMask;

    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (ihit->missCondition == SPELL_MISS_NONE && (needAliveTargetMask & ihit->effectMask))
        {
            Unit *unit = m_caster->GetGUID() == ihit->targetGUID ? m_caster : m_caster->GetMap()->GetUnit(ihit->targetGUID);

            if (unit && (unit->IsAlive() != SpellMgr::IsDeathOnlySpell(GetSpellEntry())))
                needAliveTargetMask &= ~ihit->effectMask;   // remove from need alive mask effect that have alive target
        }
    }

    // is all effects from m_needAliveTargetMask have alive targets
    return needAliveTargetMask == 0;
}

// Helper for Chain Healing
struct ChainHealingOrder : public std::binary_function < const Unit*, const Unit*, bool >
{
    const Unit* MainTarget;
    ChainHealingOrder(Unit const* Target) : MainTarget(Target) {};
    // functor for operator ">"
    bool operator()(Unit const* _Left, Unit const* _Right) const
    {
        return (ChainHealingHash(_Left) < ChainHealingHash(_Right));
    }

    int32 ChainHealingHash(Unit const* Target) const
    {

        //totems are removed in the function that gives us the target list, we only sort them
        // main is player or a pet of a player and is in a group
        if (MainTarget->GetCharmerOrOwnerOrSelf()->GetTypeId() == TYPEID_PLAYER && MainTarget->GetCharmerOrOwnerOrSelf()->ToPlayer()->GetGroup()) //
        {
            //our main target or it's pet is in a raid, now we have to check if target or it's owner is in raid with main target
            if (Target->GetCharmerOrOwnerOrSelf()->GetTypeId() == TYPEID_PLAYER && Target->GetCharmerOrOwnerOrSelf()->IsInRaidWith(/*main target OR it's owner*/MainTarget->GetCharmerOrOwnerOrSelf()))
            {
                //both in one raid, we accept and check health
                if (Target->GetHealth() == Target->GetMaxHealth())
                    return 20000;
                else
                    return (int(Target->GetHealth() / Target->GetMaxHealth() * 10000));

            }
            else // target is not in the raid with main target, but main target is in a raid, we don't accept this target
            {
                return 0;
            }
        }
        else
        {
            if (Target->GetHealth() == Target->GetMaxHealth())
                return 20000;
            else
                return (int(Target->GetHealth() / Target->GetMaxHealth() * 10000));

        }

        return 0;
    }


};

void Spell::SearchChainTarget(std::list<Unit*> &TagUnitMap, float max_range, uint32 num, SpellTargets TargetType)
{
    Unit *cur = m_targets.getUnitTarget();
    if (!cur)
        return;

    //FIXME: This very like horrible hack and wrong for most spells
    if (GetSpellEntry()->DmgClass != SPELL_DAMAGE_CLASS_MELEE)
        max_range += num * CHAIN_SPELL_JUMP_RADIUS;


    std::list<Unit*> tempUnitMap;
    if (TargetType == SPELL_TARGETS_CHAINHEAL)
    {
        SearchAreaTarget(tempUnitMap, max_range, PUSH_CHAIN, SPELL_TARGETS_ALLY);
        tempUnitMap.sort(ChainHealingOrder(m_caster));
        //if(cur->GetHealth() == cur->GetMaxHealth() && tempUnitMap.size())
        //    cur = tempUnitMap.front();
    }
    else
        SearchAreaTarget(tempUnitMap, max_range, PUSH_CHAIN, TargetType);
    tempUnitMap.remove(cur);

    while (num)
    {
        TagUnitMap.push_back(cur);
        --num;

        if (tempUnitMap.empty())
            break;

        std::list<Unit*>::iterator next;

        bool ignoreLOS = SpellMgr::SpellIgnoreLOS(GetSpellEntry(), 0);
        if (TargetType == SPELL_TARGETS_CHAINHEAL)
        {
            next = tempUnitMap.begin();
            bool bad_target = false;
            do
            {
                while (cur->GetDistance(*next) > CHAIN_SPELL_JUMP_RADIUS || (!ignoreLOS && !cur->IsWithinLOSInMap(*next)) ||
                    (*next)->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_NOT_PL_SPELL_TARGET) || bad_target || (*next) == cur ||
                    ((*next)->GetTypeId() == TYPEID_UNIT && !(*next)->IsPvP()))
                {
                    bad_target = false;
                    ++next;
                    if (next == tempUnitMap.end())
                        break;
                }

                if (next == tempUnitMap.end())
                {
                    tempUnitMap.clear();
                    break;
                }

                //sanctuary healing other faction
                if ((*next)->isInSanctuary() || cur->isInSanctuary())
                {
                    if (((*next)->GetTypeId() == TYPEID_PLAYER && cur->GetTypeId() == TYPEID_PLAYER) && ((*next)->GetCharmerOrOwnerOrSelf()->ToPlayer()->TeamForRace((*next)->GetRace()) != cur->GetCharmerOrOwnerOrSelf()->ToPlayer()->TeamForRace(cur->GetRace())))
                    {
                        bad_target = true;
                        return;
                    }
                }

                //now we can delete who we don't want in the list, we couldn't in the sorting function, and we can't get the value it saved
                if (cur->GetCharmerOrOwnerOrSelf()->GetTypeId() == TYPEID_PLAYER && cur->GetCharmerOrOwnerOrSelf()->ToPlayer()->GetGroup()) // main target (or pet)[cur] is in raid, target[next] is not in the same raid, we go on
                {
                    if ((*next)->GetCharmerOrOwnerOrSelf()->GetTypeId() == TYPEID_PLAYER && !((*next)->GetCharmerOrOwnerOrSelf()->IsInRaidWith(cur->GetCharmerOrOwnerOrSelf())))
                    {
                        bad_target = true;
                    }
                }



            } while (bad_target);

            if (next == tempUnitMap.end() || tempUnitMap.empty())
                break;

        }
        else
        {
            tempUnitMap.sort(MaNGOS::ObjectDistanceOrder(cur));
            next = tempUnitMap.begin();

            if (cur->GetDistance(*next) > CHAIN_SPELL_JUMP_RADIUS)
                break;

            while (GetSpellEntry()->DmgClass == SPELL_DAMAGE_CLASS_MELEE
                && !m_caster->isInFront(*next, max_range)
                || !m_caster->canSeeOrDetect(*next, m_caster, false)
                || (GetSpellEntry()->AttributesEx6 & SPELL_ATTR_EX6_CANT_TARGET_CCD && ((*next)->hasNegativeAuraWithInterruptFlag(AURA_INTERRUPT_FLAG_DAMAGE) || (*next)->GetTypeId() == TYPEID_UNIT && ((Creature*)(*next))->GetCreatureType() == CREATURE_TYPE_CRITTER))
                || !ignoreLOS && !cur->IsWithinLOSInMap(*next))
            {
                ++next;
                if (next == tempUnitMap.end() || cur->GetDistance(*next) > CHAIN_SPELL_JUMP_RADIUS)
                    return;
            }
        }

        cur = *next;
        tempUnitMap.erase(next);
    }
}

void Spell::SearchAreaTarget(std::list<Unit*> &TagUnitMap, float radius, const uint32 type, SpellTargets TargetType, uint32 entry, SpellScriptTargetType spellScriptTargetType)
{
    float x, y, z;
    switch (type)
    {
        case PUSH_DST_CENTER:
            CheckDst();
            x = m_targets.m_destX;
            y = m_targets.m_destY;
            z = m_targets.m_destZ;
            break;
        case PUSH_SRC_CENTER:
            CheckSrc();
            x = m_targets.m_srcX;
            y = m_targets.m_srcY;
            z = m_targets.m_srcZ;
            break;
        case PUSH_CHAIN:
        {
            Unit *target = m_targets.getUnitTarget();
            if (!target)
            {
                sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: cannot find unit target for spell ID %u\n", GetSpellEntry()->Id);
                return;
            }
            x = target->GetPositionX();
            y = target->GetPositionY();
            z = target->GetPositionZ();
            break;
        }
        default:
            x = m_caster->GetPositionX();
            y = m_caster->GetPositionY();
            z = m_caster->GetPositionZ();
            break;
    }

    switch (spellScriptTargetType)
    {
        case SPELL_TARGET_TYPE_NONE:
        {
            MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, TagUnitMap, radius, type, TargetType, entry, x, y, z);
            Cell::VisitAllObjects(x, y, m_caster->GetMap(), notifier, radius);
            if ((GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_PLAYERS_ONLY))
                TagUnitMap.remove_if(MaNGOS::ObjectTypeIdCheck(TYPEID_PLAYER, false)); // above line will select also pets and totems, remove them
            break;
        }
        case SPELL_TARGET_TYPE_CREATURE:
        {
            if (!entry)
                break;

            MaNGOS::SpellNotifierCreatureAndPlayer notifier(*this, TagUnitMap, radius, type, TargetType, entry, x, y, z);
            Cell::VisitAllObjects(x, y, m_caster->GetMap(), notifier, radius);
            break;
        }
        case SPELL_TARGET_TYPE_DEAD:
        {
            if (!entry)
                break;

            MaNGOS::SpellNotifierDeadCreature notifier(*this, TagUnitMap, radius, type, TargetType, entry, x, y, z);
            Cell::VisitAllObjects(x, y, m_caster->GetMap(), notifier, radius);
            break;
        }
        default:
            sLog.outLog(LOG_DEFAULT, "ERROR: WTF ? Oo Wrong spell script target type for this function: %i (shouldbe %i or %i)", spellScriptTargetType, SPELL_TARGET_TYPE_CREATURE, SPELL_TARGET_TYPE_DEAD);
            break;
    }
    TagUnitMap.remove_if(MaNGOS::ObjectIsTotemCheck(true)); // totems should not be affected by AoE spells (check if no exceptions?)
}

void Spell::SearchAreaTarget(std::list<GameObject*> &goList, float radius, const uint32 type, SpellTargets TargetType, uint32 entry, SpellScriptTargetType spellScriptTargetType)
{
    float x, y, z;
    switch (type)
    {
        case PUSH_DST_CENTER:
            CheckDst();
            x = m_targets.m_destX;
            y = m_targets.m_destY;
            z = m_targets.m_destZ;
            break;
        case PUSH_SRC_CENTER:
            CheckSrc();
            x = m_targets.m_srcX;
            y = m_targets.m_srcY;
            z = m_targets.m_srcZ;
            break;
        case PUSH_CHAIN:
        {
            GameObject *target = m_targets.getGOTarget();
            if (!target)
            {
                sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: cannot find unit target for spell ID %u\n", GetSpellEntry()->Id);
                return;
            }
            x = target->GetPositionX();
            y = target->GetPositionY();
            z = target->GetPositionZ();
            break;
        }
        default:
            x = m_caster->GetPositionX();
            y = m_caster->GetPositionY();
            z = m_caster->GetPositionZ();
            break;
    }

    switch (spellScriptTargetType)
    {
        case SPELL_TARGET_TYPE_GAMEOBJECT:
        {
            if (!entry)
                break;

            MaNGOS::AllGameObjectsWithEntryInGrid go_check(entry);
            MaNGOS::ObjectListSearcher<GameObject, MaNGOS::AllGameObjectsWithEntryInGrid> go_search(goList, go_check);
            Cell::VisitGridObjects(x, y, m_caster->GetMap(), go_search, radius);
            break;
        }
        default:
            sLog.outLog(LOG_DEFAULT, "ERROR: WTF ? Oo Wrong spell script target type for this function: %i (should be %i)", spellScriptTargetType, SPELL_TARGET_TYPE_GAMEOBJECT);
            break;
    }
}

WorldObject* Spell::SearchNearbyTarget(float range, SpellTargets TargetType)
{
    switch (TargetType)
    {
        case SPELL_TARGETS_ENTRY:
        {
            SpellScriptTarget::const_iterator lower = sSpellMgr.GetBeginSpellScriptTarget(GetSpellEntry()->Id);
            SpellScriptTarget::const_iterator upper = sSpellMgr.GetEndSpellScriptTarget(GetSpellEntry()->Id);
            if (lower == upper)
            {
                sLog.outLog(LOG_DB_ERR, "Spell (ID: %u) (caster Entry: %u) does not have record in `spell_script_target`", GetSpellEntry()->Id, m_caster->GetEntry());
                if (SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
                    return SearchNearbyTarget(range, SPELL_TARGETS_ALLY);
                else
                    return SearchNearbyTarget(range, SPELL_TARGETS_ENEMY);
            }

            Creature* creatureScriptTarget = NULL;
            GameObject* goScriptTarget = NULL;

            for (SpellScriptTarget::const_iterator i_spellST = lower; i_spellST != upper; ++i_spellST)
            {
                switch (i_spellST->second.type)
                {
                    case SPELL_TARGET_TYPE_GAMEOBJECT:
                    {
                        GameObject* p_GameObject = NULL;

                        if (i_spellST->second.targetEntry)
                        {
                            MaNGOS::NearestGameObjectEntryInObjectRangeCheck go_check(*m_caster, i_spellST->second.targetEntry, range);
                            MaNGOS::ObjectLastSearcher<GameObject, MaNGOS::NearestGameObjectEntryInObjectRangeCheck> checker(p_GameObject, go_check);

                            Cell::VisitGridObjects(m_caster, checker, range);

                            if (p_GameObject)
                            {
                                // remember found target and range, next attempt will find more near target with another entry
                                creatureScriptTarget = NULL;
                                goScriptTarget = p_GameObject;
                                range = go_check.GetLastRange();
                            }
                        }
                        else if (focusObject)          //Focus Object
                        {
                            float frange = m_caster->GetDistance(focusObject);
                            if (range >= frange)
                            {
                                creatureScriptTarget = NULL;
                                goScriptTarget = focusObject;
                                range = frange;
                            }
                        }
                        break;
                    }
                    case SPELL_TARGET_TYPE_CREATURE:
                    case SPELL_TARGET_TYPE_DEAD:
                    default:
                    {
                        Creature *p_Creature = NULL;

                        MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck u_check(*m_caster, i_spellST->second.targetEntry, i_spellST->second.type != SPELL_TARGET_TYPE_DEAD, range, false);
                        MaNGOS::ObjectLastSearcher<Creature, MaNGOS::NearestCreatureEntryWithLiveStateInObjectRangeCheck> searcher(p_Creature, u_check);
                        Cell::VisitAllObjects(m_caster, searcher, range);

                        if (p_Creature)
                        {
                            creatureScriptTarget = p_Creature;
                            goScriptTarget = NULL;
                            range = u_check.GetLastRange();
                        }
                        break;
                    }
                }
            }

            if (creatureScriptTarget)
                return creatureScriptTarget;
            else
                return goScriptTarget;
        }
        default:
        case SPELL_TARGETS_ENEMY:
        {
            Unit *target = NULL;
            MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck u_check(m_caster, m_caster, range);
            MaNGOS::UnitLastSearcher<MaNGOS::AnyUnfriendlyUnitInObjectRangeCheck> searcher(target, u_check);

            Cell::VisitAllObjects(m_caster, searcher, range);
            return target;
        }
        case SPELL_TARGETS_ALLY:
        {
            Unit *target = NULL;
            MaNGOS::AnyFriendlyNonSelfUnitInObjectRangeCheck u_check(m_caster, m_caster, range);
            MaNGOS::UnitLastSearcher<MaNGOS::AnyFriendlyNonSelfUnitInObjectRangeCheck> searcher(target, u_check);

            Cell::VisitAllObjects(m_caster, searcher, range);
            return target;
        }
    }
}

void Spell::SetTargetMap(uint32 i, uint32 cur)
{
    SpellNotifyPushType pushType = PUSH_NONE;
    Player *modOwner = NULL;
    if (m_originalCaster)
        modOwner = m_originalCaster->GetSpellModOwner();

    switch (sSpellMgr.SpellTargetType[cur])
    {
        case TARGET_TYPE_UNIT_CASTER:
        {
            switch (cur)
            {
                case TARGET_UNIT_CASTER:
                    AddUnitTarget(m_caster, i);
                    break;
                case TARGET_LOCATION_CASTER_FISHING_SPOT:
                {
                    //AddUnitTarget(m_caster, i);
                    float min_dis = SpellMgr::GetSpellMinRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));
                    float max_dis = SpellMgr::GetSpellMaxRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));
                    float dis = frand(min_dis, max_dis);

                    Position pos;
                    m_caster->GetPosition(&pos);
                    float angle = m_caster->GetOrientation() + frand(-0.6f, 0.6f);
                    pos.m_positionX += dis * cos(angle);
                    pos.m_positionY += dis * sin(angle);
                    //m_caster->GetValidPointInAngle(pos, dis + DEFAULT_WORLD_OBJECT_SIZE, frand(-0.6, 0.6), true, max_dis);
                    float liquidLevel = m_caster->GetMap()->GetTerrain()->GetWaterOrGroundLevel(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
                    m_targets.setDestination(pos.m_positionX, pos.m_positionY, liquidLevel);
                    if (!m_caster->IsWithinLOS(pos.m_positionX, pos.m_positionY, liquidLevel) && m_caster->GetAreaId() != 3607) // ssc lurker los bug
                    {
                        SendCastResult(SPELL_FAILED_LINE_OF_SIGHT);
                        SendChannelUpdate(0);
                        finish(false);
                        return;
                    }
                    if (!m_caster->GetTerrain()->IsInWater(pos.m_positionX, pos.m_positionY, liquidLevel))
                    {
                        SendCastResult(SPELL_FAILED_NOT_HERE);
                        SendChannelUpdate(0);
                        finish(false);
                        return;
                    }
                    break;
                }
                case TARGET_UNIT_CASTER_MASTER:
                    if (Unit* owner = m_caster->GetCharmerOrOwner())
                        AddUnitTarget(owner, i);
                    break;
                case TARGET_UNIT_CASTER_PET:
                    if (Pet* pet = m_caster->GetPet())
                        AddUnitTarget(pet, i);
                    else if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->GetClass() == CLASS_WARLOCK)
                    { // enslaved demons
                        if (Unit* charm = m_caster->GetCharm())
                            AddUnitTarget(charm, i);
                    }
                    break;
                case TARGET_ENUM_UNITS_PARTY_WITHIN_CASTER_RANGE:
                case TARGET_ENUM_UNITS_RAID_WITHIN_CASTER_RANGE:
                    pushType = PUSH_CASTER_CENTER;
                    break;
            }
            break;
        }

        case TARGET_TYPE_UNIT_TARGET:
        {
            Unit *target = m_targets.getUnitTarget();
            if (!target)
            {
                sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: no unit target for spell ID %u (effect %u target %u)", GetSpellEntry()->Id, i, cur);
                break;
            }

            switch (cur)
            {
                case TARGET_UNIT_ENEMY:
                    SelectMagnetTarget();
                case TARGET_UNIT_FRIEND_CHAIN_HEAL:
                    pushType = PUSH_CHAIN;
                    break;
                case TARGET_UNIT:
                    if (!target->IsFriendlyTo(m_caster) && GetSpellEntry()->Effect[i] == SPELL_EFFECT_DISPEL)
                    {
                        SelectMagnetTarget();
                        pushType = PUSH_CHAIN;
                        break;
                    }
                case TARGET_UNIT_FRIEND:
                case TARGET_UNIT_RAID:
                case TARGET_UNIT_PARTY:
                case TARGET_UNIT_CASTER_COMPANION:
                    AddUnitTarget(target, i);
                    break;
                case TARGET_UNIT_FRIEND_AND_PARTY:
                case TARGET_UNIT_RAID_AND_CLASS:
                    pushType = PUSH_CASTER_CENTER; // not real
                    break;
            }
            break;
        }

        case TARGET_TYPE_UNIT_NEARBY:
        {
            float range = SpellMgr::GetSpellMaxRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));

            // limit check range for some spells
            if (range > 400)
                range = 400;

            if (modOwner)
                modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_RANGE, range, this);

            WorldObject *target = NULL;

            switch (cur)
            {
                case TARGET_UNIT_ENEMY_NEAR_CASTER:
                    target = SearchNearbyTarget(range, SPELL_TARGETS_ENEMY);
                    break;
                case TARGET_UNIT_FRIEND_NEAR_CASTER:
                case TARGET_UNIT_NEAR_CASTER:
                case TARGET_UNIT_RAID_NEAR_CASTER:
                    target = SearchNearbyTarget(range, SPELL_TARGETS_ALLY);
                    break;
                case TARGET_UNIT_SCRIPT_NEAR_CASTER:
                case TARGET_GAMEOBJECT_SCRIPT_NEAR_CASTER:
                    target = SearchNearbyTarget(range, SPELL_TARGETS_ENTRY);
                    break;
            }

            if (!target)
                return;
            else if (target->GetTypeId() == TYPEID_UNIT || target->GetTypeId() == TYPEID_PLAYER)
            {
                pushType = PUSH_CHAIN;

                m_targets.setUnitTarget((Unit*)target);
            }
            else if (target->GetTypeId() == TYPEID_GAMEOBJECT)
                AddGOTarget((GameObject*)target, i);

            break;
        }

        case TARGET_TYPE_AREA_SRC:
            pushType = PUSH_SRC_CENTER;
            break;

        case TARGET_TYPE_AREA_DST:
            pushType = PUSH_DST_CENTER;
            break;

        case TARGET_TYPE_AREA_CONE:
            if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_CONE_BACK)
                pushType = PUSH_IN_BACK;
            else if (GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_CONE_LINE)
                pushType = PUSH_IN_LINE;
            else
                pushType = PUSH_IN_FRONT;
            break;

        case TARGET_TYPE_DEST_CASTER: //4+8+2
        {
            if (cur == TARGET_LOCATION_CASTER_SRC)
            {
                m_targets.setSrc(m_caster);
                break;
            }
            else if (cur == TARGET_LOCATION_CASTER_DEST)
            {
                m_targets.setDestination(m_caster);
                break;
            }

            Position pos;
            float dist;
            float allowHeightDifference = COMMON_ALLOW_HEIGHT_DIFF;

            float objSize = m_caster->GetObjectSize();
            dist = SpellMgr::GetSpellRadius(GetSpellEntry(), i, true);
            if (dist < objSize)
                dist = objSize;
            else if (cur == TARGET_LOCATION_CASTER_RANDOM_SIDE)
                dist = objSize + (dist - objSize) * rand_norm();

            if (cur == TARGET_LOCATION_CASTER_FRONT)
            {
                float x, y, z;
                float srcX, srcY, srcZ;
                float zSearchDist = 20.0f; // Falling case
                float ground = 0.0f;
                m_caster->GetPosition(x, y, z);
                m_caster->GetPosition(srcX, srcY, srcZ);
                float waterLevel = m_caster->GetTerrain()->GetWaterLevel(x, y, z, &ground);
                x += dist * cos(m_caster->GetOrientation());
                y += dist * sin(m_caster->GetOrientation());
                // Underwater blink case
                if (waterLevel != VMAP_INVALID_HEIGHT_VALUE && waterLevel > ground)
                {
                    if (z < ground)
                        z = ground;
                    // If blinking up to the surface, limit z position (do not teleport out of water)
                    if (z > waterLevel && (z - srcZ) > 1.0f)
                    {
                        float t = (waterLevel - srcZ) / (z - srcZ);
                        x = (x - srcX) * t + srcX;
                        y = (y - srcY) * t + srcY;
                        z = waterLevel;
                    }
                    m_caster->GetMap()->GetLosHitPosition(srcX, srcY, srcZ, x, y, z, -0.5f);
                    ground = m_caster->GetMap()->GetHeight(x, y, z);
                    if (ground < z)
                    {
                        m_targets.setDestination(x, y, z);
                        break;
                    }
                    // If we are leaving water, rather use pathfinding, but increase z-range position research.
                    zSearchDist = 20.0f;
                }
                if (!m_caster->GetMap()->GetWalkHitPosition(nullptr, srcX, srcY, srcZ, x, y, z, NAV_GROUND | NAV_WATER, zSearchDist, false))
                {
                    x = srcX;
                    y = srcY;
                    z = srcZ;
                }

                m_caster->GetMap()->GetLosHitPosition(srcX, srcY, srcZ, x, y, z, 1.0f);
                m_targets.setDestination(x, y, z);
                break;
            }

            switch (cur)
            {
                case TARGET_LOCATION_CASTER_FRONT_RIGHT: pos.o = -M_PI / 4;              break;
                case TARGET_LOCATION_CASTER_BACK_RIGHT:  pos.o = -3 * M_PI / 4;          break;
                case TARGET_LOCATION_CASTER_BACK_LEFT:   pos.o = 3 * M_PI / 4;           break;
                case TARGET_LOCATION_CASTER_FRONT_LEFT:  pos.o = M_PI / 4;               break;
                case TARGET_LOCATION_UNIT_MINION_POSITION:
                case TARGET_LOCATION_CASTER_FRONT:       pos.o = 0.0f;                   break;
                case TARGET_LOCATION_CASTER_BACK:        pos.o = M_PI;                   break;
                case TARGET_LOCATION_CASTER_LEFT:        pos.o = M_PI / 2;               break;
                case TARGET_LOCATION_CASTER_RIGHT:       pos.o = -M_PI / 2;              break;
                default:                                 pos.o = rand_norm() * 2 * M_PI; break;
            }
            m_caster->GetValidPointInAngle(pos, dist, pos.o, true, allowHeightDifference);
            m_targets.setDestination(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
            break;
        }

        case TARGET_TYPE_DEST_TARGET: //2+8+2
        {
            Unit *target = m_targets.getUnitTarget();
            if (!target)
            {
                sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: no unit target for spell ID %u (effect %u target %u)", GetSpellEntry()->Id, i, cur);
                break;
            }

            if (cur == TARGET_LOCATION_CASTER_TARGET_POSITION || cur == TARGET_LOCATION_UNIT_POSITION)
            {
                m_targets.setDestination(target);
                break;
            }

            Position pos;
            target->GetPosition(&pos);
            float dist;

            float objSize = target->GetObjectSize();
            dist = SpellMgr::GetSpellRadius(GetSpellEntry(), i, true);
            if (dist < objSize)
                dist = objSize;
            else if (cur == TARGET_LOCATION_CASTER_RANDOM_SIDE)
                dist = objSize + (dist - objSize) * rand_norm();

            switch (cur)
            {
                case TARGET_LOCATION_UNIT_FRONT:      pos.o = 0.0f;       break;
                case TARGET_LOCATION_UNIT_BACK:       pos.o = M_PI;       break;
                case TARGET_LOCATION_UNIT_RIGHT:      pos.o = M_PI / 2;     break;
                case TARGET_LOCATION_UNIT_LEFT:       pos.o = -M_PI / 2;    break;
                case TARGET_LOCATION_UNIT_FRONT_RIGHT: pos.o = -M_PI / 4;    break;
                case TARGET_LOCATION_UNIT_BACK_RIGHT:  pos.o = -3 * M_PI / 4;  break;
                case TARGET_LOCATION_UNIT_BACK_LEFT: pos.o = 3 * M_PI / 4;   break;
                case TARGET_LOCATION_UNIT_FRONT_LEFT:pos.o = M_PI / 4;     break;
                default:                            pos.o = rand_norm() * 2 * M_PI; break;
            }

            target->GetValidPointInAngle(pos, dist, pos.o, false);
            m_targets.setDestination(pos.m_positionX, pos.m_positionY, pos.m_positionZ);
            break;
        }

        case TARGET_TYPE_DEST_DEST: //5+8+1
        {
            if (!m_targets.HasDst())
            {
                sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: no destination for spell ID %u", GetSpellEntry()->Id);

                if (GetCaster())
                    if (Unit* owner = GetCaster()->GetCharmerOrOwner())
                        sLog.outLog(LOG_DEFAULT, "     SpellCaster = %s (%lu), a pet of %s (%lu)", GetCaster()->GetName(), GetCaster()->GetGUID(), owner->GetName(), owner->GetGUID());
                break;
            }

            float angle;
            switch (cur)
            {
                case TARGET_ENUM_UNITS_ENEMY_AOE_AT_DYNOBJ_LOC:
                case TARGET_ENUM_UNITS_FRIEND_AOE_AT_DYNOBJ_LOC:
                case TARGET_ENUM_UNITS_SCRIPT_AOE_AT_DYNOBJ_LOC:
                case TARGET_LOCATION_CURRENT_REFERENCE:
                case TARGET_LOCATION_TRAJECTORY:
                    return;
                case TARGET_LOCATION_NORTH:      angle = 0.0f;       break;
                case TARGET_LOCATION_SOUTH:       angle = M_PI;       break;
                case TARGET_LOCATION_EAST:      angle = M_PI / 2;     break;
                case TARGET_LOCATION_WEST:       angle = -M_PI / 2;    break;
                case TARGET_LOCATION_NE: angle = -M_PI / 4;    break;
                case TARGET_LOCATION_NW:  angle = -3 * M_PI / 4;  break;
                case TARGET_LOCATION_SE: angle = 3 * M_PI / 4;   break;
                case TARGET_LOCATION_SW:angle = M_PI / 4;     break;
                default:                          angle = rand_norm() * 2 * M_PI; break;
            }

            float dist, x, y, z;
            dist = SpellMgr::GetSpellRadius(GetSpellEntry(), i, true);
            if (cur == TARGET_LOCATION_RANDOM_SIDE)
                dist *= rand_norm();

            x = m_targets.m_destX;
            y = m_targets.m_destY;
            z = m_targets.m_destZ;
            m_caster->GetGroundPoint(x, y, z, dist, angle);
            m_targets.setDestination(x, y, z);
            break;
        }
        case TARGET_TYPE_DEST_SPECIAL:
        {
            switch (cur)
            {
                case TARGET_LOCATION_DATABASE:
                    if (SpellTargetPosition const* st = sSpellMgr.GetSpellTargetPosition(GetSpellEntry()->Id))
                    {
                        //TODO: fix this check
                        if (GetSpellEntry()->Effect[0] == SPELL_EFFECT_TELEPORT_UNITS
                            || GetSpellEntry()->Effect[1] == SPELL_EFFECT_TELEPORT_UNITS
                            || GetSpellEntry()->Effect[2] == SPELL_EFFECT_TELEPORT_UNITS)
                            m_targets.setDestination(st->target_X, st->target_Y, st->target_Z, st->target_Orientation, (int32)st->target_mapId);
                        else if (st->target_mapId == m_caster->GetMapId())
                            m_targets.setDestination(st->target_X, st->target_Y, st->target_Z);
                    }
                    else
                        sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: unknown target coordinates for spell ID %u", GetSpellEntry()->Id);
                    break;
                case TARGET_LOCATION_CASTER_HOME_BIND:
                    if (m_caster->GetTypeId() == TYPEID_PLAYER)
                        m_targets.setDestination(((Player*)m_caster)->m_homebindX, ((Player*)m_caster)->m_homebindY, ((Player*)m_caster)->m_homebindZ, ((Player*)m_caster)->GetOrientation(), ((Player*)m_caster)->m_homebindMapId);
                    break;
                case TARGET_LOCATION_SCRIPT_NEAR_CASTER:
                {
                    float range = SpellMgr::GetSpellMaxRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));
                    if (modOwner)
                        modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_RANGE, range, this);

                    WorldObject *target = SearchNearbyTarget(range, SPELL_TARGETS_ENTRY);
                    if (target)
                        m_targets.setDestination(target);
                    break;
                }
            }
            break;
        }

        case TARGET_TYPE_CHANNEL:
        {
            if (GetSpellEntry()->Id != 38700 && !m_originalCaster || !m_originalCaster->m_currentSpells[CURRENT_CHANNELED_SPELL])
            {
                sLog.outDebug("SPELL: no current channeled spell for spell ID %u", GetSpellEntry()->Id);
                break;
            }

            switch (cur)
            {
                case TARGET_UNIT_CHANNEL_TARGET:
                    if (Unit* target = m_originalCaster->m_currentSpells[CURRENT_CHANNELED_SPELL]->m_targets.getUnitTarget())
                        AddUnitTarget(target, i);
                    else
                        sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: cannot find channel spell target for spell ID %u", GetSpellEntry()->Id);
                    break;
                case TARGET_LOCATION_CHANNEL_TARGET_DEST:
                    if (m_originalCaster->m_currentSpells[CURRENT_CHANNELED_SPELL]->m_targets.HasDst())
                        m_targets = m_originalCaster->m_currentSpells[CURRENT_CHANNELED_SPELL]->m_targets;
                    else
                        sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: cannot find channel spell destination for spell ID %u", GetSpellEntry()->Id);
                    break;
            }
            break;
        }

        default:
        {
            switch (cur)
            {
                case TARGET_GAMEOBJECT:
                    if (m_targets.getGOTarget())
                        AddGOTarget(m_targets.getGOTarget(), i);
                    break;
                case TARGET_LOCKED:
                    if (m_targets.getGOTargetGUID())
                        AddGOTarget(m_targets.getGOTarget(), i);
                    else if (m_targets.getItemTarget())
                        AddItemTarget(m_targets.getItemTarget(), i);
                    break;
                default:
                    sLog.outLog(LOG_DEFAULT, "ERROR: Unhandled spell target %u", cur);
                    break;
            }
            break;
        }
    }

    if (pushType == PUSH_CHAIN) // Chain
    {
        Unit *target = m_targets.getUnitTarget();
        if (!target)
        {
            sLog.outLog(LOG_DEFAULT, "ERROR: SPELL: no chain unit target for spell ID %u", GetSpellEntry()->Id);
            return;
        }

        //Chain: 2, 6, 22, 25, 45, 77
        uint32 maxTargets = GetSpellEntry()->EffectChainTarget[i];
        if (modOwner)
            modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_JUMP_TARGETS, maxTargets, this);

        if (maxTargets > 1)
        {
            //otherwise, this multiplier is used for something else
            m_damageMultipliers[i] = 1.0f;
            m_applyMultiplierMask |= 1 << i;

            float range = SpellMgr::GetSpellMaxRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));
            if (modOwner)
                modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_RANGE, range, this);

            std::list<Unit*> unitList;

            switch (cur)
            {
                case TARGET_UNIT_ENEMY_NEAR_CASTER:
                case TARGET_UNIT_ENEMY:
                case TARGET_UNIT_SCRIPT_NEAR_CASTER: // fix me
                    SearchChainTarget(unitList, range, maxTargets, SPELL_TARGETS_ENEMY);
                    break;
                case TARGET_UNIT_FRIEND_CHAIN_HEAL:
                case TARGET_UNIT_FRIEND_NEAR_CASTER:  // fix me
                case TARGET_UNIT_NEAR_CASTER:
                case TARGET_UNIT_RAID_NEAR_CASTER:
                    SearchChainTarget(unitList, range, maxTargets, SPELL_TARGETS_CHAINHEAL);
                    break;
            }

            for (std::list<Unit*>::iterator itr = unitList.begin(); itr != unitList.end(); ++itr)
                AddUnitTarget(*itr, i);
        }
        else
            AddUnitTarget(target, i);
    }
    else if (pushType)
    {
        // Dummy, just for client
        if (sSpellMgr.EffectTargetType[GetSpellEntry()->Effect[i]] == SPELL_REQUIRE_DEST)
            return;

        float radius = SpellMgr::GetSpellRadius(GetSpellEntry(), i, false);
        if (modOwner)
            modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_RADIUS, radius, this);
        if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->HasUnitMovementFlag(MOVEFLAG_MOVING))
            radius += MELEE_RANGE * 2;

        if (radius > MAX_VISIBILITY_DISTANCE)
            radius = MAX_VISIBILITY_DISTANCE;

        std::list<Unit*> unitList;
        std::list<GameObject*> goList;

        switch (cur)
        {
            case TARGET_ENUM_UNITS_ENEMY_AOE_AT_SRC_LOC:
            case TARGET_ENUM_UNITS_ENEMY_AOE_AT_DEST_LOC:
            case TARGET_ENUM_UNITS_ENEMY_IN_CONE_24:
            case TARGET_ENUM_UNITS_ENEMY_IN_CONE_54:
                SearchAreaTarget(unitList, radius, pushType, SPELL_TARGETS_ENEMY);
                break;
            case TARGET_ENUM_UNITS_FRIEND_AOE_AT_SRC_LOC:
            case TARGET_ENUM_UNITS_FRIEND_AOE_AT_DEST_LOC:
            case TARGET_ENUM_UNITS_FRIEND_IN_CONE:
                SearchAreaTarget(unitList, radius, pushType, SPELL_TARGETS_ALLY);
                break;
            case TARGET_ENUM_UNITS_PARTY_AOE_AT_SRC_LOC:
            case TARGET_ENUM_UNITS_PARTY_AOE_AT_DEST_LOC:
                m_caster->GetPartyMember(unitList, radius); //fix me
                break;
            case TARGET_ENUM_GAMEOBJECTS_SCRIPT_AOE_AT_SRC_LOC: // fix me
            case TARGET_ENUM_GAMEOBJECTS_SCRIPT_AOE_AT_DEST_LOC:
                break;
            case TARGET_ENUM_UNITS_SCRIPT_AOE_AT_SRC_LOC:
            case TARGET_ENUM_UNITS_SCRIPT_AOE_AT_DEST_LOC:
            case TARGET_ENUM_UNITS_SCRIPT_IN_CONE_60: // fix me
            {
                SpellScriptTarget::const_iterator lower = sSpellMgr.GetBeginSpellScriptTarget(GetSpellEntry()->Id);
                SpellScriptTarget::const_iterator upper = sSpellMgr.GetEndSpellScriptTarget(GetSpellEntry()->Id);
                radius = SpellMgr::GetSpellRadius(GetSpellEntry(), i, SpellMgr::IsPositiveSpell(GetSpellEntry()->Id));
                if (lower == upper)
                {
                    sLog.outLog(LOG_DB_ERR, "Spell (ID: %u) (caster Entry: %u) does not have record in `spell_script_target`", GetSpellEntry()->Id, m_caster->GetEntry());

                    if (SpellMgr::IsPositiveEffect(GetSpellEntry()->Id, i))
                        SearchAreaTarget(unitList, radius, pushType, SPELL_TARGETS_ALLY);
                    else
                        SearchAreaTarget(unitList, radius, pushType, SPELL_TARGETS_ENEMY);
                }
                // let it be done in one check?
                else
                {
                    for (SpellScriptTarget::const_iterator i_spellST = lower; i_spellST != upper; ++i_spellST)
                    {
                        SpellScriptTargetType tmp = i_spellST->second.type;
                        if (tmp == SPELL_TARGET_TYPE_GAMEOBJECT)
                            SearchAreaTarget(goList, radius, pushType, SPELL_TARGETS_ENTRY, i_spellST->second.targetEntry, tmp);
                        else
                            SearchAreaTarget(unitList, radius, pushType, SPELL_TARGETS_ENTRY, i_spellST->second.targetEntry, tmp);
                    }
                }
                break;
            }
            case TARGET_UNIT_FRIEND_AND_PARTY:
                if (m_targets.getUnitTarget()->GetCharmerOrOwnerPlayerOrPlayerItself())
                    m_targets.getUnitTarget()->GetPartyMember(unitList, radius);
                break;
            case TARGET_ENUM_UNITS_PARTY_WITHIN_CASTER_RANGE:
                m_caster->GetPartyMember(unitList, radius);
                break;
            case TARGET_ENUM_UNITS_RAID_WITHIN_CASTER_RANGE:
                m_caster->GetRaidMember(unitList, radius);
                break;
            case TARGET_UNIT_RAID_AND_CLASS:
            {
                Player* targetPlayer = m_targets.getUnitTarget() && m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER
                    ? (Player*)m_targets.getUnitTarget() : NULL;

                Group* pGroup = targetPlayer ? targetPlayer->GetGroup() : NULL;
                if (pGroup)
                {
                    for (GroupReference *itr = pGroup->GetFirstMember(); itr != NULL; itr = itr->next())
                    {
                        if (Player* Target = itr->getSource())
                        {
                            // IsHostileTo check duel and controlled by enemy
                            if (targetPlayer->IsWithinDistInMap(Target, radius) &&
                                targetPlayer->GetClass() == Target->GetClass() &&
                                !m_caster->IsHostileTo(Target))
                            {
                                AddUnitTarget(Target, i);
                            }
                            if (Pet *pet = Target->GetPet())
                                if (targetPlayer->IsWithinDistInMap(pet, radius) && !m_caster->IsHostileTo(pet))
                                {
                                    if (targetPlayer->GetClass() == CLASS_WARRIOR && (Target->GetClass() == CLASS_HUNTER || pet->GetEntry() == 17252))
                                        AddUnitTarget(pet, i);
                                    else if (targetPlayer->GetClass() == CLASS_WARLOCK && Target->GetClass() == CLASS_WARLOCK && pet->GetEntry() != 17252)
                                        AddUnitTarget(pet, i);
                                }
                        }
                    }
                }
                else if (m_targets.getUnitTarget())
                    AddUnitTarget(m_targets.getUnitTarget(), i);
                break;
            }
        }

        sScriptMgr.OnSpellSetTargetMap(m_caster, unitList, m_targets, GetSpellEntry(), i);
        if (!unitList.empty())
        {

            if (GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_CANT_TARGET_SELF)
                unitList.remove_if(MaNGOS::ObjectGUIDCheck(m_caster->GetGUID()));
            //SPELL_CHECK_TARGETS_STUFF
            switch (GetSpellEntry()->Id)
            {
                case 37433:
                {
                    for (std::list<Unit*>::iterator itr = unitList.begin(); itr != unitList.end();)
                    {
                        if ((*itr)->GetTypeId() != TYPEID_PLAYER || (*itr)->IsInWater())
                            itr = unitList.erase(itr);
                        else
                            itr++;
                    }
                    break;
                }

                case 40869:     // Fatal Attraction
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 43690));
                    break;
                case 43657:
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 44007));
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 43648));
                    break;
                case 28062:     // Positive Charge
                case 39090:
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 28059));
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 39088));
                    break;
                case 28085:     // Negative Charge
                case 39093:
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 28084));
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 39091));
                    break;
                case 44869:     // Spectral Blast
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 44867));
                    unitList.remove_if(MaNGOS::ObjectGUIDCheck(m_caster->getVictimGUID()));
                    //unitList.remove_if(MaNGOS::ObjectTypeIdCheck(TYPEID_UNIT, true));
                case 45032:     // Curse of Boundless Agony
                case 45034:
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 45032));
                    unitList.remove_if(MaNGOS::UnitAuraCheck(true, 45034));
                    break;
                case 41376:     // Spite
                case 46771:     // Flame Sear
                    unitList.remove_if(MaNGOS::ObjectGUIDCheck(m_caster->getVictimGUID()));
                    break;
                case 45248:     // Shadow Blades
                    unitList.remove_if([=](Unit* unit)->bool {return fabs(m_caster->GetPositionZ() - unit->GetPositionZ()) > 5.0; });
                    break;
                case 45785: // KJ: sinister reflection  - copy model
                    unitList.remove_if((MaNGOS::ObjectEntryCheck(25708, false)));   //allow to transform only sinister reflections instead of everything nearby
                    break;
                case 30915: // broggok poison cloud grows slowly!
                case 38463:
                {
                    Aura* orginal = m_caster->GetAura(m_triggeredByAuraSpell->Id, 0);
                    if (!orginal)
                        break;
                    uint32 radius = 15 - (orginal->GetAuraDuration() / 5000);
                    unitList.remove_if(MaNGOS::ObjectDistanceCheck(m_caster, radius, true));
                }
                default:
                    break;
            }

            if (m_spellValue->MaxAffectedTargets)
                MaNGOS::RandomResizeList(unitList, m_spellValue->MaxAffectedTargets);

            for (std::list<Unit*>::iterator itr = unitList.begin(); itr != unitList.end(); ++itr)
                AddUnitTarget(*itr, i);
        }

        if (!goList.empty())
        {
            if (m_spellValue->MaxAffectedTargets)
                MaNGOS::RandomResizeList(goList, m_spellValue->MaxAffectedTargets);

            for (std::list<GameObject*>::iterator itr = goList.begin(); itr != goList.end(); ++itr)
                AddGOTarget(*itr, i);
        }
    } // Chain or Area
}

SpellCastResult Spell::prepare(SpellCastTargets * targets, Aura* triggeredByAura)
{
    m_caster->SendCombatStats(1 << COMBAT_STATS_TEST, "spell prepare %lu", NULL, targets->getUnitTargetGUID());
    m_spellState = SPELL_STATE_PREPARING;

    if (m_CastItem)
        m_castItemGUID = m_CastItem->GetGUID();
    else
        m_castItemGUID = 0;

    m_targets = *targets;

    if (triggeredByAura)
        m_triggeredByAuraSpell = triggeredByAura->GetSpellProto();

    m_caster->GetPosition(&m_cast);
    m_possiblePos = (m_caster->ToPlayer()) ? m_caster->ToPlayer()->m_desiredPosition : m_cast;

    // create and add update event for this spell
    SpellEvent* Event = new SpellEvent(this);
    m_caster->AddEvent(Event, 1, true);

    //Prevent casting at cast another spell (ServerSide check)
    if (m_caster->IsNonMeleeSpellCast(false, true) && m_cast_count)
    {
        SendCastResult(SPELL_FAILED_SPELL_IN_PROGRESS);
        finish(false);
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    }

    if (SpellMgr::CanSpellCrit(GetSpellEntry())) // only if spell can crit
    {
        if (Player* mod_owner = m_caster->GetSpellModOwner())
        {
            // fill extra crit chance from mods
            // no mod gives pct chance for spell crit (surge of light is also taken as flat in spellmgr::loadspellcustomattr
            mod_owner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_CRITICAL_CHANCE, m_extraCrit, this);
        }
     }

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (sObjectMgr.IsPlayerSpellDisabled(GetSpellEntry()->Id))
        {
            SendCastResult(SPELL_FAILED_SPELL_UNAVAILABLE);
            finish(false);
            return SPELL_FAILED_SPELL_UNAVAILABLE;
        }
        // maybe also precalculate this one?
        m_extraCrit += m_caster->GetFloatValue(PLAYER_SPELL_CRIT_PERCENTAGE1 + GetFirstSchoolInMask(m_spellSchoolMask));
    }
    else if (m_caster->GetTypeId() == TYPEID_UNIT && ((Creature*)m_caster)->isPet())
    {
        if (sObjectMgr.IsPetSpellDisabled(GetSpellEntry()->Id))
        {
            SendCastResult(SPELL_FAILED_SPELL_UNAVAILABLE);
            finish(false);
            return SPELL_FAILED_SPELL_UNAVAILABLE;
        }
    }
    else
    {
        if (sObjectMgr.IsCreatureSpellDisabled(GetSpellEntry()->Id))
        {
            finish(false);
            return SPELL_FAILED_SPELL_UNAVAILABLE;
        }
    }
    // Fill cost data
    m_powerCost = m_CastItem ? 0 : SpellMgr::CalculatePowerCost(GetSpellEntry(), m_caster, m_spellSchoolMask, this);

    SpellCastResult result = CheckCast(true);
    if (result != SPELL_CAST_OK && !IsAutoRepeat())          //always cast autorepeat dummy for triggering
    {
        if (triggeredByAura)
        {
            SendChannelUpdate(0);
            triggeredByAura->SetAuraDuration(0);
        }
        SendCastResult(result);
        finish(false);
        return result;
    }

    // Prepare data for triggers
    prepareDataForTriggerSystem();

    // calculate cast time (calculated after first CheckCast check to prevent charge counting for first CheckCast fail)
    m_casttime = SpellMgr::GetSpellCastTime(GetSpellEntry(), this);

    // HACK for instant opening of Spectral Blast Portal
    if (GetSpellEntry()->Id == 3365)
    {
        if (m_targets.getGOTarget() && m_targets.getGOTarget()->GetEntry() == 187055)
            m_casttime = 0;
    }

    if (GetCastTime() && !SpellMgr::IsChanneledSpell(GetSpellEntry()) ?
        GetSpellEntry()->InterruptFlags & SPELL_INTERRUPT_FLAG_MOVEMENT :
        GetSpellEntry()->ChannelInterruptFlags & AURA_INTERRUPT_FLAG_MOVE)
    {
        // controlled state is delivered from idle movement so should be sufficient
        m_caster->addUnitState(UNIT_STAT_CASTING_NOT_MOVE);
        m_caster->GetUnitStateMgr().PushAction(UNIT_ACTION_CONTROLLED);
    }

    // set timer base at cast time
    ReSetTimer();

    if (GetSpellEntry()->AttributesCu & SPELL_ATRR_CU_LINK_PRECAST)
        if (const std::vector<int32> *spell_triggered = sSpellMgr.GetSpellLinked(GetSpellEntry()->Id + SPELL_LINK_PRECAST))
            for (std::vector<int32>::const_iterator i = spell_triggered->begin(); i != spell_triggered->end(); ++i)
                m_caster->CastSpell(m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster, *i, true);

    if (IsTriggeredSpell())
        cast(true);
    else
    {
        // test for #541, ranged spells should be delayed by autoshot
        if (IsRangedSpell() && m_caster->getAttackTimer(RANGED_ATTACK) && m_caster->getAttackTimer(RANGED_ATTACK) < 500 && m_casttime)
        {
            m_autocastDelayTimer.Reset(m_caster->getAttackTimer(RANGED_ATTACK));
            m_timer.Delay(m_caster->getAttackTimer(RANGED_ATTACK));

            m_caster->SetCurrentCastSpell(this);
            m_selfContainer = &(m_caster->m_currentSpells[GetCurrentContainer()]);
            TriggerGlobalCooldown();
            return SPELL_CAST_OK;
        }

        // stealth must be removed at cast starting (at show channel bar)
        // skip triggered spell (item equip spell casting and other not explicit character casts/item uses)
        if (SpellMgr::isSpellBreakStealth(GetSpellEntry()))
            m_caster->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_CAST);

        m_caster->SetCurrentCastSpell(this);
        m_selfContainer = &(m_caster->m_currentSpells[GetCurrentContainer()]);
        
        SendSpellStart();

        TriggerGlobalCooldown();

        if (!m_casttime /*&& !GetSpellEntry()->StartRecoveryTime*/
            && !m_castItemGUID     //item: first cast may destroy item and second cast causes crash
            && GetCurrentContainer() == CURRENT_GENERIC_SPELL)
            cast(true);
    }

    return SPELL_CAST_OK;
}

void Spell::cancel(SpellCastResult reason)
{
    if (m_spellState == SPELL_STATE_FINISHED)
        return;

    uint32 oldState = m_spellState;
    m_spellState = SPELL_STATE_FINISHED;

    m_autoRepeat = false;
    if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
        m_caster->SendCombatStats(1 << COMBAT_STATS_CHANNEL_UPDATE, "Channeled spell %u interrupt %u", NULL, GetSpellEntry()->Id, reason);
    switch (oldState)
    {
        case SPELL_STATE_PREPARING:
            CancelGlobalCooldown();
        case SPELL_STATE_DELAYED:
        {
            SendInterrupted(0);
            SendCastResult(reason);
            break;
        }
        case SPELL_STATE_CASTING:
        {
            uint64 casterGuid = m_originalCasterGUID ? m_originalCasterGUID : m_caster->GetGUID();
            for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
            {
                if (ihit->deleted)
                    continue;

                if (ihit->missCondition == SPELL_MISS_NONE)
                {
                    Unit* unit = casterGuid == (*ihit).targetGUID ? m_caster : m_caster->GetMap()->GetUnit(ihit->targetGUID);
                    if (unit && unit->IsAlive())
                        unit->RemoveAurasByCasterSpell(GetSpellEntry()->Id, casterGuid);
                }

            }
            
            m_caster->RemoveAurasByCasterSpell(GetSpellEntry()->Id, casterGuid);
            SendChannelUpdate(0);

            if (!SpellMgr::IsChanneledSpell(GetSpellEntry()))
            {
                SendInterrupted(0);
                SendCastResult(reason);
            }
            break;
        }
        default:
            break;
    }

    SetReferencedFromCurrent(false);
    if (m_selfContainer && *m_selfContainer == this)
        *m_selfContainer = NULL;

    m_caster->RemoveDynObject(GetSpellEntry()->Id);
    m_caster->RemoveGameObject(GetSpellEntry()->Id, true);

    //set state back so finish will be processed
    m_spellState = oldState;

    finish(false);
}

void Spell::cast(bool skipCheck)
{
    // what the fuck is done here? o.O
    SpellEntry const* spellInfo = sSpellStore.LookupEntry(GetSpellEntry()->Id);
    if (!spellInfo)
        return;

    // update pointers base at GUIDs to prevent access to non-existed already object
    if (!UpdatePointers()) 
    { 
        // finish the spell if UpdatePointers() returned false, something wrong happened there 
        finish(false); 
        return;
    }

    if (!IsTriggeredSpell()) // reflective shields, molten armor etc. are not checked for it. They should ignore stealth. (as of reflective shields ignore stealth 100% sure)
    {
        if (Unit *pTarget = m_targets.getUnitTarget())
        {
            if (pTarget->IsAlive() && (pTarget->HasAuraType(SPELL_AURA_MOD_STEALTH) || pTarget->HasAuraType(SPELL_AURA_MOD_INVISIBILITY)) && !pTarget->IsFriendlyTo(m_caster) && !pTarget->isVisibleForOrDetect(m_caster, m_caster, true))
            {
                SendCastResult(SPELL_FAILED_BAD_TARGETS);
                finish(false);
                return;
            }
        }
    }

    if (m_casttime && m_caster->GetTypeId() == TYPEID_UNIT && ((Creature *)m_caster)->isPet())
    {
        if (((Creature *)m_caster)->IsAIEnabled && ((PetAI *)((Creature *)m_caster)->AI())->targetHasInterruptableAura(m_targets.getUnitTarget()))
        {
            finish(false);
            return;
        }
    }

    SetExecutedCurrently(true);
    SpellCastResult castResult = SPELL_CAST_OK;

    // cancel at lost main target unit
    if (!m_targets.getUnitTarget() && m_targets.getUnitTargetGUID() && m_targets.getUnitTargetGUID() != m_caster->GetGUID())
    {
        cancel(SPELL_FAILED_INT_LOST_TARGET);
        SetExecutedCurrently(false);
        return;
    }

    if (m_caster->GetTypeId() != TYPEID_PLAYER && m_targets.getUnitTarget() && m_targets.getUnitTarget() != m_caster && m_targets.getUnitTargetGUID() != 0)
        m_caster->SetInFront(m_targets.getUnitTarget());

    if (!IsTriggeredSpell() || IsAutoShootSpell())
    {
        castResult = CheckPower();
        if (castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            SetExecutedCurrently(false);
            if (castResult == SPELL_FAILED_NO_POWER && IsNextMeleeSwingSpell())
            {
                // Spell::cast is called from AttackerStateUpdate and normal atack is not executed after that
                // if you ran out of power, do normal white attack, extra=true so we wont get in loop
                // this fixes for example warriors heroic strike

                m_caster->AttackerStateUpdate(m_caster->GetVictim(), BASE_ATTACK, true);
            }

            return;
        }
    }

    // triggered cast called from Spell::prepare where it was already checked
    if (!skipCheck)
    {
        castResult = CheckCast(false);
        if (castResult != SPELL_CAST_OK)
        {
            SendCastResult(castResult);
            finish(false);
            SetExecutedCurrently(false);

            return;
        }
    }

    // do NOT fill again spell target map if there are already some targets defined
    if (m_UniqueTargetInfo.empty())
        FillTargetMap();

    // check for reflects
    if (m_canReflect)
        CheckForReflects();

    if (m_spellState == SPELL_STATE_FINISHED)                // stop cast if spell marked as finish somewhere in Take*/FillTargetMap
    {
        SetExecutedCurrently(false);
        return;
    }

    // traded items have trade slot instead of guid in m_itemTargetGUID
    // set to real guid to be sent later to the client
    m_targets.updateTradeSlotItem();

    if (!IsTriggeredSpell() || IsAutoShootSpell())
    {
        //TakePower();
        TakeReagents();                                         // we must remove reagents before HandleEffects to allow place crafted item in same slot
    }

    // CAST SPELL
    SendSpellCooldown();
    //SendCastResult(castResult);
    SendSpellGo();                                          // we must send smsg_spell_go packet before m_castItem delete in TakeCastItem()...

    if (spellInfo->AttributesCu & SPELL_ATTR_CU_DIRECT_DAMAGE)
        CalculateDamageDoneForAllTargets();

    //handle SPELL_AURA_ADD_TARGET_TRIGGER auras
    //are there any spells need to be triggered after hit?
    Unit::AuraList const& targetTriggers = m_caster->GetAurasByType(SPELL_AURA_ADD_TARGET_TRIGGER);
    for (Unit::AuraList::const_iterator i = targetTriggers.begin(); i != targetTriggers.end(); ++i)
    {
        SpellEntry const *auraSpellEntry = (*i)->GetSpellProto();
        uint32 auraSpellIdx = (*i)->GetEffIndex();
        if (IsAffectedBy(auraSpellEntry, auraSpellIdx))
        {
            if (SpellEntry const *spellInfo = sSpellStore.LookupEntry(auraSpellEntry->EffectTriggerSpell[auraSpellIdx]))
            {
                // Calculate chance at that moment (can be depend for example from combo points)
                int32 chance = m_caster->CalculateSpellDamage(auraSpellEntry, auraSpellIdx, (*i)->GetBasePoints(), NULL);
                m_ChanceTriggerSpells.push_back(std::make_pair(spellInfo, chance * (*i)->GetStackAmount()));
            }
        }
    }

    if (spellInfo->AttributesCu & SPELL_ATTR_CU_CHARGE)
    {
        if (spellInfo->Effect[0] == SPELL_EFFECT_CHARGE2) //swoop is always first effect
            EffectCharge2(0);
        else
            EffectCharge(0);
    }

    // Okay, everything is prepared. Now we need to distinguish between immediate and evented delayed spells
    if (IsDelayedSpell() && !SpellMgr::IsChanneledSpell(GetSpellEntry()))
    {
        // Remove used for cast item if need (it can be already NULL after TakeReagents call
        // in case delayed spell remove item at cast delay start
        TakeCastItem();

        // Okay, maps created, now prepare flags
        m_immediateHandled = false;
        m_spellState = SPELL_STATE_DELAYED;
        SetDelayStart(0);

        m_caster->ProcDamageAndSpell(m_targets.getUnitTarget(), m_procCastEnd, PROC_FLAG_NONE, PROC_EX_EX_TRIGGER_ALWAYS, 1, BASE_ATTACK, GetSpellEntry());
    }
    else
        // Immediate spell, no big deal
        handle_immediate();

    // combo points should not be taken before SPELL_AURA_ADD_TARGET_TRIGGER auras are handled
    if (!IsTriggeredSpell() || IsAutoShootSpell())
        TakePower();

    if (spellInfo->AttributesCu & SPELL_ATTR_CU_LINK_CAST)
    {
        if (const std::vector<int32> *spell_triggered = sSpellMgr.GetSpellLinked(spellInfo->Id))
            for (std::vector<int32>::const_iterator i = spell_triggered->begin(); i != spell_triggered->end(); ++i)
                if (*i < 0)
                    m_caster->RemoveAurasDueToSpell(-(*i));
                else
                    m_caster->CastSpell(m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster, *i, true);
    }

    SetExecutedCurrently(false);

    if (m_caster->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsChanneledSpell(GetSpellEntry()))
        ((Player*)m_caster)->RemoveSpellMods(this);
}

void Spell::handle_immediate()
{
    if (GetSpellEntry()->Id <= 0 || GetSpellEntry()->Id > MAX_SPELL_ID || GetSpellEntry()->Id == 32 || GetSpellEntry()->Id == 48 || GetSpellEntry()->Id == 576 || GetSpellEntry()->Id == 80 || GetSpellEntry()->Id == 160)
        return;

    SpellEntry const* spellInfo = sSpellStore.LookupEntry(GetSpellEntry()->Id);

    if (!spellInfo)
        return;

    // start channeling if applicable
    if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
    {

        int32 duration = SpellMgr::GetSpellDuration(GetSpellEntry());

        
        if (duration)
        {
            //apply haste mods
            m_caster->ModSpellCastTime(GetSpellEntry(), duration, this);
            // Apply duration mod
            if (Player* modOwner = m_caster->GetSpellModOwner())
                modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_DURATION, duration);

            // channels in pvp duration
            if (m_UniqueTargetInfo.size())
            {
                Unit* unittarget = m_caster->GetUnit(m_UniqueTargetInfo.begin()->targetGUID);
                Player* casterpl = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
                Player* targetpl = unittarget ? unittarget->GetCharmerOrOwnerPlayerOrPlayerItself() : NULL;
                if (duration > 10000 && casterpl && targetpl && casterpl->IsHostileTo(targetpl))
                    duration = 10000;

                if (unittarget && !SpellMgr::IsPositiveSpell(spellInfo->Id) && unittarget != m_caster)
                {
                    DiminishingGroup dg = SpellMgr::GetDiminishingReturnsGroupForSpell(GetSpellEntry(), m_triggeredByAuraSpell);
                    unittarget->ApplyDiminishingToDuration(dg, duration, unittarget->GetDiminishing(dg), spellInfo);
                }
            }
            m_spellState = SPELL_STATE_CASTING;
            m_caster->AddInterruptMask(GetSpellEntry()->ChannelInterruptFlags);
        }

        if (m_caster->GetTypeId() == TYPEID_PLAYER)
            ((Player*)m_caster)->RemoveSpellMods(this);
        SendChannelStart(duration);
    }

    // process immediate effects (items, ground, etc.) also initialize some variables
    _handle_immediate_phase();

    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (m_destroyed == true || ihit == m_UniqueTargetInfo.end() || m_UniqueTargetInfo.size() == 0)
            break;

        if (ihit->deleted)
            continue;

        DoAllEffectOnTarget(&(*ihit));
    }

    for (std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
    {
        if (m_destroyed == true || ihit == m_UniqueGOTargetInfo.end() || m_UniqueGOTargetInfo.size() == 0)
            break;

        if (ihit->deleted)
            continue;

        DoAllEffectOnTarget(&(*ihit));
    }

    // spell is finished, perform some last features of the spell here
    _handle_finish_phase();

    // Remove used for cast item if need (it can be already NULL after TakeReagents call
    TakeCastItem();

    if (m_spellState != SPELL_STATE_CASTING)
        finish(true);                                       // successfully finish spell cast (not last in case autorepeat or channel spell)
}

uint64 Spell::handle_delayed(uint64 t_offset)
{
    UpdatePointers();

    uint64 next_time = 0;

    if (!m_immediateHandled)
    {
        _handle_immediate_phase();
        m_immediateHandled = true;
    }
    
    bool single_missile = (m_targets.m_targetMask & TARGET_FLAG_DEST_LOCATION);

    // now recheck units targeting correctness (need before any effects apply to prevent adding immunity at first effect not allow apply second spell effect and similar cases)
    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (ihit->processed == false)
        {
            if (single_missile || ihit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ihit));
            else if (next_time == 0 || ihit->timeDelay < next_time)
                next_time = ihit->timeDelay;
        }
    }

    // now recheck gameobject targeting correctness
    for (std::list<GOTargetInfo>::iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
    {
        if (ighit->deleted)
            continue;

        if (ighit->processed == false)
        {
            if (single_missile || ighit->timeDelay <= t_offset)
                DoAllEffectOnTarget(&(*ighit));
            else if (next_time == 0 || ighit->timeDelay < next_time)
                next_time = ighit->timeDelay;
        }
    }
    // All targets passed - need finish phase
    if (next_time == 0)
    {
        // spell is finished, perform some last features of the spell here
        _handle_finish_phase();

        finish(true);                                       // successfully finish spell cast

        // return zero, spell is finished now
        return 0;
    }
    else
    {
        m_canTrigger = false; // multiple hiting spells (blizard, arcane misiles) should triger only on first hit
        // spell is unfinished, return next execution time
        return next_time;
    }
}

void Spell::_handle_immediate_phase()
{
    // handle some immediate features of the spell here
    HandleThreatSpells(GetSpellEntry()->Id);

    m_needSpellLog = IsNeedSendToClient();
    for (uint32 j = 0; j < 3; j++)
    {
        if (GetSpellEntry()->Effect[j] == 0)
            continue;

        // apply Send Event effect to ground in case empty target lists
        if (GetSpellEntry()->Effect[j] == SPELL_EFFECT_SEND_EVENT && !HaveTargetsForEffect(j))
        {
            HandleEffects(NULL, NULL, NULL, j);
            continue;
        }

        // Don't do spell log, if is school damage spell
        if (GetSpellEntry()->Effect[j] == SPELL_EFFECT_SCHOOL_DAMAGE || GetSpellEntry()->Effect[j] == 0)
            m_needSpellLog = false;
    }

    // initialize Diminishing Returns Data
    m_diminishLevel = DIMINISHING_LEVEL_1;
    m_diminishGroup = DIMINISHING_NONE;

    // process items
    for (std::list<ItemTargetInfo>::iterator ihit = m_UniqueItemInfo.begin(); ihit != m_UniqueItemInfo.end(); ++ihit)
        DoAllEffectOnTarget(&(*ihit));

    if (!m_originalCaster || GetSpellEntry()->Id == 43622)
        return;

    // process ground
    for (uint32 j = 0; j < 3; ++j)
    {
        // all those spells cant be immuned - so we dont need to check immunity for them. They are or positive or neutral
        if (sSpellMgr.EffectTargetType[GetSpellEntry()->Effect[j]] == SPELL_REQUIRE_DEST/* && GetSpellEntry()->Effect[j] != SPELL_EFFECT_TRIGGER_MISSILE*/)
        {
            if (!m_targets.HasDst()) // FIXME: this will ignore dest set in effect
                m_targets.setDestination(m_caster);
            HandleEffects(m_originalCaster, NULL, NULL, j);
        }
        else if (sSpellMgr.EffectTargetType[GetSpellEntry()->Effect[j]] == SPELL_REQUIRE_NONE)
            HandleEffects(m_originalCaster, NULL, NULL, j);
        else if (GetSpellEntry()->Id == 42339) // hallow's end missile with dummy ground effect, i dont see better way to do it now
            HandleEffects(m_originalCaster, NULL, NULL, j);
    }
}

void Spell::_handle_finish_phase()
{
    // spell log
    if (m_needSpellLog && GetSpellEntry())
        SendLogExecute();
}

void Spell::SendSpellCooldown()
{
    Player* _player = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!_player)
        return;

    // init cooldown values
    uint32 cat = 0;
    int32 rec = -1;
    int32 catrec = -1;

    // some special item spells without correct cooldown in SpellEntry
    // cooldown information stored in item prototype
    // This used in same way in WorldSession::HandleItemQuerySingleOpcode data sending to client.

    if (m_CastItem)
    {
        ItemPrototype const* proto = m_CastItem->GetProto();
        if (proto)
        {
            for (int idx = 0; idx < MAX_ITEM_PROTO_SPELLS; ++idx)
            {
                if (proto->Spells[idx].SpellId == GetSpellEntry()->Id)
                {
                    cat = proto->Spells[idx].SpellCategory;
                    rec = proto->Spells[idx].SpellCooldown;
                    catrec = proto->Spells[idx].SpellCategoryCooldown;
                    break;
                }
            }
        }
    }

    // if no cooldown found above then base at DBC data
    if (rec < 0 && catrec < 0)
    {
        cat = GetSpellEntry()->Category;
        rec = GetSpellEntry()->RecoveryTime;
        catrec = GetSpellEntry()->CategoryRecoveryTime;
    }

    // shoot spells used equipped item cooldown values already assigned in GetAttackTime(RANGED_ATTACK)
    // prevent 0 cooldowns set by another way
    if (rec <= 0 && catrec <= 0 && (cat == 76 || cat == 351))
        rec = _player->GetAttackTime(RANGED_ATTACK);

    // Now we have cooldown data (if found any), time to apply mods
    if (rec > 0)
        _player->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_COOLDOWN, rec, this);

    if (catrec > 0)
        _player->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_COOLDOWN, catrec, this);

    // replace negative cooldowns by 0
    if (rec < 0) rec = 0;
    if (catrec < 0) catrec = 0;

    // no cooldown after applying spell mods
    if (rec == 0 && catrec == 0)
        return;

    if (!rec) // only category cooldown, set for good timing
        rec = catrec;
    
    if (m_CastItem)
    {
        _player->GetCooldownMgr().AddItemCooldown(m_CastItem->GetEntry(), rec);
        SpellCategoryStore::const_iterator i_scstore = sSpellCategoryStore.find(- int32(cat));
        if (i_scstore != sSpellCategoryStore.end())
        {
            for (SpellCategorySet::const_iterator i_scset = i_scstore->second.begin(); i_scset != i_scstore->second.end(); ++i_scset)
            {
                if (*i_scset == m_CastItem->GetEntry())             // skip main spell, already handled above
                    continue;
                _player->GetCooldownMgr().AddItemCooldown(*i_scset, catrec);
            }
        }
    }
    else
    {
        _player->GetCooldownMgr().AddSpellCooldown(GetSpellEntry()->Id, rec);
        SpellCategoryStore::const_iterator i_scstore = sSpellCategoryStore.find(cat);
        if (i_scstore != sSpellCategoryStore.end())
        {
            for (SpellCategorySet::const_iterator i_scset = i_scstore->second.begin(); i_scset != i_scstore->second.end(); ++i_scset)
            {
                if (*i_scset == GetSpellEntry()->Id)             // skip main spell, already handled above
                    continue;
                _player->GetCooldownMgr().AddSpellCooldown(*i_scset, catrec);
            }
        }
    }
    
}

void Spell::update(uint32 difftime)
{
    // update pointers based at it's GUIDs
    UpdatePointers();

    if (m_targets.getUnitTargetGUID() && !m_targets.getUnitTarget())
    {
        cancel(SPELL_FAILED_INT_LOST_TARGET);
        return;
    }

    // check if caster has moved before the spell finished
    if (m_timer.GetTimeLeft() != 0 && m_caster->HasUnitState(UNIT_STAT_CASTING_NOT_MOVE)/* && !m_caster->HasUnitMovementFlag(MOVEFLAG_FALLINGFAR)*/)
    {
        // Check for movin' by(rotation keys ft. mouse button)
        //         if ((m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->isMoving() && m_casttime &&
        //             m_spellInfo->SpellFamilyFlags != SPELLFAMILY_GENERIC && !m_delayMoment))
        //             cancel();

        // add little offset for creature stop movement
        if (!IsNextMeleeSwingSpell() && !IsAutoRepeat() && !IsTriggeredSpell())
        {
            Position casterPos;
            m_caster->GetPosition(&casterPos);
            if (m_cast != casterPos && casterPos != m_possiblePos)
            {
                cancel(SPELL_FAILED_INT_CASTER_MOVED);
            }
        }
    }

    switch (m_spellState)
    {
        case SPELL_STATE_PREPARING:
        {
            if (m_autocastDelayTimer.Expired(difftime))
            {
                m_autocastDelayTimer = 0;
                if (m_caster->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL))
                    m_caster->TriggerAutocastSpell();
                SendSpellStart();
            }
            if (m_timer.Expired(difftime))
                m_timer = 0;

            if (m_timer.GetTimeLeft() == 0 && !IsNextMeleeSwingSpell() && !IsAutoRepeat())
                cast(GetSpellEntry()->CastingTimeIndex == 1);
        } break;
        case SPELL_STATE_CASTING:
        {
            if (m_timer.GetTimeLeft() > 0)
            {
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    // check if player has jumped before the channeling finished
                    if (m_caster->HasUnitState(UNIT_STAT_CASTING_NOT_MOVE) && m_caster->HasUnitMovementFlag(MOVEFLAG_FALLING))
                        cancel(SPELL_FAILED_INT_CASTER_JUMPED);
                }

                // check if there are alive targets left
                if (!IsAliveUnitPresentInTargetList())
                {
                    SendChannelUpdate(0);
                    finish();
                }

                if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_MAGE && GetSpellEntry()->SpellFamilyFlags & 0x800)
                {
                    float max_range = SpellMgr::GetSpellMaxRange(sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex));

                    if (!m_targets.getUnitTarget() || !m_caster->IsWithinDistInMap(m_targets.getUnitTarget(), max_range + 5.0))
                        cancel(SPELL_FAILED_INTERRUPTED);
                }

                if (m_timer.Expired(difftime))
                    m_timer = 0;
            }

            if (m_timer.GetInterval() == 0)
            {
                SendChannelUpdate(0);

                // channeled spell processed independently for quest targeting
                // cast at creature (or GO) quest objectives update at successful cast channel finished
                // ignore autorepeat/melee casts for speed (not exist quest for spells (hm...)
                if (!IsAutoRepeat() && !IsNextMeleeSwingSpell())
                {
                    if (Player* p = GetPlayerForCastQuestCond())
                    {
                        for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                        {
                            if (ihit->deleted)
                                continue;

                            TargetInfo* target = &*ihit;
                            if (!IS_CREATURE_GUID(target->targetGUID))
                                continue;

                            Unit* unit = m_caster->GetGUID() == target->targetGUID ? m_caster : m_caster->GetMap()->GetUnit(target->targetGUID);
                            if (!unit)
                                continue;

                            p->CastCreatureOrGO(unit->GetEntry(), unit->GetGUID(), GetSpellEntry()->Id);
                        }

                        for (std::list<GOTargetInfo>::iterator ihit = m_UniqueGOTargetInfo.begin(); ihit != m_UniqueGOTargetInfo.end(); ++ihit)
                        {
                            if (ihit->deleted)
                                continue;

                            GOTargetInfo* target = &*ihit;

                            GameObject* go = m_caster->GetMap()->GetGameObject(target->targetGUID);
                            if (!go)
                                continue;

                            p->CastCreatureOrGO(go->GetEntry(), go->GetGUID(), GetSpellEntry()->Id);
                        }
                    }
                }

                finish();
            }
        } break;
        default:
        {
        }break;
    }
}

void Spell::finish(bool ok)
{
    if (!m_caster)
        return;

    if (m_spellState == SPELL_STATE_FINISHED)
        return;

    m_spellState = SPELL_STATE_FINISHED;

    if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
        m_caster->UpdateInterruptMask();

    if (!m_caster->IsNonMeleeSpellCast(false, false, true))
    {
        if (m_caster->HasUnitState(UNIT_STAT_CASTING_NOT_MOVE))
            m_caster->GetUnitStateMgr().DropAction(UNIT_ACTION_CONTROLLED);

        m_caster->ClearUnitState(UNIT_STAT_CASTING | UNIT_STAT_CASTING_NOT_MOVE);
    }

    if (!ok)
    {
        //restore spell mods
        if (m_caster->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsChanneledSpell(GetSpellEntry()))
            ((Player*)m_caster)->RestoreSpellMods(this);
        return;
    }
    // other code related only to successfully finished spells

    // remove spell mods
    // if (m_caster->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsChanneledSpell(GetSpellEntry()))
    //    ((Player*)m_caster)->RemoveSpellMods(this);

    // Okay to remove extra attacks
    if (GetSpellEntry()->HasEffect(SPELL_EFFECT_ADD_EXTRA_ATTACKS))
        m_caster->m_extraAttacks = 0;

    // Heal caster for all health leech from all targets
    if (m_healthLeech)
    {
        m_caster->ModifyHealth(m_healthLeech);
        m_caster->SendHealSpellLog(m_caster, GetSpellEntry()->Id, uint32(m_healthLeech));
    }

    if (IsMeleeAttackResetSpell())
    {
        m_caster->resetAttackTimer(BASE_ATTACK);
        if (m_caster->haveOffhandWeapon())
            m_caster->resetAttackTimer(OFF_ATTACK);

        if (!(GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_NOT_RESET_AUTOSHOT))
            m_caster->resetAttackTimer(RANGED_ATTACK);
    }

    // call triggered spell only at successful cast (after clear combo points -> for add some if need)
    // I assume what he means is that some triggered spells may add combo points
    if (!m_TriggerSpells.empty())
        TriggerSpell();

    // Stop Attack for some spells
    if (GetSpellEntry()->Attributes & SPELL_ATTR_STOP_ATTACK_TARGET)
        m_caster->AttackStop();
}

void Spell::SendCastResult(SpellCastResult result)
{
    if (m_caster->GetTypeId() == TYPEID_UNIT && m_caster->IsAIEnabled)
    {
        if (m_caster->IsInWorld())
            m_caster->ToCreature()->AI()->SendDebug("Spell cast result of spell %u : %u",GetSpellEntry()->Id,uint8(result));
        return;
    }
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    if (((Player*)m_caster)->GetSession()->PlayerLoading())  // don't send cast results at loading time
        return;

    if (result == SPELL_CAST_OK)
    {
        WorldPacket data(SMSG_CLEAR_EXTRA_AURA_INFO, (8 + 4));
        data << m_caster->GetPackGUID();
        data << uint32(GetSpellEntry()->Id);
        ((Player*)m_caster)->SendPacketToSelf(&data);
        return;
    }
    
    m_caster->SendCombatStats(1 << COMBAT_STATS_FAILED_CAST, "Cast %u failed, result %u", NULL, GetSpellEntry()->Id, result);
    
    if (result >= SPELL_FAILED_DEBUG_START && result <= SPELL_FAILED_DEBUG_END)
        result = SPELL_FAILED_INTERRUPTED;

    WorldPacket data(SMSG_CAST_FAILED, (4 + 1 + 1));
    data << uint32(GetSpellEntry()->Id);
    data << uint8(result);                              // problem
    data << uint8(m_cast_count);                        // single cast or multi 2.3 (0/1)
    switch (result)
    {
        case SPELL_FAILED_REQUIRES_SPELL_FOCUS:
            data << uint32(GetSpellEntry()->RequiresSpellFocus);
            break;
        case SPELL_FAILED_REQUIRES_AREA:
            // hardcode areas limitation case
            if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_POTION && sSpellMgr.GetSpellElixirMask(GetSpellEntry()->Id) & ELIXIR_UNSTABLE_MASK)
            {
                // blade edge elixirs 
                data << uint32(3522);
                break;
            }

            switch (GetSpellEntry()->Id)
            {
                case 41617:                             // Cenarion Mana Salve
                case 41619:                             // Cenarion Healing Salve
                    data << uint32(3905);
                    break;
                case 41618:                             // Bottled Nethergon Energy
                case 41620:                             // Bottled Nethergon Vapor
                    data << uint32(3842);
                    break;
                case 45373:                             // Bloodberry Elixir
                    data << uint32(4075);
                    break;
                case 40817:                             // summon banishing portal
                    data << uint32(3784);
                    break;
                default:                                // default case
                    data << uint32(GetSpellEntry()->AreaId);
                    break;
            }
            break;
        case SPELL_FAILED_TOTEMS:
            if (GetSpellEntry()->Totem[0])
                data << uint32(GetSpellEntry()->Totem[0]);
            if (GetSpellEntry()->Totem[1])
                data << uint32(GetSpellEntry()->Totem[1]);
            break;
        case SPELL_FAILED_TOTEM_CATEGORY:
            if (GetSpellEntry()->TotemCategory[0])
                data << uint32(GetSpellEntry()->TotemCategory[0]);
            if (GetSpellEntry()->TotemCategory[1])
                data << uint32(GetSpellEntry()->TotemCategory[1]);
            break;
        case SPELL_FAILED_EQUIPPED_ITEM_CLASS:
            data << uint32(GetSpellEntry()->EquippedItemClass);
            data << uint32(GetSpellEntry()->EquippedItemSubClassMask);
            data << uint32(GetSpellEntry()->EquippedItemInventoryTypeMask);
            break;
    }
    ((Player*)m_caster)->SendPacketToSelf(&data);


    // this is hack for spells like shadowform, client should prevent player from casting this when not ready
    // but if it fails client will show spell icon like inactive.
    if (result == SPELL_FAILED_NOT_READY && GetSpellEntry()->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE)
    {
        WorldPacket data2(SMSG_COOLDOWN_EVENT, (4 + 8));
        data2 << GetSpellEntry()->Id;
        data2 << m_caster->GetGUID();
        m_caster->ToPlayer()->SendPacketToSelf(&data2);
    }
}

void Spell::SendSpellStart()
{
    if (!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_START id=%u", GetSpellEntry()->Id);

    uint32 castFlags = CAST_FLAG_UNKNOWN2;
    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;

    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    WorldPacket data(SMSG_SPELL_START, (8 + 8 + 4 + 4 + 2));
    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << m_caster->GetPackGUID();

    data << m_caster->GetPackGUID();
    data << uint32(GetSpellEntry()->Id);;                       // spellId
    data << uint8(m_cast_count);                            // pending spell cast?
    data << uint16(castFlags);                              // cast flags
    data << uint32(m_timer.GetTimeLeft());                                // delay?
    data << m_targets;

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    m_caster->BroadcastPacket(&data, true);
}

void Spell::SendSpellGo()
{
    // not send invisible spell casting
    if (!IsNeedSendToClient())
        return;

    sLog.outDebug("Sending SMSG_SPELL_GO id=%u", GetSpellEntry()->Id);
    m_caster->SendCombatStats(1 << COMBAT_STATS_FAILED_CAST, "Spell %u go! target lowguid %u", NULL, GetSpellEntry()->Id, GUID_LOPART(m_targets.getUnitTargetGUID()));

    uint32 castFlags = CAST_FLAG_UNKNOWN9;

    // triggered spells with spell visual != 0 and not auto shot
    if ((IsTriggeredSpell() && (GetSpellEntry()->AttributesEx4 & SPELL_ATTR_EX4_AUTOSHOT) == 0) || m_triggeredByAuraSpell)
        castFlags |= CAST_FLAG_HIDDEN_COMBATLOG;

    if (IsRangedSpell())
        castFlags |= CAST_FLAG_AMMO;                        // arrows/bullets visual

    WorldPacket data(SMSG_SPELL_GO, 50);                    // guess size
    if (m_CastItem)
        data << m_CastItem->GetPackGUID();
    else
        data << m_caster->GetPackGUID();

    data << m_caster->GetPackGUID();
    data << uint32(GetSpellEntry()->Id);                        // spellId
    data << uint16(castFlags);                              // cast flags
    data << uint32(WorldTimer::getMSTime());                // timestamp

    WriteSpellGoTargets(&data);

    data << m_targets;

    if (castFlags & CAST_FLAG_AMMO)                         // projectile info
        WriteAmmoToPacket(&data);

    m_caster->BroadcastPacket(&data, true);
}

void Spell::WriteAmmoToPacket(WorldPacket * data)
{
    uint32 ammoInventoryType = 0;
    uint32 ammoDisplayID = 0;

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(RANGED_ATTACK);
        if (pItem)
        {
            ammoInventoryType = pItem->GetProto()->InventoryType;
            if (ammoInventoryType == INVTYPE_THROWN)
                ammoDisplayID = pItem->GetProto()->DisplayInfoID;
            else
            {
                uint32 ammoID = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                if (ammoID)
                {
                    ItemPrototype const *pProto = ObjectMgr::GetItemPrototype(ammoID);
                    if (pProto)
                    {
                        ammoDisplayID = pProto->DisplayInfoID;
                        ammoInventoryType = pProto->InventoryType;
                    }
                }
                else if (m_caster->GetDummyAura(46699))      // Requires No Ammo
                {
                    ammoDisplayID = 5996;                   // normal arrow
                    ammoInventoryType = INVTYPE_AMMO;
                }
            }
        }
    }
    // TODO: implement selection ammo data based at ranged weapon stored in equipmodel/equipinfo/equipslot fields

    *data << uint32(ammoDisplayID);
    *data << uint32(ammoInventoryType);
}

void Spell::WriteSpellGoTargets(WorldPacket * data)
{
    *data << (uint8)m_countOfHit;
    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if ((*ihit).missCondition == SPELL_MISS_NONE)       // Add only hits
        {
            *data << uint64(ihit->targetGUID);
            m_needAliveTargetMask |= ihit->effectMask;
        }
    }

    for (std::list<GOTargetInfo>::iterator ighit = m_UniqueGOTargetInfo.begin(); ighit != m_UniqueGOTargetInfo.end(); ++ighit)
    {
        if (ighit->deleted)
            continue;

        *data << uint64(ighit->targetGUID);                 // Always hits
    }

    *data << (uint8)m_countOfMiss;
    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (ihit->missCondition != SPELL_MISS_NONE)        // Add only miss
        {
            *data << uint64(ihit->targetGUID);
            *data << uint8(ihit->missCondition);
            if (ihit->missCondition == SPELL_MISS_REFLECT)
                *data << uint8(ihit->reflectResult);
        }
    }

    // Reset m_needAliveTargetMask for non channeled spell
    if (!SpellMgr::IsChanneledSpell(GetSpellEntry()))
        m_needAliveTargetMask = 0;
}

void Spell::SendLogExecute()
{
    Unit *target = m_targets.getUnitTarget() ? m_targets.getUnitTarget() : m_caster;

    if (!target || !target->IsInWorld())
        return;

    WorldPacket data(SMSG_SPELLLOGEXECUTE, (8 + 4 + 4 + 4 + 4 + 8));

    if (m_caster->GetTypeId() == TYPEID_PLAYER)
        data << m_caster->GetPackGUID();
    else
        data << target->GetPackGUID();

    data << uint32(GetSpellEntry()->Id);
    uint32 count1 = 1;
    data << uint32(count1);                                 // count1 (effect count?)
    for (uint32 i = 0; i < count1; ++i)
    {
        data << uint32(GetSpellEntry()->Effect[0]);             // spell effect
        uint32 count2 = 1;
        data << uint32(count2);                             // count2 (target count?)
        for (uint32 j = 0; j < count2; ++j)
        {
            switch (GetSpellEntry()->Effect[0])
            {
                case SPELL_EFFECT_POWER_DRAIN:
                    if (Unit *unit = m_targets.getUnitTarget())
                        data << unit->GetPackGUID();
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    data << float(0);
                    break;
                case SPELL_EFFECT_ADD_EXTRA_ATTACKS:
                    if (Unit *unit = m_targets.getUnitTarget())
                        data << unit->GetPackGUID();
                    else
                        data << uint8(0);
                    data << uint32(m_caster->m_extraAttacks);
                    break;
                case SPELL_EFFECT_DURABILITY_DAMAGE:
                    if (Unit *unit = m_targets.getUnitTarget())
                        data << unit->GetPackGUID();
                    else
                        data << uint8(0);
                    data << uint32(0);
                    data << uint32(0);
                    break;
                case SPELL_EFFECT_OPEN_LOCK:
                case SPELL_EFFECT_OPEN_LOCK_ITEM:
                    if (Item *item = m_targets.getItemTarget())
                        data << item->GetPackGUID();
                    else
                        data << uint8(0);
                    break;
                case SPELL_EFFECT_CREATE_ITEM:
                    data << uint32(GetSpellEntry()->EffectItemType[0]);
                    break;
                case SPELL_EFFECT_SUMMON:
                case SPELL_EFFECT_SUMMON_WILD:
                case SPELL_EFFECT_SUMMON_GUARDIAN:
                case SPELL_EFFECT_TRANS_DOOR:
                case SPELL_EFFECT_SUMMON_PET:
                case SPELL_EFFECT_SUMMON_POSSESSED:
                case SPELL_EFFECT_SUMMON_TOTEM:
                case SPELL_EFFECT_SUMMON_OBJECT_WILD:
                case SPELL_EFFECT_CREATE_HOUSE:
                case SPELL_EFFECT_DUEL:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT1:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT2:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT3:
                case SPELL_EFFECT_SUMMON_TOTEM_SLOT4:
                case SPELL_EFFECT_SUMMON_PHANTASM:
                case SPELL_EFFECT_SUMMON_CRITTER:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT1:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT2:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT3:
                case SPELL_EFFECT_SUMMON_OBJECT_SLOT4:
                case SPELL_EFFECT_SUMMON_DEMON:
                case SPELL_EFFECT_150:
                    if (Unit *unit = m_targets.getUnitTarget())
                        data << unit->GetPackGUID();
                    else if (m_targets.getItemTargetGUID())
                        data.appendPackGUID(m_targets.getItemTargetGUID());
                    else if (GameObject *go = m_targets.getGOTarget())
                        data << go->GetPackGUID();
                    else
                        data << uint8(0);                   // guid
                    break;
                case SPELL_EFFECT_FEED_PET:
                    data << uint32(m_targets.getItemTargetEntry());
                    break;
                case SPELL_EFFECT_DISMISS_PET:
                    if (Unit *unit = m_targets.getUnitTarget())
                        data << unit->GetPackGUID();
                    else
                        data << uint8(0);
                    break;
                default:
                    return;
            }
        }
    }

    m_caster->BroadcastPacket(&data, true);
}

void Spell::SendInterrupted(uint8 result)
{
    WorldPacket data(SMSG_SPELL_FAILURE, (8 + 4 + 1));
    data << m_caster->GetPackGUID();
    data << GetSpellEntry()->Id;
    data << result;
    m_caster->BroadcastPacket(&data, true);

    data.Initialize(SMSG_SPELL_FAILED_OTHER, (8 + 4));
    data << m_caster->GetPackGUID();
    data << GetSpellEntry()->Id;
    m_caster->BroadcastPacket(&data, true);
}

void Spell::SendChannelUpdate(uint32 time)
{
    if (time == 0)
    {
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT, 0);
        m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, 0);
        m_caster->SendCombatStats(1<<COMBAT_STATS_CHANNEL_UPDATE, "Channeled spell end (%u)", NULL, GetSpellEntry()->Id);
    }

    WorldPacket data(MSG_CHANNEL_UPDATE, 8 + 4);
    data << m_caster->GetPackGUID();
    data << uint32(time);

    m_caster->BroadcastPacket(&data, true);
}

void Spell::SendChannelStart(uint32 duration)
{
    WorldObject* target = NULL;

    // select first not resisted target from target list for _0_ effect
    if (!m_UniqueTargetInfo.empty())
    {
        for (std::list<TargetInfo>::iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
        {
            if (itr->deleted)
                continue;

            if ((itr->effectMask & (1 << 0)) && itr->reflectResult == SPELL_MISS_NONE && itr->targetGUID != m_caster->GetGUID())
            {
                target = m_caster->GetMap()->GetUnit(itr->targetGUID);
                break;
            }
        }
    }
    else if (!m_UniqueGOTargetInfo.empty())
    {
        for (std::list<GOTargetInfo>::iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
        {
            if (itr->deleted)
                continue;

            if (itr->effectMask & (1 << 0))
            {
                target = m_caster->GetMap()->GetGameObject(itr->targetGUID);
                break;
            }
        }
    }

    WorldPacket data(MSG_CHANNEL_START, (8 + 4 + 4));
    data << m_caster->GetPackGUID();
    data << uint32(GetSpellEntry()->Id);
    data << uint32(duration);
    m_caster->SendCombatStats(1<<COMBAT_STATS_CHANNEL_UPDATE, "Channeled spell start (%u, %u)", NULL, GetSpellEntry()->Id, duration);
    m_caster->BroadcastPacket(&data, true);

    m_timer = duration;
    if (target)
        m_caster->SetUInt64Value(UNIT_FIELD_CHANNEL_OBJECT, target->GetGUID());
    m_caster->SetUInt32Value(UNIT_CHANNEL_SPELL, GetSpellEntry()->Id);
}

void Spell::SendResurrectRequest(Player* target)
{
    // Both players and NPCs can resurrect using spells - have a look at creature 28487 for example
    // However, the packet structure differs slightly

    const char* sentName = m_caster->GetTypeId() == TYPEID_PLAYER ? "" : m_caster->GetName();

    WorldPacket data(SMSG_RESURRECT_REQUEST, (8 + 4 + strlen(sentName) + 1 + 1 + 1));
    data << uint64(m_caster->GetGUID());
    data << uint32(strlen(sentName) + 1);

    data << sentName;
    data << uint8(0);

    data << uint8(m_caster->GetTypeId() == TYPEID_PLAYER ? 0 : 1);

    if (GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_IGNORE_RESURRECTION_TIMER)
        data << uint32(0);

    target->SendPacketToSelf(&data);
}

void Spell::SendPlaySpellVisual(uint32 SpellID)
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    WorldPacket data(SMSG_PLAY_SPELL_VISUAL, 12);
    data << uint64(m_caster->GetGUID());
    data << uint32(SpellID);                                // spell visual id?
    ((Player*)m_caster)->SendPacketToSelf(&data);
}

void Spell::TakeCastItem()
{
    if (!m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    // not remove cast item at triggered spell (equipping, weapon damage, etc)
    if (IsTriggeredSpell())
        return;

    ItemPrototype const *proto = m_CastItem->GetProto();

    if (!proto)
    {
        // This code is to avoid a crash
        // I'm not sure, if this is really an error, but I guess every item needs a prototype
        sLog.outLog(LOG_DEFAULT, "ERROR: Cast item has no item prototype highId=%d, lowId=%d", m_CastItem->GetGUIDHigh(), m_CastItem->GetGUIDLow());
        return;
    }

    bool expendable = false;
    bool withoutCharges = false;

    for (int i = 0; i < MAX_ITEM_PROTO_SPELLS; ++i)
    {
        if (proto->Spells[i].SpellId)
        {
            // item has limited charges
            if (proto->Spells[i].SpellCharges)
            {
                if (proto->Spells[i].SpellCharges < 0)
                    expendable = true;

                int32 charges = m_CastItem->GetSpellCharges(i);

                // item has charges left
                if (charges)
                {
                    (charges > 0) ? --charges : ++charges;  // abs(charges) less at 1 after use
                    if (proto->Stackable < 2)
                        m_CastItem->SetSpellCharges(i, charges);
                    m_CastItem->SetState(ITEM_CHANGED, (Player*)m_caster);
                }

                // all charges used
                withoutCharges = (charges == 0);
            }
        }
    }

    if (expendable && withoutCharges && !sWorld.getConfig(CONFIG_HAPPY_TESTING))
    {
        uint32 count = 1;
        ((Player*)m_caster)->DestroyItemCount(m_CastItem, count, true);

        // prevent crash at access to deleted m_targets.getItemTarget
        if (m_CastItem == m_targets.getItemTarget())
            m_targets.setItemTarget(NULL);

        m_CastItem = NULL;
        m_castItemGUID = 0;
    }
}

void Spell::TakePower()
{
    if (m_CastItem || m_triggeredByAuraSpell)
        return;

    bool hit = true;
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        if (GetSpellEntry()->powerType == POWER_RAGE || GetSpellEntry()->powerType == POWER_ENERGY)
            if (uint64 targetGUID = m_targets.getUnitTargetGUID())
                for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                {
                    if (ihit->deleted)
                        continue;

                    if (ihit->targetGUID == targetGUID)
                    {
                        if (ihit->missCondition != SPELL_MISS_NONE)
                            hit = false;
                        else if (((Player*)m_caster)->GetClass() == CLASS_DRUID && ihit->missCondition == SPELL_MISS_MISS && GetSpellEntry()->powerType == POWER_ENERGY) // not sure if it's limited only to druid/energy
                            hit = false;
                        break;
                    }
                }

        if (hit && SpellMgr::NeedsComboPoints(GetSpellEntry()))
            ((Player*)m_caster)->ClearComboPoints();
    }

    if (!m_powerCost)
        return;

    // health as power used
    if (GetSpellEntry()->powerType == POWER_HEALTH)
    {
        m_caster->ModifyHealth(-(int32)m_powerCost);
        return;
    }

    if (GetSpellEntry()->powerType >= MAX_POWERS)
    {
        sLog.outLog(LOG_DEFAULT, "ERROR: Spell::TakePower: Unknown power type '%d'", GetSpellEntry()->powerType);
        return;
    }

    Powers powerType = Powers(GetSpellEntry()->powerType);

    if (hit || (SpellMgr::NeedsComboPoints(GetSpellEntry()) && m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->GetClass() == CLASS_DRUID))  // not sure if it's limited only to druid
        m_caster->ModifyPower(powerType, -m_powerCost);
    else
        m_caster->ModifyPower(powerType, -irand(0, m_powerCost / 4));

    // Set the five second timer
    if (powerType == POWER_MANA && m_powerCost > 0)
        m_caster->SetLastManaUse(WorldTimer::getMSTime());
}

void Spell::TakeReagents()
{
    if (IsTriggeredSpell())                                  // reagents used in triggered spell removed by original spell or don't must be removed.
        return;

    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    Player* p_caster = (Player*)m_caster;
    if (p_caster->CanNoReagentCast(GetSpellEntry()))
        return;

    for (uint32 x = 0; x < 8; ++x)
    {
        if (GetSpellEntry()->Reagent[x] <= 0)
            continue;

        uint32 itemid = GetSpellEntry()->Reagent[x];
        uint32 itemcount = GetSpellEntry()->ReagentCount[x];

        // if CastItem is also spell reagent
        if (m_CastItem)
        {
            ItemPrototype const *proto = m_CastItem->GetProto();
            if (proto && proto->ItemId == itemid)
            {
                for (int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }

                m_CastItem = NULL;
                m_castItemGUID = 0;
            }
        }

        // if getItemTarget is also spell reagent
        if (m_targets.getItemTargetEntry() == itemid)
            m_targets.setItemTarget(NULL);

        p_caster->DestroyItemCount(itemid, itemcount, true);
    }
}

void Spell::HandleThreatSpells(uint32 spellId)
{
    if (!m_targets.getUnitTarget() || !spellId)
        return;

    if (!m_targets.getUnitTarget()->CanHaveThreatList())
        return;

    SpellThreatEntry const *threatSpell = sSpellThreatStore.LookupEntry<SpellThreatEntry>(spellId);
    if (!threatSpell)
        return;

    m_targets.getUnitTarget()->AddThreat(m_caster, float(threatSpell->threat));

    DEBUG_LOG("Spell %u, rank %u, added an additional %i threat", spellId, sSpellMgr.GetSpellRank(spellId), threatSpell->threat);
}

void Spell::HandleEffects(Unit *pUnitTarget, Item *pItemTarget, GameObject *pGOTarget, uint32 i, float /*DamageMultiplier*/)
{
    unitTarget = pUnitTarget;
    itemTarget = pItemTarget;
    gameObjTarget = pGOTarget;

    uint8 eff = GetSpellEntry()->Effect[i];
    uint32 mechanic = GetSpellEntry()->EffectMechanic[i];

    sLog.outDebug("Spell: Effect : %u", eff);

    //Simply return. Do not display "immune" in red text on client
    if (unitTarget && unitTarget->IsImmunedToSpellEffect(eff, mechanic))
        return;

    // must be done here, can happen that cc spell has secondary effect (death coil) and target is immune only to cc effect
    if (unitTarget && GetSpellEntry()->AttributesCu & SPELL_ATTR_CU_AURA_CC) 
    {
        switch (GetSpellEntry()->EffectApplyAuraName[i])
        {
        case SPELL_AURA_MOD_POSSESS:
        case SPELL_AURA_MOD_CONFUSE:
        case SPELL_AURA_MOD_CHARM:
        case SPELL_AURA_MOD_FEAR:
        case SPELL_AURA_MOD_STUN:
            unitTarget->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_CC);
            if (!unitTarget->IsStandState())
                unitTarget->SetStandState(PLAYER_STATE_NONE);
            break;
        default: break;
        }
    }
    
    //we do not need DamageMultiplier here.
    damage = CalculateDamage(i, NULL);

    if (eff < TOTAL_SPELL_EFFECTS)
    {
        if (!sScriptMgr.OnSpellHandleEffect(m_caster, unitTarget, itemTarget, gameObjTarget, GetSpellEntry(), i))
            (*this.*SpellEffects[eff])(i);
    }
}

void Spell::TriggerSpell()
{
    for (TriggerSpells::iterator si = m_TriggerSpells.begin(); si != m_TriggerSpells.end(); ++si)
    {
        Spell* spell = new Spell(m_caster, (*si), true, m_originalCasterGUID, m_selfContainer, true);
        spell->prepare(&m_targets);                         // use original spell original targets
    }
}

SpellCastResult Spell::CheckCast(bool strict)
{
    // check cooldowns to prevent cheating
    if (!IsTriggeredSpell() && m_caster->GetCharmerOrOwnerPlayerOrPlayerItself())
    {
        CooldownMgr& ownercm = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself()->GetCooldownMgr();
        if (m_CastItem && ownercm.HasItemCooldown(m_CastItem->GetEntry()))
            return SPELL_FAILED_NOT_READY;
        if (!m_CastItem && ownercm.HasSpellCooldown(GetSpellEntry()->Id))
            return SPELL_FAILED_NOT_READY;
    }

    if (!IsTriggeredSpell() && m_caster->ToCreature())
    {
        if (m_caster->ToCreature()->isSchoolProhibited((SpellSchoolMask)GetSpellEntry()->SchoolMask))
            return SPELL_FAILED_NOT_READY;
    }

    if (GetSpellEntry()->Attributes & SPELL_ATTR_DISABLED_WHILE_ACTIVE && m_caster->HasAura(GetSpellEntry()->Id, 0))
        return SPELL_FAILED_NOT_READY;

    if (GetSpellEntry()->Id == 34477 && m_targets.getUnitTarget() && m_targets.getUnitTarget()->HasAura(35097, 0))
        return SPELL_FAILED_AURA_BOUNCED;// missdirection already active

    // Check global cooldown
    if (strict && !IsTriggeredSpell() && HasGlobalCooldown())
        return SPELL_FAILED_NOT_READY;

    // only allow triggered spells if at an ended battleground
    if (!IsTriggeredSpell() && m_caster->GetTypeId() == TYPEID_PLAYER)
        if (BattleGround * bg = ((Player*)m_caster)->GetBattleGround())
            if (bg->GetStatus() == STATUS_WAIT_LEAVE)
                return SPELL_FAILED_DONT_REPORT;

    if (!IsTriggeredSpell() && SpellMgr::IsNonCombatSpell(GetSpellEntry()) && m_caster->IsInCombat())
        return SPELL_FAILED_AFFECTING_COMBAT;

    // only check at first call, Stealth auras are already removed at second call
    // for now, ignore triggered spells
    if (strict && !IsTriggeredSpell())
    {
        // Cannot be used in this stance/form
        SpellCastResult shapeError = SpellMgr::GetErrorAtShapeshiftedCast(GetSpellEntry(), m_caster->m_form);
        if (shapeError != SPELL_CAST_OK)
            return shapeError;

        if ((GetSpellEntry()->Attributes & SPELL_ATTR_ONLY_STEALTHED) && !(m_caster->HasStealthAura()))
            return SPELL_FAILED_ONLY_STEALTHED;
    }

    // caster state requirements
    if (GetSpellEntry()->CasterAuraState && !m_caster->HasAuraState(AuraState(GetSpellEntry()->CasterAuraState)))
        return SPELL_FAILED_CASTER_AURASTATE;

    if (GetSpellEntry()->CasterAuraStateNot && m_caster->HasAuraState(AuraState(GetSpellEntry()->CasterAuraStateNot)))
        return SPELL_FAILED_CASTER_AURASTATE;

    // cancel autorepeat spells if cast start when moving 'n' Check for movin' by(rotation keys ft. mouse button)
    // (not wand currently autorepeat cast delayed to moving stop anyway in spell update code)
    if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->ToPlayer()->isMoving())
    {
        //         uint32 ct = SpellMgr::GetSpellCastTime(GetSpellEntry(), this);
        //         if (ct && m_spellInfo->SpellFamilyFlags != SPELLFAMILY_GENERIC)
        //             return SPELL_FAILED_MOVING;

        if (/*!m_caster->HasUnitMovementFlag(MOVEFLAG_FALLINGFAR) && */IsAutoRepeat())
            return SPELL_FAILED_MOVING;
    }

    if (!IsTriggeredSpell() && m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->IsGameMaster() && sWorld.getConfig(CONFIG_VMAP_INDOOR_CHECK))
    {
        if (GetSpellEntry()->Attributes & SPELL_ATTR_OUTDOORS_ONLY &&
            !m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_OUTDOORS;

        /*if (GetSpellEntry()->Attributes & SPELL_ATTR_INDOORS_ONLY &&
            m_caster->GetTerrain()->IsOutdoors(m_caster->GetPositionX(), m_caster->GetPositionY(), m_caster->GetPositionZ()))
            return SPELL_FAILED_ONLY_INDOORS; UNUSED*/
    }

    if (GetSpellEntry()->AttributesEx6 & SPELL_ATTR_EX6_NOT_IN_RAID_INSTANCE && m_caster->GetInstanceId())
    {
        if (Map *PlrMap = m_caster->GetMap())
            if (PlrMap->IsRaid())
                return SPELL_FAILED_NOT_HERE;
    }

    // can't cast while seated
    if (m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->IsSitState() && !(IsTriggeredSpell() || IsAutoShootSpell()) && !(GetSpellEntry()->Attributes & SPELL_ATTR_CASTABLE_WHILE_SITTING))
    {
        ((Player*)m_caster)->SetStandState(UNIT_STAND_STATE_STAND);
    }

    if (Unit *target = m_targets.getUnitTarget())
    {
        // target state requirements (not allowed state), apply to self also
        if (GetSpellEntry()->TargetAuraStateNot && target->HasAuraState(AuraState(GetSpellEntry()->TargetAuraStateNot)))
            return SPELL_FAILED_TARGET_AURASTATE;

        if (target != m_caster)
        {
            // target state requirements (apply to non-self only), to allow cast affects to self like Dirty Deeds
            if (GetSpellEntry()->TargetAuraState && !target->HasAuraState(AuraState(GetSpellEntry()->TargetAuraState)))
                return SPELL_FAILED_TARGET_AURASTATE;

            // Not allow players casting on flying player
            if (target->IsTaxiFlying() && m_caster->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
                return SPELL_FAILED_BAD_TARGETS;

            if ((!IsTriggeredSpell() || IsAutoShootSpell()) && !SpellMgr::SpellIgnoreLOS(GetSpellEntry(), 0) && !m_caster->IsWithinLOSInMap(target))
                return SPELL_FAILED_LINE_OF_SIGHT;

            // auto selection spell rank implemented in WorldSession::HandleCastSpellOpcode
            // this case can be triggered if rank not found (too low-level target for first rank)
            if (m_caster->GetTypeId() == TYPEID_PLAYER && !SpellMgr::IsPassiveSpell(GetSpellEntry()->Id) && !m_CastItem)
            {
                for (int i = 0; i < 3; i++)
                {
                    if (SpellMgr::IsPositiveEffect(GetSpellEntry()->Id, i) && GetSpellEntry()->Effect[i] == SPELL_EFFECT_APPLY_AURA)
                        if (target->GetLevel() + 10 < GetSpellEntry()->spellLevel)
                            return SPELL_FAILED_LOWLEVEL;
                }
            }
        }

        // check pet presents
        for (int j = 0; j < 3; j++)
        {
            if (GetSpellEntry()->EffectImplicitTargetA[j] == TARGET_UNIT_CASTER_PET)
            {
                target = m_caster->GetPet();
                if (!target)
                    target = m_caster->GetCharm();
                if (!target)
                {
                    if (m_triggeredByAuraSpell)              // not report pet not existence for triggered spells
                        return SPELL_FAILED_DONT_REPORT;
                    else
                        return SPELL_FAILED_NO_PET;
                }
                break;
            }
        }

        //check creature type
        //ignore self casts (including area casts when caster selected as target)
        if (target != m_caster)
        {
            if (!CheckTargetCreatureType(target))
            {
                if (target->GetTypeId() == TYPEID_PLAYER)
                    return SPELL_FAILED_TARGET_IS_PLAYER;
                else
                    return SPELL_FAILED_BAD_TARGETS;
            }

            // need to implement isTappedBy
            /*if (m_caster->GetTypeId() == TYPEID_PLAYER)
            {
            // Do not these spells to target creatures not tapped by us (Banish, Polymorph, many quest spells)
            if (GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_CANT_TARGET_TAPPED)
            {
            if (target->GetTypeId() == TYPEID_UNIT)
            {
            Creature *targetCreature = (Creature*)target;
            if (targetCreature->hasLootRecipient() && !targetCreature->isTappedBy((Player*)m_caster))
            return SPELL_FAILED_CANT_CAST_ON_TAPPED;
            }
            }
            }*/
        }

        if (GetSpellEntry()->EffectImplicitTargetA[0] == TARGET_UNIT_SCRIPT_NEAR_CASTER)
        {
            SpellScriptTarget::const_iterator lower = sSpellMgr.GetBeginSpellScriptTarget(GetSpellEntry()->Id);
            SpellScriptTarget::const_iterator upper = sSpellMgr.GetEndSpellScriptTarget(GetSpellEntry()->Id);
            if (lower != upper)
            {
                bool found = false;
                for (SpellScriptTarget::const_iterator i_spellST = lower; i_spellST != upper; ++i_spellST)
                {
                    if (i_spellST->second.type == SPELL_TARGET_TYPE_GAMEOBJECT)
                    {
                        found = true; break;
                    }
                    if (i_spellST->second.type == SPELL_TARGET_TYPE_CREATURE && target->IsAlive() &&
                        i_spellST->second.targetEntry == target->GetEntry())
                    {
                        found = true; break;
                    }
                    if (i_spellST->second.type == SPELL_TARGET_TYPE_DEAD && !target->IsAlive() &&
                        i_spellST->second.targetEntry == target->GetEntry())
                    {
                        found = true; break;
                    }
                }

                if (!found)
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        // TODO: this check can be applied and for player to prevent cheating when IsPositiveSpell will return always correct result.
        // check target for pet/charmed casts (not self targeted), self targeted cast used for area effects and etc
        if (m_caster != target && m_caster->GetTypeId() == TYPEID_UNIT && m_caster->GetCharmerOrOwnerGUID())
        {
            // check correctness positive/negative cast target (pet cast real check and cheating check)
            if (SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
            {
                //dispel positivity is dependant on target, don't check it
                if (m_caster->IsHostileTo(target) && !SpellMgr::IsDispel(GetSpellEntry()))
                    return SPELL_FAILED_BAD_TARGETS;
            }
            else
            {
                if (m_caster->IsFriendlyTo(target))
                    return SPELL_FAILED_BAD_TARGETS;
            }
        }

        /* This prevented to cast heal on players in cyclone, which should be possible
        if (IsPositiveSpell(GetSpellEntry()->Id))
        {
        if (target->IsImmunedToSpell(GetSpellEntry(),false))
        return SPELL_FAILED_TARGET_AURASTATE;
        }
        */

        if (SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
        {
            if (m_spellInfo->Id == 3411 && !target->IsAlive())
                return SPELL_FAILED_BAD_TARGETS;
            if (m_spellInfo->Id == 37455 && target->getFaction() == 1691 ||
                m_spellInfo->Id == 37456 && target->getFaction() == 1690)
                return SPELL_FAILED_BAD_TARGETS; // explicit check for chess healing spells (pieces are not hostile to players, only to eachother)
            if (m_spellInfo->Id == 10060 && target->getClassMask() & 0x9)
                return SPELL_FAILED_BAD_TARGETS; // power infusion on rogue and warriors
        }

        //Must be behind the target.
        if ((GetSpellEntry()->AttributesEx2 & SPELL_ATTR_EX2_FROM_BEHIND) && (GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_UNK9) && !m_caster->IsBehindTarget(target, strict)
            && (GetSpellEntry()->SpellFamilyName != SPELLFAMILY_DRUID || GetSpellEntry()->SpellFamilyFlags != 0x0000000000020000LL))
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_BEHIND;
        }

        //Target must be facing you.
        if ((GetSpellEntry()->Attributes == 0x150010) && !target->HasInArc(M_PI, m_caster))
        {
            SendInterrupted(2);
            return SPELL_FAILED_NOT_INFRONT;
        }

        // check if target is in combat
        if (target != m_caster && (GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_NOT_IN_COMBAT_TARGET) && target->IsInCombat())
        {
            return SPELL_FAILED_TARGET_AFFECTING_COMBAT;
        }
    }
    else if (!IsTriggeredSpell())
    {
        // check pet presents
        for (int j = 0; j < 3; j++)
        {
            if (GetSpellEntry()->EffectImplicitTargetA[j] == TARGET_UNIT_CASTER_PET)
            {
                if (Pet *pPet = m_caster->GetPet())
                {
                    if (!SpellMgr::SpellIgnoreLOS(GetSpellEntry(), j) && !pPet->IsWithinLOSInMap(m_caster))
                        return SPELL_FAILED_LINE_OF_SIGHT;
                }
                break;
            }
        }
    }

    if (m_CastItem)
    {
        if (GetSpellEntry()->NeedFillTargetMapForTargets(0) && m_targets.IsEmpty() && !m_targets.HasDst())
            return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
    }

    // Spell cast only on battleground
    if ((GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_BATTLEGROUND) && m_caster->GetTypeId() == TYPEID_PLAYER)
        if (!((Player*)m_caster)->InBattleGround())
            return SPELL_FAILED_ONLY_BATTLEGROUNDS;

    // do not allow spells to be cast in arenas
    // - with greater than 15 min CD without SPELL_ATTR_EX4_USABLE_IN_ARENA flag
    // - with SPELL_ATTR_EX4_NOT_USABLE_IN_ARENA flag
    // - flying mounts/spells
    if ((GetSpellEntry()->AttributesEx4 & SPELL_ATTR_EX4_NOT_USABLE_IN_ARENA) ||
        GetSpellEntry()->HasApplyAura(SPELL_AURA_FLY) || GetSpellEntry()->HasApplyAura(SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED) || GetSpellEntry()->HasApplyAura(SPELL_AURA_MOD_SPEED_FLIGHT) ||
        SpellMgr::GetSpellRecoveryTime(GetSpellEntry()) > 15 * MINUTE * 1000 && !(GetSpellEntry()->AttributesEx4 & SPELL_ATTR_EX4_USABLE_IN_ARENA))
        if (MapEntry const* mapEntry = sMapStore.LookupEntry(m_caster->GetMapId()))
            if (mapEntry->IsBattleArena())
                return SPELL_FAILED_NOT_IN_ARENA;

    // zone check
    uint32 zoneid, areaid;
    if (m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        zoneid = ((Player*)m_caster)->GetCachedZone();
        areaid = ((Player*)m_caster)->GetCachedArea();
    }
    else
    {
        zoneid = m_caster->GetZoneId();
        areaid = m_caster->GetAreaId();
    }

    if (!SpellMgr::IsSpellAllowedInLocation(GetSpellEntry(), m_caster->GetMapId(), zoneid, areaid))
        return SPELL_FAILED_REQUIRES_AREA;

    // not let players cast spells at mount (and let do it to creatures)
    if (m_caster->IsMounted() && m_caster->GetTypeId() == TYPEID_PLAYER && !IsTriggeredSpell() &&
        !SpellMgr::IsPassiveSpell(GetSpellEntry()->Id) && !(GetSpellEntry()->Attributes & SPELL_ATTR_CASTABLE_WHILE_MOUNTED))
    {
        if (m_caster->IsTaxiFlying())
            return SPELL_FAILED_NOT_FLYING;
        else
            return SPELL_FAILED_NOT_MOUNTED;
    }

    // always (except passive and triggered spells) check items (focus object can be required for any type casts)
    if (!SpellMgr::IsPassiveSpell(GetSpellEntry()->Id) && !IsTriggeredSpell())
    {
        SpellCastResult castResult = CheckItems();
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    if (!IsTriggeredSpell() || GetSpellEntry()->Id == 33395) // hack for water elemental freeze since it is cast as triggered spell
    {
        SpellCastResult castResult = SPELL_CAST_OK;
        if (!IsNextMeleeSwingSpell())
            castResult = CheckRange(strict);
        if (castResult != SPELL_CAST_OK)
            return castResult;

        castResult = CheckPower();
        if (castResult != SPELL_CAST_OK)
            return castResult;

        castResult = CheckCasterAuras();
        if (castResult != SPELL_CAST_OK)
            return castResult;
    }

    for (int i = 0; i < 3; i++)
    {
        // for effects of spells that have only one target
        switch (GetSpellEntry()->Effect[i])
        {
            case SPELL_EFFECT_DUMMY:
            {
                if (GetSpellEntry()->SpellIconID == 1648 || GetSpellEntry()->Id == 16053)        // Execute, dominion of soul 20% hp
                {
                    if (!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if (GetSpellEntry()->Id == 51582)          // Rocket Boots Engaged
                {
                    if (m_caster->IsInWater())
                        return SPELL_FAILED_ONLY_ABOVEWATER;
                }
                else if (GetSpellEntry()->SpellIconID == 156)      // Holy Shock
                {
                    // spell different for friends and enemies
                    // hart version required facing
                    if (m_targets.getUnitTarget() && !m_caster->IsFriendlyTo(m_targets.getUnitTarget()) && !m_caster->HasInArc(M_PI, m_targets.getUnitTarget()))
                        return SPELL_FAILED_UNIT_NOT_INFRONT;
                }
                else if (GetSpellEntry()->Id == 19938)          // Awaken Peon
                {
                    Unit *unit = m_targets.getUnitTarget();
                    if (!unit || !unit->HasAura(17743, 0))
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            {
                // Hammer of Wrath
                if (GetSpellEntry()->SpellVisual == 7250)
                {
                    if (!m_targets.getUnitTarget())
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                    if (m_targets.getUnitTarget()->GetHealth() > m_targets.getUnitTarget()->GetMaxHealth()*0.2)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                break;
            }
            case SPELL_EFFECT_LEARN_SPELL:
            {
                if (GetSpellEntry()->EffectImplicitTargetA[i] != TARGET_UNIT_CASTER_PET)
                    break;

                Pet* pet = m_caster->GetPet();

                if (!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(GetSpellEntry()->EffectTriggerSpell[i]);

                if (!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if (!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                    return SPELL_FAILED_TOO_MANY_SKILLS;

                if (GetSpellEntry()->spellLevel > pet->GetLevel())
                    return SPELL_FAILED_LOWLEVEL;

                if (!pet->HasTPForSpell(learn_spellproto->Id))
                    return SPELL_FAILED_TRAINING_POINTS;

                if (!pet->IsRightSpellIdForPet(GetSpellEntry()->EffectTriggerSpell[i]))
                    return SPELL_FAILED_NO_PET; // Don't know what to put here, anyway this will happen only in trying to bug/cheat

                break;
            }
            case SPELL_EFFECT_TRANS_DOOR:
            {
                if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_MAGE && GetSpellEntry()->SpellFamilyFlags & 0x80000000)
                {
                    if (m_caster->ToPlayer() && m_caster->ToPlayer()->GetBattleGround())
                        return SPELL_FAILED_NOT_IN_BATTLEGROUND;
                }
                break;
            }
            case SPELL_EFFECT_LEARN_PET_SPELL:
            {
                Pet* pet = m_caster->GetPet();

                if (!pet)
                    return SPELL_FAILED_NO_PET;

                SpellEntry const *learn_spellproto = sSpellStore.LookupEntry(GetSpellEntry()->EffectTriggerSpell[i]);

                if (!learn_spellproto)
                    return SPELL_FAILED_NOT_KNOWN;

                if (!pet->CanTakeMoreActiveSpells(learn_spellproto->Id))
                    return SPELL_FAILED_TOO_MANY_SKILLS;

                if (GetSpellEntry()->spellLevel > pet->GetLevel())
                    return SPELL_FAILED_LOWLEVEL;

                if (!pet->HasTPForSpell(learn_spellproto->Id))
                    return SPELL_FAILED_TRAINING_POINTS;

                if (!pet->IsRightSpellIdForPet(GetSpellEntry()->EffectTriggerSpell[i]))
                    return SPELL_FAILED_NO_PET; // Don't know what to put here, anyway this will happen only in trying to bug/cheat

                break;
            }
            case SPELL_EFFECT_FEED_PET:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getItemTarget())
                    return SPELL_FAILED_BAD_TARGETS;

                Pet* pet = m_caster->GetPet();

                if (!pet)
                    return SPELL_FAILED_NO_PET;

                if (!pet->HaveInDiet(m_targets.getItemTarget()->GetProto()))
                    return SPELL_FAILED_WRONG_PET_FOOD;

                if (!pet->GetCurrentFoodBenefitLevel(m_targets.getItemTarget()->GetProto()->ItemLevel))
                    return SPELL_FAILED_FOOD_LOWLEVEL;

                if (m_caster->IsInCombat() || pet->IsInCombat())
                    return SPELL_FAILED_AFFECTING_COMBAT;

                break;
            }
            case SPELL_EFFECT_POWER_BURN:
            case SPELL_EFFECT_POWER_DRAIN:
            {
                // Can be area effect, Check only for players and not check if target - caster (spell can have multiply drain/burn effects)
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                    if (Unit* target = m_targets.getUnitTarget())
                        if (target != m_caster && target->GetPowerType() != GetSpellEntry()->EffectMiscValue[i])
                            return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_CHARGE:
            case SPELL_EFFECT_CHARGE2:
            {
                if (m_caster->HasUnitState(UNIT_STAT_ROOT))
                    return SPELL_FAILED_ROOTED;

                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                {
                    if (BattleGround const *bg = m_caster->ToPlayer()->GetBattleGround())
                    {
                        if (bg->GetStatus() != STATUS_IN_PROGRESS)
                            return SPELL_FAILED_TRY_AGAIN;
                    }
                }

                if (Unit* target = m_targets.getUnitTarget())
                {
                    if (!target->IsAlive())
                        return SPELL_FAILED_BAD_TARGETS;

                    Position dest;
                    target->GetPosition(&dest);
                    bool result;
                    // in pet cast/autocast cases CheckCast is called multiple times, we need only last of paths
                    _path.reinitialize();

                    float angle = m_caster->GetAngle(target) - m_caster->GetOrientation() - M_PI;
                    if (sWorld.getConfig(CONFIG_MOVEMENT_ENABLE_LONG_CHARGE))
                    {
                        // no length limit, real warrior charges even through kamboja if he has to
                        result = _path.calculate(dest.m_positionX, dest.m_positionY, dest.m_positionZ);
                        _path.stepBack(2.0f);
                    }
                    else
                    {
                        m_caster->GetValidPointInAngle(dest, 1.0f, angle, false, 2.0f);
                        _path.setPathLengthLimit(SpellMgr::GetSpellMaxRange(GetSpellEntry()) * 1.5f);
                        result = _path.calculate(dest.m_positionX, dest.m_positionY, dest.m_positionZ);
                    }

                    if (_path.getPathType() & PATHFIND_SHORT)
                        return SPELL_FAILED_OUT_OF_RANGE;
                    else if (!result)
                        return SPELL_FAILED_NOPATH;
                }
                break;
            }
            case SPELL_EFFECT_SKINNING:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER || !m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() != TYPEID_UNIT)
                    return SPELL_FAILED_BAD_TARGETS;

                if (!(m_targets.getUnitTarget()->GetUInt32Value(UNIT_FIELD_FLAGS) & UNIT_FLAG_SKINNABLE))
                    return SPELL_FAILED_TARGET_UNSKINNABLE;

                Creature* creature = (Creature*)m_targets.getUnitTarget();
                if (creature->GetCreatureType() != CREATURE_TYPE_CRITTER && (!creature->lootForBody || !creature->loot.empty()))
                {
                    return SPELL_FAILED_TARGET_NOT_LOOTED;
                }

                uint32 skill = creature->GetCreatureInfo()->GetRequiredLootSkill();

                int32 skillValue = ((Player*)m_caster)->GetSkillValue(skill);
                int32 TargetLevel = m_targets.getUnitTarget()->GetLevel();
                int32 ReqValue = (skillValue < 100 ? (TargetLevel - 10) * 10 : TargetLevel * 5);
                if (ReqValue > skillValue)
                    return SPELL_FAILED_LOW_CASTLEVEL;

                // chance for fail at orange skinning attempt
                if ((m_selfContainer && (*m_selfContainer) == this) &&
                    skillValue < sWorld.GetConfigMaxSkillValue() &&
                    (ReqValue < 0 ? 0 : ReqValue) > irand(skillValue - 25, skillValue + 37))
                    return SPELL_FAILED_TRY_AGAIN;

                break;
            }
            case SPELL_EFFECT_OPEN_LOCK_ITEM:
            case SPELL_EFFECT_OPEN_LOCK:
            {
                if (GetSpellEntry()->EffectImplicitTargetA[i] != TARGET_GAMEOBJECT &&
                    GetSpellEntry()->EffectImplicitTargetA[i] != TARGET_LOCKED)
                    break;

                if (m_caster->GetTypeId() != TYPEID_PLAYER  // only players can open locks, gather etc.
                    // we need a go target in case of TARGET_GAMEOBJECT
                    || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_GAMEOBJECT && !m_targets.getGOTarget()
                    // we need a go target, or an openable item target in case of TARGET_LOCKED
                    || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_LOCKED && !m_targets.getGOTarget() &&
                    (!m_targets.getItemTarget() || !m_targets.getItemTarget()->GetProto()->LockID || m_targets.getItemTarget()->GetOwner() != m_caster))
                    return SPELL_FAILED_BAD_TARGETS;

                // In BattleGround players can use only flags and banners
                if (((Player*)m_caster)->InBattleGround() &&
                    !((Player*)m_caster)->isAllowUseBattleGroundObject() && GetSpellEntry()->EffectMiscValue[i] != 4/*traps*/)
                    return SPELL_FAILED_TRY_AGAIN;

                // get the lock entry
                uint32 lockId = 0;
                if (GameObject* go = m_targets.getGOTarget())
                {
                    lockId = go->GetLockId();
                    if (!lockId)
                        return SPELL_FAILED_BAD_TARGETS;
                }
                else if (Item* itm = m_targets.getItemTarget())
                    lockId = itm->GetProto()->LockID;

                SkillType skillId = SKILL_NONE;
                int32 reqSkillValue = 0;
                int32 skillValue = 0;

                SpellCastResult res = CanOpenLock(i, lockId, skillId, reqSkillValue, skillValue);
                if (res != SPELL_CAST_OK)
                    return res;

                // chance for fail at orange mining/herb/LockPicking gathering attempt
                // second check prevent fail at rechecks
                if (skillId != SKILL_NONE && (!m_selfContainer || ((*m_selfContainer) != this)))
                {
                    bool canFailAtMax = skillId != SKILL_HERBALISM && skillId != SKILL_MINING;

                    // chance for failure in orange gather / lockpick (gathering skill can't fail at maxskill)
                    if ((canFailAtMax || skillValue < sWorld.GetConfigMaxSkillValue()) && reqSkillValue > irand(skillValue - 25, skillValue + 37))
                        return SPELL_FAILED_TRY_AGAIN;
                }

                break;
            }
            case SPELL_EFFECT_RESURRECT_PET:
            {
                Creature *pet = m_caster->GetPet();
                if (!pet)
                    return SPELL_FAILED_NO_PET;

                if (pet->IsAlive())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                break;
            }
            // This is generic summon effect now and don't make this check for summon types similar
            // SPELL_EFFECT_SUMMON_CRITTER, SPELL_EFFECT_SUMMON_WILD or SPELL_EFFECT_SUMMON_GUARDIAN.
            // These won't show up in m_caster->GetPetGUID()
            case SPELL_EFFECT_SUMMON:
            {
                switch (GetSpellEntry()->EffectMiscValueB[i])
                {
                    case SUMMON_TYPE_POSESSED:
                        if (m_caster->GetCharmGUID())
                            return SPELL_FAILED_ALREADY_HAVE_CHARM;
                        break; // temp summon of possesed creature, can have normal pet
                    //case SUMMON_TYPE_POSESSED2:
                    //case SUMMON_TYPE_POSESSED3:
                    case SUMMON_TYPE_DEMON:
                    case SUMMON_TYPE_SUMMON:
                    {
                        if (m_caster->GetPetGUID())
                            return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                        if (m_caster->GetCharmGUID())
                            return SPELL_FAILED_ALREADY_HAVE_CHARM;
                        break;
                    }
                }
                break;
            }
            // Don't make this check for SPELL_EFFECT_SUMMON_CRITTER, SPELL_EFFECT_SUMMON_WILD or SPELL_EFFECT_SUMMON_GUARDIAN.
            // These won't show up in m_caster->GetPetGUID()
            case SPELL_EFFECT_SUMMON_POSSESSED:
            case SPELL_EFFECT_SUMMON_PHANTASM:
            case SPELL_EFFECT_SUMMON_DEMON:
            {
                if (m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if (m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PET:
            {
                if (m_caster->GetPetGUID())                  //let warlock do a replacement summon
                {

                    Pet* pet = ((Player*)m_caster)->GetPet();

                    if (m_caster->GetTypeId() == TYPEID_PLAYER && m_caster->GetClass() == CLASS_WARLOCK)
                    {
                        if (strict)                         //starting cast, trigger pet stun (cast by pet so it doesn't attack player)
                            pet->CastSpell(pet, 32752, true, NULL, NULL, pet->GetGUID());
                    }
                    else
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;
                }

                if (m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                break;
            }
            case SPELL_EFFECT_SUMMON_PLAYER:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                const Player * pCaster = m_caster->ToPlayer();

                if (!pCaster->GetSelection())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = m_targets.getUnitTarget()->ToPlayer();
                if (!target || m_caster == target || (!target->IsInSameRaidWith(pCaster) && m_spellInfo->Id != 48955))
                    return SPELL_FAILED_BAD_TARGETS;

                if (pCaster->GetBattleGround())
                    return SPELL_FAILED_DONT_REPORT; // Ritual of Summoning Effect is triggered so don't report

                // check if our map is dungeon
                if (sMapStore.LookupEntry(m_caster->GetMapId())->IsDungeon())
                {
                    InstanceTemplate const* instance = ObjectMgr::GetInstanceTemplate(m_caster->GetMapId());
                    if (!instance)
                        return SPELL_FAILED_TARGET_NOT_IN_INSTANCE;

                    if (!target->Satisfy(sObjectMgr.GetAccessRequirement(instance->access_id), m_caster->GetMapId()))
                        return SPELL_FAILED_BAD_TARGETS;

                    // if is in instance and summoner and summoned have different instance id's don't summon
                    if (!target->CanBeSummonedBy(pCaster))
                        return SPELL_FAILED_TARGET_LOCKED_TO_RAID_INSTANCE;
                }
                break;
            }
            case SPELL_EFFECT_FRIEND_SUMMON:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER)
                    return SPELL_FAILED_BAD_TARGETS;

                if (!((Player*)m_caster)->GetSelection())
                    return SPELL_FAILED_BAD_TARGETS;

                Player* target = sObjectMgr.GetPlayer(((Player*)m_caster)->GetSelection());

                if (!target || !target->IsReferAFriendLinked(((Player*)m_caster)))
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            case SPELL_EFFECT_LEAP:
            case SPELL_EFFECT_TELEPORT_UNITS_FACE_CASTER:
            {
                //Do not allow to cast it before BG starts.
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                    if (BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                        if (bg->GetStatus() != STATUS_IN_PROGRESS)
                            return SPELL_FAILED_TRY_AGAIN;
                break;
            }
            case SPELL_EFFECT_TELEPORT_UNITS:
            {
                //Do not allow use of Trinket before BG starts
                if (m_caster->GetTypeId() == TYPEID_PLAYER)
                    if (GetSpellEntry()->Id == 22563 || GetSpellEntry()->Id == 22564)
                        if (BattleGround const *bg = ((Player*)m_caster)->GetBattleGround())
                            if (bg->GetStatus() != STATUS_IN_PROGRESS)
                                return SPELL_FAILED_TRY_AGAIN;
                break;
            }
            case SPELL_EFFECT_STEAL_BENEFICIAL_BUFF:
            {
                if (m_targets.getUnitTarget() == m_caster)
                    return SPELL_FAILED_BAD_TARGETS;
                break;
            }
            case SPELL_EFFECT_ENERGIZE:
            {
                // Consume Magic
                if (GetSpellEntry()->Id == 32676)
                {
                    if (m_caster->GetTypeId() != TYPEID_PLAYER)
                        return SPELL_FAILED_UNKNOWN;

                    std::vector<uint32> priest_buffs;
                    priest_buffs.clear();
                    Unit::AuraMap& Auras = m_caster->GetAuras();
                    for (Unit::AuraMap::iterator i = Auras.begin(); i != Auras.end(); ++i)
                    {
                        // get only priests auras
                        SpellEntry const *spellInfo = i->second->GetSpellProto();
                        if (spellInfo->SpellFamilyName != SPELLFAMILY_PRIEST)
                            continue;
                        // do not select passive auras
                        if (i->second->IsPassive())
                            continue;
                        // only beneficial effects count
                        if (!SpellMgr::IsPositiveSpell(spellInfo->Id))
                            continue;
                        if (spellInfo->Dispel != DISPEL_MAGIC)
                            continue;
                        priest_buffs.push_back(spellInfo->Id);
                    }
                    if (!priest_buffs.empty())
                    {
                        // remove random aura from caster
                        uint32 rand_buff = urand(0, priest_buffs.size() - 1);
                        m_caster->RemoveAurasDueToSpell(priest_buffs[rand_buff]);
                    }
                    else    // if nothing to dispell, send error and break a spell
                        return SPELL_FAILED_NOTHING_TO_DISPEL;
                }
                break;
            }
            case SPELL_EFFECT_DISPEL:
            {
                Unit* target = m_targets.getUnitTarget();
                if (!target || i != 0 || GetSpellEntry()->Effect[1] != 0 || m_IsTriggeredSpell)
                    break; // only for single target when dispel is only effect

                uint32 dispelMask = SpellMgr::GetDispellMask(DispelType(GetSpellEntry()->EffectMiscValue[0]));
                bool anydispelable = false;
                Unit::AuraMap& Auras = target->GetAuras();
                for (Unit::AuraMap::iterator itr = Auras.begin(); itr != Auras.end(); ++itr)
                {
                    Aura *aur = (*itr).second;
                    if (aur && (1 << aur->GetSpellProto()->Dispel) & dispelMask)
                    {
                        if (aur->GetSpellProto()->Dispel == DISPEL_MAGIC &&
                            aur->IsPositive() == target->IsFriendlyTo(m_caster))
                            continue;

                        if (aur->GetCaster() && aur->GetCaster()->GetSpellModOwner())
                        {
                            if (aur->GetCaster()->GetSpellModOwner()->GetTotalFlatMods(GetSpellEntry()->Id, SPELLMOD_RESIST_DISPEL_CHANCE) >= 100)
                                continue; // not dispelable
                        }
                        m_caster->SendCombatStats(1 << COMBAT_STATS_TEST, "dispelable buff %u",target,aur->GetId());
                        anydispelable = true;
                        break;
                    }
                }
                if (!anydispelable)
                    return SPELL_FAILED_NOTHING_TO_DISPEL;
                break;
            }

            default:break;
        }
    }

    for (int i = 0; i < 3; i++)
    {
        switch (GetSpellEntry()->EffectApplyAuraName[i])
        {
            case SPELL_AURA_DUMMY:
            {
                if (GetSpellEntry()->Id == 1515)
                {
                    if (!m_targets.getUnitTarget() || m_targets.getUnitTarget()->GetTypeId() == TYPEID_PLAYER)
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                    if (m_targets.getUnitTarget()->GetLevel() > m_caster->GetLevel())
                        return SPELL_FAILED_HIGHLEVEL;

                    // use SMSG_PET_TAME_FAILURE?
                    if (!((Creature*)m_targets.getUnitTarget())->GetCreatureInfo()->isTameable())
                        return SPELL_FAILED_BAD_TARGETS;

                    if (m_caster->GetPetGUID())
                        return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                    if (m_caster->GetCharmGUID())
                        return SPELL_FAILED_ALREADY_HAVE_CHARM;
                }
            }break;
            case SPELL_AURA_MOD_POSSESS:
            case SPELL_AURA_MOD_CHARM:
            {
                if (m_caster->GetPetGUID())
                    return SPELL_FAILED_ALREADY_HAVE_SUMMON;

                if (m_caster->GetCharmGUID())
                    return SPELL_FAILED_ALREADY_HAVE_CHARM;

                if (m_caster->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (Unit *owner = m_targets.getUnitTarget()->GetOwner())
                    if (owner->GetTypeId() == TYPEID_PLAYER)
                        return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_targets.getUnitTarget()->GetCharmerGUID())
                    return SPELL_FAILED_CHARMED;

                if (int32(m_targets.getUnitTarget()->GetLevel()) > CalculateDamage(i, m_targets.getUnitTarget()))
                    return SPELL_FAILED_HIGHLEVEL;

                break;
            }
            case SPELL_AURA_MOUNTED:
            {
                if (m_caster->IsInWater())
                    return SPELL_FAILED_ONLY_ABOVEWATER;

                if (m_caster->GetTypeId() == TYPEID_PLAYER && ((Player*)m_caster)->GetTransport())
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                // Ignore map check if spell have AreaId. AreaId already checked and this prevent special mount spells
                if (m_caster->GetTypeId() == TYPEID_PLAYER && !sMapStore.LookupEntry(m_caster->GetMapId())->IsMountAllowed() && !IsTriggeredSpell() && !GetSpellEntry()->AreaId)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                uint32 areaid = m_caster->GetTypeId() == TYPEID_PLAYER ? ((Player*)m_caster)->GetCachedArea() : m_caster->GetAreaId();
                if (areaid == 35)
                    return SPELL_FAILED_NO_MOUNTS_ALLOWED;

                if (m_caster->IsInDisallowedMountForm())
                    return SPELL_FAILED_NOT_SHAPESHIFT;

                break;
            }
            case SPELL_AURA_RANGED_ATTACK_POWER_ATTACKER_BONUS:
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                // can be cast at non-friendly unit or own pet/charm
                if (m_caster->IsFriendlyTo(m_targets.getUnitTarget()))
                    return SPELL_FAILED_TARGET_FRIENDLY;

                break;
            }
            case SPELL_AURA_MOD_INCREASE_FLIGHT_SPEED:
            case SPELL_AURA_FLY:
            {
                // not allow cast fly spells at old maps by players (all spells is self target)
                if (m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->IsGameMaster())
                {
                    MapEntry const* mapEntry = sMapStore.LookupEntry(m_caster->GetMapId());
                    if (mapEntry && mapEntry->MapID < 530)
                        return SPELL_FAILED_NOT_HERE;
                }
                break;
            }
            case SPELL_AURA_PERIODIC_MANA_LEECH:
            {
                if (!m_targets.getUnitTarget())
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;

                if (m_caster->GetTypeId() != TYPEID_PLAYER || m_CastItem)
                    break;

                if (m_targets.getUnitTarget()->GetPowerType() != POWER_MANA)
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            case SPELL_AURA_MIRROR_IMAGE:
            {
                Unit* pTarget = m_targets.getUnitTarget();

                // In case of TARGET_SCRIPT, we have already added a target. Use it here (and find a better solution)
                if (m_UniqueTargetInfo.size() == 1)
                    pTarget = m_caster->GetMap()->GetCreature(m_UniqueTargetInfo.front().targetGUID);

                if (!pTarget)
                    return SPELL_FAILED_BAD_TARGETS;

                if (pTarget->GetTypeId() != TYPEID_UNIT)    // Target must be creature. TODO: Check if target can also be player
                    return SPELL_FAILED_BAD_TARGETS;

                if (pTarget == m_caster)                    // Clone self can't be accepted
                    return SPELL_FAILED_BAD_TARGETS;

                // It is assumed that target can not be cloned if already cloned by same or other clone auras
                if (!pTarget->GetAurasByType(SPELL_AURA_MIRROR_IMAGE).empty())
                    return SPELL_FAILED_BAD_TARGETS;

                break;
            }
            default:
                break;
        }
    }

    if (!SpellMgr::SpellIgnoreLOS(GetSpellEntry(), 0))
    {
        if (!m_targets.getUnitTarget() && !m_targets.getGOTarget() && !m_targets.getItemTarget())
            if (m_targets.m_destX && m_targets.m_destY && m_targets.m_destZ && !m_caster->IsWithinLOS(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ))
                return SPELL_FAILED_LINE_OF_SIGHT;
    }

    // check if caster has at least 1 combo point for spells that require combo points
    if ((GetSpellEntry()->AttributesEx & (SPELL_ATTR_EX_REQ_COMBO_POINTS1 | SPELL_ATTR_EX_REQ_COMBO_POINTS2)) && m_caster->ToPlayer() && !m_caster->ToPlayer()->GetComboPoints() && !IsTriggeredSpell())
        return SPELL_FAILED_NO_COMBO_POINTS;

    // all ok
    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPetCast(Unit* target)
{
    if (!m_caster->IsAlive())
        return SPELL_FAILED_CASTER_DEAD;

    if (m_caster->IsNonMeleeSpellCast(false) && !IsTriggeredSpell())  //prevent spellcast interruption by another spellcast
        return SPELL_FAILED_SPELL_IN_PROGRESS;
    if (m_caster->IsInCombat() && SpellMgr::IsNonCombatSpell(GetSpellEntry()))
        return SPELL_FAILED_AFFECTING_COMBAT;

    if (m_caster->GetTypeId() == TYPEID_UNIT && (((Creature*)m_caster)->isPet() || m_caster->isCharmed()))
    {
        //dead owner (pets still alive when owners ressed?)
        if (m_caster->GetCharmerOrOwner() && !m_caster->GetCharmerOrOwner()->IsAlive())
            return SPELL_FAILED_CASTER_DEAD;

        if (!target && m_targets.getUnitTarget())
            target = m_targets.getUnitTarget();

        bool need = false;
        for (uint32 i = 0; i < 3; i++)
        {
            if (GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_UNIT_ENEMY || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_UNIT_FRIEND || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_UNIT || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_UNIT_PARTY || GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_LOCATION_CASTER_TARGET_POSITION)
            {
                need = true;
                if (!target)
                    return SPELL_FAILED_BAD_IMPLICIT_TARGETS;
                break;
            }
        }
        if (need)
            m_targets.setUnitTarget(target);

        Unit* _target = m_targets.getUnitTarget();

        if (_target)                                         //for target dead/target not valid
        {
            if (!_target->IsAlive())
                return SPELL_FAILED_BAD_TARGETS;

            if (!IsValidSingleTargetSpell(_target))
                return SPELL_FAILED_BAD_TARGETS;
        }
        //cooldown
        if (m_caster->GetCharmerOrOwnerPlayerOrPlayerItself() &&
            m_caster->GetCharmerOrOwnerPlayerOrPlayerItself()->GetCooldownMgr().HasSpellCooldown(GetSpellEntry()->Id))
            return SPELL_FAILED_NOT_READY;
    }

    return CheckCast(true);
}

SpellCastResult Spell::CheckCasterAuras() const
{
    // Flag drop spells totally immuned to caster auras
    // FIXME: find more nice check for all totally immuned spells
    // AttributesEx3 & 0x10000000?
    if (GetSpellEntry()->Id == 23336 || GetSpellEntry()->Id == 23334 || GetSpellEntry()->Id == 34991)
        return SPELL_CAST_OK;

    uint8 school_immune = 0;
    uint32 mechanic_immune = 0;
    uint32 dispel_immune = 0;

    //Check if the spell grants school or mechanic immunity.
    //We use bitmasks so the loop is done only once and not on every aura check below.
    if (GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_DISPEL_AURAS_ON_IMMUNITY)
    {
        for (int i = 0; i < 3; i++)
        {
            if (GetSpellEntry()->EffectApplyAuraName[i] == SPELL_AURA_SCHOOL_IMMUNITY)
                school_immune |= uint32(GetSpellEntry()->EffectMiscValue[i]);
            else if (GetSpellEntry()->EffectApplyAuraName[i] == SPELL_AURA_MECHANIC_IMMUNITY)
                mechanic_immune |= 1 << uint32(GetSpellEntry()->EffectMiscValue[i]);
            else if (GetSpellEntry()->EffectApplyAuraName[i] == SPELL_AURA_DISPEL_IMMUNITY)
                dispel_immune |= SpellMgr::GetDispellMask(DispelType(GetSpellEntry()->EffectMiscValue[i]));
        }
        //immune movement impairment and loss of control
        if (GetSpellEntry()->Id == (uint32)42292)
            mechanic_immune = IMMUNE_TO_MOVEMENT_IMPAIRMENT_AND_LOSS_CONTROL_MASK;
    }

    //Check whether the cast should be prevented by any state you might have.
    SpellCastResult prevented_reason = SPELL_CAST_OK;
    // Have to check if there is a stun aura. Otherwise will have problems with ghost aura apply while logging out
    if ((!(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED) && m_caster->HasAuraType(SPELL_AURA_MOD_STUN)) ||
        m_caster->HasAura(33786)) // CYCLONE IS UBER STUN
        prevented_reason = SPELL_FAILED_STUNNED;
    else if (m_caster->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_CONFUSED) && !(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
        prevented_reason = SPELL_FAILED_CONFUSED;
    else if (m_caster->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_FLEEING) && !(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
        prevented_reason = SPELL_FAILED_FLEEING;
    else if (m_caster->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SILENCED) && GetSpellEntry()->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
        prevented_reason = SPELL_FAILED_SILENCED;
    else if (m_caster->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_PACIFIED) && GetSpellEntry()->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
        prevented_reason = SPELL_FAILED_PACIFIED;

    // Attr must make flag drop spell totally immune from all effects
    if (prevented_reason != SPELL_CAST_OK)
    {
        if (school_immune || mechanic_immune || dispel_immune)
        {
            //Checking auras is needed now, because you are prevented by some state but the spell grants immunity.
            Unit::AuraMap const& auras = m_caster->GetAuras();
            for (Unit::AuraMap::const_iterator itr = auras.begin(); itr != auras.end(); ++itr)
            {
                if (itr->second)
                {
                    if (SpellMgr::GetSpellMechanicMask(itr->second->GetSpellProto()) & mechanic_immune
                        || SpellMgr::GetEffectMechanicMask(itr->second->GetSpellProto(), itr->second->GetEffIndex()) & mechanic_immune)
                        continue;
                    if ((SpellMgr::GetSpellSchoolMask(itr->second->GetSpellProto()) & school_immune) &&
                        !(itr->second->GetSpellProto()->AttributesEx & SPELL_ATTR_EX_UNAFFECTED_BY_SCHOOL_IMMUNE))
                        continue;
                    if ((1 << (itr->second->GetSpellProto()->Dispel)) & dispel_immune)
                        continue;

                    //Make a second check for spell failed so the right SPELL_FAILED message is returned.
                    //That is needed when your casting is prevented by multiple states and you are only immune to some of them.
                    switch (itr->second->GetModifier()->m_auraname)
                    {
                        case SPELL_AURA_MOD_STUN:
                            if (!(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_STUNNED))
                                return SPELL_FAILED_STUNNED;
                            break;
                        case SPELL_AURA_MOD_CONFUSE:
                            if (!(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_CONFUSED))
                                return SPELL_FAILED_CONFUSED;
                            break;
                        case SPELL_AURA_MOD_FEAR:
                            if (!(GetSpellEntry()->AttributesEx5 & SPELL_ATTR_EX5_USABLE_WHILE_FEARED))
                                return SPELL_FAILED_FLEEING;
                            break;
                        case SPELL_AURA_MOD_SILENCE:
                        case SPELL_AURA_MOD_PACIFY:
                        case SPELL_AURA_MOD_PACIFY_SILENCE:
                            if (GetSpellEntry()->PreventionType == SPELL_PREVENTION_TYPE_PACIFY)
                                return SPELL_FAILED_PACIFIED;
                            else if (GetSpellEntry()->PreventionType == SPELL_PREVENTION_TYPE_SILENCE)
                                return SPELL_FAILED_SILENCED;
                            break;
                    }
                }
            }
        }
        //You are prevented from casting and the spell cast does not grant immunity. Return a failed error.
        else
            return prevented_reason;
    }

    return SPELL_CAST_OK;
}

bool Spell::CanAutoCast(Unit* target)
{
    // null is not passed to this function
    uint64 targetguid = target->GetGUID();

    for (uint32 j = 0; j < 3; j++)
    {
        if (GetSpellEntry()->Effect[j] == SPELL_EFFECT_APPLY_AURA)
        {
            if (GetSpellEntry()->StackAmount <= 1)
            {
                if (target->HasAura(GetSpellEntry()->Id, j))
                    return false;
            }
            else
            {
                if (target->GetAuras().count(Unit::spellEffectPair(GetSpellEntry()->Id, j)) >= GetSpellEntry()->StackAmount)
                    return false;
            }
        }
        else if (SpellMgr::IsAreaAuraEffect(GetSpellEntry()->Effect[j]))
        {
            if (target->HasAura(GetSpellEntry()->Id, j))
                return false;
        }
    }

    // dash & dive dont use when near or not in combat
    if (GetSpellEntry()->Effect[0] == SPELL_EFFECT_APPLY_AURA &&
        GetSpellEntry()->EffectApplyAuraName[0] == SPELL_AURA_MOD_INCREASE_SPEED &&
        GetSpellEntry()->SpellVisual == 2276)
    {
        if (!m_caster->GetVictim() || m_caster->IsWithinMeleeRange(m_caster->GetVictim()))
            return false;
    }
    SpellCastResult result = CheckPetCast(target);

    if (result == SPELL_CAST_OK || result == SPELL_FAILED_UNIT_NOT_INFRONT)
    {
        FillTargetMap();
        //check if among target units, our WANTED target is as well (->only self cast spells return false)
        for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
        {
            if (ihit->deleted)
                continue;

            if (ihit->targetGUID == targetguid)
                return true;
        }
    }
    return false;                                           //target invalid
}

SpellCastResult Spell::CheckRange(bool strict)
{
    Unit *target = m_targets.getUnitTarget();
    GameObject *pGoTarget = m_targets.getGOTarget();

    // self cast doesn't need range checking -- also for Starshards fix
    if (GetSpellEntry()->rangeIndex == 1)
        return SPELL_CAST_OK;

    SpellRangeEntry const* srange = sSpellRangeStore.LookupEntry(GetSpellEntry()->rangeIndex);
    float max_range = SpellMgr::GetSpellMaxRange(srange);
    if (!strict) max_range += 4.0f;
    float min_range = SpellMgr::GetSpellMinRange(srange);
    uint32 range_type = SpellMgr::GetSpellRangeType(srange);

    if (Player* modOwner = m_caster->GetSpellModOwner())
        modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_RANGE, max_range, this);

    if (target && target != m_caster)
    {
        if (range_type == SPELL_RANGE_MELEE)
        {
            if (!m_caster->IsWithinMeleeRange(target, max_range - NOMINAL_MELEE_RANGE + MELEE_RANGE))
                return SPELL_FAILED_OUT_OF_RANGE;
        }
        else if (!m_caster->IsWithinCombatRange(target, max_range))
            return SPELL_FAILED_OUT_OF_RANGE;               //0x5A;

        if (range_type == SPELL_RANGE_RANGED)
        {
            if (m_caster->IsWithinMeleeRange(target))
                return SPELL_FAILED_TOO_CLOSE;
        }
        else if (min_range && m_caster->IsWithinCombatRange(target, min_range)) // skip this check if min_range = 0
            return SPELL_FAILED_TOO_CLOSE;

        if (!m_caster->GetOwnerGUID() && // pets can cast eg. intercept
            (GetSpellEntry()->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc(M_PI, target))
            return SPELL_FAILED_UNIT_NOT_INFRONT;
    }

    // NOTE(asj) For now, let as be as selective as possible, so call this only
    // in case of Traps. In future this might be also required by other GO.
    if (pGoTarget)
    {
        // distance from target in checks
        float dist = m_caster->GetDistance(pGoTarget);

        switch (pGoTarget->GetGoType())
        {
            case GAMEOBJECT_TYPE_TRAP:
                if (dist > max_range)
                    return SPELL_FAILED_OUT_OF_RANGE;
                if (min_range && dist < min_range)
                    return SPELL_FAILED_TOO_CLOSE;
                if (m_caster->GetTypeId() == TYPEID_PLAYER && (GetSpellEntry()->FacingCasterFlags & SPELL_FACING_FLAG_INFRONT) && !m_caster->HasInArc(M_PI, pGoTarget))
                    return SPELL_FAILED_NOT_INFRONT;
                break;
            case GAMEOBJECT_TYPE_CHEST:
            case GAMEOBJECT_TYPE_GOOBER:
            case GAMEOBJECT_TYPE_BUTTON:
                if (dist > max_range)
                    return SPELL_FAILED_OUT_OF_RANGE;
                break;
        }
    }

    // TODO verify that such spells really use bounding radius
    if (m_targets.m_targetMask == TARGET_FLAG_DEST_LOCATION && m_targets.m_destX != 0 && m_targets.m_destY != 0 && m_targets.m_destZ != 0)
    {
        if (!m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, max_range))
            return SPELL_FAILED_OUT_OF_RANGE;
        if (min_range && m_caster->IsWithinDist3d(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ, min_range))
            return SPELL_FAILED_TOO_CLOSE;
    }

    return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckPower()
{
    // item cast not used power
    if (m_CastItem)
        return SPELL_CAST_OK;

    // health as power used - need check health amount
    if (GetSpellEntry()->powerType == POWER_HEALTH)
    {
        if (m_caster->GetHealth() <= m_powerCost)
            return SPELL_FAILED_CASTER_AURASTATE;
        return SPELL_CAST_OK;
    }
    // Check valid power type
    if (GetSpellEntry()->powerType >= MAX_POWERS)
    {
        sLog.outLog(LOG_DEFAULT, "ERROR: Spell::CheckMana: Unknown power type '%d'", GetSpellEntry()->powerType);
        return SPELL_FAILED_UNKNOWN;
    }
    // Check power amount
    Powers powerType = Powers(GetSpellEntry()->powerType);
    if (m_caster->GetPower(powerType) < m_powerCost)
        return SPELL_FAILED_NO_POWER;
    else
        return SPELL_CAST_OK;
}

SpellCastResult Spell::CheckItems()
{
    if (m_caster->GetTypeId() != TYPEID_PLAYER)
        return SPELL_CAST_OK;

    Player* p_caster = (Player*)m_caster;

    if (!m_CastItem)
    {
        if (m_castItemGUID)
            return SPELL_FAILED_ITEM_NOT_READY;
    }
    else
    {
        uint32 itemid = m_CastItem->GetEntry();
        if (!p_caster->HasItemCount(itemid, 1))
            return SPELL_FAILED_ITEM_NOT_READY;

        ItemPrototype const *proto = m_CastItem->GetProto();
        if (!proto)
            return SPELL_FAILED_ITEM_NOT_READY;

        for (int i = 0; i < 5; i++)
            if (proto->Spells[i].SpellCharges)
                if (m_CastItem->GetSpellCharges(i) == 0)
                    return SPELL_FAILED_NO_CHARGES_REMAIN;

        // consumable cast item checks
        if (proto->Class == ITEM_CLASS_CONSUMABLE && m_targets.getUnitTarget())
        {
            // such items should only fail if there is no suitable effect at all - see Rejuvenation Potions for example
            SpellCastResult failReason = SPELL_CAST_OK;
            for (int i = 0; i < 3; i++)
            {
                // skip check, pet not required like checks, and for TARGET_PET m_targets.getUnitTarget() is not the real target but the caster
                if (GetSpellEntry()->EffectImplicitTargetA[i] == TARGET_UNIT_CASTER_PET)
                    continue;

                if (GetSpellEntry()->Effect[i] == SPELL_EFFECT_HEAL)
                {
                    if (m_targets.getUnitTarget()->GetHealth() == m_targets.getUnitTarget()->GetMaxHealth())
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_HEALTH;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }

                // Mana Potion, Rage Potion, Thistle Tea(Rogue), ...
                if (GetSpellEntry()->Effect[i] == SPELL_EFFECT_ENERGIZE)
                {
                    if (GetSpellEntry()->EffectMiscValue[i] < 0 || GetSpellEntry()->EffectMiscValue[i] >= MAX_POWERS)
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }

                    Powers power = Powers(GetSpellEntry()->EffectMiscValue[i]);
                    if (m_targets.getUnitTarget()->GetPower(power) == m_targets.getUnitTarget()->GetMaxPower(power))
                    {
                        failReason = SPELL_FAILED_ALREADY_AT_FULL_POWER;
                        continue;
                    }
                    else
                    {
                        failReason = SPELL_CAST_OK;
                        break;
                    }
                }
            }
            if (failReason != SPELL_CAST_OK)
                return failReason;
        }
    }

    // check target item
    if (m_targets.getItemTargetGUID())
    {
        if (m_caster->GetTypeId() != TYPEID_PLAYER)
            return SPELL_FAILED_BAD_TARGETS;

        if (!m_targets.getItemTarget())
            return SPELL_FAILED_ITEM_GONE;

        if (!m_targets.getItemTarget()->IsFitToSpellRequirements(GetSpellEntry()))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }
    // if not item target then required item must be equipped
    else
    {
        if (m_caster->GetTypeId() == TYPEID_PLAYER && !((Player*)m_caster)->HasItemFitToSpellReqirements(GetSpellEntry()))
            return SPELL_FAILED_EQUIPPED_ITEM_CLASS;
    }

    // check spell focus object
    if (GetSpellEntry()->RequiresSpellFocus)
    {
        GameObject* ok = NULL;
        MaNGOS::GameObjectFocusCheck go_check(m_caster, GetSpellEntry()->RequiresSpellFocus);
        MaNGOS::ObjectSearcher<GameObject, MaNGOS::GameObjectFocusCheck> checker(ok, go_check);

        Cell::VisitGridObjects(m_caster, checker, m_caster->GetMap()->GetVisibilityDistance());

        if (!ok || !ok->isSpawned())
            return SPELL_FAILED_REQUIRES_SPELL_FOCUS;

        focusObject = ok;                                   // game object found in range
    }

    // check reagents (ignore triggered spells with reagents processed by original spell) and special reagent ignore case.
    if (!IsTriggeredSpell() && !p_caster->CanNoReagentCast(GetSpellEntry()))
    {
        for (uint32 i = 0; i < 8; i++)
        {
            if (GetSpellEntry()->Reagent[i] <= 0)
                continue;

            uint32 itemid = GetSpellEntry()->Reagent[i];
            uint32 itemcount = GetSpellEntry()->ReagentCount[i];

            // if CastItem is also spell reagent
            if (m_CastItem && m_CastItem->GetEntry() == itemid)
            {
                ItemPrototype const *proto = m_CastItem->GetProto();
                if (!proto)
                    return SPELL_FAILED_ITEM_NOT_READY;
                for (int s = 0; s < MAX_ITEM_PROTO_SPELLS; ++s)
                {
                    // CastItem will be used up and does not count as reagent
                    int32 charges = m_CastItem->GetSpellCharges(s);
                    if (proto->Spells[s].SpellCharges < 0 && abs(charges) < 2)
                    {
                        ++itemcount;
                        break;
                    }
                }
            }
            if (!p_caster->HasItemCount(itemid, itemcount))
                return SPELL_FAILED_ITEM_NOT_READY;         //0x54
        }
    }

    // check totem-item requirements (items presence in inventory)
    uint32 totems = 2;
    for (int i = 0; i < 2; ++i)
    {
        if (GetSpellEntry()->Totem[i] != 0)
        {
            if (p_caster->HasItemCount(GetSpellEntry()->Totem[i], 1))
            {
                totems -= 1;
                continue;
            }
        }
        else
            totems -= 1;
    }
    if (totems != 0)
        return SPELL_FAILED_TOTEMS;                     //0x7C

    // Check items for TotemCategory  (items presence in inventory)
    uint32 TotemCategory = 2;
    for (int i = 0; i < 2; ++i)
    {
        if (GetSpellEntry()->TotemCategory[i] != 0)
        {
            if (p_caster->HasItemTotemCategory(GetSpellEntry()->TotemCategory[i]))
            {
                TotemCategory -= 1;
                continue;
            }
        }
        else
            TotemCategory -= 1;
    }
    if (TotemCategory != 0)
        return SPELL_FAILED_TOTEM_CATEGORY;             //0x7B

    // special checks for spell effects
    for (int i = 0; i < 3; i++)
    {
        switch (GetSpellEntry()->Effect[i])
        {
            case SPELL_EFFECT_APPLY_AURA:                   //}
                if (GetSpellEntry()->Id == 17624)           //}}
                    GetCaster()->RemoveAllAurasOnDeath();   //}}} FIXME: HACK: i have absolutely NO IDEA where to put this. But it works.
                break;                                      //}
            case SPELL_EFFECT_CREATE_ITEM:
            {
                if (!IsTriggeredSpell() && GetSpellEntry()->EffectItemType[i])
                {
                    ItemPosCountVec dest;
                    uint8 msg = p_caster->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, GetSpellEntry()->EffectItemType[i], 1);
                    if (msg != EQUIP_ERR_OK)
                    {
                        p_caster->SendEquipError(msg, NULL, NULL);
                        return SPELL_FAILED_DONT_REPORT;
                    }
                }
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM:
            {
                Item* targetItem = m_targets.getItemTarget();
                if (!targetItem)
                    return SPELL_FAILED_ITEM_NOT_FOUND;

                if (targetItem->GetProto()->ItemLevel < GetSpellEntry()->baseLevel)
                    return SPELL_FAILED_LOWLEVEL;
                SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(GetSpellEntry()->EffectMiscValue[0]);
                if (!pEnchant)
                    return SPELL_FAILED_ERROR;
                // Not allow enchant in trade slot for some enchant type
                if (targetItem->GetOwner() != m_caster && (GetSpellEntry()->AttributesEx2 & (SPELL_ATTR_EX2_NOT_USABLE_VIA_TRADE | SPELL_ATTR_EX2_UNK3)))
                    return SPELL_FAILED_NOT_TRADEABLE;
                break;
            }
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            {
                Item *item = m_targets.getItemTarget();
                if (!item)
                    return SPELL_FAILED_ITEM_NOT_FOUND;
                if (item->GetProto()->ItemLevel < GetSpellEntry()->spellLevel && GetSpellEntry()->SpellFamilyName == SPELLFAMILY_GENERIC)
                        return SPELL_FAILED_LOWLEVEL;
                   
                SpellItemEnchantmentEntry const *pEnchant = sSpellItemEnchantmentStore.LookupEntry(GetSpellEntry()->EffectMiscValue[0]);
                if (!pEnchant)
                    return SPELL_FAILED_ERROR;
                // Not allow enchant in trade slot for some enchant type
                if (item->GetOwner() != m_caster && (GetSpellEntry()->AttributesEx2 & (SPELL_ATTR_EX2_NOT_USABLE_VIA_TRADE | SPELL_ATTR_EX2_UNK3)))
                    return SPELL_FAILED_NOT_TRADEABLE;
                break;
            }
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
                // check item existence in effect code (not output errors at offhand hold item effect to main hand for example
                break;
            case SPELL_EFFECT_DISENCHANT:
            {
                if (!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                // prevent disenchanting in trade slot
                if (m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID())
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                ItemPrototype const* itemProto = m_targets.getItemTarget()->GetProto();
                if (!itemProto)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;

                uint32 item_quality = itemProto->Quality;
                // 2.0.x addon: Check player enchanting level against the item disenchanting requirements
                uint32 item_disenchantskilllevel = itemProto->RequiredDisenchantSkill;
                if (item_disenchantskilllevel == uint32(-1))
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (item_disenchantskilllevel > p_caster->GetSkillValue(SKILL_ENCHANTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                if (item_quality > 4 || item_quality < 2)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (itemProto->Class != ITEM_CLASS_WEAPON && itemProto->Class != ITEM_CLASS_ARMOR)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                if (!itemProto->DisenchantID)
                    return SPELL_FAILED_CANT_BE_DISENCHANTED;
                break;
            }
            case SPELL_EFFECT_PROSPECTING:
            {
                if (!m_targets.getItemTarget())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //ensure item is a prospectable ore
                if (!(m_targets.getItemTarget()->GetProto()->BagFamily & BAG_FAMILY_MASK_MINING_SUPP) || m_targets.getItemTarget()->GetProto()->Class != ITEM_CLASS_TRADE_GOODS)
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //prevent prospecting in trade slot
                if (m_targets.getItemTarget()->GetOwnerGUID() != m_caster->GetGUID())
                    return SPELL_FAILED_CANT_BE_PROSPECTED;
                //Check for enough skill in jewelcrafting
                uint32 item_prospectingskilllevel = m_targets.getItemTarget()->GetProto()->RequiredSkillRank;
                if (item_prospectingskilllevel >p_caster->GetSkillValue(SKILL_JEWELCRAFTING))
                    return SPELL_FAILED_LOW_CASTLEVEL;
                //make sure the player has the required ores in inventory
                if (m_targets.getItemTarget()->GetCount() < 5)
                    return SPELL_FAILED_PROSPECT_NEED_MORE;

                if (!LootTemplates_Prospecting.HaveLootfor(m_targets.getItemTargetEntry()))
                    return SPELL_FAILED_CANT_BE_PROSPECTED;

                break;
            }
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            {
                if (m_caster->GetTypeId() != TYPEID_PLAYER) return SPELL_FAILED_TARGET_NOT_PLAYER;
                if (m_attackType != RANGED_ATTACK)
                    break;
                Item *pItem = ((Player*)m_caster)->GetWeaponForAttack(m_attackType);
                if (!pItem || pItem->IsBroken())
                    return SPELL_FAILED_EQUIPPED_ITEM;

                switch (pItem->GetProto()->SubClass)
                {
                    case ITEM_SUBCLASS_WEAPON_THROWN:
                    {
                        uint32 ammo = pItem->GetEntry();
                        if (!((Player*)m_caster)->HasItemCount(ammo, 1))
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_GUN:
                    case ITEM_SUBCLASS_WEAPON_BOW:
                    case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                    {
                        uint32 ammo = ((Player*)m_caster)->GetUInt32Value(PLAYER_AMMO_ID);
                        if (!ammo)
                        {
                            // Requires No Ammo
                            if (m_caster->GetDummyAura(46699))
                                break;                      // skip other checks

                            return SPELL_FAILED_NO_AMMO;
                        }

                        ItemPrototype const *ammoProto = ObjectMgr::GetItemPrototype(ammo);
                        if (!ammoProto)
                            return SPELL_FAILED_NO_AMMO;

                        if (ammoProto->Class != ITEM_CLASS_PROJECTILE)
                            return SPELL_FAILED_NO_AMMO;

                        // check ammo ws. weapon compatibility
                        switch (pItem->GetProto()->SubClass)
                        {
                            case ITEM_SUBCLASS_WEAPON_BOW:
                            case ITEM_SUBCLASS_WEAPON_CROSSBOW:
                                if (ammoProto->SubClass != ITEM_SUBCLASS_ARROW)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            case ITEM_SUBCLASS_WEAPON_GUN:
                                if (ammoProto->SubClass != ITEM_SUBCLASS_BULLET)
                                    return SPELL_FAILED_NO_AMMO;
                                break;
                            default:
                                return SPELL_FAILED_NO_AMMO;
                        }

                        if (!((Player*)m_caster)->HasItemCount(ammo, 1))
                            return SPELL_FAILED_NO_AMMO;
                    };  break;
                    case ITEM_SUBCLASS_WEAPON_WAND:
                    default:
                        break;
                }
                break;
            }
            default:break;
        }
    }

    return SPELL_CAST_OK;
}

void Spell::Delayed() // only called in DealDamage()
{
    if (!m_caster)// || m_caster->GetTypeId() != TYPEID_PLAYER)
        return;

    //if (m_spellState == SPELL_STATE_DELAYED)
    //    return;                                             // spell is active and can't be time-backed

    // spells not loosing casting time (slam, dynamites, bombs..)
    //if(!(GetSpellEntry()->InterruptFlags & SPELL_INTERRUPT_FLAG_DAMAGE))
    //    return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if (int32(m_timer.GetTimeLeft()) + delaytime > m_casttime) // push back to zero
    {
        delaytime = m_casttime - m_timer.GetTimeLeft();
        m_timer.Reset(m_casttime);
    }
    else
        m_timer.Delay(delaytime); // push back just a moment

    sLog.outDetail("Spell %u partially interrupted for (%d) ms at damage", GetSpellEntry()->Id, delaytime);

    WorldPacket data(SMSG_SPELL_DELAYED, 8 + 4);
    data << m_caster->GetPackGUID();
    data << uint32(delaytime);

    m_caster->BroadcastPacket(&data, true);
}

void Spell::DelayedChannel()
{
    if (!m_caster || m_caster->GetTypeId() != TYPEID_PLAYER || getState() != SPELL_STATE_CASTING)
        return;

    //check resist chance
    int32 resistChance = 100;                               //must be initialized to 100 for percent modifiers
    ((Player*)m_caster)->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_NOT_LOSE_CASTING_TIME, resistChance, this);
    resistChance += m_caster->GetTotalAuraModifier(SPELL_AURA_RESIST_PUSHBACK) - 100;
    if (roll_chance_i(resistChance))
        return;

    int32 delaytime = GetNextDelayAtDamageMsTime();

    if (int32(m_timer.GetTimeLeft()) < delaytime) // channel pushed so much that time is up
    {
        delaytime = m_timer.GetTimeLeft();  
        m_timer = 0;
    }
    else                                    
        m_timer.Update(delaytime); // just skip some msconds of channe

    sLog.outDebug("Spell %u partially interrupted for %i ms, new duration: %u ms", GetSpellEntry()->Id, delaytime, m_timer.GetTimeLeft());

    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if ((*ihit).missCondition == SPELL_MISS_NONE)
        {
            Unit* unit = m_caster->GetGUID() == ihit->targetGUID ? m_caster : m_caster->GetMap()->GetUnit(ihit->targetGUID);
            if (unit)
            {
                for (int j = 0; j < 3; j++)
                    if (ihit->effectMask & (1 << j))
                        unit->DelayAura(GetSpellEntry()->Id, j, delaytime);
            }

        }
    }

    for (int j = 0; j < 3; j++)
    {
        // partially interrupt persistent area auras
        DynamicObject* dynObj = m_caster->GetDynObject(GetSpellEntry()->Id, j);
        if (dynObj)
            dynObj->Delay(delaytime);
    }

    SendChannelUpdate(m_timer.GetTimeLeft());
}

bool Spell::UpdatePointers()
{
    if (m_originalCasterGUID == m_caster->GetGUID())
        m_originalCaster = m_caster;
    else
    {
        m_originalCaster = m_caster->GetMap()->GetUnit(m_originalCasterGUID);
        if (m_originalCaster && !m_originalCaster->IsInWorld()) m_originalCaster = NULL;
    }

    if (m_castItemGUID && m_caster->GetTypeId() == TYPEID_PLAYER)
    {
        m_CastItem = ((Player*)m_caster)->GetItemByGuid(m_castItemGUID);
        // cast item not found, somehow the item is no longer where we expected 
        if (!m_CastItem) 
            return false;
    }

    m_targets.Update(m_caster);
    return true;
}

bool Spell::IsAffectedBy(SpellEntry const *spellInfo, uint32 effectId)
{
    return sSpellMgr.IsAffectedBySpell(GetSpellEntry(), spellInfo->Id, effectId, spellInfo->EffectItemType[effectId]);
}

bool Spell::CheckTargetCreatureType(Unit* target) const
{
    uint32 spellCreatureTargetMask = GetSpellEntry()->TargetCreatureType;

    // Curse of Doom : not find another way to fix spell target check :/
    if (GetSpellEntry()->SpellFamilyName == SPELLFAMILY_WARLOCK && GetSpellEntry()->SpellFamilyFlags == 0x0200000000LL)
    {
        // not allow cast at player
        if (target->isCharmedOwnedByPlayerOrPlayer())
            return false;

        spellCreatureTargetMask = 0x7FF;
    }

    // Dismiss Pet and Taming Lesson skipped and Control Roskipped
    if (GetSpellEntry()->Id == 2641 || GetSpellEntry()->Id == 23356 || GetSpellEntry()->Id == 30009)
        spellCreatureTargetMask = 0;

    if (spellCreatureTargetMask)
    {
        uint32 TargetCreatureType = target->GetCreatureTypeMask();

        return !TargetCreatureType || (spellCreatureTargetMask & TargetCreatureType);
    }
    return true;
}

CurrentSpellTypes Spell::GetCurrentContainer()
{
    if (IsNextMeleeSwingSpell())
        return(CURRENT_MELEE_SPELL);
    else if (IsAutoRepeat())
        return(CURRENT_AUTOREPEAT_SPELL);
    else if (SpellMgr::IsChanneledSpell(GetSpellEntry()))
        return(CURRENT_CHANNELED_SPELL);
    else
        return(CURRENT_GENERIC_SPELL);
}

bool Spell::CanIgnoreNotAttackableFlags()
{
    switch (GetSpellEntry()->Id)
    {
        case 14813:     // Dark Iron Drunk Mug
        case 32958:     // Crystal Channel
        case 44877:     // Living Flare Master
        case 45023:     // Fel Consumption
        case 38482:     // dark conclave ritual disrupt
            return true;
        default:
            return false;
    }
}

bool Spell::CheckTarget(Unit* target, uint32 eff)
{
    if (target->GetCharmerOrOwnerPlayerOrPlayerItself() && !sSpellMgr.IsPositiveEffect(GetSpellEntry()->Id, 0) &&
        target->isInSanctuary() && GetSpellEntry()->Id != 8326 && GetSpellEntry()->Id != 20584) // no non-positive spells in sanctuary, except ghost
        return false;

    if (GetSpellEntry()->Effect[eff] == SPELL_EFFECT_APPLY_AURA && (GetSpellEntry()->EffectImplicitTargetA[eff] == TARGET_UNIT_FRIEND_AND_PARTY ||
        GetSpellEntry()->EffectImplicitTargetA[eff] == TARGET_UNIT_RAID_AND_CLASS) && target->GetLevel() < GetSpellEntry()->spellLevel)
        return false;

    // Check targets for creature type mask and remove not appropriate (skip explicit self target case, maybe need other explicit targets)
    if (GetSpellEntry()->EffectImplicitTargetA[eff] != TARGET_UNIT_CASTER)
    {
        if (!CheckTargetCreatureType(target))
            return false;
    }

    // hack for level req
    switch (GetSpellEntry()->Id)
    {
        // Songflower Serenade
        case 15366:
            // Mol'dar's Moxie
        case 18222:
            // Slip'kik's Savvy
        case 22820:
            // Rallying Cry of the Dragonslayer
        case 22888:
            // Traces of Silithyst
        case 29534:
        {
            if (target->GetLevel() >= 64)
                return false;
        }
    }

    if (GetSpellEntry()->AttributesEx3 & SPELL_ATTR_EX3_PLAYERS_ONLY && target->GetTypeId() != TYPEID_PLAYER &&
        GetSpellEntry()->EffectImplicitTargetA[eff] != TARGET_UNIT_CASTER)
        return false;

    if (GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_CANT_TARGET_SELF && m_caster == target &&
        !(GetSpellEntry()->EffectImplicitTargetA[eff] == TARGET_UNIT_CASTER && GetSpellEntry()->EffectImplicitTargetB[eff] == 0))
        return false;

    // Check targets for not_selectable unit flag and remove
    // A player can cast spells on his pet (or other controlled unit) though in any state
    if (target != m_caster && target->GetCharmerOrOwnerGUID() != m_caster->GetGUID())
    {
        // any unattackable target skipped
        if (!CanIgnoreNotAttackableFlags() && target->HasFlag(UNIT_FIELD_FLAGS, UNIT_FLAG_SPAWNING))
            return false;
    }

    //Check player targets and remove if in GM mode or GM invisibility (for not self casting case)
    if (target != m_caster && target->GetTypeId() == TYPEID_PLAYER)
    {
        if (((Player*)target)->GetVisibility() == VISIBILITY_OFF)
            return false;

        if (((Player*)target)->IsGameMaster() && !SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
            return false;
    }

    //Do not check LOS for triggered spells
    if (IsTriggeredSpell() && !sSpellMgr.IsNotIgnoreTriggeredSpell(GetSpellEntry()) && (!sWorld.getConfig(CONFIG_VMAP_TOTEM) || !m_caster->ToTotem()))
        return true;

    //Check targets for LOS visibility (except spells without range limitations)
    switch (GetSpellEntry()->Effect[eff])
    {
        case SPELL_EFFECT_FRIEND_SUMMON:
        case SPELL_EFFECT_SUMMON_PLAYER:                    // from anywhere
            break;
        case SPELL_EFFECT_DUMMY:
            if (GetSpellEntry()->Id != 20577)                      // Cannibalize
                break;
            //fall through
        case SPELL_EFFECT_RESURRECT:
        case SPELL_EFFECT_RESURRECT_NEW:
            // player far away, maybe his corpse near?
            if (target != m_caster && !SpellMgr::SpellIgnoreLOS(GetSpellEntry(), eff) && !target->IsWithinLOSInMap(m_caster))
            {
                if (!m_targets.getCorpseTargetGUID())
                    return false;

                Corpse *corpse = ObjectAccessor::GetCorpse(*m_caster, m_targets.getCorpseTargetGUID());
                if (!corpse)
                    return false;

                if (target->GetGUID() != corpse->GetOwnerGUID())
                    return false;

                if (!SpellMgr::SpellIgnoreLOS(GetSpellEntry(), eff) && !corpse->IsWithinLOSInMap(m_caster))
                    return false;
            }

            // all ok by some way or another, skip normal check
            break;
        default:
            if (target == m_caster || SpellMgr::SpellIgnoreLOS(GetSpellEntry(), eff))
                return true;

            // AOE spells with destination
            if ((sSpellMgr.SpellTargetType[GetSpellEntry()->EffectImplicitTargetA[eff]] == TARGET_TYPE_AREA_DST ||
                sSpellMgr.SpellTargetType[GetSpellEntry()->EffectImplicitTargetB[eff]] == TARGET_TYPE_AREA_DST) &&
                m_targets.HasDst())
            {
                //caster and victim must see spell destination, not nesescary see each other
                if (target->IsInMap(m_caster) &&
                    target->IsWithinLOS(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ) &&
                    m_caster->IsWithinLOS(m_targets.m_destX, m_targets.m_destY, m_targets.m_destZ))
                    return true;
            }
            else if (!target->IsWithinLOSInMap(m_caster)) // normal case
                return false;

            break;
    }

    return true;
}

Unit* Spell::SelectMagnetTarget() // Grounding totem | Intervene
{
    Unit* target = m_targets.getUnitTarget();

    if (!target)
        return NULL;

    if (GetSpellEntry()->DmgClass == SPELL_DAMAGE_CLASS_MAGIC)
    {
        if (GetSpellEntry()->Attributes & (SPELL_ATTR_ABILITY | SPELL_ATTR_UNAFFECTED_BY_INVULNERABILITY) || GetSpellEntry()->AttributesEx & SPELL_ATTR_EX_CANT_BE_REDIRECTED)
            return target;

        if (target->HasAuraType(SPELL_AURA_SPELL_MAGNET))
        {
            Unit::AuraList const& magnetAuras = target->GetAurasByType(SPELL_AURA_SPELL_MAGNET);
            for (Unit::AuraList::const_iterator itr = magnetAuras.begin(); itr != magnetAuras.end(); ++itr)
            {
                if (Unit* magnet = (*itr)->GetCaster())
                {
                    if ((*itr)->m_procCharges>0)
                    {
                        (*itr)->SetAuraProcCharges((*itr)->m_procCharges-1);
                        target = magnet;
                        m_targets.setUnitTarget(target);
                        AddUnitTarget(target, 0, true);
                        uint64 targetGUID = target->GetGUID();
                        for (std::list<TargetInfo>::iterator ihit= m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
                        {
                            if (ihit->deleted)
                                continue;

                            if (targetGUID == ihit->targetGUID)                 // Found in list
                            {
                                (*ihit).damage = target->GetHealth();
                                break;
                            }
                        }
                        break;
                    }
                }
            }
        }
    }
    else if(GetSpellEntry()->DmgClass == SPELL_DAMAGE_CLASS_MELEE || GetSpellEntry()->DmgClass == SPELL_DAMAGE_CLASS_RANGED)
    {
        if (target->HasAuraType(SPELL_AURA_ADD_CASTER_HIT_TRIGGER))
        {
            Unit::AuraList const& hitTriggerAuras = target->GetAurasByType(SPELL_AURA_ADD_CASTER_HIT_TRIGGER);
            for (Unit::AuraList::const_iterator itr = hitTriggerAuras.begin(); itr != hitTriggerAuras.end(); ++itr)
            {
                if (Unit* hitTarget = (*itr)->GetCaster())
                {
                    if ((*itr)->m_procCharges > 0)
                    {
                        (*itr)->SetAuraProcCharges((*itr)->m_procCharges-1);
                        (*itr)->UpdateAuraCharges();
                        if ((*itr)->m_procCharges <= 0)
                            target->RemoveAurasByCasterSpell((*itr)->GetId(), (*itr)->GetCasterGUID());
                    }
                    target = hitTarget;
                    m_targets.setUnitTarget(target);
                    AddUnitTarget(target, 0, true);
                    break;
                }
            }
        }
    }
    return target;
}

void Spell::CheckForReflects()
{
    if (SpellMgr::IsPositiveSpell(GetSpellEntry()->Id))
        return;

    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        if (ihit->missCondition != SPELL_MISS_NONE)
            continue;

        Unit* itrtarget = m_caster->GetUnit(ihit->targetGUID);

        if (!itrtarget)
            continue;

        ihit->missCondition = itrtarget->SpellReflectCheck(GetSpellEntry());
        if (ihit->missCondition == SPELL_MISS_NONE)
            continue;

        m_countOfHit--;
        m_countOfMiss++;

        ihit->reflectResult = m_caster->SpellHitResult(m_caster, GetSpellEntry());

        if (ihit->reflectResult == SPELL_MISS_REFLECT)     // Impossible reflect again, so simply deflect spell
            ihit->reflectResult = SPELL_MISS_PARRY;

        // Increase time interval for reflected spells by 1.5
        ihit->timeDelay += ihit->timeDelay >> 1;
    }
}

bool Spell::IsNeedSendToClient() const
{
    return GetSpellEntry()->SpellVisual != 0 || SpellMgr::IsChanneledSpell(GetSpellEntry()) ||
        GetSpellEntry()->speed > 0.0f || !m_triggeredByAuraSpell && !IsTriggeredSpell();
}

bool Spell::HaveTargetsForEffect(uint8 effect) const
{
    for (std::list<TargetInfo>::const_iterator itr = m_UniqueTargetInfo.begin(); itr != m_UniqueTargetInfo.end(); ++itr)
    {
        if (itr->deleted)
            continue;

        if (itr->effectMask & (1 << effect))
            return true;
    }

    for (std::list<GOTargetInfo>::const_iterator itr = m_UniqueGOTargetInfo.begin(); itr != m_UniqueGOTargetInfo.end(); ++itr)
    {
        if (itr->deleted)
            continue;

        if (itr->effectMask & (1 << effect))
            return true;
    }

    for (std::list<ItemTargetInfo>::const_iterator itr = m_UniqueItemInfo.begin(); itr != m_UniqueItemInfo.end(); ++itr)
        if (itr->effectMask & (1 << effect))
            return true;

    return false;
}

SpellEvent::SpellEvent(Spell* spell) : BasicEvent()
{
    m_Spell = spell;
}

SpellEvent::~SpellEvent()
{
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel(SPELL_FAILED_INT_DESTROY_SPELLEVENT);

    if (m_Spell->IsDeletable())
    {
        delete m_Spell;
    }
    else
    {
        sLog.outLog(LOG_DEFAULT, "ERROR: ~SpellEvent: %s %u tried to delete non-deletable spell %u. Was not deleted, causes memory leak.",
            (m_Spell->GetCaster()->GetTypeId() == TYPEID_PLAYER ? "Player" : "Creature"), m_Spell->GetCaster()->GetGUIDLow(), m_Spell->GetSpellEntry()->Id);
    }
}

bool SpellEvent::Execute(uint64 e_time, uint32 p_time)
{
    // update spell if it is not finished
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->update(p_time);

    // check spell state to process
    switch (m_Spell->getState())
    {
        case SPELL_STATE_FINISHED:
        {
            // spell was finished, check deletable state
            if (m_Spell->IsDeletable())
            {
                // check, if we do have unfinished triggered spells

                return(true);                               // spell is deletable, finish event
            }
            // event will be re-added automatically at the end of routine)
        } break;

        case SPELL_STATE_DELAYED:
        {
            // first, check, if we have just started
            if (m_Spell->GetDelayStart() != 0)
            {
                // no, we aren't, do the typical update
                // check, if we have channeled spell on our hands
                if (SpellMgr::IsChanneledSpell(m_Spell->GetSpellEntry()))
                {
                    // evented channeled spell is processed separately, cast once after delay, and not destroyed till finish
                    // check, if we have casting anything else except this channeled spell and autorepeat
                    if (m_Spell->GetCaster()->IsNonMeleeSpellCast(false, true, true))
                    {
                        // another non-melee non-delayed spell is cast now, abort
                        m_Spell->cancel(SPELL_FAILED_INT_BY_OTHER_CAST);
                    }
                    // Check if target of channeled spell still in range
                    else if (m_Spell->CheckRange(false))
                        m_Spell->cancel(SPELL_FAILED_INT_CHANNEL_RANGE);
                    else
                    {
                        // do the action (pass spell to channeling state)
                        m_Spell->handle_immediate();
                    }
                    // event will be re-added automatically at the end of routine)
                }
                else
                {
                    // run the spell handler and think about what we can do next
                    uint64 t_offset = e_time - m_Spell->GetDelayStart();
                    uint64 n_offset = m_Spell->handle_delayed(t_offset);
                    if (n_offset)
                    {
                        // re-add us to the queue
                        m_Spell->GetCaster()->AddEvent(this, m_Spell->GetDelayStart() + n_offset, false);
                        return(false);                      // event not complete
                    }
                    // event complete
                    // finish update event will be re-added automatically at the end of routine)
                }
            }
            else
            {
                // delaying had just started, record the moment
                m_Spell->SetDelayStart(e_time);
                // re-plan the event for the delay moment
                m_Spell->GetCaster()->AddEvent(this, e_time + m_Spell->GetDelayMoment(), false);
                return(false);                              // event not complete
            }
        } break;

        default:
        {
            // all other states
            // event will be re-added automatically at the end of routine)
        } break;
    }

    // spell processing not complete, plan event on the next update interval
    m_Spell->GetCaster()->AddEvent(this, e_time + 1, false);
    return(false);                                          // event not complete
}

void SpellEvent::Abort(uint64 /*e_time*/)
{
    // oops, the spell we try to do is aborted
    if (m_Spell->getState() != SPELL_STATE_FINISHED)
        m_Spell->cancel(SPELL_FAILED_INT_ABORT_SPELLEVENT);
}

bool SpellEvent::IsDeletable() const
{
    return m_Spell->IsDeletable();
}

SpellCastResult Spell::CanOpenLock(uint32 effIndex, uint32 lockId, SkillType& skillId, int32& reqSkillValue, int32& skillValue)
{
    if (!lockId)                                             // possible case for GO and maybe for items.
        return SPELL_CAST_OK;

    // Get LockInfo
    LockEntry const *lockInfo = sLockStore.LookupEntry(lockId);

    if (!lockInfo)
        return SPELL_FAILED_BAD_TARGETS;

    bool reqKey = false;                                    // some locks not have reqs

    for (int j = 0; j < MAX_LOCK_CASE; ++j)
    {
        switch (lockInfo->Type[j])
        {
            // check key item (many fit cases can be)
            case LOCK_KEY_ITEM:
                if (lockInfo->Index[j] && m_CastItem && m_CastItem->GetEntry() == lockInfo->Index[j])
                    return SPELL_CAST_OK;
                reqKey = true;
                break;
                // check key skill (only single first fit case can be)
            case LOCK_KEY_SKILL:
            {
                reqKey = true;

                // wrong locktype, skip
                if (uint32(GetSpellEntry()->EffectMiscValue[effIndex]) != lockInfo->Index[j])
                    continue;

                skillId = SkillByLockType(LockType(lockInfo->Index[j]));

                if (skillId != SKILL_NONE)
                {
                    // skill bonus provided by casting spell (mostly item spells)
                    // add the damage modifier from the spell cast (cheat lock / skeleton key etc.) (use m_currentBasePoints, CalculateDamage returns wrong value)
                    uint32 spellSkillBonus = uint32(m_currentBasePoints[effIndex] + 1);
                    reqSkillValue = lockInfo->Skill[j];

                    // castitem check: rogue using skeleton keys. the skill values should not be added in this case.
                    skillValue = m_CastItem || m_caster->GetTypeId() != TYPEID_PLAYER ?
                        0 : ((Player*)m_caster)->GetSkillValue(skillId);

                    skillValue += spellSkillBonus;

                    if (skillValue < reqSkillValue)
                        return SPELL_FAILED_LOW_CASTLEVEL;
                }

                return SPELL_CAST_OK;
            }
        }
    }

    if (reqKey)
        return SPELL_FAILED_BAD_TARGETS;

    return SPELL_CAST_OK;
}

bool Spell::IsValidSingleTargetEffect(Unit const* target, Targets type) const
{
    switch (type)
    {
        case TARGET_UNIT_ENEMY:
            return !m_caster->IsFriendlyTo(target);
        case TARGET_UNIT_FRIEND:
        case TARGET_UNIT_FRIEND_AND_PARTY:
            return m_caster->IsFriendlyTo(target);
        case TARGET_UNIT_PARTY:
            return m_caster != target && m_caster->IsInPartyWith(target);
        case TARGET_UNIT_RAID:
            return m_caster->IsInRaidWith(target);
    }
    return true;
}

bool Spell::IsValidSingleTargetSpell(Unit const* target) const
{
    for (int i = 0; i < 3; ++i)
    {
        if (!IsValidSingleTargetEffect(target, Targets(GetSpellEntry()->EffectImplicitTargetA[i])))
            return false;
        // Need to check B?
        //if(!IsValidSingleTargetEffect(GetSpellEntry()->EffectImplicitTargetB[i], target)
        //    return false;
    }
    return true;
}

void Spell::CalculateDamageDoneForAllTargets()
{
    float multiplier[3];
    for (int i = 0; i < 3; ++i)
    {
        if (m_applyMultiplierMask & (1 << i))
        {
            // Get multiplier
            multiplier[i] = GetSpellEntry()->DmgMultiplier[i];
            // Apply multiplier mods
            if (m_originalCaster)
                if (Player* modOwner = m_originalCaster->GetSpellModOwner())
                    modOwner->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_EFFECT_PAST_FIRST, multiplier[i], this);
        }
    }

    for (std::list<TargetInfo>::iterator ihit = m_UniqueTargetInfo.begin(); ihit != m_UniqueTargetInfo.end(); ++ihit)
    {
        if (ihit->deleted)
            continue;

        TargetInfo &target = *ihit;

        uint32 mask = target.effectMask;
        if (!mask)
            continue;

        Unit* unit = m_caster->GetGUID() == target.targetGUID ? m_caster : m_caster->GetMap()->GetUnit(target.targetGUID);
        if (!unit)
            continue;

        if (target.missCondition == SPELL_MISS_NONE)                          // In case spell hit target, do all effect on that target
        {
            target.damage += CalculateDamageDone(unit, mask, multiplier);
            if (m_originalCaster)
                target.crit = m_originalCaster->isSpellCrit(unit, GetSpellEntry(), m_spellSchoolMask, m_attackType, m_extraCrit);
            else
                target.crit = m_caster->isSpellCrit(unit, GetSpellEntry(), m_spellSchoolMask, m_attackType, m_extraCrit);
        }
        else if (target.missCondition == SPELL_MISS_REFLECT)                // In case spell reflect from target, do all effect on caster (if hit)
        {
            if (target.reflectResult == SPELL_MISS_NONE)       // If reflected spell hit caster -> do all effect on him
            {
                target.damage += CalculateDamageDone(m_caster, mask, multiplier);
                target.crit = m_caster->isSpellCrit(m_caster, GetSpellEntry(), m_spellSchoolMask, m_attackType, m_extraCrit);
            }
        }
    }
}

int32 Spell::CalculateDamageDone(Unit *unit, const uint32 effectMask, float *multiplier)
{
    int32 damageDone = 0;
    unitTarget = unit;
    for (uint32 i = 0; i < 3; ++i)
    {
        if (effectMask & (1 << i))
        {
            m_damage = 0;
            damage = CalculateDamage(i, NULL);

            switch (GetSpellEntry()->Effect[i])
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                    SpellDamageSchoolDmg(i);
                    break;
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                    SpellDamageWeaponDmg(i);
                    break;
                case SPELL_EFFECT_HEAL:
                    SpellDamageHeal(i);
                    break;
            }

            if (m_damage > 0)
            {
                if (IsAreaEffectTarget[GetSpellEntry()->EffectImplicitTargetA[i]] || IsAreaEffectTarget[GetSpellEntry()->EffectImplicitTargetB[i]]
                    || GetSpellEntry()->Effect[i] == SPELL_EFFECT_APPLY_AREA_AURA_ENEMY)
                {
                    if (int32 reducedPct = unit->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_AOE_DAMAGE_AVOIDANCE))
                        m_damage = m_damage * (100 + reducedPct) / 100;
                }
            }
            if (m_applyMultiplierMask & (1 << i))
            {
                m_damage *= m_damageMultipliers[i];
                m_damageMultipliers[i] *= multiplier[i];
            }

            damageDone += m_damage;
        }
    }

    return damageDone;
}

void Spell::SetSpellValue(SpellValueMod mod, int32 value)
{
    switch (mod)
    {
        case SPELLVALUE_BASE_POINT0:
            m_spellValue->EffectBasePoints[0] = value - int32(GetSpellEntry()->EffectBaseDice[0]);
            m_currentBasePoints[0] = m_spellValue->EffectBasePoints[0]; //this should be removed in the future
            break;
        case SPELLVALUE_BASE_POINT1:
            m_spellValue->EffectBasePoints[1] = value - int32(GetSpellEntry()->EffectBaseDice[1]);
            m_currentBasePoints[1] = m_spellValue->EffectBasePoints[1];
            break;
        case SPELLVALUE_BASE_POINT2:
            m_spellValue->EffectBasePoints[2] = value - int32(GetSpellEntry()->EffectBaseDice[2]);
            m_currentBasePoints[2] = m_spellValue->EffectBasePoints[2];
            break;
        case SPELLVALUE_MAX_TARGETS:
            m_spellValue->MaxAffectedTargets = (uint32)value;
            break;
    }
}

enum GCDLimits
{
    MIN_GCD = 1000,
    MAX_GCD = 1500
};

bool Spell::HasGlobalCooldown()
{
    Player* owner = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner)
        return false;
    return owner->GetCooldownMgr().HasGlobalCooldown((m_caster->GetTypeId() != TYPEID_PLAYER) ? PETS_GCD_CATEGORY: GetSpellEntry()->StartRecoveryCategory);
}

void Spell::TriggerGlobalCooldown()
{
    Player* owner = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner)
        return;
    int32 gcd = GetSpellEntry()->StartRecoveryTime;
    if (!gcd && m_caster->GetTypeId() != TYPEID_PLAYER && !m_caster->ToTotem())
        gcd = MAX_GCD; // force pet spells to have gcd (enslave/MC abusing)

    if (!gcd)
        return;

    // Global cooldown can't leave range 1..1.5 secs
    // There are some spells (mostly not cast directly by player) that have < 1 sec and > 1.5 sec global cooldowns
    // but as tests show are not affected by any spell mods.
    if (gcd >= MIN_GCD && gcd <= MAX_GCD)
    {
        // gcd modifier auras are applied only to own spells and only players have such mods
        //if (m_caster->GetTypeId() == TYPEID_PLAYER)
        //    ((Player*)m_caster)->ApplySpellMod(GetSpellEntry()->Id, SPELLMOD_GLOBAL_COOLDOWN, gcd, this);

        // Apply haste rating
        gcd = int32(float(gcd) * m_caster->GetFloatValue(UNIT_MOD_CAST_SPEED));
        if (gcd < MIN_GCD)
            gcd = MIN_GCD;
        else if (gcd > MAX_GCD)
            gcd = MAX_GCD;
    }
    owner->GetCooldownMgr().AddGlobalCooldown((m_caster->GetTypeId() != TYPEID_PLAYER) ? PETS_GCD_CATEGORY : GetSpellEntry()->StartRecoveryCategory, gcd);
}

void Spell::CancelGlobalCooldown()
{
    Player* owner = m_caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner)
        return;

    if (!GetSpellEntry()->StartRecoveryTime)
        return;

    // Cancel global cooldown when interrupting current cast
    if (m_caster->m_currentSpells[CURRENT_GENERIC_SPELL] != this)
        return;

    owner->GetCooldownMgr().CancelGlobalCooldown(GetSpellEntry()->StartRecoveryCategory);
}
