// Copyright 2024 Black Tek Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "monster.h"
#include "game.h"
#include "spells.h"
#include "events.h"
#include "configmanager.h"
#include "iologindata.h"

extern Game g_game;
extern Monsters g_monsters;
extern Events* g_events;
extern ConfigManager g_config;

int32_t Monster::despawnRange;
int32_t Monster::despawnRadius;

uint32_t Monster::monsterAutoID = 0x40000000;

MonsterPtr Monster::createMonster(const std::string& name)
{
	const auto& mType = g_monsters.getMonsterType(name);
	if (!mType) {
		return nullptr;
	}
	return std::make_shared<Monster>(mType);
}

Monster::Monster(MonsterType* mType) :
	nameDescription(mType->nameDescription),
	mType(mType)
{
	defaultOutfit = mType->info.outfit;
	currentOutfit = mType->info.outfit;
	skull = mType->info.skull;
	health = mType->info.health;
	healthMax = mType->info.healthMax;
	baseSpeed = mType->info.baseSpeed;
	internalLight = mType->info.light;
	hiddenHealth = mType->info.hiddenHealth;
	targetList.reserve(24);

	// register creature events
	for (const std::string& scriptName : mType->info.scripts) {
		if (!registerCreatureEvent(scriptName)) {
			std::cout << "[Warning - Monster::Monster] Unknown event name: " << scriptName << std::endl;
		}
	}
}

Monster::~Monster()
{
	clearTargetList();
	clearFriendList();
}

void Monster::addList()
{
	g_game.addMonster(getMonster());
}

void Monster::removeList()
{
	g_game.removeMonster(getMonster());
}

const std::string& Monster::getName() const
{
	if (name.empty()) {
		return mType->name;
	}
	return name;
}

void Monster::setName(const std::string& name)
{
	if (getName() == name) {
		return;
	}

	this->name = name;

	// NOTE: Due to how client caches known creatures,
	// it is not feasible to send creature update to everyone that has ever met it
	CreatureConstPtr constMe = this->getCreature();
	g_game.notifySpectators(constMe);
}

const std::string& Monster::getNameDescription() const
{
	if (nameDescription.empty()) {
		return mType->nameDescription;
	}
	return nameDescription;
}

bool Monster::canSee(const Position& pos) const
{
	return Creature::canSee(getPosition(), pos, 9, 9);
}

bool Monster::canWalkOnFieldType(const CombatType_t combatType) const
{
	switch (combatType) {
		case COMBAT_ENERGYDAMAGE:
			return mType->info.canWalkOnEnergy;
		case COMBAT_FIREDAMAGE:
			return mType->info.canWalkOnFire;
		case COMBAT_EARTHDAMAGE:
			return mType->info.canWalkOnPoison;
		default:
			return true;
	}
}

void Monster::onAttackedCreatureDisappear(bool)
{
	attackTicks = 0;
}

void Monster::onCreatureAppear(const CreaturePtr& creature, const bool isLogin)
{
	Creature::onCreatureAppear(creature, isLogin);

	if (mType->info.creatureAppearEvent != -1) {
		// onCreatureAppear(self, creature)
		LuaScriptInterface* scriptInterface = mType->info.scriptInterface;
		if (!scriptInterface->reserveScriptEnv()) {
			std::cout << "[Error - Monster::onCreatureAppear] Call stack overflow" << std::endl;
			return;
		}

		ScriptEnvironment* env = scriptInterface->getScriptEnv();
		env->setScriptId(mType->info.creatureAppearEvent, scriptInterface);

		lua_State* L = scriptInterface->getLuaState();
		scriptInterface->pushFunction(mType->info.creatureAppearEvent);

		LuaScriptInterface::pushSharedPtr(L, getMonster());
		LuaScriptInterface::setMetatable(L, -1, "Monster");

		LuaScriptInterface::pushSharedPtr(L, creature);
		LuaScriptInterface::setCreatureMetatable(L, -1, creature);

		if (scriptInterface->callFunction(2)) {
			return;
		}
	}

	if (creature == getCreature()) {
		//We just spawned lets look around to see who is there.
		if (isSummon()) {
			isMasterInRange = canSee(getMaster()->getPosition());
		}

		updateTargetList();
		updateIdleStatus();
	} else {
		onCreatureEnter(creature);
	}
}

void Monster::onRemoveCreature(const CreaturePtr& creature, const bool isLogout)
{
	Creature::onRemoveCreature(creature, isLogout);

	if (mType->info.creatureDisappearEvent != -1) {
		// onCreatureDisappear(self, creature)
		LuaScriptInterface* scriptInterface = mType->info.scriptInterface;
		if (!scriptInterface->reserveScriptEnv()) {
			std::cout << "[Error - Monster::onCreatureDisappear] Call stack overflow" << std::endl;
			return;
		}

		ScriptEnvironment* env = scriptInterface->getScriptEnv();
		env->setScriptId(mType->info.creatureDisappearEvent, scriptInterface);

		lua_State* L = scriptInterface->getLuaState();
		scriptInterface->pushFunction(mType->info.creatureDisappearEvent);

		LuaScriptInterface::pushSharedPtr(L, getMonster());
		LuaScriptInterface::setMetatable(L, -1, "Monster");

		LuaScriptInterface::pushSharedPtr(L, creature);
		LuaScriptInterface::setCreatureMetatable(L, -1, creature);

		if (scriptInterface->callFunction(2)) {
			return;
		}
	}

	if (creature == this->getCreature()) {
		if (spawn) {
			spawn->startSpawnCheck();
		}

		setIdle(true);
	} else {
		onCreatureLeave(creature);
	}
}

void Monster::onCreatureMove(const CreaturePtr& creature, const TilePtr& newTile, const Position& newPos,
                             const TilePtr& oldTile, const Position& oldPos, bool teleport)
{
	Creature::onCreatureMove(creature, newTile, newPos, oldTile, oldPos, teleport);

	if (mType->info.creatureMoveEvent != -1) {
		// onCreatureMove(self, creature, oldPosition, newPosition)
		LuaScriptInterface* scriptInterface = mType->info.scriptInterface;
		if (!scriptInterface->reserveScriptEnv()) {
			std::cout << "[Error - Monster::onCreatureMove] Call stack overflow" << std::endl;
			return;
		}

		ScriptEnvironment* env = scriptInterface->getScriptEnv();
		env->setScriptId(mType->info.creatureMoveEvent, scriptInterface);

		lua_State* L = scriptInterface->getLuaState();
		scriptInterface->pushFunction(mType->info.creatureMoveEvent);

		LuaScriptInterface::pushSharedPtr(L, getMonster());
		LuaScriptInterface::setMetatable(L, -1, "Monster");

		LuaScriptInterface::pushSharedPtr(L, creature);
		LuaScriptInterface::setCreatureMetatable(L, -1, creature);

		LuaScriptInterface::pushPosition(L, oldPos);
		LuaScriptInterface::pushPosition(L, newPos);

		if (scriptInterface->callFunction(4)) {
			return;
		}
	}

	if (creature == this->getCreature()) {
		if (isSummon()) {
			isMasterInRange = canSee(getMaster()->getPosition());
		}

		updateTargetList();
		updateIdleStatus();
	} else {
		const bool canSeeNewPos = canSee(newPos);
		const bool canSeeOldPos = canSee(oldPos);

		if (canSeeNewPos && !canSeeOldPos) {
			onCreatureEnter(creature);
		} else if (!canSeeNewPos && canSeeOldPos) {
			onCreatureLeave(creature);
		}

		if (canSeeNewPos && isSummon() && getMaster() == creature) {
			isMasterInRange = true; //Follow master again
		}

		updateIdleStatus();

		if (!isSummon()) {
			if (getFollowCreature()) {
				const Position& followPosition = getFollowCreature()->getPosition();
				const Position& position = getPosition();

				const int32_t offset_x = Position::getDistanceX(followPosition, position);
				const int32_t offset_y = Position::getDistanceY(followPosition, position);
				if ((offset_x > 1 || offset_y > 1) && mType->info.changeTargetChance > 0) {
					Direction dir = getDirectionTo(position, followPosition);
					const Position& checkPosition = getNextPosition(dir, position);

					if (const auto& tile = g_game.map.getTile(checkPosition)) {
						const auto& topCreature = tile->getTopCreature();
						if (topCreature && getFollowCreature() != topCreature && isOpponent(topCreature)) {
							selectTarget(topCreature);
						}
					}
				}
			} else if (isOpponent(creature)) {
				//we have no target lets try pick this one
				selectTarget(creature);
			}
		}
	}
}

