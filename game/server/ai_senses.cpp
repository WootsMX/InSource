//========= Copyright � 1996-2005, Valve Corporation, All rights reserved. ============//
//
// Purpose:
//
// $NoKeywords: $
//=============================================================================//

#include "cbase.h"

#include "ai_senses.h"

#include "soundent.h"
#include "team.h"
#include "ai_basenpc.h"
#include "saverestore_utlvector.h"



// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

// Use this to disable caching and other optimizations in senses
#define DEBUG_SENSES 1

#ifdef DEBUG_SENSES
#define AI_PROFILE_SENSES(tag) AI_PROFILE_SCOPE(tag)
#else
#define AI_PROFILE_SENSES(tag) ((void)0)
#endif

const float AI_STANDARD_NPC_SEARCH_TIME = .25;
const float AI_EFFICIENT_NPC_SEARCH_TIME = .35;
const float AI_HIGH_PRIORITY_SEARCH_TIME = 0.15;
const float AI_MISC_SEARCH_TIME  = 0.45;

//-----------------------------------------------------------------------------

CAI_SensedObjectsManager g_AI_SensedObjectsManager;

//-----------------------------------------------------------------------------

#pragma pack(push)
#pragma pack(1)

struct AISightIterVal_t
{
	char  array;
	short iNext;
	char  SeenArray;
};

#pragma pack(pop)


//=============================================================================
//
// CAI_Senses
//
//=============================================================================

BEGIN_SIMPLE_DATADESC( CAI_Senses )

	DEFINE_FIELD( m_LookDist,			FIELD_FLOAT	),
	DEFINE_FIELD( m_LastLookDist, 	FIELD_FLOAT	),
	DEFINE_FIELD( m_TimeLastLook, 	FIELD_TIME	),
	DEFINE_FIELD( m_iSensingFlags, FIELD_INTEGER ),
	//								m_iAudibleList		(no way to save?)
	DEFINE_UTLVECTOR(m_SeenHighPriority, FIELD_EHANDLE ),
	DEFINE_UTLVECTOR(m_SeenNPCs, 		FIELD_EHANDLE ),
	DEFINE_UTLVECTOR(m_SeenMisc, 		FIELD_EHANDLE ),
	//								m_SeenArrays		(not saved, rebuilt)

	// Could fold these three and above timer into one concept, but would invalidate savegames
	DEFINE_FIELD( m_TimeLastLookHighPriority, 	FIELD_TIME	),
	DEFINE_FIELD( m_TimeLastLookNPCs, 	FIELD_TIME	),
	DEFINE_FIELD( m_TimeLastLookMisc, 	FIELD_TIME	),

END_DATADESC()

//-----------------------------------------------------------------------------

bool CAI_Senses::CanHearSound( CSound *pSound )
{
	if ( pSound->m_hOwner.Get() == GetCharacter() )
		return false;

	if ( GetOuter() )
	{
		if( GetOuter()->GetState() == NPC_STATE_SCRIPT && pSound->IsSoundType( SOUND_DANGER ) )
		{
			// For now, don't hear danger in scripted sequences. This probably isn't a
			// good long term solution, but it makes the Bank Exterior work better.
			return false;
		}

		if ( GetOuter()->IsInAScript() )
			return false;
	}

	// @TODO (toml 10-18-02): what about player sounds and notarget?
	float flHearDistanceSq = pSound->Volume() * GetCharacter()->HearingSensitivity();
	flHearDistanceSq *= flHearDistanceSq;
	if( pSound->GetSoundOrigin().DistToSqr( GetCharacter()->EarPosition() ) <= flHearDistanceSq )
	{
		return GetCharacter()->QueryHearSound( pSound );
	}

	return false;
}


//-----------------------------------------------------------------------------
// Listen - npcs dig through the active sound list for
// any sounds that may interest them. (smells, too!)

