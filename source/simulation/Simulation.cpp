#include "precompiled.h"

#include <vector>

#include "EntityFormation.h"
#include "EntityManager.h"
#include "LOSManager.h"
#include "Projectile.h"
#include "Scheduler.h"
#include "Simulation.h"
#include "TurnManager.h"
#include "graphics/Model.h"
#include "graphics/Terrain.h"
#include "graphics/Unit.h"
#include "graphics/UnitManager.h"
#include "lib/timer.h"
#include "ps/CLogger.h"
#include "ps/Game.h"
#include "ps/GameAttributes.h"
#include "ps/Loader.h"
#include "ps/LoaderThunks.h"
#include "network/NetMessage.h"
#include "ps/Profile.h"
#include "renderer/Renderer.h"
#include "renderer/WaterManager.h"
#include "simulation/Entity.h"
#include "simulation/LOSManager.h"
#include "simulation/TerritoryManager.h"
#include "simulation/EntityTemplateCollection.h"

#include "gui/CGUI.h"

CSimulation::CSimulation(CGame *pGame):
	m_pGame(pGame),
	m_pWorld(pGame->GetWorld()),
	m_pTurnManager((g_SinglePlayerTurnManager=new CSinglePlayerTurnManager())),
	m_DeltaTime(0)
{
}

CSimulation::~CSimulation()
{
	delete g_SinglePlayerTurnManager;
	g_SinglePlayerTurnManager=NULL;
}

int CSimulation::Initialize(CGameAttributes* pAttribs)
{
	m_Random.seed(0);		// TODO: Store a random seed in CGameAttributes and synchronize it accross the network

	m_pTurnManager->Initialize(m_pGame->GetNumPlayers());

	// Call the game startup script 
	// TODO: Maybe don't do this if we're in Atlas
	// [2006-06-26 20ms]
	g_ScriptingHost.RunScript( "scripts/game_startup.js" );

	// [2006-06-26 3647ms]
	g_EntityManager.m_screenshotMode = pAttribs->m_ScreenshotMode;
	g_EntityManager.InitializeAll();

	// [2006-06-26: 61ms]
	m_pWorld->GetLOSManager()->Initialize(pAttribs->m_LOSSetting, pAttribs->m_FogOfWar);

	m_pWorld->GetTerritoryManager()->Initialize();

	return 0;
}


void CSimulation::RegisterInit(CGameAttributes *pAttribs)
{
	RegMemFun1(this, &CSimulation::Initialize, pAttribs, L"CSimulation", 3900);
}



void CSimulation::Update(double frameTime)
{
	m_DeltaTime += frameTime;
	
	if( m_DeltaTime >= 0.0 && frameTime )
	{
		PROFILE( "simulation turn" );
		// A new simulation frame is required.
		MICROLOG( L"calculate simulation" );
		Simulate();
		m_DeltaTime -= (m_pTurnManager->GetTurnLength()/1000.0);
		if( m_DeltaTime >= 0.0 )
		{
			// The desired sim frame rate can't be achieved. Settle for process & render
			// frames as fast as possible.
			frameTime -= m_DeltaTime; // so the animation stays in sync with the sim
			m_DeltaTime = 0.0;
		}
	}

	PROFILE_START( "simulation interpolation" );
	Interpolate(frameTime, ((1000.0*m_DeltaTime) / (float)m_pTurnManager->GetTurnLength()) + 1.0);
	PROFILE_END( "simulation interpolation" );
}

void CSimulation::Interpolate(double frameTime, double offset)
{
	const std::vector<CUnit*>& units=m_pWorld->GetUnitManager().GetUnits();
	for (uint i=0;i<units.size();++i)
		units[i]->GetModel()->Update((float)frameTime);

	g_EntityManager.interpolateAll( (float)offset );
	m_pWorld->GetProjectileManager().InterpolateAll( (float)offset );
	g_Renderer.GetWaterManager()->m_WaterTexTimer += frameTime;
}

void CSimulation::Simulate()
{
	PROFILE_START( "scheduler tick" );
	g_Scheduler.update(m_pTurnManager->GetTurnLength());
	PROFILE_END( "scheduler tick" );

	PROFILE_START( "entity updates" );
	g_EntityManager.updateAll( m_pTurnManager->GetTurnLength() );
	PROFILE_END( "entity updates" );

	PROFILE_START( "projectile updates" );
	m_pWorld->GetProjectileManager().UpdateAll( m_pTurnManager->GetTurnLength() );
	PROFILE_END( "projectile updates" );

	PROFILE_START( "los update" );
	m_pWorld->GetLOSManager()->Update();
	PROFILE_END( "los update" );

	PROFILE_START( "turn manager update" );
	m_pTurnManager->NewTurn();
	m_pTurnManager->IterateBatch(0, TranslateMessage, this);
	PROFILE_END( "turn manager update" );
}

// Location randomizer, for group orders...
// Having the group turn up at the destination with /some/ sort of cohesion is
// good but tasking them all to the exact same point will leave them brawling
// for it at the other end (it shouldn't, but the PASAP pathfinder is too
// simplistic)