void Monster::onCreatureSay(const CreaturePtr& creature, SpeakClasses type, const std::string& text)
{
	Creature::onCreatureSay(creature, type, text);

	if (mType->info.creatureSayEvent != -1) {
		// onCreatureSay(self, creature, type, message)
		LuaScriptInterface* scriptInterface = mType->info.scriptInterface;
		if (!scriptInterface->reserveScriptEnv()) {
			std::cout << "[Error - Monster::onCreatureSay] Call stack overflow" << std::endl;
			return;
		}

		ScriptEnvironment* env = scriptInterface->getScriptEnv();
		env->setScriptId(mType->info.creatureSayEvent, scriptInterface);

		lua_State* L = scriptInterface->getLuaState();
		scriptInterface->pushFunction(mType->info.creatureSayEvent);

		LuaScriptInterface::pushSharedPtr(L, getMonster());
		LuaScriptInterface::setMetatable(L, -1, "Monster");

		LuaScriptInterface::pushSharedPtr(L, creature);
		LuaScriptInterface::setCreatureMetatable(L, -1, creature);

		lua_pushinteger(L, type);
		LuaScriptInterface::pushString(L, text);

		scriptInterface->callVoidFunction(4);
	}
}

void Monster::addFriend(const CreaturePtr& creature)
{
	assert(creature != this->getCreature());
	const auto& result = friendList.insert(creature);
}

void Monster::removeFriend(const CreaturePtr& creature)
{
	if (auto it = friendList.find(creature); it != friendList.end()) {
		friendList.erase(it);
	}
}

void Monster::addTarget(const CreaturePtr& creature, bool pushFront /* = false */)
{
	assert(creature != this->getCreature());

	// Ensure the creature is not already in targetList
	auto it = std::ranges::find_if(targetList, [&](const CreatureWeakPtr& weakTarget) {
		auto target = weakTarget.lock();
		return target && target == creature;
		});

	if (it == targetList.end()) {
		if (pushFront) {
			targetList.insert(targetList.begin(), creature);
		}
		else {
			targetList.push_back(creature);
		}
	}
}

void Monster::removeTarget(const CreaturePtr& creature)
{
	auto it = std::ranges::find_if(targetList, [&](const CreatureWeakPtr& weakTarget) {
		auto target = weakTarget.lock();
		return target && target == creature;
		});

	if (it != targetList.end()) {
		targetList.erase(it);
	}
}


void Monster::updateTargetList()
{
	auto friendIterator = friendList.begin();
	while (friendIterator != friendList.end()) {
		const auto& creature = *friendIterator;
		if (creature->getHealth() <= 0 || !canSee(creature->getPosition())) {
			friendIterator = friendList.erase(friendIterator);
		}
		else {
			++friendIterator;
		}
	}

	// Clean up targetList with expired or invalid targets
	auto targetIterator = targetList.begin();
	while (targetIterator != targetList.end()) {
		CreaturePtr creature = targetIterator->lock();
		if (!creature || creature->getHealth() <= 0 || !canSee(creature->getPosition())) {
			targetIterator = targetList.erase(targetIterator); // Remove expired/null entries
		}
		else {
			++targetIterator;
		}
	}

	// Update with new spectators
	SpectatorVec spectators;
	g_game.map.getSpectators(spectators, position, true);
	spectators.erase(this->getCreature());
	for (const auto& spectator : spectators) {
		onCreatureFound(spectator);
	}
}

void Monster::clearTargetList()
{
	targetList.clear();
}

void Monster::clearFriendList()
{
	friendList.clear();
}

void Monster::onCreatureFound(const CreaturePtr& creature, bool pushFront/* = false*/)
{
	if (!creature) {
		return;
	}

	if (!canSee(creature->getPosition())) {
		return;
	}

	if (isFriend(creature)) {
		addFriend(creature);
	}

	if (isOpponent(creature)) {
		addTarget(creature, pushFront);
	}

	updateIdleStatus();
}

void Monster::onCreatureEnter(const CreaturePtr& creature)
{
	// std::cout << "onCreatureEnter - " << creature->getName() << std::endl;

	if (getMaster() == creature) {
		//Follow master again
		isMasterInRange = true;
	}

	onCreatureFound(creature, true);
}

bool Monster::isFriend(const CreatureConstPtr& creature) const
{
	if (isSummon() && getMaster()->getPlayer()) {
		const auto& masterPlayer = getMaster()->getPlayer();
		PlayerConstPtr tmpPlayer = nullptr;

		if (creature->getPlayer()) {
			tmpPlayer = creature->getPlayer();
		} else {
			const auto& creatureMaster = creature->getMaster();

			if (creatureMaster && creatureMaster->getPlayer()) {
				tmpPlayer = creatureMaster->getPlayer();
			}
		}

		if (tmpPlayer && (tmpPlayer == getMaster() || masterPlayer->isPartner(tmpPlayer))) {
			return true;
		}
	} else if (creature->getMonster() && !creature->isSummon()) {
		return true;
	}

	return false;
}

bool Monster::isOpponent(const CreatureConstPtr& creature) const
{
	if (isSummon() && getMaster()->getPlayer()) {
		if (creature != getMaster()) {
			return true;
		}
	} else {
		if ((creature->getPlayer() && !creature->getPlayer()->hasFlag(PlayerFlag_IgnoredByMonsters)) ||
		        (creature->getMaster() && creature->getMaster()->getPlayer())) {
			return true;
		}
	}

	return false;
}

void Monster::onCreatureLeave(const CreaturePtr& creature)
{
	// std::cout << "onCreatureLeave - " << creature->getName() << std::endl;

	if (getMaster() == creature) {
		//Take random steps and only use defense abilities (e.g. heal) until its master comes back
		isMasterInRange = false;
	}

	//update friendList
	if (isFriend(creature)) {
		removeFriend(creature);
	}

	//update targetList
	if (isOpponent(creature)) {
		removeTarget(creature);
		updateIdleStatus();

		if (!isSummon() && targetList.empty()) {
			int32_t walkToSpawnRadius = g_config.getNumber(ConfigManager::DEFAULT_WALKTOSPAWNRADIUS);
			if (walkToSpawnRadius > 0 && !Position::areInRange(position, masterPos, walkToSpawnRadius, walkToSpawnRadius)) {
				walkToSpawn();
			}
		}
	}
}

bool Monster::searchTarget(TargetSearchType_t searchType /*= TARGETSEARCH_DEFAULT*/)
{
	std::list<CreaturePtr> resultList;
	const Position& myPos = getPosition();

	// First, collect valid targets from targetList
	for (const auto& weakCreature : targetList) {
		CreaturePtr creature = weakCreature.lock();
		if (creature && getFollowCreature() != creature && isTarget(creature)) {
			if (searchType == TARGETSEARCH_RANDOM || canUseAttack(myPos, creature)) {
				resultList.push_back(creature);
			}
		}
	}

	switch (searchType) {
	case TARGETSEARCH_NEAREST: {
		CreaturePtr target = nullptr;

		if (!resultList.empty()) {
			auto it = resultList.begin();
			target = *it;

			if (++it != resultList.end()) {
				const Position& targetPosition = target->getPosition();
				int32_t minRange = Position::getDistanceX(myPos, targetPosition) + Position::getDistanceY(myPos, targetPosition);
				do {
					const Position& pos = (*it)->getPosition();
					const int32_t distance = Position::getDistanceX(myPos, pos) + Position::getDistanceY(myPos, pos);
					if (distance < minRange) {
						target = *it;
						minRange = distance;
					}
				} while (++it != resultList.end());
			}
		}
		else {
			int32_t minRange = std::numeric_limits<int32_t>::max();
			for (const auto& weakCreature : targetList) {
				CreaturePtr creature = weakCreature.lock();
				if (!creature || !isTarget(creature)) {
					continue;
				}

				const Position& pos = creature->getPosition();
				int32_t distance = Position::getDistanceX(myPos, pos) + Position::getDistanceY(myPos, pos);
				if (distance < minRange) {
					target = creature;
					minRange = distance;
				}
			}
		}

		if (target && selectTarget(target)) {
			return true;
		}
		break;
	}

	case TARGETSEARCH_DEFAULT:
	case TARGETSEARCH_ATTACKRANGE:
	case TARGETSEARCH_RANDOM:
	default: {
		if (!resultList.empty()) {
			auto it = resultList.begin();
			std::advance(it, uniform_random(0, resultList.size() - 1));
			return selectTarget(*it);
		}

		if (searchType == TARGETSEARCH_ATTACKRANGE) {
			return false;
		}

		break;
	}
	}

	// Pick the first valid target in the list
	for (const auto& weakTarget : targetList) {
		CreaturePtr target = weakTarget.lock();
		if (target && getFollowCreature() != target && selectTarget(target)) {
			return true;
		}
	}
	return false;
}