void CAI_Senses::Listen( void )
{
	m_iAudibleList = SOUNDLIST_EMPTY; 

	int iSoundMask = GetCharacter()->GetSoundInterests();
	
	if ( iSoundMask != SOUND_NONE && !(GetCharacter()->HasSpawnFlags(SF_NPC_WAIT_TILL_SEEN)) )
	{
		int	iSound = CSoundEnt::ActiveList();
		
		while ( iSound != SOUNDLIST_EMPTY )
		{
			CSound *pCurrentSound = CSoundEnt::SoundPointerForIndex( iSound );

			if ( pCurrentSound	&& (iSoundMask & pCurrentSound->SoundType()) && CanHearSound( pCurrentSound ) )
			{
	 			// the npc cares about this sound, and it's close enough to hear.
				pCurrentSound->m_iNextAudible = m_iAudibleList;
				m_iAudibleList = iSound;
			}

			iSound = pCurrentSound->NextSound();
		}
	}
	
	GetCharacter()->OnListened();
}

//-----------------------------------------------------------------------------

bool CAI_Senses::ShouldSeeEntity( CBaseEntity *pSightEnt )
{
	if ( pSightEnt == GetCharacter() )
		return false;
		
	if ( GetCharacter()->OnlySeeAliveEntities() && !pSightEnt->IsAlive() )
		return false;

    if ( pSightEnt->m_lifeState == LIFE_RESPAWNABLE )
        return false;

	if ( pSightEnt->IsPlayer() && ( pSightEnt->GetFlags() & FL_NOTARGET ) )
		return false;

	// don't notice anyone waiting to be seen by the player
	if ( pSightEnt->m_spawnflags & SF_NPC_WAIT_TILL_SEEN )
		return false;

	if ( GetOuter() && !pSightEnt->CanBeSeenBy( GetOuter() ) )
		return false;
	
	if ( !GetCharacter()->QuerySeeEntity( pSightEnt ) )
		return false;

	return true;
}

//-----------------------------------------------------------------------------

bool CAI_Senses::CanSeeEntity( CBaseEntity *pSightEnt )
{
    return GetCharacter()->IsAbleToSee( pSightEnt );
	//return ( GetCharacter()->FInViewCone( pSightEnt ) && GetCharacter()->FVisible( pSightEnt ) );
}



//-----------------------------------------------------------------------------

bool CAI_Senses::DidSeeEntity( CBaseEntity *pSightEnt ) const
{
	AISightIter_t iter;
	CBaseEntity *pTestEnt;

	pTestEnt = GetFirstSeenEntity( &iter );

	while( pTestEnt )
	{
		if ( pSightEnt == pTestEnt )
			return true;
		pTestEnt = GetNextSeenEntity( &iter );
	}
	return false;
}

//-----------------------------------------------------------------------------

void CAI_Senses::NoteSeenEntity( CBaseEntity *pSightEnt )
{
	pSightEnt->m_pLink = GetCharacter()->m_pLink;
	GetCharacter()->m_pLink = pSightEnt;
}
		
//-----------------------------------------------------------------------------

bool CAI_Senses::WaitingUntilSeen( CBaseEntity *pSightEnt )
{
	if ( GetCharacter()->m_spawnflags & SF_NPC_WAIT_TILL_SEEN )
	{
		if ( pSightEnt->IsPlayer() )
		{
			CBasePlayer *pPlayer = ToBasePlayer( pSightEnt );
			Vector zero =  Vector(0,0,0);
			// don't link this client in the list if the npc is wait till seen and the player isn't facing the npc
			if ( pPlayer
				// && pPlayer->FVisible( GetCharacter() ) 
				&& pPlayer->FInViewCone( GetCharacter() )
				&& FBoxVisible( pSightEnt, static_cast<CBaseEntity*>(GetCharacter()), zero ) )
			{
				// player sees us, become normal now.
				GetCharacter()->m_spawnflags &= ~SF_NPC_WAIT_TILL_SEEN;
				return false;
			}
		}
		return true;
	}

	return false;
}

