/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#include <new>
#include <engine/shared/config.h>
#include <game/server/gamecontext.h>
#include <game/mapitems.h>

#include "character.h"
#include "game/generated/protocol.h"
#include "laser.h"
#include "projectile.h"
#include "../fng2define.h"
#include "shield.h"

//input count
struct CInputCount
{
	int m_Presses;
	int m_Releases;
};

CInputCount CountInput(int Prev, int Cur)
{
	CInputCount c = {0, 0};
	Prev &= INPUT_STATE_MASK;
	Cur &= INPUT_STATE_MASK;
	int i = Prev;

	while(i != Cur)
	{
		i = (i+1)&INPUT_STATE_MASK;
		if(i&1)
			c.m_Presses++;
		else
			c.m_Releases++;
	}

	return c;
}


MACRO_ALLOC_POOL_ID_IMPL(CCharacter, MAX_CLIENTS)

// Character, "physical" player's part
CCharacter::CCharacter(CGameWorld *pWorld)
: CEntity(pWorld, CGameWorld::ENTTYPE_CHARACTER)
{
	m_ProximityRadius = ms_PhysSize;
	m_Health = 0;
	m_Armor = 0;

	m_InvincibleTick = 0;
	m_Killer.m_KillerID = -1;
	m_Killer.m_uiKillerHookTicks = 0;
}

void CCharacter::Reset()
{
	Destroy();
}

bool CCharacter::Spawn(CPlayer *pPlayer, vec2 Pos)
{
	m_Killer.m_KillerID = -1;
	m_Killer.m_uiKillerHookTicks = 0;

	m_EmoteStop = -1;
	m_LastAction = -1;
	m_LastNoAmmoSound = -1;
	m_LastWeapon = WEAPON_HAMMER;
	m_QueuedWeapon = -1;

	m_LastHookedPlayer = -1;

	m_pPlayer = pPlayer;
	m_Pos = Pos;

	m_Core.Reset();
	m_Core.Init(&GameServer()->m_World.m_Core, GameServer()->Collision());
	m_Core.m_Pos = m_Pos;
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = &m_Core;

	m_ReckoningTick = 0;
	mem_zero(&m_SendCore, sizeof(m_SendCore));
	mem_zero(&m_ReckoningCore, sizeof(m_ReckoningCore));

	GameServer()->m_World.InsertEntity(this);
	m_Alive = true;

	GameServer()->m_pController->OnCharacterSpawn(this);
	//Support grenade fng here
	if (m_aWeapons[WEAPON_RIFLE].m_Got)
		m_ActiveWeapon = WEAPON_RIFLE;
	else if (m_aWeapons[WEAPON_GRENADE].m_Got)
		m_ActiveWeapon = WEAPON_GRENADE;
	else if (m_aWeapons[WEAPON_HAMMER].m_Got)
		m_ActiveWeapon = WEAPON_HAMMER;
	else if (m_aWeapons[WEAPON_GUN].m_Got)
		m_ActiveWeapon = WEAPON_GUN;

	m_InvincibleTick = Server()->TickSpeed()*0.35f;
	m_SpawnTick = Server()->Tick();

	m_MoltenAt= -1;
	m_MoltenByHammer = false;
	// m_FrozenBy = -1;
	
	m_InvisibleTick = 0;
	m_Visible = true;

	m_Spree = 0;
	m_HammerFreeze = g_Config.m_SvKillingSpreeKills == 0;
	m_HammerForce = g_Config.m_SvKillingSpreeKills == 0;
	m_GrenadeLauncher = g_Config.m_SvKillingSpreeKills == 0;
	m_ShotGun = g_Config.m_SvKillingSpreeKills == 0;
	m_JetPack = g_Config.m_SvKillingSpreeKills == 0;
	m_SpeedRunner = g_Config.m_SvKillingSpreeKills == 0;
	m_RifleSpread = g_Config.m_SvKillingSpreeKills == 0;
	m_Invisible = g_Config.m_SvKillingSpreeKills == 0;
	m_TeamProtect = g_Config.m_SvKillingSpreeKills == 0;

	return true;
}

void CCharacter::Destroy()
{
	if(g_Config.m_SvSmoothFreezeMode)
		GameServer()->SendTuningParams(m_pPlayer->GetCID());
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	m_Alive = false;
}

void CCharacter::SetWeapon(int W)
{
	if (!IsFrozen()) {
		if (W == m_ActiveWeapon)
			return;

		m_LastWeapon = m_ActiveWeapon;
		m_QueuedWeapon = -1;
		m_ActiveWeapon = W;
		GameServer()->CreateSound(m_Pos, SOUND_WEAPON_SWITCH);
	}
	else {
		if (W == m_LastWeapon || W == WEAPON_NINJA)
			return;

		m_LastWeapon = W;
		m_QueuedWeapon = -1;
		GameServer()->CreateSoundGlobal(SOUND_WEAPON_SWITCH, m_pPlayer->GetCID());
	}

	
	if(m_ActiveWeapon < 0 || m_ActiveWeapon >= NUM_WEAPONS)
		m_ActiveWeapon = 0;
}