void Monster::onFollowCreatureComplete(const CreatureConstPtr& creature)
{
	if (creature) {
		auto it = std::ranges::find_if(targetList, [&](const CreatureWeakPtr& weakTarget) {
			auto target = weakTarget.lock();
			return target && target == creature;
			});

		if (it != targetList.end()) {
			CreaturePtr target = it->lock();
			targetList.erase(it);

			if (hasFollowPath) {
				targetList.insert(targetList.begin(), target);
			}
			else if (!isSummon()) {
				targetList.push_back(target);
			}
		}
	}
}



BlockType_t Monster::blockHit(const CreaturePtr& attacker, CombatType_t combatType, int32_t& damage,
                              bool checkDefense /* = false*/, bool checkArmor /* = false*/, bool /* field = false */, bool /* ignoreResistances = false */)
{
	BlockType_t blockType = Creature::blockHit(attacker, combatType, damage, checkDefense, checkArmor);

	if (damage != 0) {
		int32_t elementMod = 0;
		if (const auto it = mType->info.elementMap.find(combatType); it != mType->info.elementMap.end()) {
			elementMod = it->second;
		}

		if (elementMod != 0) {
			damage = static_cast<int32_t>(std::round(damage * ((100 - elementMod) / 100.)));
			if (damage <= 0) {
				damage = 0;
				blockType = BLOCK_ARMOR;
			}
		}
	}

	return blockType;
}

bool Monster::isTarget(const CreatureConstPtr& creature) const
{
	if (creature->isRemoved() || !creature->isAttackable() ||
	        creature->getZone() == ZONE_PROTECTION || !canSeeCreature(creature)) {
		return false;
	}

	if (creature->getPosition().z != getPosition().z) {
		return false;
	}
	return true;
}

bool Monster::selectTarget(const CreaturePtr& creature)
{
	if (!isTarget(creature)) {
		return false;
	}

	auto it = std::ranges::find_if(targetList, [&](const CreatureWeakPtr& weakTarget) {
		auto target = weakTarget.lock();
		return target && target == creature;
		});

	if (it == targetList.end()) {
		return false;
	}

	if (isHostile() || isSummon()) {
		if (setAttackedCreature(creature) && !isSummon()) {
			g_dispatcher.addTask(createTask([id = getID()]() { g_game.checkCreatureAttack(id); }));
		}
	}

	return setFollowCreature(creature);
}


void Monster::setIdle(const bool idle)
{
	if (isRemoved() || getHealth() <= 0) {
		return;
	}

	isIdle = idle;

	if (!isIdle) {
		g_game.addCreatureCheck(this->getCreature());
	} else {
		onIdleStatus();
		clearTargetList();
		clearFriendList();
		Game::removeCreatureCheck(this->getCreature());
	}
}

void Monster::updateIdleStatus()
{
	bool idle = false;
	if (!isSummon() && targetList.empty()) {
		// check if there are aggressive conditions
		idle = std::ranges::find_if(conditions, [](const Condition* condition) {
			return condition->isAggressive();
		}) == conditions.end();
	}

	setIdle(idle);
}

void Monster::onAddCondition(const ConditionType_t type)
{
	if (type == CONDITION_FIRE || type == CONDITION_ENERGY || type == CONDITION_POISON) {
		updateMapCache();
	}

	updateIdleStatus();
}

void Monster::onEndCondition(const ConditionType_t type)
{
	if (type == CONDITION_FIRE || type == CONDITION_ENERGY || type == CONDITION_POISON) {
		ignoreFieldDamage = false;
		updateMapCache();
	}

	updateIdleStatus();
}

void Monster::onThink(const uint32_t interval)
{
	Creature::onThink(interval);

	if (mType->info.thinkEvent != -1) {
		// onThink(self, interval)
		LuaScriptInterface* scriptInterface = mType->info.scriptInterface;
		if (!scriptInterface->reserveScriptEnv()) {
			std::cout << "[Error - Monster::onThink] Call stack overflow" << std::endl;
			return;
		}

		ScriptEnvironment* env = scriptInterface->getScriptEnv();
		env->setScriptId(mType->info.thinkEvent, scriptInterface);

		lua_State* L = scriptInterface->getLuaState();
		scriptInterface->pushFunction(mType->info.thinkEvent);

		LuaScriptInterface::pushUserdata<Monster>(L, this);
		LuaScriptInterface::setMetatable(L, -1, "Monster");

		lua_pushinteger(L, interval);

		if (scriptInterface->callFunction(2)) {
			return;
		}
	}

	if (!isInSpawnRange(position)) {
		g_game.addMagicEffect(this->getPosition(), CONST_ME_POFF);
		if (g_config.getBoolean(ConfigManager::REMOVE_ON_DESPAWN)) {
			g_game.removeCreature(this->getCreature(), false);
		} else {
			g_game.internalTeleport(this->getCreature(), masterPos);
			setIdle(true);
		}
	} else {
		updateIdleStatus();

		if (!isIdle) {
			addEventWalk();

			if (isSummon()) {
				if (!getAttackedCreature()) {
					if (getMaster() && getMaster()->getAttackedCreature()) {
						//This happens if the monster is summoned during combat
						selectTarget(getMaster()->getAttackedCreature());
					} else if (getMaster() != getFollowCreature()) {
						//Our master has not ordered us to attack anything, lets follow him around instead.
						setFollowCreature(getMaster());
					}
				} else if (getAttackedCreature() == this->getCreature()) {
					setFollowCreature(nullptr);
				} else if (getFollowCreature() != getAttackedCreature()) {
					//This happens just after a master orders an attack, so lets follow it as well.
					setFollowCreature(getAttackedCreature());
				}
			} else if (!targetList.empty()) {
				if (!getFollowCreature() || !hasFollowPath) {
					searchTarget();
				} else if (isFleeing()) {
					if (getAttackedCreature() && !canUseAttack(getPosition(), getAttackedCreature())) {
						searchTarget(TARGETSEARCH_ATTACKRANGE);
					}
				}
			}

			onThinkTarget(interval);
			onThinkYell(interval);
			onThinkDefense(interval);
		}
	}
}

void Monster::doAttacking(const uint32_t interval)
{
	if (!getAttackedCreature() || (isSummon() && getAttackedCreature() == this->getCreature())) {
		return;
	}

	bool lookUpdated = false;
	bool resetTicks = interval != 0;
	attackTicks += interval;

	const Position& myPos = getPosition();
	const Position& targetPos = getAttackedCreature()->getPosition();

	for (const spellBlock_t& spellBlock : mType->info.attackSpells) {
		bool inRange = false;

		if (getAttackedCreature() == nullptr) {
			break;
		}

		if (canUseSpell(myPos, targetPos, spellBlock, interval, inRange, resetTicks)) {
			if (spellBlock.chance >= static_cast<uint32_t>(uniform_random(1, 100))) {
				if (!lookUpdated) {
					updateLookDirection();
					lookUpdated = true;
				}

				minCombatValue = spellBlock.minCombatValue;
				maxCombatValue = spellBlock.maxCombatValue;
				spellBlock.spell->castSpell(this->getCreature(), getAttackedCreature());

				if (spellBlock.isMelee) {
					lastMeleeAttack = OTSYS_TIME();
				}
			}
		}

		if (!inRange && spellBlock.isMelee) {
			//melee swing out of reach
			lastMeleeAttack = 0;
		}
	}

	// ensure ranged creatures turn to player
	if (!lookUpdated && lastMeleeAttack == 0) {
		updateLookDirection();
	}

	if (resetTicks) {
		attackTicks = 0;
	}
}