//-----------------------------------------------------------------------------

bool CAI_Senses::SeeEntity( CBaseEntity *pSightEnt )
{
	GetCharacter()->OnSeeEntity( pSightEnt );

	// insert at the head of my sight list
	NoteSeenEntity( pSightEnt );

	return true;
}

//-----------------------------------------------------------------------------

CBaseEntity *CAI_Senses::GetFirstSeenEntity( AISightIter_t *pIter, seentype_t iSeenType ) const
{ 
	COMPILE_TIME_ASSERT( sizeof( AISightIter_t ) == sizeof( AISightIterVal_t ) );
	
	AISightIterVal_t *pIterVal = (AISightIterVal_t *)pIter;
	
	// If we're searching for a specific type, start in that array
	pIterVal->SeenArray = (char)iSeenType;
	int iFirstArray = ( iSeenType == SEEN_ALL ) ? 0 : iSeenType;

	for ( int i = iFirstArray; i < ARRAYSIZE( m_SeenArrays ); i++ )
	{
		if ( m_SeenArrays[i]->Count() != 0 )
		{
			pIterVal->array = i;
			pIterVal->iNext = 1;
			return (*m_SeenArrays[i])[0];
		}
	}
	
	(*pIter) = (AISightIter_t)(-1); 
	return NULL;
}

//-----------------------------------------------------------------------------

CBaseEntity *CAI_Senses::GetNextSeenEntity( AISightIter_t *pIter ) const	
{ 
	if ( ((int)*pIter) != -1 )
	{
		AISightIterVal_t *pIterVal = (AISightIterVal_t *)pIter;
		
		for ( int i = pIterVal->array;  i < ARRAYSIZE( m_SeenArrays ); i++ )
		{
			for ( int j = pIterVal->iNext; j < m_SeenArrays[i]->Count(); j++ )
			{
				if ( (*m_SeenArrays[i])[j].Get() != NULL )
				{
					pIterVal->array = i;
					pIterVal->iNext = j+1;
					return (*m_SeenArrays[i])[j];
				}
			}
			pIterVal->iNext = 0;

			// If we're searching for a specific type, don't move to the next array
			if ( pIterVal->SeenArray != SEEN_ALL )
				break;
		}
		(*pIter) = (AISightIter_t)(-1); 
	}
	return NULL;
}

//-----------------------------------------------------------------------------

void CAI_Senses::BeginGather()
{
	// clear my sight list
	GetCharacter()->m_pLink = NULL;
}

//-----------------------------------------------------------------------------

void CAI_Senses::EndGather( int nSeen, CUtlVector<EHANDLE> *pResult )
{
	pResult->SetCount( nSeen );
	if ( nSeen )
	{
		CBaseEntity *pCurrent = GetCharacter()->m_pLink;
		for (int i = 0; i < nSeen; i++ )
		{
			if ( !pCurrent )
			{
				(*pResult).Remove(i);
				break;
			}

			Assert( pCurrent );
			(*pResult)[i].Set( pCurrent );
			pCurrent = pCurrent->m_pLink;
		}
		GetCharacter()->m_pLink = NULL;
	}
}

//-----------------------------------------------------------------------------
// Look - Base class npc function to find enemies or 
// food by sight. iDistance is distance ( in units ) that the 
// npc can see.
//
// Sets the sight bits of the m_afConditions mask to indicate
// which types of entities were sighted.
// Function also sets the Looker's m_pLink 
// to the head of a link list that contains all visible ents.
// (linked via each ent's m_pLink field)
//

void CAI_Senses::Look( int iDistance )
{
	if ( m_TimeLastLook != gpGlobals->curtime || m_LastLookDist != iDistance )
	{
		//-----------------------------
		
		LookForHighPriorityEntities( iDistance );
		LookForNPCs( iDistance);
		LookForObjects( iDistance );
		
		//-----------------------------
		
		m_LastLookDist = iDistance;
		m_TimeLastLook = gpGlobals->curtime;
	}
	
	GetCharacter()->OnLooked( iDistance );
}