bool CCharacter::IsGrounded()
{
	if(GameServer()->Collision()->CheckPoint(m_Pos.x+m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	if(GameServer()->Collision()->CheckPoint(m_Pos.x-m_ProximityRadius/2, m_Pos.y+m_ProximityRadius/2+5))
		return true;
	return false;
}


void CCharacter::HandleFreeze()
{
	//we basically freeze the first tick, after we actually got shot, so all input for all players is fair(pumpnetwork is after ontick)
	if(m_Core.m_Freeze.m_ActivationTick != 0 && m_Core.m_Freeze.m_ActivationTick != Server()->Tick() && m_ActiveWeapon != WEAPON_NINJA) {
		//do actual freezing
		m_ActiveWeapon = WEAPON_NINJA;
		ResetInput();
	}
	
	if(m_ActiveWeapon != WEAPON_NINJA || !IsAlive())
		return;


	if ((Server()->Tick() - m_Core.m_Freeze.m_ActivationTick) % Server()->TickSpeed() == 0) {
		GameServer()->CreateDamageInd(m_Pos, 0, (m_Core.m_Freeze.m_Duration - (Server()->Tick() - m_Core.m_Freeze.m_ActivationTick) / Server()->TickSpeed()), m_pPlayer->GetTeam(), m_pPlayer->GetCID());
	}

	if ((Server()->Tick() - m_Core.m_Freeze.m_ActivationTick) > (m_Core.m_Freeze.m_Duration * Server()->TickSpeed()))
	{
		Unfreeze(-1);

		m_InvincibleTick = Server()->TickSpeed()*0.5;
		return;
	}

	// force freeze
	SetWeapon(WEAPON_NINJA);
	
	return;
}


void CCharacter::DoWeaponSwitch()
{
	// make sure we can switch
	if((g_Config.m_SvPerWeaponReload == 0 && m_ReloadTimer[GetActiveWeaponForReload()] != 0) || m_QueuedWeapon == -1)
		return;

	// switch Weapon
	SetWeapon(m_QueuedWeapon);
}

void CCharacter::HandleWeaponSwitch()
{
	int WantedWeapon = m_ActiveWeapon;
	if(m_QueuedWeapon != -1)
		WantedWeapon = m_QueuedWeapon;

	// select Weapon
	int Next = CountInput(m_LatestPrevInput.m_NextWeapon, m_LatestInput.m_NextWeapon).m_Presses;
	int Prev = CountInput(m_LatestPrevInput.m_PrevWeapon, m_LatestInput.m_PrevWeapon).m_Presses;

	if(Next < 128) // make sure we only try sane stuff
	{
		while(Next) // Next Weapon selection
		{
			WantedWeapon = (WantedWeapon+1)%NUM_WEAPONS;
			if(m_aWeapons[WantedWeapon].m_Got)
				Next--;
		}
	}

	if(Prev < 128) // make sure we only try sane stuff
	{
		while(Prev) // Prev Weapon selection
		{
			WantedWeapon = (WantedWeapon-1)<0?NUM_WEAPONS-1:WantedWeapon-1;
			if(m_aWeapons[WantedWeapon].m_Got)
				Prev--;
		}
	}

	// Direct Weapon selection
	if(m_LatestInput.m_WantedWeapon)
		WantedWeapon = m_Input.m_WantedWeapon-1;

	// check for insane values
	if(WantedWeapon >= 0 && WantedWeapon < NUM_WEAPONS && WantedWeapon != m_ActiveWeapon && m_aWeapons[WantedWeapon].m_Got)
		m_QueuedWeapon = WantedWeapon;

	DoWeaponSwitch();
}

bool CCharacter::IsFrozen(){
	return m_Core.m_Freeze.m_ActivationTick != 0;
}

int CCharacter::GetActiveWeaponForReload() {
	if(g_Config.m_SvPerWeaponReload)
		return m_ActiveWeapon;
	return 0;
}

void CCharacter::FireWeapon()
{
	if(m_ReloadTimer[GetActiveWeaponForReload()] != 0 || (IsFrozen() && m_Core.m_Freeze.m_ActivationTick != Server()->Tick()))
		return;

	DoWeaponSwitch();
	vec2 Direction = normalize(vec2(m_LatestInput.m_TargetX, m_LatestInput.m_TargetY));

	bool FullAuto = false;
	if(m_ActiveWeapon == WEAPON_GRENADE || m_ActiveWeapon == WEAPON_SHOTGUN || m_ActiveWeapon == WEAPON_RIFLE)
		FullAuto = true;


	// check if we gonna fire
	bool WillFire = false;
	if(CountInput(m_LatestPrevInput.m_Fire, m_LatestInput.m_Fire).m_Presses)
		WillFire = true;

	if(FullAuto && (m_LatestInput.m_Fire&1) && m_aWeapons[m_ActiveWeapon].m_Ammo)
		WillFire = true;

	if(!WillFire || (Server()->Tick() < m_SpawnTick + Server()->TickSpeed()*0.35f))
		return;

	// check for ammo
	if(!m_aWeapons[m_ActiveWeapon].m_Ammo)
	{
		// 125ms is a magical limit of how fast a human can click
		m_ReloadTimer[GetActiveWeaponForReload()] = 125 * Server()->TickSpeed() / 1000;
		if(m_LastNoAmmoSound+Server()->TickSpeed() <= Server()->Tick())
		{
			GameServer()->CreateSound(m_Pos, SOUND_WEAPON_NOAMMO);
			m_LastNoAmmoSound = Server()->Tick();
		}
		return;
	}

	vec2 ProjStartPos = m_Pos+Direction*m_ProximityRadius*0.75f;

	int WeaponFireDelay = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Firedelay * Server()->TickSpeed() / 1000;

	if(m_Invisible)
	{
		m_InvisibleTick = 0;
		m_Visible = true;	
	}

	switch(m_ActiveWeapon)
	{
		case WEAPON_HAMMER:
		{
			GameServer()->CreateSound(m_Pos, SOUND_HAMMER_FIRE);

			CCharacter *apEnts[MAX_CLIENTS];
			int Hits = 0;
			int Num = GameServer()->m_World.FindEntities(ProjStartPos, m_ProximityRadius*0.5f, (CEntity**)apEnts,
														MAX_CLIENTS, CGameWorld::ENTTYPE_CHARACTER);

			for (int i = 0; i < Num; ++i)
			{
				CCharacter *pTarget = apEnts[i];

				if ((pTarget == this) || GameServer()->Collision()->IntersectLine(ProjStartPos, pTarget->m_Pos, NULL, NULL))
					continue;

				// set his velocity to fast upward (for now)
				if(length(pTarget->m_Pos-ProjStartPos) > 0.0f)
					GameServer()->CreateHammerHit(pTarget->m_Pos-normalize(pTarget->m_Pos-ProjStartPos)*m_ProximityRadius*0.5f);
				else
					GameServer()->CreateHammerHit(ProjStartPos);

				pTarget->TakeHammerHit(this);
				Hits++;
			}

			// if we Hit anything, we have to wait for the reload
			if(Hits)
				m_ReloadTimer[GetActiveWeaponForReload()] = Server()->TickSpeed()/3;

		} break;

		case WEAPON_GUN:
		{
			new CProjectile(GameWorld(), WEAPON_GUN,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GunLifetime),
				1, 0, 0, -1, WEAPON_GUN);

			GameServer()->CreateSound(m_Pos, SOUND_GUN_FIRE);
		} break;

		case WEAPON_SHOTGUN:
		{
			int ShotSpread = 2;

			for(int i = -ShotSpread; i <= ShotSpread; ++i)
			{
				float Spreading[] = {-0.185f, -0.070f, 0, 0.070f, 0.185f};
				float a = GetAngle(Direction);
				a += Spreading[i+2];
				float v = 1-(absolute(i)/(float)ShotSpread);
				float Speed = mix((float)GameServer()->Tuning()->m_ShotgunSpeeddiff, 1.0f, v);
				new CProjectile(GameWorld(), WEAPON_SHOTGUN,
					m_pPlayer->GetCID(),
					ProjStartPos,
					vec2(cosf(a), sinf(a))*Speed,
					(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_ShotgunLifetime),
					1, 0, 0, -1, WEAPON_SHOTGUN);
			}

			GameServer()->CreateSound(m_Pos, SOUND_SHOTGUN_FIRE);
		} break;

		case WEAPON_GRENADE:
		{
			++m_pPlayer->m_Stats.m_Shots;
			new CProjectile(GameWorld(), WEAPON_GRENADE,
				m_pPlayer->GetCID(),
				ProjStartPos,
				Direction,
				(int)(Server()->TickSpeed()*GameServer()->Tuning()->m_GrenadeLifetime),
				1, true, 0, SOUND_GRENADE_EXPLODE, WEAPON_GRENADE);

			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_FIRE);
			//WeaponFireDelay = GameServer()->Tuning()->m_GrenadeFireDelay * Server()->TickSpeed() / 1000;
		} break;

		case WEAPON_RIFLE:
		{
			++m_pPlayer->m_Stats.m_Shots;
			if(m_RifleSpread)
			{
				for(int i = 0; i <= 1; ++i)
				{
					float a = GetAngle(Direction) + rand() * i;
					new CLaser(GameWorld(), m_Pos, vec2(cosf(a), sinf(a)), GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
				}
			}
			else
				new CLaser(GameWorld(), m_Pos, Direction, GameServer()->Tuning()->m_LaserReach, m_pPlayer->GetCID());
			GameServer()->CreateSound(m_Pos, SOUND_RIFLE_FIRE);
			//WeaponFireDelay = GameServer()->Tuning()->m_LaserFireDelay * Server()->TickSpeed() / 1000;
		} break;

		case WEAPON_NINJA:
		{
		} break;

	}

	m_AttackTick = Server()->Tick();
	
	if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0) // -1 == unlimited
		m_aWeapons[m_ActiveWeapon].m_Ammo--;

	if(!m_ReloadTimer[GetActiveWeaponForReload()])
		m_ReloadTimer[GetActiveWeaponForReload()] = WeaponFireDelay;
}