bool Monster::canUseAttack(const Position& pos, const CreatureConstPtr& target) const
{
	if (isHostile()) {
		const Position& targetPos = target->getPosition();
		const uint32_t distance = std::max<uint32_t>(Position::getDistanceX(pos, targetPos), Position::getDistanceY(pos, targetPos));
		for (const spellBlock_t& spellBlock : mType->info.attackSpells) {
			if (spellBlock.range != 0 && distance <= spellBlock.range) {
				return g_game.isSightClear(pos, targetPos, true);
			}
		}
		return false;
	}
	return true;
}

bool Monster::canUseSpell(const Position& pos, const Position& targetPos,
                          const spellBlock_t& sb, const uint32_t interval, bool& inRange, bool& resetTicks) const
{
	inRange = true;

	if (sb.isMelee) {
		if (isFleeing() || (OTSYS_TIME() - lastMeleeAttack) < sb.speed) {
			return false;
		}
	} else {
		if (sb.speed > attackTicks) {
			resetTicks = false;
			return false;
		}

		if (attackTicks % sb.speed >= interval) {
			//already used this spell for this round
			return false;
		}
	}

	if (sb.range != 0 && std::max<uint32_t>(Position::getDistanceX(pos, targetPos), Position::getDistanceY(pos, targetPos)) > sb.range) {
		inRange = false;
		return false;
	}
	return true;
}

void Monster::onThinkTarget(const uint32_t interval)
{
	if (!isSummon()) {
		if (mType->info.changeTargetSpeed != 0) {
			bool canChangeTarget = true;

			if (challengeFocusDuration > 0) {
				challengeFocusDuration -= interval;

				if (challengeFocusDuration <= 0) {
					challengeFocusDuration = 0;
				}
			}

			if (targetChangeCooldown > 0) {
				targetChangeCooldown -= interval;

				if (targetChangeCooldown <= 0) {
					targetChangeCooldown = 0;
					targetChangeTicks = mType->info.changeTargetSpeed;
				} else {
					canChangeTarget = false;
				}
			}

			if (canChangeTarget) {
				targetChangeTicks += interval;

				if (targetChangeTicks >= mType->info.changeTargetSpeed) {
					targetChangeTicks = 0;
					targetChangeCooldown = mType->info.changeTargetSpeed;

					if (challengeFocusDuration > 0) {
						challengeFocusDuration = 0;
					}

					if (mType->info.changeTargetChance >= uniform_random(1, 100)) {
						if (mType->info.targetDistance <= 1) {
							searchTarget(TARGETSEARCH_RANDOM);
						} else {
							searchTarget(TARGETSEARCH_NEAREST);
						}
					}
				}
			}
		}
	}
}

void Monster::onThinkDefense(const uint32_t interval)
{
	bool resetTicks = true;
	defenseTicks += interval;

	for (const spellBlock_t& spellBlock : mType->info.defenseSpells) {
		if (spellBlock.speed > defenseTicks) {
			resetTicks = false;
			continue;
		}

		if (defenseTicks % spellBlock.speed >= interval) {
			//already used this spell for this round
			continue;
		}

		if ((spellBlock.chance >= static_cast<uint32_t>(uniform_random(1, 100)))) {
			minCombatValue = spellBlock.minCombatValue;
			maxCombatValue = spellBlock.maxCombatValue;
			spellBlock.spell->castSpell(this->getMonster(), this->getCreature());
		}
	}

	if (!isSummon() && summons.size() < mType->info.maxSummons && hasFollowPath) {
		for (const summonBlock_t& summonBlock : mType->info.summons) {
			if (summonBlock.speed > defenseTicks) {
				resetTicks = false;
				continue;
			}

			if (summons.size() >= mType->info.maxSummons) {
				continue;
			}

			if (defenseTicks % summonBlock.speed >= interval) {
				//already used this spell for this round
				continue;
			}

			uint32_t summonCount = 0;
			std::string lowerSummonName = summonBlock.name;
			toLowerCaseString(lowerSummonName);

			for (const auto& summon : summons) {
				if (summon->getRegisteredName() == lowerSummonName) {
					++summonCount;
				}
			}

			if (summonCount >= summonBlock.max) {
				continue;
			}

			if (summonBlock.chance < static_cast<uint32_t>(uniform_random(1, 100))) {
				continue;
			}

			if (MonsterPtr summon = Monster::createMonster(summonBlock.name)) {
				if (g_game.placeCreature(summon, getPosition(), false, summonBlock.force, summonBlock.effect)) {
					summon->setDropLoot(false);
					summon->setSkillLoss(false);
					summon->setMaster(this->getMonster());
					if (summonBlock.masterEffect != CONST_ME_NONE) {
						g_game.addMagicEffect(getPosition(), summonBlock.masterEffect);
					}
				}
			}
		}
	}

	if (resetTicks) {
		defenseTicks = 0;
	}
}

void Monster::onThinkYell(const uint32_t interval)
{
	if (mType->info.yellSpeedTicks == 0) {
		return;
	}

	yellTicks += interval;
	if (yellTicks >= mType->info.yellSpeedTicks) {
		yellTicks = 0;

		if (!mType->info.voiceVector.empty() && (mType->info.yellChance >= static_cast<uint32_t>(uniform_random(1, 100)))) {
			uint32_t index = uniform_random(0, mType->info.voiceVector.size() - 1);
			const voiceBlock_t& vb = mType->info.voiceVector[index];

			if (vb.yellText) {
				g_game.internalCreatureSay(this->getMonster(), TALKTYPE_MONSTER_YELL, vb.text, false);
			} else {
				g_game.internalCreatureSay(this->getMonster(), TALKTYPE_MONSTER_SAY, vb.text, false);
			}
		}
	}
}

bool Monster::walkToSpawn()
{
	if (walkingToSpawn || !spawn || !targetList.empty()) {
		return false;
	}

	int32_t distance = std::max<int32_t>(Position::getDistanceX(position, masterPos), Position::getDistanceY(position, masterPos));
	if (distance == 0) {
		return false;
	}

	listWalkDir.clear();
	if (!getPathTo(masterPos, listWalkDir, 0, std::max<int32_t>(0, distance - 5), true, true, distance)) {
		return false;
	}

	walkingToSpawn = true;
	startAutoWalk();
	return true;
}

void Monster::onWalk()
{
	Creature::onWalk();
}

void Monster::onWalkComplete()
{
	// Continue walking to spawn
	if (walkingToSpawn) {
		walkingToSpawn = false;
		walkToSpawn();
	}
}

bool Monster::pushItem(const ItemPtr& item)
{
	const Position& centerPos = item->getPosition();

	static std::vector<std::pair<int32_t, int32_t>> relList {
		{-1, -1}, {0, -1}, {1, -1},
		{-1,  0},          {1,  0},
		{-1,  1}, {0,  1}, {1,  1}
	};

	std::ranges::shuffle(relList, getRandomGenerator());

	for (const auto& it : relList) {
		Position tryPos(centerPos.x + it.first, centerPos.y + it.second, centerPos.z);
		const auto& tile = g_game.map.getTile(tryPos);
		if (tile && g_game.canThrowObjectTo(centerPos, tryPos, true, true)) {
			CylinderPtr n_parent = item->getParent();
			CylinderPtr t_parent = tile;
			if (g_game.internalMoveItem(n_parent, t_parent, INDEX_WHEREEVER, item, item->getItemCount(), std::nullopt) == RETURNVALUE_NOERROR) {
				return true;
			}
		}
	}
	return false;
}

void Monster::pushItems(const TilePtr& tile)
{
	//We can not use iterators here since we can push the item to another tile
	//which will invalidate the iterator.
	//start from the end to minimize the amount of traffic
	if (const auto& items = tile->getItemList()) {
		uint32_t moveCount = 0;
		uint32_t removeCount = 0;

		int32_t downItemSize = tile->getDownItemCount();
		for (int32_t i = downItemSize; --i >= 0;) {
			if (ItemPtr item = items->at(i); item && item->hasProperty(CONST_PROP_MOVEABLE) && (item->hasProperty(CONST_PROP_BLOCKPATH)
			        || item->hasProperty(CONST_PROP_BLOCKSOLID))) {
				if (moveCount < 20 && Monster::pushItem(item)) {
					++moveCount;
				} else if (g_game.internalRemoveItem(item) == RETURNVALUE_NOERROR) {
					++removeCount;
				}
			}
		}

		if (removeCount > 0) {
			g_game.addMagicEffect(tile->getPosition(), CONST_ME_POFF);
		}
	}
}