//-----------------------------------------------------------------------------

bool CAI_Senses::Look( CBaseEntity *pSightEnt )
{
	if ( WaitingUntilSeen( pSightEnt ) )
		return false;
	
	if ( ShouldSeeEntity( pSightEnt ) && CanSeeEntity( pSightEnt ) )
	{
		return SeeEntity( pSightEnt );
	}
	return false;
}



//-----------------------------------------------------------------------------

int CAI_Senses::LookForHighPriorityEntities( int iDistance )
{
	int nSeen = 0;
	if ( gpGlobals->curtime - m_TimeLastLookHighPriority > AI_HIGH_PRIORITY_SEARCH_TIME )
	{
		AI_PROFILE_SENSES(CAI_Senses_LookForHighPriorityEntities);
		m_TimeLastLookHighPriority = gpGlobals->curtime;
		
		BeginGather();
	
		const Vector &origin = GetAbsOrigin();
		
		// Players
		for ( int i = 1; i <= gpGlobals->maxClients; i++ )
		{
			CBaseEntity *pPlayer = UTIL_PlayerByIndex( i );

			if ( pPlayer )
			{
				if ( IsWithinSenseDistance( pPlayer->GetAbsOrigin(), origin, iDistance ) && Look( pPlayer ) )
				{
					nSeen++;
				}

			}
		}
	
		EndGather( nSeen, &m_SeenHighPriority );
    }
    else
    {
    	for ( int i = m_SeenHighPriority.Count() - 1; i >= 0; --i )
    	{
    		if ( m_SeenHighPriority[i].Get() == NULL )
    			m_SeenHighPriority.FastRemove( i );    			
    	}
    	nSeen = m_SeenHighPriority.Count();
    }
	
	return nSeen;
}

//-----------------------------------------------------------------------------

int CAI_Senses::LookForNPCs( int iDistance )
{
	bool bRemoveStaleFromCache = false;
	const Vector &origin = GetAbsOrigin();
	AI_Efficiency_t efficiency = (GetOuter()) ? GetOuter()->GetEfficiency() : AIE_NORMAL;
	float timeNPCs = ( efficiency < AIE_VERY_EFFICIENT ) ? AI_STANDARD_NPC_SEARCH_TIME : AI_EFFICIENT_NPC_SEARCH_TIME;
	if ( gpGlobals->curtime - m_TimeLastLookNPCs > timeNPCs )
	{
		AI_PROFILE_SENSES(CAI_Senses_LookForNPCs);

		m_TimeLastLookNPCs = gpGlobals->curtime;

		if ( efficiency < AIE_SUPER_EFFICIENT )
		{
			int i, nSeen = 0;

			BeginGather();

			CAI_BaseNPC **ppAIs = g_AI_Manager.AccessAIs();
			
			for ( i = 0; i < g_AI_Manager.NumAIs(); i++ )
			{
				if ( ppAIs[i] != GetCharacter() && ( ppAIs[i]->ShouldNotDistanceCull() || IsWithinSenseDistance( origin, ppAIs[i]->GetAbsOrigin(), iDistance ) ) )
				{
					if ( Look( ppAIs[i] ) )
					{
						nSeen++;
					}
				}
			}

			EndGather( nSeen, &m_SeenNPCs );

			return nSeen;
		}

		bRemoveStaleFromCache = true;
		// Fall through
	}

    for ( int i = m_SeenNPCs.Count() - 1; i >= 0; --i )
    {
    	if ( m_SeenNPCs[i].Get() == NULL )
		{
    		m_SeenNPCs.FastRemove( i );
		}
		else if ( bRemoveStaleFromCache )
		{
			if ( ( !((CAI_BaseNPC *)m_SeenNPCs[i].Get())->ShouldNotDistanceCull() && 
				   !IsWithinSenseDistance( origin, m_SeenNPCs[i]->GetAbsOrigin(), iDistance ) ) ||
				 !Look( m_SeenNPCs[i] ) )
			{
	    		m_SeenNPCs.FastRemove( i );
			}
		}
    }

    return m_SeenNPCs.Count();
}