// Task them all to a point within a radius of the target, radius depends upon
// the number of units in the group.

void RandomizeLocations(CEntityOrder order, const std::vector<HEntity> &entities, bool isQueued)
{
	std::vector<HEntity>::const_iterator it;
	float radius = 2.0f * sqrt( (float)entities.size() - 1 ); 

	for (it = entities.begin(); it < entities.end(); it++)
	{
		float _x, _y;
		CEntityOrder randomizedOrder = order;
		
		CSimulation* sim = g_Game->GetSimulation();
		do
		{
			_x = sim->RandFloat() * 2.0f - 1.0f;
			_y = sim->RandFloat() * 2.0f - 1.0f;
		}
		while( ( _x * _x ) + ( _y * _y ) > 1.0f );

		randomizedOrder.m_target_location.x += _x * radius;
		randomizedOrder.m_target_location.y += _y * radius;

		// Clamp it to within the map, just in case.
		float mapsize = (float)g_Game->GetWorld()->GetTerrain()->GetVerticesPerSide() * CELL_SIZE;
		randomizedOrder.m_target_location.x = clamp(randomizedOrder.m_target_location.x, 0.0f, mapsize);
		randomizedOrder.m_target_location.y = clamp(randomizedOrder.m_target_location.y, 0.0f, mapsize);

		if( !isQueued )
			(*it)->clearOrders();

		(*it)->pushOrder( randomizedOrder );
	}
}

void FormationLocations(CEntityOrder order, const std::vector<HEntity> &entities, bool isQueued)
{
	CVector2D upvec(0.0f, 1.0f);
	std::vector<HEntity>::const_iterator it = entities.begin();
	CEntityFormation* formation = (*it)->GetFormation();


	for (; it != entities.end(); it++)
	{
		CEntityOrder orderCopy = order;
		CVector2D posDelta = orderCopy.m_target_location - formation->GetPosition();
		CVector2D formDelta = formation->GetSlotPosition( (*it)->m_formationSlot );
		
		posDelta = posDelta.normalize();
		//Rotate the slot position's offset vector according to the rotation of posDelta.
		CVector2D rotDelta;
		float deltaCos = posDelta.dot(upvec);
		float deltaSin = sinf( acosf(deltaCos) );
		rotDelta.x = formDelta.x * deltaCos - formDelta.y * deltaSin;
		rotDelta.y = formDelta.x * deltaSin + formDelta.y * deltaCos; 

		orderCopy.m_target_location += rotDelta;

		// Clamp it to within the map, just in case.
		float mapsize = (float)g_Game->GetWorld()->GetTerrain()->GetVerticesPerSide() * CELL_SIZE;
		orderCopy.m_target_location.x = clamp(orderCopy.m_target_location.x, 0.0f, mapsize);
		orderCopy.m_target_location.y = clamp(orderCopy.m_target_location.y, 0.0f, mapsize);

		if( !isQueued )
			(*it)->clearOrders();

		(*it)->pushOrder( orderCopy );
	}
}

void QueueOrder(CEntityOrder order, const std::vector<HEntity> &entities, bool isQueued)
{
	std::vector<HEntity>::const_iterator it;

	for (it = entities.begin(); it < entities.end(); it++)
	{
		if( !isQueued )
			(*it)->clearOrders();

		(*it)->pushOrder( order );
	}
}