bool Monster::pushCreature(const CreaturePtr& creature)
{
	static std::vector<Direction> dirList {
			DIRECTION_NORTH,
		DIRECTION_WEST, DIRECTION_EAST,
			DIRECTION_SOUTH
	};
	std::ranges::shuffle(dirList, getRandomGenerator());

	for (const Direction dir : dirList) {
		const Position& tryPos = Spells::getCasterPosition(creature, dir);
		const auto& toTile = g_game.map.getTile(tryPos);
		if (toTile && !toTile->hasFlag(TILESTATE_BLOCKPATH)) {
			if (g_game.internalMoveCreature(creature, dir) == RETURNVALUE_NOERROR) {
				return true;
			}
		}
	}
	return false;
}

void Monster::pushCreatures(const TilePtr& tile)
{
	//We can not use iterators here since we can push a creature to another tile
	//which will invalidate the iterator.
	if (const auto& creatures = tile->getCreatures()) {
		uint32_t removeCount = 0;
		MonsterPtr lastPushedMonster = nullptr;

		for (size_t i = 0; i < creatures->size();) {
			const auto& monster = creatures->at(i)->getMonster();
			if (monster && monster->isPushable()) {
				if (monster != lastPushedMonster && Monster::pushCreature(monster)) {
					lastPushedMonster = monster;
					continue;
				}

				monster->changeHealth(-monster->getHealth());
				removeCount++;
			}

			++i;
		}

		if (removeCount > 0) {
			g_game.addMagicEffect(tile->getPosition(), CONST_ME_BLOCKHIT);
		}
	}
}

bool Monster::getNextStep(Direction& direction, uint32_t& flags)
{
	if (!walkingToSpawn && (isIdle || isDead())) {
		// we don't have anyone watching, might as well stop walking
		eventWalk = 0;
		return false;
	}

	bool result = false;
	if (!walkingToSpawn && (!getFollowCreature() || !hasFollowPath) && (!isSummon() || !isMasterInRange)) {
		if (getTimeSinceLastMove() >= 1000) {
			randomStepping = true;
			// choose a random direction
			result = getRandomStep(getPosition(), direction);
		}
	}
	else if ((isSummon() && isMasterInRange) || getFollowCreature() || walkingToSpawn) {
		if (!hasFollowPath && getMaster() && !getMaster()->getPlayer()) {
			randomStepping = true;
			result = getRandomStep(getPosition(), direction);
		}
		else {
			randomStepping = false;
			result = Creature::getNextStep(direction, flags);
			if (result) {
				flags |= FLAG_PATHFINDING;
			}
			else {
				if (ignoreFieldDamage) {
					ignoreFieldDamage = false;
					updateMapCache();
				}
				// target dancing
				if (getAttackedCreature() && getAttackedCreature() == getFollowCreature()) {
					if (isFleeing()) {
						result = getDanceStep(getPosition(), direction, false, false);
					}
					else if (mType->info.staticAttackChance < static_cast<uint32_t>(uniform_random(1, 100))) {
						result = getDanceStep(getPosition(), direction);
					}
				}
			}
		}
	}

	if (result && (canPushItems() || canPushCreatures())) {
		const Position& pos = Spells::getCasterPosition(this->getMonster(), direction);
		if (const auto& tile = g_game.map.getTile(pos)) {
			if (canPushItems()) {
				Monster::pushItems(tile);
			}

			if (canPushCreatures()) {
				Monster::pushCreatures(tile);
			}
		}
	}

	return result;
}

bool Monster::getRandomStep(const Position& creaturePos, Direction& direction)
{
	for (const Direction dir : getShuffleDirections()) {
		if (canWalkTo(creaturePos, dir)) {
			direction = dir;
			return true;
		}
	}
	return false;
}

bool Monster::getDanceStep(const Position& creaturePos, Direction& direction,
                           const bool keepAttack /*= true*/, const bool keepDistance /*= true*/)
{
	const bool canDoAttackNow = canUseAttack(creaturePos, getAttackedCreature());

	assert(getAttackedCreature() != nullptr);
	const Position& centerPos = getAttackedCreature()->getPosition();

	const int_fast32_t offset_x = Position::getOffsetX(creaturePos, centerPos);
	const int_fast32_t offset_y = Position::getOffsetY(creaturePos, centerPos);

	const int_fast32_t distance_x = std::abs(offset_x);
	const int_fast32_t distance_y = std::abs(offset_y);

	const uint32_t centerToDist = std::max<uint32_t>(distance_x, distance_y);

	std::vector<Direction> dirList;

	if (!keepDistance || offset_y >= 0) {
		uint32_t tmpDist = std::max<uint32_t>(distance_x, std::abs((creaturePos.getY() - 1) - centerPos.getY()));
		if (tmpDist == centerToDist && canWalkTo(creaturePos, DIRECTION_NORTH)) {
			bool result = true;

			if (keepAttack) {
				result = (!canDoAttackNow || canUseAttack(Position(creaturePos.x, creaturePos.y - 1, creaturePos.z), getAttackedCreature()));
			}

			if (result) {
				dirList.push_back(DIRECTION_NORTH);
			}
		}
	}

	if (!keepDistance || offset_y <= 0) {
		if (const uint32_t tmpDist = std::max<uint32_t>(distance_x, std::abs((creaturePos.getY() + 1) - centerPos.getY())); tmpDist == centerToDist && canWalkTo(creaturePos, DIRECTION_SOUTH)) {
			bool result = true;

			if (keepAttack) {
				result = (!canDoAttackNow || canUseAttack(Position(creaturePos.x, creaturePos.y + 1, creaturePos.z), getAttackedCreature()));
			}

			if (result) {
				dirList.push_back(DIRECTION_SOUTH);
			}
		}
	}

	if (!keepDistance || offset_x <= 0) {
		if (const uint32_t tmpDist = std::max<uint32_t>(std::abs((creaturePos.getX() + 1) - centerPos.getX()), distance_y); tmpDist == centerToDist && canWalkTo(creaturePos, DIRECTION_EAST)) {
			bool result = true;

			if (keepAttack) {
				result = (!canDoAttackNow || canUseAttack(Position(creaturePos.x + 1, creaturePos.y, creaturePos.z), getAttackedCreature()));
			}

			if (result) {
				dirList.push_back(DIRECTION_EAST);
			}
		}
	}

	if (!keepDistance || offset_x >= 0) {
		if (const uint32_t tmpDist = std::max<uint32_t>(std::abs((creaturePos.getX() - 1) - centerPos.getX()), distance_y); tmpDist == centerToDist && canWalkTo(creaturePos, DIRECTION_WEST)) {
			bool result = true;

			if (keepAttack) {
				result = (!canDoAttackNow || canUseAttack(Position(creaturePos.x - 1, creaturePos.y, creaturePos.z), getAttackedCreature()));
			}

			if (result) {
				dirList.push_back(DIRECTION_WEST);
			}
		}
	}

	if (!dirList.empty()) {
		std::ranges::shuffle(dirList, getRandomGenerator());
		direction = dirList[uniform_random(0, dirList.size() - 1)];
		return true;
	}
	return false;
}