void CCharacter::HandleWeapons()
{
	bool IsWeaponReloading = m_ReloadTimer[GetActiveWeaponForReload()] > 0;

	// check reload timer
	for(int i = 0; i < NUM_WEAPONS; ++i)
	{
		if(m_ReloadTimer[i])
			m_ReloadTimer[i]--;
	}

	if(IsWeaponReloading)
		return;

	// fire Weapon, if wanted
	FireWeapon();

	// ammo regen
	int AmmoRegenTime = g_pData->m_Weapons.m_aId[m_ActiveWeapon].m_Ammoregentime;
	if(AmmoRegenTime)
	{
		// If equipped and not active, regen ammo?
		if (m_ReloadTimer[GetActiveWeaponForReload()] <= 0)
		{
			if (m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart < 0)
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = Server()->Tick();

			if ((Server()->Tick() - m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart) >= AmmoRegenTime * Server()->TickSpeed() / 1000)
			{
				// Add some ammo
				m_aWeapons[m_ActiveWeapon].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 5);
				m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
			}
		}
		else
		{
			m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart = -1;
		}
	}

	return;
}

bool CCharacter::GiveWeapon(int Weapon, int Ammo)
{
	if(m_aWeapons[Weapon].m_Ammo < g_pData->m_Weapons.m_aId[Weapon].m_Maxammo || !m_aWeapons[Weapon].m_Got)
	{
		m_aWeapons[Weapon].m_Got = true;
		m_aWeapons[Weapon].m_Ammo = min(g_pData->m_Weapons.m_aId[Weapon].m_Maxammo, Ammo);
		return true;
	}
	return false;
}

void CCharacter::Freeze(int TimeInSec) {
	if(!IsFrozen())m_LastWeapon = m_ActiveWeapon;
	m_MoltenAt= -1;
	m_MoltenByHammer = false;
	m_Core.m_Freeze.m_ActivationTick = Server()->Tick();
	m_Core.m_Freeze.m_Duration = TimeInSec;
	
	if(g_Config.m_SvSmoothFreezeMode)
		GameServer()->SendFakeTuningParams(m_pPlayer->GetCID());
}

void CCharacter::Unfreeze(int pPlayerID) {
	//Set Weapon to old one, and unfreeze
	m_ActiveWeapon = m_LastWeapon;
	m_Core.m_Freeze.m_ActivationTick = 0;
	m_Killer.m_uiKillerHookTicks = 0;
	m_Killer.m_KillerID = m_pPlayer->GetCID();

	if (pPlayerID != -1) {
		int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[pPlayerID], WEAPON_HAMMER);
		// send the kill message
		CNetMsg_Sv_KillMsg Msg;
		Msg.m_Killer = pPlayerID;
		Msg.m_Victim = m_pPlayer->GetCID();
		Msg.m_Weapon = WEAPON_HAMMER;
		Msg.m_ModeSpecial = ModeSpecial;
		GameServer()->SendPackMsg(&Msg, MSGFLAG_VITAL);
	}
		
	if(g_Config.m_SvSmoothFreezeMode)
		GameServer()->SendTuningParams(m_pPlayer->GetCID());

}

void CCharacter::SetEmote(int Emote, int Tick)
{
	m_EmoteType = Emote;
	m_EmoteStop = Tick;
}

void CCharacter::SetKiller(int pKillerID, unsigned int pHookTicks) {
	if (m_Killer.m_uiKillerHookTicks < pHookTicks) {
		m_Killer.m_uiKillerHookTicks = pHookTicks;
		m_Killer.m_KillerID = pKillerID;
	}
}

void CCharacter::OnPredictedInput(CNetObj_PlayerInput *pNewInput)
{
	if (IsFrozen()) {
		pNewInput->m_Direction = 0;
		pNewInput->m_Jump = 0;
		pNewInput->m_Fire = 0;
		pNewInput->m_Hook = 0;
	}

	// check for changes
	if(mem_comp(&m_Input, pNewInput, sizeof(CNetObj_PlayerInput)) != 0)
		m_LastAction = Server()->Tick();

	// copy new input
	mem_copy(&m_Input, pNewInput, sizeof(m_Input));
	m_NumInputs++;

	// it is not allowed to aim in the center
	if(m_Input.m_TargetX == 0 && m_Input.m_TargetY == 0)
		m_Input.m_TargetY = -1;
}

void CCharacter::OnDirectInput(CNetObj_PlayerInput *pNewInput)
{
	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
	mem_copy(&m_LatestInput, pNewInput, sizeof(m_LatestInput));

	// it is not allowed to aim in the center
	if(m_LatestInput.m_TargetX == 0 && m_LatestInput.m_TargetY == 0)
		m_LatestInput.m_TargetY = -1;

	if(m_NumInputs > 2 && m_pPlayer->GetTeam() != TEAM_SPECTATORS)
	{
		HandleWeaponSwitch();
		FireWeapon();
	}

	mem_copy(&m_LatestPrevInput, &m_LatestInput, sizeof(m_LatestInput));
}

void CCharacter::ResetInput()
{
	m_Input.m_Direction = 0;
	m_Input.m_Hook = 0;
	// simulate releasing the fire button
	if((m_Input.m_Fire&1) != 0)
		m_Input.m_Fire++;
	m_Input.m_Fire &= INPUT_STATE_MASK;
	m_Input.m_Jump = 0;
	m_LatestPrevInput = m_LatestInput = m_Input;
}

void CCharacter::PreTick()
{
	if (m_InvincibleTick > 0) 
		--m_InvincibleTick;

	if(m_pPlayer->m_ForceBalanced)
	{
		char Buf[128];
		str_format(Buf, sizeof(Buf), "You were moved to %s due to team balancing", GameServer()->m_pController->GetTeamName(m_pPlayer->GetTeam()));
		GameServer()->SendBroadcast(Buf, m_pPlayer->GetCID());

		m_pPlayer->m_ForceBalanced = false;
	}

	//before the core update to know if we hooked somebody before it.
	m_LastHookedPlayer = m_Core.m_HookedPlayer;

	m_Core.m_Input = m_Input;
	m_Core.Tick(true);
}