//-----------------------------------------------------------------------------

int CAI_Senses::LookForObjects( int iDistance )
{	
	const int BOX_QUERY_MASK = FL_OBJECT;
	int	nSeen = 0;

	if ( gpGlobals->curtime - m_TimeLastLookMisc > AI_MISC_SEARCH_TIME )
	{
		AI_PROFILE_SENSES(CAI_Senses_LookForObjects);
		m_TimeLastLookMisc = gpGlobals->curtime;
		
		BeginGather();

		const Vector &origin = GetAbsOrigin();
		int iter;
		CBaseEntity *pEnt = g_AI_SensedObjectsManager.GetFirst( &iter );
		while ( pEnt )
		{
			if ( pEnt->GetFlags() & BOX_QUERY_MASK )
			{
				if ( IsWithinSenseDistance( origin, pEnt->GetAbsOrigin(), iDistance ) && Look( pEnt) )
				{
					nSeen++;
				}
				else
				{
				}
			}
			pEnt = g_AI_SensedObjectsManager.GetNext( &iter );
		}
		
		EndGather( nSeen, &m_SeenMisc );
	}
    else
    {
    	for ( int i = m_SeenMisc.Count() - 1; i >= 0; --i )
    	{
    		if ( m_SeenMisc[i].Get() == NULL )
    			m_SeenMisc.FastRemove( i );    			
    	}
    	nSeen = m_SeenMisc.Count();
    }

	return nSeen;
}

//-----------------------------------------------------------------------------

float CAI_Senses::GetTimeLastUpdate( CBaseEntity *pEntity )
{
	if ( !pEntity )
		return 0;
	if ( pEntity->IsPlayer() )
		return m_TimeLastLookHighPriority;
	if ( pEntity->IsNPC() )
		return m_TimeLastLookNPCs;
	return m_TimeLastLookMisc;
}

//-----------------------------------------------------------------------------

CSound* CAI_Senses::GetFirstHeardSound( AISoundIter_t *pIter )
{
	int iFirst = GetAudibleList(); 

	if ( iFirst == SOUNDLIST_EMPTY )
	{
		*pIter = (AISoundIter_t)SOUNDLIST_EMPTY;
		return NULL;
	}
	
	*pIter = (AISoundIter_t)iFirst;
	return CSoundEnt::SoundPointerForIndex( iFirst );
}

//-----------------------------------------------------------------------------

CSound* CAI_Senses::GetNextHeardSound( AISoundIter_t *pIter )
{
	int iCurrent = (int)*pIter;
	if ( iCurrent == SOUNDLIST_EMPTY )
	{
		*pIter = (AISoundIter_t)SOUNDLIST_EMPTY;
		return NULL;
	}
	
	iCurrent = CSoundEnt::SoundPointerForIndex( iCurrent )->m_iNextAudible;
	*pIter = (AISoundIter_t)iCurrent;
	if ( iCurrent == SOUNDLIST_EMPTY )
		return NULL;
	
	return CSoundEnt::SoundPointerForIndex( iCurrent );
}

//-----------------------------------------------------------------------------