bool Monster::getDistanceStep(const Position& targetPos, Direction& direction, bool flee /* = false */)
{
	const Position& creaturePos = getPosition();

	int_fast32_t dx = Position::getDistanceX(creaturePos, targetPos);
	int_fast32_t dy = Position::getDistanceY(creaturePos, targetPos);

	if (int32_t distance = std::max<int32_t>(dx, dy); !flee && (distance > mType->info.targetDistance || !g_game.isSightClear(creaturePos, targetPos, true))) {
		return false; // let the A* calculate it
	} else if (!flee && distance == mType->info.targetDistance) {
		return true; // we don't really care here, since it's what we wanted to reach (a dance-step will take of dancing in that position)
	}

	int_fast32_t offsetx = Position::getOffsetX(creaturePos, targetPos);
	int_fast32_t offsety = Position::getOffsetY(creaturePos, targetPos);

	if (dx <= 1 && dy <= 1) {
		//seems like a target is near, it this case we need to slow down our movements (as a monster)
		if (stepDuration < 2) {
			stepDuration++;
		}
	} else if (stepDuration > 0) {
		stepDuration--;
	}

	if (offsetx == 0 && offsety == 0) {
		return getRandomStep(creaturePos, direction); // player is "on" the monster so let's get some random step and rest will be taken care later.
	}

	if (dx == dy) {
		//player is diagonal to the monster
		if (offsetx >= 1 && offsety >= 1) {
			// player is NW
			//escape to SE, S or E [and some extra]
			bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
			bool e = canWalkTo(creaturePos, DIRECTION_EAST);

			if (s && e) {
				direction = boolean_random() ? DIRECTION_SOUTH : DIRECTION_EAST;
				return true;
			} else if (s) {
				direction = DIRECTION_SOUTH;
				return true;
			} else if (e) {
				direction = DIRECTION_EAST;
				return true;
			} else if (canWalkTo(creaturePos, DIRECTION_SOUTHEAST)) {
				direction = DIRECTION_SOUTHEAST;
				return true;
			}

			/* fleeing */
			bool n = canWalkTo(creaturePos, DIRECTION_NORTH);
			bool w = canWalkTo(creaturePos, DIRECTION_WEST);

			if (flee) {
				if (n && w) {
					direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_WEST;
					return true;
				} else if (n) {
					direction = DIRECTION_NORTH;
					return true;
				} else if (w) {
					direction = DIRECTION_WEST;
					return true;
				}
			}

			/* end of fleeing */

			if (w && canWalkTo(creaturePos, DIRECTION_SOUTHWEST)) {
				direction = DIRECTION_WEST;
			} else if (n && canWalkTo(creaturePos, DIRECTION_NORTHEAST)) {
				direction = DIRECTION_NORTH;
			}

			return true;
		} else if (offsetx <= -1 && offsety <= -1) {
			//player is SE
			//escape to NW , W or N [and some extra]
			bool w = canWalkTo(creaturePos, DIRECTION_WEST);
			bool n = canWalkTo(creaturePos, DIRECTION_NORTH);

			if (w && n) {
				direction = boolean_random() ? DIRECTION_WEST : DIRECTION_NORTH;
				return true;
			} else if (w) {
				direction = DIRECTION_WEST;
				return true;
			} else if (n) {
				direction = DIRECTION_NORTH;
				return true;
			}

			if (canWalkTo(creaturePos, DIRECTION_NORTHWEST)) {
				direction = DIRECTION_NORTHWEST;
				return true;
			}

			/* fleeing */
			bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
			bool e = canWalkTo(creaturePos, DIRECTION_EAST);

			if (flee) {
				if (s && e) {
					direction = boolean_random() ? DIRECTION_SOUTH : DIRECTION_EAST;
					return true;
				} else if (s) {
					direction = DIRECTION_SOUTH;
					return true;
				} else if (e) {
					direction = DIRECTION_EAST;
					return true;
				}
			}

			/* end of fleeing */

			if (s && canWalkTo(creaturePos, DIRECTION_SOUTHWEST)) {
				direction = DIRECTION_SOUTH;
			} else if (e && canWalkTo(creaturePos, DIRECTION_NORTHEAST)) {
				direction = DIRECTION_EAST;
			}

			return true;
		} else if (offsetx >= 1 && offsety <= -1) {
			//player is SW
			//escape to NE, N, E [and some extra]
			bool n = canWalkTo(creaturePos, DIRECTION_NORTH);
			bool e = canWalkTo(creaturePos, DIRECTION_EAST);
			if (n && e) {
				direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_EAST;
				return true;
			} else if (n) {
				direction = DIRECTION_NORTH;
				return true;
			} else if (e) {
				direction = DIRECTION_EAST;
				return true;
			}

			if (canWalkTo(creaturePos, DIRECTION_NORTHEAST)) {
				direction = DIRECTION_NORTHEAST;
				return true;
			}

			/* fleeing */
			bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
			bool w = canWalkTo(creaturePos, DIRECTION_WEST);

			if (flee) {
				if (s && w) {
					direction = boolean_random() ? DIRECTION_SOUTH : DIRECTION_WEST;
					return true;
				} else if (s) {
					direction = DIRECTION_SOUTH;
					return true;
				} else if (w) {
					direction = DIRECTION_WEST;
					return true;
				}
			}

			/* end of fleeing */

			if (w && canWalkTo(creaturePos, DIRECTION_NORTHWEST)) {
				direction = DIRECTION_WEST;
			} else if (s && canWalkTo(creaturePos, DIRECTION_SOUTHEAST)) {
				direction = DIRECTION_SOUTH;
			}

			return true;
		} else if (offsetx <= -1 && offsety >= 1) {
			// player is NE
			//escape to SW, S, W [and some extra]
			bool w = canWalkTo(creaturePos, DIRECTION_WEST);
			bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
			if (w && s) {
				direction = boolean_random() ? DIRECTION_WEST : DIRECTION_SOUTH;
				return true;
			} else if (w) {
				direction = DIRECTION_WEST;
				return true;
			} else if (s) {
				direction = DIRECTION_SOUTH;
				return true;
			} else if (canWalkTo(creaturePos, DIRECTION_SOUTHWEST)) {
				direction = DIRECTION_SOUTHWEST;
				return true;
			}

			/* fleeing */
			bool n = canWalkTo(creaturePos, DIRECTION_NORTH);
			bool e = canWalkTo(creaturePos, DIRECTION_EAST);

			if (flee) {
				if (n && e) {
					direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_EAST;
					return true;
				} else if (n) {
					direction = DIRECTION_NORTH;
					return true;
				} else if (e) {
					direction = DIRECTION_EAST;
					return true;
				}
			}

			/* end of fleeing */

			if (e && canWalkTo(creaturePos, DIRECTION_SOUTHEAST)) {
				direction = DIRECTION_EAST;
			} else if (n && canWalkTo(creaturePos, DIRECTION_NORTHWEST)) {
				direction = DIRECTION_NORTH;
			}

			return true;
		}
	}

	//Now let's decide where the player is located to the monster (what direction) so we can decide where to escape.
	if (dy > dx) {
		Direction playerDir = offsety < 0 ? DIRECTION_SOUTH : DIRECTION_NORTH;
		switch (playerDir) {
			case DIRECTION_NORTH: {
				// Player is to the NORTH, so obviously we need to check if we can go SOUTH, if not then let's choose WEST or EAST
				// and again if we can't we need to decide about some diagonal movements.
				if (canWalkTo(creaturePos, DIRECTION_SOUTH)) {
					direction = DIRECTION_SOUTH;
					return true;
				}

				bool w = canWalkTo(creaturePos, DIRECTION_WEST);
				bool e = canWalkTo(creaturePos, DIRECTION_EAST);
				if (w && e && offsetx == 0) {
					direction = boolean_random() ? DIRECTION_WEST : DIRECTION_EAST;
					return true;
				} else if (w && offsetx <= 0) {
					direction = DIRECTION_WEST;
					return true;
				} else if (e && offsetx >= 0) {
					direction = DIRECTION_EAST;
					return true;
				}

				/* fleeing */
				if (flee) {
					if (w && e) {
						direction = boolean_random() ? DIRECTION_WEST : DIRECTION_EAST;
						return true;
					} else if (w) {
						direction = DIRECTION_WEST;
						return true;
					} else if (e) {
						direction = DIRECTION_EAST;
						return true;
					}
				}

				/* end of fleeing */

				bool sw = canWalkTo(creaturePos, DIRECTION_SOUTHWEST);
				bool se = canWalkTo(creaturePos, DIRECTION_SOUTHEAST);
				if (sw || se) {
					// we can move both dirs
					if (sw && se) {
						direction = boolean_random() ? DIRECTION_SOUTHWEST : DIRECTION_SOUTHEAST;
					} else if (w) {
						direction = DIRECTION_WEST;
					} else if (sw) {
						direction = DIRECTION_SOUTHWEST;
					} else if (e) {
						direction = DIRECTION_EAST;
					} else if (se) {
						direction = DIRECTION_SOUTHEAST;
					}
					return true;
				}

				/* fleeing */
				if (flee && canWalkTo(creaturePos, DIRECTION_NORTH)) {
					// towards player, yea
					direction = DIRECTION_NORTH;
					return true;
				}

				/* end of fleeing */
				break;
			}

			case DIRECTION_SOUTH: {
				if (canWalkTo(creaturePos, DIRECTION_NORTH)) {
					direction = DIRECTION_NORTH;
					return true;
				}

				bool w = canWalkTo(creaturePos, DIRECTION_WEST);
				bool e = canWalkTo(creaturePos, DIRECTION_EAST);
				if (w && e && offsetx == 0) {
					direction = boolean_random() ? DIRECTION_WEST : DIRECTION_EAST;
					return true;
				} else if (w && offsetx <= 0) {
					direction = DIRECTION_WEST;
					return true;
				} else if (e && offsetx >= 0) {
					direction = DIRECTION_EAST;
					return true;
				}

				/* fleeing */
				if (flee) {
					if (w && e) {
						direction = boolean_random() ? DIRECTION_WEST : DIRECTION_EAST;
						return true;
					} else if (w) {
						direction = DIRECTION_WEST;
						return true;
					} else if (e) {
						direction = DIRECTION_EAST;
						return true;
					}
				}

				/* end of fleeing */

				bool nw = canWalkTo(creaturePos, DIRECTION_NORTHWEST);
				bool ne = canWalkTo(creaturePos, DIRECTION_NORTHEAST);
				if (nw || ne) {
					// we can move both dirs
					if (nw && ne) {
						direction = boolean_random() ? DIRECTION_NORTHWEST : DIRECTION_NORTHEAST;
					} else if (w) {
						direction = DIRECTION_WEST;
					} else if (nw) {
						direction = DIRECTION_NORTHWEST;
					} else if (e) {
						direction = DIRECTION_EAST;
					} else if (ne) {
						direction = DIRECTION_NORTHEAST;
					}
					return true;
				}

				/* fleeing */
				if (flee && canWalkTo(creaturePos, DIRECTION_SOUTH)) {
					// towards player, yea
					direction = DIRECTION_SOUTH;
					return true;
				}

				/* end of fleeing */
				break;
			}

			default:
				break;
		}
	} else {
		Direction playerDir = offsetx < 0 ? DIRECTION_EAST : DIRECTION_WEST;
		switch (playerDir) {
			case DIRECTION_WEST: {
				if (canWalkTo(creaturePos, DIRECTION_EAST)) {
					direction = DIRECTION_EAST;
					return true;
				}

				bool n = canWalkTo(creaturePos, DIRECTION_NORTH);
				bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
				if (n && s && offsety == 0) {
					direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_SOUTH;
					return true;
				} else if (n && offsety <= 0) {
					direction = DIRECTION_NORTH;
					return true;
				} else if (s && offsety >= 0) {
					direction = DIRECTION_SOUTH;
					return true;
				}

				/* fleeing */
				if (flee) {
					if (n && s) {
						direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_SOUTH;
						return true;
					} else if (n) {
						direction = DIRECTION_NORTH;
						return true;
					} else if (s) {
						direction = DIRECTION_SOUTH;
						return true;
					}
				}

				/* end of fleeing */

				bool se = canWalkTo(creaturePos, DIRECTION_SOUTHEAST);
				bool ne = canWalkTo(creaturePos, DIRECTION_NORTHEAST);
				if (se || ne) {
					if (se && ne) {
						direction = boolean_random() ? DIRECTION_SOUTHEAST : DIRECTION_NORTHEAST;
					} else if (s) {
						direction = DIRECTION_SOUTH;
					} else if (se) {
						direction = DIRECTION_SOUTHEAST;
					} else if (n) {
						direction = DIRECTION_NORTH;
					} else if (ne) {
						direction = DIRECTION_NORTHEAST;
					}
					return true;
				}

				/* fleeing */
				if (flee && canWalkTo(creaturePos, DIRECTION_WEST)) {
					// towards player, yea
					direction = DIRECTION_WEST;
					return true;
				}

				/* end of fleeing */
				break;
			}

			case DIRECTION_EAST: {
				if (canWalkTo(creaturePos, DIRECTION_WEST)) {
					direction = DIRECTION_WEST;
					return true;
				}

				bool n = canWalkTo(creaturePos, DIRECTION_NORTH);
				bool s = canWalkTo(creaturePos, DIRECTION_SOUTH);
				if (n && s && offsety == 0) {
					direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_SOUTH;
					return true;
				} else if (n && offsety <= 0) {
					direction = DIRECTION_NORTH;
					return true;
				} else if (s && offsety >= 0) {
					direction = DIRECTION_SOUTH;
					return true;
				}

				/* fleeing */
				if (flee) {
					if (n && s) {
						direction = boolean_random() ? DIRECTION_NORTH : DIRECTION_SOUTH;
						return true;
					} else if (n) {
						direction = DIRECTION_NORTH;
						return true;
					} else if (s) {
						direction = DIRECTION_SOUTH;
						return true;
					}
				}

				/* end of fleeing */

				bool nw = canWalkTo(creaturePos, DIRECTION_NORTHWEST);
				bool sw = canWalkTo(creaturePos, DIRECTION_SOUTHWEST);
				if (nw || sw) {
					if (nw && sw) {
						direction = boolean_random() ? DIRECTION_NORTHWEST : DIRECTION_SOUTHWEST;
					} else if (n) {
						direction = DIRECTION_NORTH;
					} else if (nw) {
						direction = DIRECTION_NORTHWEST;
					} else if (s) {
						direction = DIRECTION_SOUTH;
					} else if (sw) {
						direction = DIRECTION_SOUTHWEST;
					}
					return true;
				}

				/* fleeing */
				if (flee && canWalkTo(creaturePos, DIRECTION_EAST)) {
					// towards player, yea
					direction = DIRECTION_EAST;
					return true;
				}

				/* end of fleeing */
				break;
			}

			default:
				break;
		}
	}

	return true;
}