void CCharacter::Tick()
{
	m_Core.TickDeferred();

	if (!m_Alive)
		return;
	
	if(m_GrenadeLauncher && !m_aWeapons[WEAPON_GRENADE].m_Got) {
		if(GiveWeapon(WEAPON_GRENADE, 10))
			GameServer()->CreateSound(m_Pos, SOUND_PICKUP_GRENADE);
	}
	if(m_ShotGun && !m_aWeapons[WEAPON_SHOTGUN].m_Got) {
		if(GiveWeapon(WEAPON_SHOTGUN, 10))
			GameServer()->CreateSound(m_Pos, SOUND_PICKUP_SHOTGUN);
	}

	if(m_Invisible)
	{
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			//CCharacterCore *pCharCore = m_pWorld->m_apCharacters[i];
			CCharacter *pCharCore = GameServer()->GetPlayerChar(i);
			if(!pCharCore)
				continue;
			
			//player *p = (player*)ent;
			if(pCharCore == this) // || !(p->flags&FLAG_ALIVE)
				continue; // make sure that we don't nudge our self

			// handle player <-> player collision
			float Distance = distance(m_Pos, pCharCore->m_Pos);
			vec2 Dir = normalize(m_Pos - pCharCore->m_Pos);
			if(Distance < 28.0f*1.25f && Distance > 0.0f)
			{
				//GameServer()->SendChatTarget(-1, "test");
				m_Visible = true;
				m_InvisibleTick = 0;
				new CShield(&GameServer()->m_World, m_Pos, GetPlayer()->GetCID(), 1); 
			}
		}
	}
	
	if(m_JetPack && m_Core.m_Freeze.m_ActivationTick <= 0)
	{
	    vec2 Direction = normalize(vec2(0, 10.0f));
	    if(!(m_Core.m_Jumped&1))
		{
	        if(m_Input.m_Jump)
			{
	            /*TakeDamage(Direction * -1.0f * (400.0f / 100.0f / 6.11f), 0, m_pPlayer->GetCID(), m_ActiveWeapon);*/ // fix botdetect bug
				m_Core.m_Vel += Direction * -1.5f * (400.0f / 100.0f / 6.11f);
				
                if(Server()->Tick() % (1 * Server()->TickSpeed() / 10) == 0)
				{
					if(m_Invisible)
					{
						m_Visible = true;
						m_InvisibleTick = 0;
					}
					GameServer()->CreateSound(m_Core.m_Pos, SOUND_WEAPON_SWITCH);
					GameServer()->CreateDamageInd(
						m_Core.m_Vel.y < 0 ?
							m_Core.m_Pos - vec2((m_Input.m_Direction != 0 ? (m_Input.m_Direction > 0 ? 28 : -28) : 0), -28) :
							m_Core.m_Pos - vec2((m_Input.m_Direction != 0 ? (m_Input.m_Direction > 0 ? 28 : -28) : 0), 28),
						m_Core.m_Vel.y < 0 ? 0.3f : 60.0f,
						(m_Core.m_Freeze.m_Duration - (Server()->Tick() - m_Core.m_Freeze.m_ActivationTick) / Server()->TickSpeed()),
						m_pPlayer->GetTeam(),
						m_pPlayer->GetCID()
					);
				}
			}
		}
	}
	
	if(m_SpeedRunner && m_Core.m_Freeze.m_ActivationTick <= 0)
    {
        const float MaxSpeed = IsGrounded() ? GameServer()->m_World.m_Core.m_Tuning.m_GroundControlSpeed * 3 : 0.0f;
        const float Accel = IsGrounded() ? GameServer()->m_World.m_Core.m_Tuning.m_GroundControlAccel : 0.0f;
        if(m_Input.m_Direction == 1)
            m_Core.m_Vel.x = SaturatedAdd(-MaxSpeed, MaxSpeed, m_Core.m_Vel.x, Accel);
        else if(m_Input.m_Direction == -1)
            m_Core.m_Vel.x = SaturatedAdd(-MaxSpeed, MaxSpeed, m_Core.m_Vel.x, -Accel);
    }

	// handle death-tiles and leaving gamelayer
	if(GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x+m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y-m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameServer()->Collision()->GetCollisionAt(m_Pos.x-m_ProximityRadius/3.f, m_Pos.y+m_ProximityRadius/3.f)&CCollision::COLFLAG_DEATH ||
		GameLayerClipped(m_Pos))
	{
		Die(m_pPlayer->GetCID(), WEAPON_WORLD);
	}

	int flag = 0;
	if ((flag |= GameServer()->Collision()->GetCollisionAt(m_Pos.x + m_ProximityRadius / 3.f, m_Pos.y - m_ProximityRadius / 3.f))& (CCollision::COLFLAG_SPIKE_NORMAL | CCollision::COLFLAG_SPIKE_RED | CCollision::COLFLAG_SPIKE_BLUE | CCollision::COLFLAG_SPIKE_GOLD | CCollision::COLFLAG_SPIKE_GREEN | CCollision::COLFLAG_SPIKE_PURPLE) ||
		(flag |= GameServer()->Collision()->GetCollisionAt(m_Pos.x + m_ProximityRadius / 3.f, m_Pos.y + m_ProximityRadius / 3.f))& (CCollision::COLFLAG_SPIKE_NORMAL | CCollision::COLFLAG_SPIKE_RED | CCollision::COLFLAG_SPIKE_BLUE | CCollision::COLFLAG_SPIKE_GOLD | CCollision::COLFLAG_SPIKE_GREEN | CCollision::COLFLAG_SPIKE_PURPLE) ||
		(flag |= GameServer()->Collision()->GetCollisionAt(m_Pos.x - m_ProximityRadius / 3.f, m_Pos.y - m_ProximityRadius / 3.f))& (CCollision::COLFLAG_SPIKE_NORMAL | CCollision::COLFLAG_SPIKE_RED | CCollision::COLFLAG_SPIKE_BLUE | CCollision::COLFLAG_SPIKE_GOLD | CCollision::COLFLAG_SPIKE_GREEN | CCollision::COLFLAG_SPIKE_PURPLE) ||
		(flag |= GameServer()->Collision()->GetCollisionAt(m_Pos.x - m_ProximityRadius / 3.f, m_Pos.y + m_ProximityRadius / 3.f))& (CCollision::COLFLAG_SPIKE_NORMAL | CCollision::COLFLAG_SPIKE_RED | CCollision::COLFLAG_SPIKE_BLUE | CCollision::COLFLAG_SPIKE_GOLD | CCollision::COLFLAG_SPIKE_GREEN | CCollision::COLFLAG_SPIKE_PURPLE))
	{
		DieSpikes(m_Killer.m_KillerID, flag);
	}

	//HandleFreeze
	HandleFreeze();

	// handle Weapons
	HandleWeapons();

	// Previnput
	m_PrevInput = m_Input;
	
	//Invisible
	if(m_Invisible && m_Visible)
		m_InvisibleTick = m_InvisibleTick + 1;
	
	if(m_Invisible && m_InvisibleTick >= 100 && !IsFrozen())
	{
		m_Visible = false;
		m_InvisibleTick = 0;
		new CShield(&GameServer()->m_World, m_Pos, GetPlayer()->GetCID(), 1); 
	}

	if(m_LastHookedPlayer != -1)
	{
		CCharacter *pChar = GameServer()->GetPlayerChar(m_LastHookedPlayer);
		if(pChar && pChar->m_Invisible) {
			pChar->m_InvisibleTick = 0;
			pChar->m_Visible = true;
		}
	}
	if(m_Invisible && (m_LastHookedPlayer != -1 || m_Input.m_Hook)) {
		m_InvisibleTick = 0;
		m_Visible = true;	
	}

	if (m_LastHookedPlayer != -1 && g_Config.m_SvKillTakeOverTime != -1) {
		CPlayer *pPlayer = GameServer()->m_apPlayers[m_LastHookedPlayer];
		if (pPlayer) {
			//if the hooker is not in the team of the hooked player, we check how long "we" hooked the hooked player already
			if (pPlayer->GetTeam() != m_pPlayer->GetTeam() || !GameServer()->m_pController->IsTeamplay()) {
				//if the player is hooked over 0.25 seconds, this character is "allowed" to call himself the killer of the hooked player
				if (m_Core.m_HookTick >= Server()->TickSpeed() / (1000.f / (float)g_Config.m_SvKillTakeOverTime)) {
					CCharacter* pChr = pPlayer->GetCharacter();
					if (pChr) {
						//if we stop hook the hooked player, we have 0 hook ticks on the target
						if (m_Core.m_HookState == HOOK_RETRACTED && pChr->m_Killer.m_KillerID == m_pPlayer->GetCID()) {
							pChr->m_Killer.m_uiKillerHookTicks = 0;
						}
						// else we "try" to set us as killer
						else pChr->SetKiller(m_pPlayer->GetCID(), m_Core.m_HookTick);
					}
				}
			}
			else {
				//if we hook someone from the same team, we are the killer, if he falls into spikes
				CCharacter* pChr = pPlayer->GetCharacter();
				if (pChr) {
					pChr->m_Killer.m_uiKillerHookTicks = 0;
					pChr->m_Killer.m_KillerID = m_LastHookedPlayer;
				}
			}
		}
	}
	return;
}