CSound *CAI_Senses::GetClosestSound( bool fScent, int validTypes, bool bUsePriority )
{
	float flBestDist = MAX_COORD_RANGE*MAX_COORD_RANGE;// so first nearby sound will become best so far.
	float flDist;
	int iBestPriority = SOUND_PRIORITY_VERY_LOW;
	
	AISoundIter_t iter;
	
	CSound *pResult = NULL;
	CSound *pCurrent = GetFirstHeardSound( &iter );

	Vector earPosition = GetCharacter()->EarPosition();
	
	while ( pCurrent )
	{
		if ( ( !fScent && pCurrent->FIsSound() ) || 
			 ( fScent && pCurrent->FIsScent() ) )
		{
			if( pCurrent->IsSoundType( validTypes ) && !GetCharacter()->ShouldIgnoreSound( pCurrent ) )
			{
				if( !bUsePriority || GetCharacter()->GetSoundPriority(pCurrent) >= iBestPriority )
				{
					flDist = ( pCurrent->GetSoundOrigin() - earPosition ).LengthSqr();

					if ( flDist < flBestDist )
					{
						pResult = pCurrent;
						flBestDist = flDist;

						iBestPriority = GetCharacter()->GetSoundPriority(pCurrent);
					}
				}
			}
		}
		
		pCurrent = GetNextHeardSound( &iter );
	}
	
	return pResult;
}

//-----------------------------------------------------------------------------

void CAI_Senses::PerformSensing( void )
{
	AI_PROFILE_SCOPE	(CAI_BaseNPC_PerformSensing);
		
	// -----------------
	//  Look	
	// -----------------
	if( !HasSensingFlags(SENSING_FLAGS_DONT_LOOK) )
		Look( m_LookDist );
	
	// ------------------
	//  Listen
	// ------------------
	if( !HasSensingFlags(SENSING_FLAGS_DONT_LISTEN) )
		Listen();
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

void CAI_SensedObjectsManager::Init()
{
	CBaseEntity *pEnt = NULL;
	while ( ( pEnt = gEntList.NextEnt( pEnt ) ) != NULL )
	{
		OnEntitySpawned( pEnt );
	}

	gEntList.AddListenerEntity( this );
}

//-----------------------------------------------------------------------------

void CAI_SensedObjectsManager::Term()
{
	gEntList.RemoveListenerEntity( this );
	m_SensedObjects.RemoveAll();
}

//-----------------------------------------------------------------------------

CBaseEntity *CAI_SensedObjectsManager::GetFirst( int *pIter )
{
	if ( m_SensedObjects.Count() )
	{
		*pIter = 1;
		return m_SensedObjects[0];
	}
	
	*pIter = 0;
	return NULL;
}

//-----------------------------------------------------------------------------

CBaseEntity *CAI_SensedObjectsManager::GetNext( int *pIter )
{
	int i = *pIter;
	if ( i && i < m_SensedObjects.Count() )
	{
		(*pIter)++;
		return m_SensedObjects[i];
	}

	*pIter = 0;
	return NULL;
}

//-----------------------------------------------------------------------------

void CAI_SensedObjectsManager::OnEntitySpawned( CBaseEntity *pEntity )
{
	if ( ( pEntity->GetFlags() & FL_OBJECT ) && !pEntity->IsPlayer() && !pEntity->IsNPC() )
	{
		m_SensedObjects.AddToTail( pEntity );
	}
}

//-----------------------------------------------------------------------------

void CAI_SensedObjectsManager::OnEntityDeleted( CBaseEntity *pEntity )
{
	if ( ( pEntity->GetFlags() & FL_OBJECT ) && !pEntity->IsPlayer() && !pEntity->IsNPC() )
	{
		int i = m_SensedObjects.Find( pEntity );
		if ( i != m_SensedObjects.InvalidIndex() )
			m_SensedObjects.FastRemove( i );
	}
}

void CAI_SensedObjectsManager::AddEntity( CBaseEntity *pEntity )
{
	if ( m_SensedObjects.Find( pEntity ) != m_SensedObjects.InvalidIndex() )
		return;

	// We shouldn't be adding players or NPCs to this list
	Assert( !pEntity->IsPlayer() && !pEntity->IsNPC() );

	// Add the object flag so it gets removed when it dies
	pEntity->AddFlag( FL_OBJECT );
	m_SensedObjects.AddToTail( pEntity );
}

//=============================================================================