bool Monster::canWalkTo(Position pos, const Direction direction)
{
	pos = getNextPosition(direction, pos);
	if (isInSpawnRange(pos)) {
		if (getWalkCache(pos) == 0) {
			return false;
		}

		if (auto tile = g_game.map.getTile(pos); tile && tile->getTopVisibleCreature(this->getMonster()) == nullptr && tile->queryAdd(this->getMonster(), FLAG_PATHFINDING) == RETURNVALUE_NOERROR) {
			return true;
		}
	}
	return false;
}

void Monster::death(const CreaturePtr&)
{
	const auto monsterId = getID();
	// rewardboss
	if (const auto it = g_game.rewardBossTracking.find(monsterId); it != g_game.rewardBossTracking.end()) {
		if (isRewardBoss())	{
			auto& bossScoreTable = it->second;
			uint32_t topContributerId = 0;
			int32_t topScore = 0;
			int32_t totalScore = 0;
			int32_t totalDamageDone = 0;
			int32_t totalDamageTaken = 0;
			int32_t totalHealingDone = 0;
			const int32_t contributors = bossScoreTable.playerScoreTable.size();

			for (const auto& [playerId, score] : bossScoreTable.playerScoreTable) {
				const int32_t playerScore = score.damageDone + score.damageTaken + score.healingDone; 
				totalScore += playerScore;
				totalDamageDone += score.damageDone;
				totalDamageTaken += score.damageTaken;
				totalHealingDone += score.healingDone;
				
				if (playerScore > topScore) {
					topScore = playerScore;
					topContributerId = playerId;
				}
			}

			const auto& creatureLoot = mType->info.lootItems;
			int64_t currentTime = time(nullptr);

			for (const auto& [playerId, score] : bossScoreTable.playerScoreTable) {

				const auto contributionScore =
					(score.damageDone * g_config.getFloat(ConfigManager::REWARD_RATE_DAMAGE_DONE))
					+ (score.damageTaken * g_config.getFloat(ConfigManager::REWARD_RATE_DAMAGE_TAKEN))
					+ (score.healingDone * (g_config.getFloat(ConfigManager::REWARD_RATE_DAMAGE_DONE)));
				// we should never see 0's here, but better safe than sorry.
				const float expectedScore = (contributionScore) ? (totalScore / (contributors * 3.0)) : 0;
				const int32_t lootRate = std::max<int32_t>(g_config.getFloat(ConfigManager::REWARD_BASE_RATE), 1.0);
				
				const auto& player = g_game.getPlayerByGUID(playerId);
				const auto& rewardContainer = Item::CreateItem(ITEM_REWARD_CONTAINER)->getContainer();
				rewardContainer->getItem()->setIntAttr(ITEM_ATTRIBUTE_DATE, currentTime);
				rewardContainer->getItem()->setIntAttr(ITEM_ATTRIBUTE_REWARDID, getMonster()->getID());

				bool hasLoot = false;
				auto isTopPlayer = (playerId == topContributerId) ? true : false;
				// we only need to confirm contribution counts because users can set specific types of contribution rates to 0
				// contribution only counts if you pull your own weight, so lets check expected score
				if (contributionScore >= expectedScore) {
					for (const auto& lootBlock : creatureLoot) {					
						if (!lootBlock.unique || (lootBlock.unique && isTopPlayer)) {
							int32_t adjustedChance = (static_cast<int32_t>(lootBlock.chance) * lootRate);

							const auto chance = uniform_random(1, MAX_LOOTCHANCE);
							const auto count = uniform_random(1, lootBlock.countmax);
							
							if (chance <= adjustedChance) {
								auto lootItem = Item::CreateItem(lootBlock.id, count);
								lootItem->setIntAttr(ITEM_ATTRIBUTE_DATE, currentTime);
								lootItem->setIntAttr(ITEM_ATTRIBUTE_REWARDID, monsterId);
								CylinderPtr holder = rewardContainer;
								if (g_game.internalAddItem(holder, lootItem) == RETURNVALUE_NOERROR) {
									hasLoot = true;
								} 
							}
						}
					}
				} else {
					// player contributed but not enough.
					if (player)	{
						player->sendTextMessage(MESSAGE_LOOT, "You did not receive any loot.");
					}
				}
				if (hasLoot) {
					if (player) {
						CylinderPtr holder = player->getRewardChest()->getContainer();
						if (g_game.internalAddItem(holder, rewardContainer->getItem()) == RETURNVALUE_NOERROR) {
							player->sendTextMessage(MESSAGE_LOOT, "The following items dropped by " + getMonster()->getName() + " are available in your reward chest: " + rewardContainer->getContentDescription() + ".");
						}
					} else {
						DBInsert rewardQuery("INSERT INTO `player_rewarditems` (`player_id`, `pid`, `sid`, `itemtype`, `count`, `attributes`, `augments`) VALUES ");
						PropWriteStream propWriteStream;

						ItemBlockList itemList;
						int32_t currentPid = 1;
						for (const auto& subItem : rewardContainer->getItemList()) {
							itemList.emplace_back(currentPid, subItem);
						}

						IOLoginData::addRewardItems(playerId, itemList, rewardQuery, propWriteStream);
					}
				}
			}
			g_game.resetDamageTracking(monsterId);
		}
	}

	setAttackedCreature(nullptr);

	for (const auto& summon : summons) {
		summon->changeHealth(-summon->getHealth());
	}
	summons.clear();

	clearTargetList();
	clearFriendList();
	onIdleStatus();
}