void CCharacter::TickDefered()
{
	// advance the dummy
	{
		CWorldCore TempWorld;
		m_ReckoningCore.Init(&TempWorld, GameServer()->Collision());
		m_ReckoningCore.Tick(false);
		m_ReckoningCore.TickDeferred();
		m_ReckoningCore.Move();
		m_ReckoningCore.Quantize();
	}

	//lastsentcore
	vec2 StartPos = m_Core.m_Pos;
	vec2 StartVel = m_Core.m_Vel;
	bool StuckBefore = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));

	m_Core.Move();
	bool StuckAfterMove = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Core.Quantize();
	bool StuckAfterQuant = GameServer()->Collision()->TestBox(m_Core.m_Pos, vec2(28.0f, 28.0f));
	m_Pos = m_Core.m_Pos;

	if(!StuckBefore && (StuckAfterMove || StuckAfterQuant))
	{
		// Hackish solution to get rid of strict-aliasing warning
		union
		{
			float f;
			unsigned u;
		}StartPosX, StartPosY, StartVelX, StartVelY;

		StartPosX.f = StartPos.x;
		StartPosY.f = StartPos.y;
		StartVelX.f = StartVel.x;
		StartVelY.f = StartVel.y;

		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "STUCK!!! %d %d %d %f %f %f %f %x %x %x %x",
			StuckBefore,
			StuckAfterMove,
			StuckAfterQuant,
			StartPos.x, StartPos.y,
			StartVel.x, StartVel.y,
			StartPosX.u, StartPosY.u,
			StartVelX.u, StartVelY.u);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);
	}

	int Events = m_Core.m_TriggeredEvents;
	QuadroMask Mask = CmaskAllExceptOne(m_pPlayer->GetCID());

	if(Events&COREEVENT_GROUND_JUMP) GameServer()->CreateSound(m_Pos, SOUND_PLAYER_JUMP, Mask);

	if(Events&COREEVENT_HOOK_ATTACH_PLAYER) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_PLAYER, CmaskAll());
	if(Events&COREEVENT_HOOK_ATTACH_GROUND) GameServer()->CreateSound(m_Pos, SOUND_HOOK_ATTACH_GROUND, Mask);
	if(Events&COREEVENT_HOOK_HIT_NOHOOK) GameServer()->CreateSound(m_Pos, SOUND_HOOK_NOATTACH, Mask);


	if(m_pPlayer->GetTeam() == TEAM_SPECTATORS)
	{
		m_Pos.x = m_Input.m_TargetX;
		m_Pos.y = m_Input.m_TargetY;
	}

	// update the m_SendCore if needed
	{
		CNetObj_Character Predicted;
		CNetObj_Character Current;
		mem_zero(&Predicted, sizeof(Predicted));
		mem_zero(&Current, sizeof(Current));
		m_ReckoningCore.Write(&Predicted);
		m_Core.Write(&Current);

		// only allow dead reackoning for a top of 3 seconds
		if(m_ReckoningTick+Server()->TickSpeed()*3 < Server()->Tick() || mem_comp(&Predicted, &Current, sizeof(CNetObj_Character)) != 0)
		{
			m_ReckoningTick = Server()->Tick();
			m_SendCore = m_Core;
			m_ReckoningCore = m_Core;
		}
	}
	
	m_pPlayer->m_Stats.m_NumJumped += m_Core.m_CoreStats.m_NumJumped;
	m_pPlayer->m_Stats.m_NumTilesMoved += m_Core.m_CoreStats.m_NumTilesMoved;
	m_pPlayer->m_Stats.m_NumHooks += m_Core.m_CoreStats.m_NumHooks;
	if(m_Core.m_CoreStats.m_MaxSpeed > m_pPlayer->m_Stats.m_MaxSpeed) m_pPlayer->m_Stats.m_MaxSpeed = m_Core.m_CoreStats.m_MaxSpeed;
	m_pPlayer->m_Stats.m_NumTeeCollisions += m_Core.m_CoreStats.m_NumTeeCollisions;
	mem_zero(&m_Core.m_CoreStats, sizeof(m_Core.m_CoreStats));
	
	if(IsFrozen() && m_Core.m_Freeze.m_ActivationTick != Server()->Tick()) ++m_pPlayer->m_Stats.m_NumFreezeTicks;
}

void CCharacter::TickPaused()
{
	++m_AttackTick;
	if(IsFrozen()) ++m_Core.m_Freeze.m_ActivationTick;
	++m_ReckoningTick;
	if(m_LastAction != -1)
		++m_LastAction;
	if(m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart > -1)
		++m_aWeapons[m_ActiveWeapon].m_AmmoRegenStart;
	if(m_EmoteStop > -1)
		++m_EmoteStop;
}