uint CSimulation::TranslateMessage(CNetMessage* pMsg, uint clientMask, void* UNUSED(userdata))
{
	CEntityOrder order;
	bool isQueued = true;
	
#define ENTITY_POSITION(_msg, _order) do\
	{ \
		_msg *msg=(_msg *)pMsg; \
		isQueued = msg->m_IsQueued != 0; \
		order.m_type=CEntityOrder::_order; \
		order.m_target_location.x=(float)msg->m_TargetX; \
		order.m_target_location.y=(float)msg->m_TargetY; \
		RandomizeLocations(order, msg->m_Entities, isQueued); \
	} while(0)
#define ENTITY_POSITION_FORM(_msg, _order) do\
	{ \
		_msg *msg=(_msg *)pMsg; \
		isQueued = msg->m_IsQueued != 0; \
		order.m_type=CEntityOrder::_order; \
		order.m_target_location.x=(float)msg->m_TargetX; \
		order.m_target_location.y=(float)msg->m_TargetY; \
		FormationLocations(order, msg->m_Entities, isQueued); \
	} while(0)
#define ENTITY_ENTITY_INT(_msg, _order) do\
	{ \
		_msg *msg=(_msg *)pMsg; \
		isQueued = msg->m_IsQueued != 0; \
		order.m_type=CEntityOrder::_order; \
		order.m_target_entity=msg->m_Target; \
		order.m_action=msg->m_Action; \
		QueueOrder(order, msg->m_Entities, isQueued); \
	} while(0)
#define ENTITY_INT_STRING(_msg, _order) do\
	{ \
		_msg *msg=(_msg *)pMsg; \
		isQueued = msg->m_IsQueued != 0; \
		order.m_type=CEntityOrder::_order; \
		order.m_produce_name=msg->m_Name; \
		order.m_produce_type=msg->m_Type; \
		QueueOrder(order, msg->m_Entities, isQueued); \
	} while(0)
	
	switch (pMsg->GetType())
	{
		case NMT_AddWaypoint:
		{
			CAddWaypoint *msg=(CAddWaypoint *)pMsg;
			isQueued = msg->m_IsQueued != 0;
			order.m_type=CEntityOrder::ORDER_LAST;
			order.m_target_location.x=(float)msg->m_TargetX;
			order.m_target_location.y=(float)msg->m_TargetY;
			for(CEntityIt it = msg->m_Entities.begin(); it != msg->m_Entities.end(); ++it)
			{
				HEntity& hentity = *it;

				const CEntityOrders& order_queue = hentity->m_orderQueue;
				for(CEntityOrderCRIt ord_it = order_queue.rbegin(); ord_it != order_queue.rend(); ++ord_it)
				{
					if (ord_it->m_type == CEntityOrder::ORDER_PATH_END_MARKER)
					{
						order.m_type = CEntityOrder::ORDER_GOTO;
						hentity->pushOrder(order);
						break;
					}
					if (ord_it->m_type == CEntityOrder::ORDER_PATROL)
					{
						order.m_type = ord_it->m_type;
						hentity->pushOrder(order);
						break;
					}
				}
				if (order.m_type == CEntityOrder::ORDER_LAST)
				{
					LOG(ERROR, "simulation", "Got an AddWaypoint message for an entity that isn't moving.");
				}
			}
			break;
		}
		case NMT_Goto:
			ENTITY_POSITION(CGoto, ORDER_GOTO);
			break;
		case NMT_Run:
			ENTITY_POSITION(CRun, ORDER_RUN);
			break;
		case NMT_Patrol:
			ENTITY_POSITION(CPatrol, ORDER_PATROL);
			break;
		case NMT_FormationGoto:
			ENTITY_POSITION_FORM(CFormationGoto, ORDER_GOTO);
			break;

		//TODO: make formation move to within range of target and then attack normally
		case NMT_Generic:
			ENTITY_ENTITY_INT(CGeneric, ORDER_GENERIC);
			break;
		case NMT_FormationGeneric:
			ENTITY_ENTITY_INT(CFormationGeneric, ORDER_GENERIC);
			break;
		case NMT_NotifyRequest:
			ENTITY_ENTITY_INT(CNotifyRequest, ORDER_NOTIFY_REQUEST);
			break;
		case NMT_Produce:
			ENTITY_INT_STRING(CProduce, ORDER_PRODUCE);
			break;
		case NMT_PlaceObject:
			{
				CPlaceObject *msg = (CPlaceObject *) pMsg;
				isQueued = msg->m_IsQueued != 0;
				
				// Figure out the player
				CPlayer* player = 0;
				if(msg->m_Entities.size() > 0) 
					player = msg->m_Entities[0]->GetPlayer();
				else
					player = g_Game->GetLocalPlayer();

				// Create the object
				CVector3D pos(msg->m_X/1000.0f, msg->m_Y/1000.0f, msg->m_Z/1000.0f);
				HEntity newObj = g_EntityManager.createFoundation( msg->m_Template, player, pos, msg->m_Angle/1000.0f );
				newObj->m_actor->SetPlayerID(player->GetPlayerID());
				if( newObj->Initialize() )
				{
					// Order all the selected units to work on the new object using the given action
					order.m_type = CEntityOrder::ORDER_START_CONSTRUCTION;
					order.m_new_obj = newObj;
					QueueOrder(order, msg->m_Entities, isQueued);
				}
			} while(0)
			break;
		
	}

	return clientMask;
}

uint CSimulation::GetMessageMask(CNetMessage* UNUSED(pMsg), uint UNUSED(oldMask), void* UNUSED(userdata))
{
	//CSimulation *pSimulation=(CSimulation *)userdata;

	// Pending a complete visibility/minimal-update implementation, we'll
	// simply select the first 32 connected clients ;-)
	return 0xFFFFFFFF;
}

void CSimulation::QueueLocalCommand(CNetMessage *pMsg)
{
	m_pTurnManager->QueueLocalCommand(pMsg);
}


// Get a random integer between 0 and maxVal-1 from the simulation's random number generator
int CSimulation::RandInt(int maxVal) 
{
	boost::uniform_smallint<int> distr(0, maxVal-1);
	return distr(m_Random);
}

// Get a random float in [0, 1) from the simulation's random number generator
float CSimulation::RandFloat() 
{
	// Cannot use uniform_01 here because it is not a real distribution, but rather an
	// utility class that makes a copy of the generator, and therefore it would repeatedly
	// return the same values because it never modifies our copy of the generator.
	boost::uniform_real<float> distr(0.0f, 1.0f);
	return distr(m_Random);
}