ItemPtr Monster::getCorpse(const CreaturePtr& lastHitCreature, const CreaturePtr& mostDamageCreature)
{
	const auto& corpse = Creature::getCorpse(lastHitCreature, mostDamageCreature);
	if (corpse) {
		if (mostDamageCreature) {
			if (mostDamageCreature->getPlayer()) {
				corpse->setCorpseOwner(mostDamageCreature->getID());
			} else {
				const auto& mostDamageCreatureMaster = mostDamageCreature->getMaster();
				if (mostDamageCreatureMaster && mostDamageCreatureMaster->getPlayer()) {
					corpse->setCorpseOwner(mostDamageCreatureMaster->getID());
				}
			}
		}
	}
	return corpse;
}

bool Monster::isInSpawnRange(const Position& pos) const
{
	if (!spawn) {
		return true;
	}

	if (Monster::despawnRadius == 0) {
		return true;
	}

	if (!Spawns::isInZone(masterPos, Monster::despawnRadius, pos)) {
		return false;
	}

	if (Monster::despawnRange == 0) {
		return true;
	}

	if (Position::getDistanceZ(pos, masterPos) > Monster::despawnRange) {
		return false;
	}

	return true;
}

bool Monster::getCombatValues(int32_t& min, int32_t& max)
{
	if (minCombatValue == 0 && maxCombatValue == 0) {
		return false;
	}

	min = minCombatValue;
	max = maxCombatValue;
	return true;
}

void Monster::updateLookDirection()
{
	Direction newDir = getDirection();

	if (getAttackedCreature()) {
		const Position& pos = getPosition();
		const Position& attackedCreaturePos = getAttackedCreature()->getPosition();
		const int_fast32_t offsetx = Position::getOffsetX(attackedCreaturePos, pos);
		const int_fast32_t offsety = Position::getOffsetY(attackedCreaturePos, pos);

		const int32_t dx = std::abs(offsetx);
		const int32_t dy = std::abs(offsety);
		if (dx > dy) {
			//look EAST/WEST
			if (offsetx < 0) {
				newDir = DIRECTION_WEST;
			} else {
				newDir = DIRECTION_EAST;
			}
		} else if (dx < dy) {
			//look NORTH/SOUTH
			if (offsety < 0) {
				newDir = DIRECTION_NORTH;
			} else {
				newDir = DIRECTION_SOUTH;
			}
		} else if (offsetx < 0 && offsety < 0) {
			// target to north-west
			newDir = DIRECTION_WEST;
		} else if (offsetx < 0 && offsety > 0) {
			// target to south-west
			newDir = DIRECTION_WEST;
		}else if (offsetx > 0 && offsety < 0) {
			// target to north-east
			newDir = DIRECTION_EAST;
		} else {
			// target to south-east
			newDir = DIRECTION_EAST;
		}
	}

	g_game.internalCreatureTurn(this->getMonster(), newDir);
}

void Monster::dropLoot(const ContainerPtr& corpse, const CreaturePtr&)
{
	if (getMonster()->isRewardBoss()) {
		int64_t currentTime = std::time(nullptr);
		const auto& rewardContainer = Item::CreateItem(ITEM_REWARD_CONTAINER);

		rewardContainer->setIntAttr(ITEM_ATTRIBUTE_DATE, currentTime);
		rewardContainer->setIntAttr(ITEM_ATTRIBUTE_REWARDID, getMonster()->getID());

		corpse->internalAddThing(rewardContainer);
	}
	else if (corpse && lootDrop) {
		g_events->eventMonsterOnDropLoot(this->getMonster(), corpse);
	}
}

void Monster::setNormalCreatureLight()
{
	internalLight = mType->info.light;
}

void Monster::drainHealth(const CreaturePtr& attacker, const int32_t damage)
{
	Creature::drainHealth(attacker, damage);

	if (damage > 0 && randomStepping) {
		ignoreFieldDamage = true;
		updateMapCache();
	}

	if (isInvisible()) {
		removeCondition(CONDITION_INVISIBLE);
	}
}

void Monster::changeHealth(const int32_t healthChange, const bool sendHealthChange/* = true*/)
{
	//In case a player with ignore flag set attacks the monster
	setIdle(false);
	Creature::changeHealth(healthChange, sendHealthChange);
}

bool Monster::challengeCreature(const CreaturePtr& creature, bool force/* = false*/)
{
	if (isSummon()) {
		return false;
	}

	if (!mType->info.isChallengeable && !force) {
		return false;
	}

	bool result = selectTarget(creature);
	if (result) {
		targetChangeCooldown = 8000;
		challengeFocusDuration = targetChangeCooldown;
		targetChangeTicks = 0;
	}
	return result;
}

void Monster::getPathSearchParams(const CreatureConstPtr& creature, FindPathParams& fpp) const
{
	Creature::getPathSearchParams(creature, fpp);

	fpp.minTargetDist = 1;
	fpp.maxTargetDist = mType->info.targetDistance;

	if (isSummon()) {
		if (getMaster() == creature) {
			fpp.maxTargetDist = 2;
			fpp.fullPathSearch = true;
		} else if (mType->info.targetDistance <= 1) {
			fpp.fullPathSearch = true;
		} else {
			fpp.fullPathSearch = !canUseAttack(getPosition(), creature);
		}
	} else if (isFleeing()) {
		//Distance should be higher than the client view range (Map::maxClientViewportX/Map::maxClientViewportY)
		fpp.maxTargetDist = Map::maxViewportX;
		fpp.clearSight = false;
		fpp.keepDistance = true;
		fpp.fullPathSearch = false;
	} else if (mType->info.targetDistance <= 1) {
		fpp.fullPathSearch = true;
	} else {
		fpp.fullPathSearch = !canUseAttack(getPosition(), creature);
	}
}

bool Monster::canPushItems() const
{
	if (const auto& master = getMaster() ? getMaster()->getMonster() : nullptr) {
		return master->mType->info.canPushItems;
	}

	return mType->info.canPushItems;
}