bool CCharacter::IncreaseHealth(int Amount)
{
	if(m_Health >= 10)
		return false;
	m_Health = clamp(m_Health+Amount, 0, 10);
	return true;
}

bool CCharacter::IncreaseArmor(int Amount)
{
	if(m_Armor >= 10)
		return false;
	m_Armor = clamp(m_Armor+Amount, 0, 10);
	return true;
}


void CCharacter::Die(int Killer, int Weapon)
{
	// we got to wait 0.5 secs before respawning
	m_pPlayer->m_RespawnTick = Server()->Tick() + Server()->TickSpeed() / 2;
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "kill killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	GameServer()->SendPackMsg(&Msg, MSGFLAG_VITAL);

	// a nice sound
	GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE);
	if(g_Config.m_SvSmoothFreezeMode)
		GameServer()->SendTuningParams(m_pPlayer->GetCID());

	EndSpree(Killer);

	// this is for auto respawn after 3 secs
	m_pPlayer->m_DieTick = Server()->Tick();

	m_Alive = false;
	GameServer()->m_World.RemoveEntity(this);
	GameServer()->m_World.m_Core.m_apCharacters[m_pPlayer->GetCID()] = 0;
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
}

void CCharacter::DieSpikes(int pKillerID, int spikes_flag) {
	int Weapon = 0;


	if (spikes_flag&CCollision::COLFLAG_SPIKE_NORMAL) 		Weapon = WEAPON_SPIKE_NORMAL;
	else if (spikes_flag&CCollision::COLFLAG_SPIKE_RED)		Weapon = WEAPON_SPIKE_RED;
	else if (spikes_flag&CCollision::COLFLAG_SPIKE_BLUE)	Weapon = WEAPON_SPIKE_BLUE;
	else if (spikes_flag&CCollision::COLFLAG_SPIKE_GOLD)	Weapon = WEAPON_SPIKE_GOLD;
	else if (spikes_flag&CCollision::COLFLAG_SPIKE_GREEN)	Weapon = WEAPON_SPIKE_GREEN;
	else if (spikes_flag&CCollision::COLFLAG_SPIKE_PURPLE)	Weapon = WEAPON_SPIKE_PURPLE;

	// if the player leaves the game, he will be nullptr and we handle it like a selfkill
	if (pKillerID == -1 || GameServer()->m_apPlayers[pKillerID] == 0) pKillerID = m_pPlayer->GetCID();

	EndSpree(pKillerID);

	if (!IsFrozen() || pKillerID == m_pPlayer->GetCID()) Weapon = WEAPON_WORLD;

	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[pKillerID], Weapon);

	//if needed the mod can tell the character to not die with modespecial = -1
	if(ModeSpecial >= 0){
		char aBuf[256];
		str_format(aBuf, sizeof(aBuf), "kill spikes killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
			pKillerID, Server()->ClientName(pKillerID),
			m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
		GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

		if(IsFrozen() && pKillerID != m_pPlayer->GetCID())
		{
			if (GameServer()->GetPlayerChar(pKillerID))
				GameServer()->GetPlayerChar(pKillerID)->AddSpree();

			// send the kill message
			CNetMsg_Sv_KillMsg Msg;
			Msg.m_Killer = pKillerID;
			Msg.m_Victim = m_pPlayer->GetCID();
			Msg.m_Weapon = WEAPON_NINJA;
			Msg.m_ModeSpecial = ModeSpecial;
			GameServer()->SendPackMsg(&Msg, MSGFLAG_VITAL);

			if(GameServer()->m_pController->IsTeamplay() && GameServer()->m_pController->IsFalseSpike(GameServer()->m_apPlayers[pKillerID]->GetTeam(), spikes_flag))
			{
				CCharacter *pKiller = ((CPlayer *)GameServer()->m_apPlayers[pKillerID])->GetCharacter();
				if (pKiller && !pKiller->IsFrozen()) {
					pKiller->Freeze(g_Config.m_SvFalseSpikeFreeze);
					GameServer()->CreateSound(pKiller->m_Pos, SOUND_TEE_CRY);
				}
			}
			else {
				//set attacker's face to happy (taunt!)
				CCharacter* pKiller = ((CPlayer*)GameServer()->m_apPlayers[pKillerID])->GetCharacter();
				if (pKiller)
				{
					pKiller->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
				}

				if (GameServer()->m_pController->IsTeamplay()) {
					GameServer()->CreateSoundTeam(m_Pos, SOUND_CTF_CAPTURE, GameServer()->m_apPlayers[pKillerID]->GetTeam(), pKillerID);
					GameServer()->CreateSoundTeam(m_Pos, SOUND_CTF_GRAB_PL, m_pPlayer->GetTeam(), m_pPlayer->GetCID());
				} else {
					GameServer()->CreateSoundGlobal(SOUND_CTF_CAPTURE, pKillerID);
					GameServer()->CreateSoundGlobal(SOUND_CTF_GRAB_PL, m_pPlayer->GetCID());			
				}
			}
		}
		//if not frozen or selfkill
		else {
			// send the kill message
			CNetMsg_Sv_KillMsg Msg;
			Msg.m_Killer = m_pPlayer->GetCID();
			Msg.m_Victim = m_pPlayer->GetCID();
			Msg.m_Weapon = WEAPON_WORLD;
			Msg.m_ModeSpecial = ModeSpecial;
			GameServer()->SendPackMsg(&Msg, MSGFLAG_VITAL);
		}

		// a nice sound
		GameServer()->CreateSound(m_Pos, SOUND_PLAYER_DIE);
		if(g_Config.m_SvSmoothFreezeMode)
			GameServer()->SendTuningParams(m_pPlayer->GetCID());

		// this is for auto respawn after 3 secs
		m_pPlayer->m_DieTick = Server()->Tick();

		GameServer()->m_World.RemoveEntity(this);
		Destroy();
		GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
	}

}

void CCharacter::Hit(int Killer, int Weapon)
{
	int ModeSpecial = GameServer()->m_pController->OnCharacterDeath(this, GameServer()->m_apPlayers[Killer], Weapon);
	
	m_Killer.m_uiKillerHookTicks = 0;
	m_Killer.m_KillerID = Killer;

	char aBuf[256];
	str_format(aBuf, sizeof(aBuf), "hit killer='%d:%s' victim='%d:%s' weapon=%d special=%d",
		Killer, Server()->ClientName(Killer),
		m_pPlayer->GetCID(), Server()->ClientName(m_pPlayer->GetCID()), Weapon, ModeSpecial);
	GameServer()->Console()->Print(IConsole::OUTPUT_LEVEL_DEBUG, "game", aBuf);

	// send the kill message
	CNetMsg_Sv_KillMsg Msg;
	Msg.m_Killer = Killer;
	Msg.m_Victim = m_pPlayer->GetCID();
	Msg.m_Weapon = Weapon;
	Msg.m_ModeSpecial = ModeSpecial;
	GameServer()->SendPackMsg(&Msg, MSGFLAG_VITAL);

	GameServer()->CreateSoundGlobal(SOUND_HIT, Killer);
	GameServer()->CreateDeath(m_Pos, m_pPlayer->GetCID());
}

bool CCharacter::TakeDamage(vec2 Force, int Dmg, int From, int Weapon)
{
	m_Core.m_Vel += Force;

	CPlayer *pPlayer = GameServer()->m_apPlayers[From];

	if(m_Invisible) {
		m_Visible = true;
		m_InvisibleTick = 0;
	}
	if ((Weapon == WEAPON_RIFLE || Weapon == WEAPON_GRENADE) && !IsFrozen()) {
		if ((GameServer()->m_pController->IsFriendlyFire(m_pPlayer->GetCID(), From) && !g_Config.m_SvTeamdamage) || From == m_pPlayer->GetCID()) {
			if (From == m_pPlayer->GetCID()) {
				//don't loose nades, when hitting yourself
				if(m_aWeapons[WEAPON_GRENADE].m_Got) m_aWeapons[WEAPON_GRENADE].m_Ammo = min(m_aWeapons[m_ActiveWeapon].m_Ammo + 1, 5);
			}
			return false;
		}
		
		//this is better for grenade fng
		if(Weapon == WEAPON_GRENADE && Dmg < g_Config.m_SvGrenadeDamageToHit) return false;

		if (m_InvincibleTick == 0) {
			Freeze(g_Config.m_SvHitFreeze);
			Hit(From, Weapon);

			//set attacker's face to happy (taunt!)
			if (From >= 0 && From != m_pPlayer->GetCID() && GameServer()->m_apPlayers[From])
			{
				CCharacter *pChr = pPlayer->GetCharacter();
				if (pChr)
				{
					pChr->SetEmote(EMOTE_HAPPY, Server()->Tick() + Server()->TickSpeed());
				}
			}

			GameServer()->CreateSound(m_Pos, SOUND_PLAYER_PAIN_SHORT);

			m_EmoteType = EMOTE_PAIN;
			m_EmoteStop = Server()->Tick() + 500 * Server()->TickSpeed() / 1000;
		}
	}
	return true;
}


void CCharacter::TakeHammerHit(CCharacter* pFrom)
{
	vec2 Dir;
	if (length(m_Pos - pFrom->m_Pos) > 0.0f)
		Dir = normalize(m_Pos - pFrom->m_Pos);
	else
		Dir = vec2(0.f, -1.f);

	vec2 Push = vec2(0.f, -1.f) + normalize(Dir + vec2(0.f, -1.1f)) * 10.0f;
	if (GameServer()->m_pController->IsTeamplay() && pFrom->GetPlayer() && m_pPlayer->GetTeam() == pFrom->GetPlayer()->GetTeam() && IsFrozen()) {
		Push.x *= g_Config.m_SvMeltHammerScaleX*0.01f;
		Push.y *= g_Config.m_SvMeltHammerScaleY*0.01f;
	}
	else {
		Push.x *= g_Config.m_SvHammerScaleX*0.01f;
		Push.y *= g_Config.m_SvHammerScaleY*0.01f;
	}
	if (pFrom->m_HammerForce)
		Push *= 1.5f;

	m_Core.m_Vel += Push;

	CPlayer* pPlayer = pFrom->GetPlayer();
	if (pPlayer) {
		if(GameServer()->m_pController->IsTeamplay() && pPlayer->GetTeam() == m_pPlayer->GetTeam()) {
			m_Killer.m_uiKillerHookTicks = 0;
			m_Killer.m_KillerID = m_pPlayer->GetCID();
			if (IsFrozen()) {
				if (((float)m_Core.m_Freeze.m_Duration - (float)(Server()->Tick() - m_Core.m_Freeze.m_ActivationTick - 1) / (float)Server()->TickSpeed()) < 3.f) {
					Unfreeze(pPlayer->GetCID());
					m_MoltenAt = Server()->Tick();
					m_MoltenByHammer = true;
				}
				else {
					m_Core.m_Freeze.m_ActivationTick -= Server()->TickSpeed() * 3;
				}
				++pPlayer->m_Stats.m_UnfreezingHammerHits;
			}
		} else if(pFrom->m_HammerFreeze && !IsFrozen()) {
			m_Killer.m_KillerID = pPlayer->GetCID();
			Freeze(g_Config.m_SvHitFreeze);
		}
	}
	else {
		m_Killer.m_uiKillerHookTicks = 0;
		m_Killer.m_KillerID = pPlayer->GetCID();
	}
}

void CCharacter::Snap(int SnappingClient)
{
	float Distance = INFINITY;
	if(NetworkClipped(SnappingClient, Distance))
		return;
	
	int ClientID = m_pPlayer->GetCID();

	if(m_Invisible && ClientID != SnappingClient && !m_Visible && GameServer()->m_apPlayers[SnappingClient]->GetTeam() != TEAM_SPECTATORS && !IsFrozen() /* && !GetPlayer()->m_Paused*/)
		return;

	if(SnappingClient > -1 && GameServer()->m_apPlayers[SnappingClient] && !GameServer()->m_apPlayers[SnappingClient]->AddSnappingClient(m_pPlayer->GetCID(), Distance, GameServer()->m_apPlayers[SnappingClient]->m_ClientVersion, ClientID)) return;

	CNetObj_Character *pCharacter = static_cast<CNetObj_Character *>(Server()->SnapNewItem(NETOBJTYPE_CHARACTER, ClientID, sizeof(CNetObj_Character)));
	if(!pCharacter)
		return;

	// write down the m_Core
	if(!m_ReckoningTick || GameServer()->m_World.m_Paused)
	{
		// no dead reckoning when paused because the client doesn't know
		// how far to perform the reckoning
		pCharacter->m_Tick = 0;
		m_Core.Write(pCharacter);
	}
	else
	{
		pCharacter->m_Tick = m_ReckoningTick;
		m_SendCore.Write(pCharacter);
	}

	// set emote
	if (m_EmoteStop < Server()->Tick())
	{
		m_EmoteType = EMOTE_NORMAL;
		m_EmoteStop = -1;
	}

	pCharacter->m_Emote = m_EmoteType;

	pCharacter->m_AmmoCount = 0;
	pCharacter->m_Health = 0;
	pCharacter->m_Armor = 0;
	
	if(GameServer()->m_pController->UseFakeTeams() && IsFrozen()){
		pCharacter->m_Weapon = WEAPON_GUN;
	} else pCharacter->m_Weapon = m_ActiveWeapon;
	pCharacter->m_AttackTick = m_AttackTick;

	pCharacter->m_Direction = m_Input.m_Direction;

	int HookedID = pCharacter->m_HookedPlayer;
	if (HookedID != -1 && SnappingClient > -1 && GameServer()->m_apPlayers[SnappingClient] && !GameServer()->m_apPlayers[SnappingClient]->IsSnappingClient(HookedID, GameServer()->m_apPlayers[SnappingClient]->m_ClientVersion, HookedID)) {
		pCharacter->m_HookedPlayer = -1;
	}
	else if(IsAlive()) pCharacter->m_HookedPlayer = HookedID;
	else pCharacter->m_HookedPlayer = -1;


	if(m_pPlayer->GetCID() == SnappingClient || SnappingClient == -1 ||
		(!g_Config.m_SvStrictSpectateMode && m_pPlayer->GetCID() == GameServer()->m_apPlayers[SnappingClient]->m_SpectatorID))
	{
		pCharacter->m_Health = 10;
		pCharacter->m_Armor = 0;
		if(m_aWeapons[m_ActiveWeapon].m_Ammo > 0)
			pCharacter->m_AmmoCount = m_aWeapons[m_ActiveWeapon].m_Ammo;
	}

	if(pCharacter->m_Emote == EMOTE_NORMAL && (m_pPlayer->m_Emotion == EMOTE_NORMAL || m_pPlayer->m_EmotionDuration == 0))
	{
		if(250 - ((Server()->Tick() - m_LastAction)%(250)) < 5)
			pCharacter->m_Emote = EMOTE_BLINK;
	} else if(pCharacter->m_Emote == EMOTE_NORMAL && m_pPlayer->m_Emotion != EMOTE_NORMAL && m_pPlayer->m_EmotionDuration != 0){
		pCharacter->m_Emote = m_pPlayer->m_Emotion;
	}

	pCharacter->m_PlayerFlags = GetPlayer()->m_PlayerFlags;
}


int CCharacter::NetworkClipped(int SnappingClient, float& Distance)
{
	return NetworkClipped(SnappingClient, Distance, m_Pos);
}

int CCharacter::NetworkClipped(int SnappingClient, float& Distance, vec2 CheckPos)
{
	if (SnappingClient == -1)
		return 0;

	float dx = GameServer()->m_apPlayers[SnappingClient]->m_ViewPos.x - CheckPos.x;
	float dy = GameServer()->m_apPlayers[SnappingClient]->m_ViewPos.y - CheckPos.y;

	if (absolute(dx) > 1000.0f || absolute(dy) > 800.0f)
		return 1;
	
	Distance = distance(GameServer()->m_apPlayers[SnappingClient]->m_ViewPos, CheckPos);
	if (distance(GameServer()->m_apPlayers[SnappingClient]->m_ViewPos, CheckPos) > 1100.0f)
		return 1;
	return 0;
}


void CCharacter::AddSpree()
{
	m_Spree++;
	const int NumMsg = 6;
	char aBuf[128];

	if(m_Spree % g_Config.m_SvKillingSpreeKills == 0)
	{
		static const char aaSpreeMsg[NumMsg][32] = {
			"is on a killing spree",
			"is on a rampage",
			"is dominating",
			"is unstoppable",
			"is the tee to kill",
			"is godlike"
		};

		int bigman = min(m_Spree / g_Config.m_SvKillingSpreeKills, NumMsg) - 1;

		str_format(aBuf, sizeof(aBuf), "%s %s with %d kills!", Server()->ClientName(m_pPlayer->GetCID()), aaSpreeMsg[bigman], m_Spree);
		GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);

		if(!m_HammerFreeze && m_Spree == g_Config.m_SvKillingSpreeKills * 1) {
			m_HammerFreeze = true;
			GameServer()->SendBroadcast("you got the killingspree award [IceHammer]", m_pPlayer->GetCID());
		}

		else if(!m_HammerForce && m_Spree == g_Config.m_SvKillingSpreeKills * 2) {
			m_HammerForce = true;
			GameServer()->SendBroadcast("you got the killingspree award [BigHammer]", m_pPlayer->GetCID());
		}

		else if(!m_GrenadeLauncher && m_Spree == g_Config.m_SvKillingSpreeKills * 3) {
			m_GrenadeLauncher = true;
			GameServer()->SendBroadcast("you got the killingspree award [Grenade]", m_pPlayer->GetCID());
		}

		else if(!m_JetPack && m_Spree == g_Config.m_SvKillingSpreeKills * 4) {
			m_JetPack = true;
			GameServer()->SendBroadcast("you got the killingspree award [JetPack]", m_pPlayer->GetCID());
		}

		else if(!m_SpeedRunner && m_Spree == g_Config.m_SvKillingSpreeKills * 5) {
			m_SpeedRunner = true;
			GameServer()->SendBroadcast("you got the killingspree award [MaxSpeed]", m_pPlayer->GetCID());
		}

		else if(!m_RifleSpread && m_Spree == g_Config.m_SvKillingSpreeKills * 6) {
			m_RifleSpread = true;
			GameServer()->SendBroadcast("you got the killingspree award [Laser2x]", m_pPlayer->GetCID());
		}

		else if(!m_TeamProtect && m_Spree == g_Config.m_SvKillingSpreeKills * 7) {
			m_TeamProtect = true;
			GameServer()->SendBroadcast("you got the last killingspree award [TeamProtection]", m_pPlayer->GetCID());
		}

		else if(!m_Invisible && m_Spree == g_Config.m_SvKillingSpreeKills * 8) {
			m_Invisible = true;
			GameServer()->SendBroadcast("you got the killingspree award [Invisibility]", m_pPlayer->GetCID());
		}

		else if(!m_GrenadeLauncher && m_Spree == g_Config.m_SvKillingSpreeKills * 9) {
			m_ShotGun = true;
			GameServer()->SendBroadcast("you got the killingspree award [Shotgun]", m_pPlayer->GetCID());
		}
	}
}

void CCharacter::EndSpree(int Killer)
{
	if(m_Spree >= g_Config.m_SvKillingSpreeKills) {
		if(m_Spree >= (g_Config.m_SvKillingSpreeKills * 5) + (g_Config.m_SvKillingSpreeKills * 2)) {
			GameServer()->CreateRingExplosion(m_Pos, m_pPlayer->GetCID(), 1, 15, 10, true);
		} else {
			GameServer()->CreateSound(m_Pos, SOUND_GRENADE_EXPLODE);
			GameServer()->CreateExplosion(m_Pos, m_pPlayer->GetCID(), WEAPON_RIFLE, true);
		}

		if(g_Config.m_SvKillingSpreePrint && g_Config.m_SvKillingSpreeKills > 0) {
			char aBuf[128];
			if(Killer != m_pPlayer->GetCID()) {
				str_format(aBuf, sizeof(aBuf), "%s %d-kills killing spree was ended by %s", Server()->ClientName(m_pPlayer->GetCID()), m_Spree, Server()->ClientName(Killer));
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
				GameServer()->SendBroadcast("You lost all of your items", m_pPlayer->GetCID());
			} else {
				str_format(aBuf, sizeof(aBuf), "%s %d-kills killing spree was ended.", Server()->ClientName(m_pPlayer->GetCID()), m_Spree);
				GameServer()->SendChat(-1, CGameContext::CHAT_ALL, aBuf);
				GameServer()->SendBroadcast("You lost all of your items", m_pPlayer->GetCID());
			}
		}
	}

	m_HammerFreeze = false;
	m_HammerForce = false;
	m_JetPack = false;
	m_SpeedRunner = false;
	m_RifleSpread = false;
	m_Invisible = false;
	m_TeamProtect = false;
	m_GrenadeLauncher = false;
	m_ShotGun = false;
	m_Spree = 0;
}