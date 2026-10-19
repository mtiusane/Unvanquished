/*
===========================================================================

Unvanquished GPL Source Code
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of the Unvanquished GPL Source Code (Unvanquished Source Code).

Unvanquished is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Unvanquished is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Unvanquished; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

===========================================================================
*/

#include "common/Common.h"
#include "sg_bot_util.h"
#include "botlib/bot_types.h"
#include "botlib/bot_api.h"
#include "shared/bot_nav_shared.h"
#include "shared/navgen/navgen.h"

#include "shared/bg_gameplay.h"

#include <glm/geometric.hpp>
#include <glm/gtx/norm.hpp>

//trace distance to trigger AI obstacled avoiding procedures.
//this is a constant as fiddling with this value will impact both
//server performance and efficiency of AI obstacle procedures.
constexpr float BOT_OBSTACLE_AVOID_RANGE = 20.0f;

//tells if all navmeshes loaded successfully
// Only G_BotNavInit and G_BotNavCleanup should set it
navMeshStatus_t navMeshLoaded = navMeshStatus_t::UNINITIALIZED;

/*
===========================
Navigation Mesh Generation
===========================
*/

static Cvar::Cvar<int> g_bot_navgen_maxThreads(
	"g_bot_navgen_maxThreads", "Number of background threads to use when generating navmeshes. 0 for main thread",
	Cvar::NONE, 1);

// TODO: Latch(), when supported in gamelogic
Cvar::Cvar<bool> g_bot_navmeshReduceTypes(
	"g_bot_navmeshReduceTypes", "generate/use fewer navmeshes by sharing meshes between similar species",
	Cvar::NONE, false);

// blocks the main thread!
void G_BlockingGenerateNavmesh( std::bitset<PCL_NUM_CLASSES> classes )
{
	std::string mapName = Cvar::GetValue( "mapname" );
	NavmeshGenerator navgen;
	navgen.LoadMapAndEnqueueTasks( mapName, classes, g_bot_navmeshReduceTypes.Get() );
	navgen.StartBackgroundThreads( g_bot_navgen_maxThreads.Get() );
	navgen.WaitInMainThread( []( float ) {} );
}

static Cvar::Range<Cvar::Cvar<int>> msecPerFrame(
	"g_bot_navgen_msecPerFrame", "time budget per frame for single-threaded navmesh generation",
	Cvar::NONE, 20, 1, 1500 );

static Cvar::Cvar<int> frameToggle("g_bot_navgen_frame", "FOR INTERNAL USE", Cvar::NONE, 0);
static Cvar::Cvar<bool> g_bot_autocrouch("g_bot_autocrouch", "whether bots should crouch when they detect an obstacle", Cvar::NONE, true);

static NavmeshGenerator navgen;
bool usingBackgroundThreads;
static std::unique_ptr<NavgenTask> generatingNow; // used if generating on the main thread
static int nextLogTime;

static void G_BotBackgroundNavgenShutdown()
{
	navgen.~NavmeshGenerator();
	new (&navgen) NavmeshGenerator();
	generatingNow.reset();
}

// Returns true if done
static bool MainThreadBackgroundNavgen()
{
	int stopTime = Sys::Milliseconds() + msecPerFrame.Get();

	if ( !generatingNow )
	{
		generatingNow = navgen.PopTask();
		if ( !generatingNow )
		{
			return true;
		}
	}

	// HACK: if the game simulation time gets behind the real time, the server runs a bunch of
	// game frames within one server frame to catch up. If we do a lot of navgen then it can't
	// catch up which may lead to a cascade of very long server frames. What we want to do is
	// one navgen slice per *server frame*, not per game frame. There doesn't seem to be any API
	// for the server frame count, so put a command in the command buffer and check for the
	// next server frame by seeing whether it has executed.
	static int lastToggle = -12345;
	if ( lastToggle == frameToggle.Get() )
	{
		return false;
	}
	lastToggle = frameToggle.Get();
	trap_SendConsoleCommand( "toggle g_bot_navgen_frame" );

	while ( Sys::Milliseconds() < stopTime )
	{
		if ( navgen.Step( *generatingNow ) )
		{
			generatingNow->context.DoMainThreadTasks();
			generatingNow.reset();
			break;
		}
	}

	return false;
}

void G_BotBackgroundNavgen()
{
	if ( navMeshLoaded != navMeshStatus_t::GENERATING )
	{
		return;
	}

	bool done;

	if ( usingBackgroundThreads )
	{
		done = navgen.ThreadsDone();
		navgen.HandleFinishedTasks();
	}
	else
	{
		done = MainThreadBackgroundNavgen();
	}

	if ( done )
	{
		G_BotBackgroundNavgenShutdown();

		navMeshLoaded = navMeshStatus_t::UNINITIALIZED; // HACK: make G_BotNavInit not return at the start
		G_BotNavInit( 0 );

		if ( navMeshLoaded == navMeshStatus_t::LOAD_FAILED )
		{
			trap_SendServerCommand( -1, "print_tr " QQ( N_( "^1Bot navmesh generation failed!" ) ) );
			G_BotDelAllBots();
		}
		else
		{
			ASSERT_EQ( navMeshLoaded, navMeshStatus_t::LOADED );
			// Bots on spectate will now join in their think function
		}
	}
	else if ( level.time > nextLogTime )
	{
		std::string percent = std::to_string( int(navgen.FractionComplete() * 100) );
		trap_SendServerCommand( -1, va( "print_tr %s %s",
			QQ( N_( "Server: generating bot navigation meshes... $1$%" ) ), percent.c_str() ) );
		nextLogTime = level.time + 10000;
	}
}

/*
========================
Navigation Mesh Loading
========================
*/

extern void BotAddSavedObstacles();
// FIXME: use nav handle instead of classes
// see g_bot_navgen_onDemand for meaning of generateNeeded
void G_BotNavInit( int generateNeeded )
{
	if ( navMeshLoaded != navMeshStatus_t::UNINITIALIZED )
	{
		return;
	}

	Log::Notice( "==== Bot Navigation Initialization ====" );
	if ( g_bot_navmeshReduceTypes.Get() )
	{
		Log::Notice( "Using approximate navmesh substitutions for similar classes to reduce number"
		             " of navmeshes needed (g_bot_navmeshReduceTypes)" );
	}

	std::bitset<PCL_NUM_CLASSES> missing;
	std::string mapName = Cvar::GetValue( "mapname" );
	NavgenConfig config = ReadNavgenConfig( mapName );

	for ( class_t i : RequiredNavmeshes( g_bot_navmeshReduceTypes.Get() ) )
	{
		switch ( G_BotSetupNav( config, i ) )
		{
		case navMeshStatus_t::UNINITIALIZED:
			if ( generateNeeded )
			{
				missing[ i ] = true;
				break;
			}
			DAEMON_FALLTHROUGH;
		case navMeshStatus_t::LOAD_FAILED:
			Log::Warn( "Cannot load navigation mesh file for %s", BG_Class( i )->name );
			navMeshLoaded = navMeshStatus_t::LOAD_FAILED;
			G_BotShutdownNav();
			return;
		case navMeshStatus_t::LOADED:
			break;
		case navMeshStatus_t::GENERATING:
			ASSERT_UNREACHABLE();
		}
	}

	if ( missing.any() )
	{
		G_BotShutdownNav(); // TODO: allow shutdown/load of individual species

		if ( generateNeeded == -1 )
		{
			G_BlockingGenerateNavmesh( missing );
			return G_BotNavInit( 0 );
		}
		else
		{
			ASSERT( !generatingNow );
			navgen.LoadMapAndEnqueueTasks( mapName, missing, g_bot_navmeshReduceTypes.Get() );
			usingBackgroundThreads = g_bot_navgen_maxThreads.Get() > 0;

			if ( usingBackgroundThreads )
			{
				navgen.StartBackgroundThreads( g_bot_navgen_maxThreads.Get() );
			}

			navMeshLoaded = navMeshStatus_t::GENERATING;
			return;
		}
	}

	navMeshLoaded = navMeshStatus_t::LOADED;
	BotAddSavedObstacles();
}

void G_BotNavCleanup()
{
	G_BotShutdownNav();
	G_BotBackgroundNavgenShutdown();
	navMeshLoaded = navMeshStatus_t::UNINITIALIZED;
}

/*
========================
Bot Navigation Querys
========================
*/

static float RadiusFromBounds2D( const glm::vec3& mins, const glm::vec3& maxs )
{
	float rad1s = Square( mins[0] ) + Square( mins[1] );
	float rad2s = Square( maxs[0] ) + Square( maxs[1] );
	return sqrtf( std::max( rad1s, rad2s ) );
}

float BotGetGoalRadius( const gentity_t *self )
{
	botTarget_t &t = self->botMind->goal;
	glm::vec3 self_mins = VEC2GLM( self->r.mins );
	glm::vec3 self_maxs = VEC2GLM( self->r.maxs );
	if ( t.targetsCoordinates() )
	{
		// we check the coord to be (almost) in our bounding box
		return RadiusFromBounds2D( self_mins, self_maxs ) + BOT_OBSTACLE_AVOID_RANGE;
	}

	// we don't check if the entity is valid: an outdated result is
	// better than failing here
	const gentity_t *target = t.getTargetedEntity();
	if ( target->s.modelindex == BA_H_MEDISTAT || target->s.modelindex == BA_A_BOOSTER )
	{
		// we want to be quite close to medistat.
		// TODO(freem): is this really what we want for booster?
		return self->r.maxs[0] + target->r.maxs[0];
	}

	return RadiusFromBounds2D( VEC2GLM( target->r.mins ), VEC2GLM( target->r.maxs ) )
	       + RadiusFromBounds2D( self_mins, self_maxs ) + BOT_OBSTACLE_AVOID_RANGE;
}

bool GoalInRange( const gentity_t *self, float r )
{
	gentity_t *ent = nullptr;
	// we don't need to check the goal is valid here

	if ( self->botMind->goal.targetsCoordinates() )
	{
		// We handle the z direction here differently,
		// to still count standing over a turret on the
		// target point or sitting next to a buildable
		// counts as a "we have reached our destination".
		glm::vec3 deltaPos = VEC2GLM( self->s.origin ) - self->botMind->nav().tpos;
		return Length2D( deltaPos ) < r
				&& fabsf( deltaPos.z ) <= 90;
	}

	while ( ( ent = G_IterateEntitiesWithinRadius( ent, VEC2GLM( self->s.origin ), r ) ) )
	{
		if ( ent == self->botMind->goal.getTargetedEntity() )
		{
			return true;
		}
	}

	return false;
}

float DistanceToGoal2DSquared( const gentity_t *self )
{
	glm::vec3 vec = self->botMind->goal.getPos() - VEC2GLM( self->s.origin );

	return Square( vec[ 0 ] ) + Square( vec[ 1 ] );
}

float DistanceToGoal( const gentity_t *self )
{
	glm::vec3 targetPos = self->botMind->goal.getPos();
	glm::vec3 selfPos = VEC2GLM( self->s.origin );
	return glm::distance( selfPos, targetPos );
}

float DistanceToGoalSquared( const gentity_t *self )
{
	glm::vec3 targetPos = self->botMind->goal.getPos();
	glm::vec3 selfPos = VEC2GLM( self->s.origin );
	return glm::distance2( selfPos, targetPos );
}

bool BotPathIsWalkable( const gentity_t *self, botTarget_t target )
{
	botTrace_t trace;

	glm::vec3 viewNormal = BG_GetClientNormal( &self->client->ps );
	glm::vec3 selfPos = VEC2GLM( self->s.origin ) + self->r.mins[2] * viewNormal;

	if ( !G_BotNavTrace( self->s.number, &trace, selfPos, target.getPos() ) )
	{
		return false;
	}

	return trace.frac >= 1.0f;
}

/*
========================
Local Bot Navigation
========================
*/

bool BotTraceForFloor( gentity_t *self, botMoveDir_t dir )
{
	// this is using a daemon trace to a solid surface
	// another feasible method might be to do a recast navmesh trace
	trace_t trace;
	glm::vec3 dirVec;
	glm::vec3 viewangles = VEC2GLM( self->client->ps.viewangles );
	viewangles[ PITCH ] = 0.f;
	AngleVectors( viewangles, &dirVec, nullptr, nullptr );
	// we only handle a subset of the possible values of the bitfield `dir`
	if ( dir == MOVE_BACKWARD )
	{
		dirVec.x = -dirVec.x;
		dirVec.y = -dirVec.y;
	}
	else if ( dir == MOVE_LEFT )
	{
		std::swap( dirVec.x, dirVec.y );
		dirVec.x = -dirVec.x;
	}
	else if ( dir == MOVE_RIGHT )
	{
		std::swap( dirVec.x, dirVec.y );
		dirVec.y = -dirVec.y;
	}
	else
	{
		return false;
	}
	// dirVec.z is 0
	glm::normalize( dirVec );
	glm::vec3 playerMins, playerMaxs;
	class_t pClass = static_cast<class_t>( self->client->ps.stats[STAT_CLASS] );
	BG_BoundingBox( pClass, &playerMins, &playerMaxs, nullptr, nullptr, nullptr );
	float width = playerMaxs.x - playerMins.x;
	float height = playerMaxs.z - playerMins.z;
	// a horizontal square with side length `width` that does not overlap with
	// the player's bbox in a diagonal direction has to have a distance of
	// at least sqrt(2) * width.
	// even though we are doing a raytrace and are not tracing the path of a square
	// area, this distance works well in experiments.
	glm::vec3 start = VEC2GLM( self->s.origin ) + dirVec * ( static_cast<float>( M_SQRT2 ) * width );
	start.z += playerMaxs.z;
	glm::vec3 end = start;
	end.z -= height * 2.f;
	trap_Trace( &trace, start, {}, {}, end, self->num(), MASK_SOLID | CONTENTS_PLAYERCLIP, 0 );
	return trace.fraction < 1.f;
}

static signed char BotGetMaxMoveSpeed( gentity_t *self )
{
	if ( usercmdButtonPressed( self->botMind->cmdBuffer.buttons, BTN_WALKING ) )
	{
		return 63;
	}

	return 127;
}

void BotStrafeDodge( gentity_t *self )
{
	usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;
	signed char speed = BotGetMaxMoveSpeed( self );
	bool floorRight = BotTraceForFloor( self, MOVE_RIGHT );
	bool floorLeft = BotTraceForFloor( self, MOVE_LEFT );

	if ( self->client->time1000 >= 500 && floorRight )
	{
		botCmdBuffer->rightmove = speed;
	}
	else if ( floorLeft )
	{
		botCmdBuffer->rightmove = -speed;
	}
	else if ( floorRight )
	{
		botCmdBuffer->rightmove = speed;
	}

	if ( ( self->client->time10000 % 2000 ) < 1000 )
	{
		if ( ( botCmdBuffer->rightmove < 0.f && floorRight ) || ( botCmdBuffer->rightmove > 0.f && floorLeft ) )
		{
			botCmdBuffer->rightmove *= -1;
		}
	}

	if ( ( self->client->time1000 % 300 ) >= 100 && ( self->client->time10000 % 3000 ) > 2000 )
	{
		botCmdBuffer->rightmove = 0;
	}
}

void BotMoveInDir( gentity_t *self, uint32_t dir )
{
	usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;
	signed char speed = BotGetMaxMoveSpeed( self );

	if ( dir & MOVE_FORWARD )
	{
		botCmdBuffer->forwardmove = speed;
	}
	else if ( dir & MOVE_BACKWARD )
	{
		botCmdBuffer->forwardmove = -speed;
	}

	if ( dir & MOVE_RIGHT )
	{
		botCmdBuffer->rightmove = speed;
	}
	else if ( dir & MOVE_LEFT )
	{
		botCmdBuffer->rightmove = -speed;
	}
}

void BotAlternateStrafe( gentity_t *self )
{
	usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;
	signed char speed = BotGetMaxMoveSpeed( self );
	bool floorRight = BotTraceForFloor( self, MOVE_RIGHT );
	bool floorLeft = BotTraceForFloor( self, MOVE_LEFT );

	if ( level.time % 8000 < 4000 && floorRight )
	{
		botCmdBuffer->rightmove = speed;
	}
	else if ( floorLeft )
	{
		botCmdBuffer->rightmove = -speed;
	}
	else if ( floorRight )
	{
		botCmdBuffer->rightmove = speed;
	}
}

void BotStandStill( gentity_t *self )
{
	usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;

	BotWalk( self, false );
	BotSprint( self, false );
	botCmdBuffer->forwardmove = 0;
	botCmdBuffer->rightmove = 0;
	botCmdBuffer->upmove = 0;
}

// Wallwalk-capable classes (dretch, mantis, advanced granger) "bite" on a wall to
// climb to a target above them. A jump or pounce issued while the bot is on the wall
// pops it off, so BotIsWallClimbing (below) suppresses jumps/pounces for as long as the
// bot is biting: both while it is physically attached (EF_WALLCLIMB/EF_WALLCLIMBCEILING,
// set by the engine) and for g_bot_wallwalkClimbHoldTime milliseconds after a climb is
// issued (before the attach takes hold). Declared here (before BotJump) because BotJump
// needs it; the sibling wallwalk cvars live further down.
static Cvar::Cvar<int> g_bot_wallwalkClimbHoldTime(
	"g_bot_wallwalkClimbHoldTime", "how long (ms) after issuing a wall climb a wallwalk-capable bot keeps biting instead of jumping/pouncing, so it doesn't pop off the wall", Cvar::NONE, 500 );

// True when a wallwalk-capable bot is biting on a wall/ceiling, so jumps/pounces are
// suppressed and the climb is kept alive. This covers two cases: the bot is physically
// attached to a surface (the engine sets EF_WALLCLIMB/EF_WALLCLIMBCEILING in
// PM_GroundClimbTrace), or it issued a climb within the g_bot_wallwalkClimbHoldTime hold
// window (the attach has not taken hold yet). While true the bot finishes reaching the
// target instead of being popped off -- as a human wallwalker does.
static bool BotIsWallClimbing( gentity_t *self )
{
	if ( !BG_ClassHasAbility( self->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER ) )
	{
		return false;
	}
	// While physically attached to a wall/ceiling the bot must not jump or pounce, or it
	// pops off the surface and whiffs the enemy above it. This holds for the whole climb,
	// independent of the (possibly already-expired) hold window below.
	if ( self->client->ps.eFlags & ( EF_WALLCLIMB | EF_WALLCLIMBCEILING ) )
	{
		return true;
	}
	// A climb was just issued (within the hold window): keep biting so the attach takes
	// hold instead of being popped off by a jump.
	return level.time - self->botMind->lastWallClimbTime < g_bot_wallwalkClimbHoldTime.Get();
}

// Forward declared: defined below, used by BotJump to also suppress a jump while the bot
// is *attempting* a wall bite (enemy above, at a wall) even before it is attached, so it
// doesn't pop off the wall and whiff the enemy when it reaches them near an edge.
static bool BotIsAttemptingWallBite( gentity_t *self );

bool BotJump( gentity_t *self )
{
	// Don't pop a bot off a wall/ceiling it is currently biting: suppress jumps/pounces
	// while the bot is attached to the surface, within the climb hold window, or is still
	// attempting to bite an enemy that sits above it (a jump there pops it off and whiffs
	// the enemy, whereas a human wallwalker stays on the wall until it reaches the target).
	if ( BotIsWallClimbing( self ) )
	{
		return false;
	}

	// Also suppress while attempting a bite (enemy above, at a wall) before the bot is
	// attached: popping off the wall there whiffs the enemy above it, as a human
	// wallwalker would not.
	if ( BotIsAttemptingWallBite( self ) )
	{
		return false;
	}

	if ( self->client->pers.team == TEAM_HUMANS )
	{
		int staminaJumpCost = BG_Class( self->client->ps.stats[ STAT_CLASS ] )->staminaJumpCost;

		if ( self->client->ps.stats[STAT_STAMINA] < staminaJumpCost )
		{
			return false;
		}
	}

	self->botMind->cmdBuffer.upmove = 127;
	return true;
}

bool BotSprint( gentity_t *self, bool enable )
{
	self->botMind->willSprint( enable );
	BotWalk( self, !enable );
	return enable;
}

void BotWalk( gentity_t *self, bool enable )
{
	usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;

	if ( !enable )
	{
		if ( usercmdButtonPressed( botCmdBuffer->buttons, BTN_WALKING ) )
		{
			usercmdReleaseButton( botCmdBuffer->buttons, BTN_WALKING );
			botCmdBuffer->forwardmove *= 2;
			botCmdBuffer->rightmove *= 2;
		}
		return;
	}

	if ( !usercmdButtonPressed( botCmdBuffer->buttons, BTN_WALKING ) )
	{
		usercmdPressButton( botCmdBuffer->buttons, BTN_WALKING );
		botCmdBuffer->forwardmove /= 2;
		botCmdBuffer->rightmove /= 2;
	}
}

// search for obstacle forward, and return pointer on it if any
static const gentity_t* BotGetPathBlocker( gentity_t *self, const glm::vec3 &dir )
{
	glm::vec3 playerMins, playerMaxs;
	trace_t trace;

	if ( !( self && self->client ) )
	{
		return nullptr;
	}

	class_t pClass = static_cast<class_t>( self->client->ps.stats[STAT_CLASS] );
	BG_BoundingBox( pClass, &playerMins, &playerMaxs, nullptr, nullptr, nullptr );

	//account for how large we can step
	playerMins[2] += STEPSIZE;
	playerMaxs[2] += STEPSIZE;

	glm::vec3 origin = VEC2GLM( self->s.origin );
	glm::vec3 end = origin + BOT_OBSTACLE_AVOID_RANGE * dir;

	trap_Trace( &trace, origin, playerMins, playerMaxs, end, self->s.number, MASK_PLAYERSOLID, 0 );
	if ( ( trace.fraction < 1.0f && trace.plane.normal[ 2 ] < MIN_WALK_NORMAL ) || g_entities[ trace.entityNum ].s.eType == entityType_t::ET_BUILDABLE )
	{
		return &g_entities[trace.entityNum];
	}
	return nullptr;
}

typedef enum
{
	OBSTACLE_NONE,
	OBSTACLE_JUMPABLE,
	OBSTACLE_LADDER
} obstacle_t;

// checks if jumping would get rid of blocker
// return true if yes
//
// This currently doesn't handle jumping over map geometry, but that could be
// nice some day.
static obstacle_t BotDetectObstacle( gentity_t *self, const gentity_t *blocker, const glm::vec3 &dir )
{
	glm::vec3 playerMins;
	glm::vec3 playerMaxs;
	float jumpMagnitude;
	trace_t tr1, tr2;

	//already normalized

	class_t pClass = static_cast<class_t>( self->client->ps.stats[STAT_CLASS] );
	BG_BoundingBox( pClass, &playerMins, &playerMaxs, nullptr, nullptr, nullptr );

	playerMins[2] += STEPSIZE;
	playerMaxs[2] += STEPSIZE;

	//Log::Debug(vtos(self->movedir));
	glm::vec3 origin = VEC2GLM( self->s.origin );
	glm::vec3 end = origin + BOT_OBSTACLE_AVOID_RANGE * dir;

	//make sure we are moving into a block
	trap_Trace( &tr1, origin, playerMins, playerMaxs, end, self->s.number, MASK_PLAYERSOLID, 0 );
	if ( tr1.fraction >= 1.0f || blocker != &g_entities[tr1.entityNum] )
	{
		return OBSTACLE_NONE;
	}

	jumpMagnitude = BG_Class( ( class_t )self->client->ps.stats[STAT_CLASS] )->jumpMagnitude;

	//find the actual height of our jump
	jumpMagnitude = Square( jumpMagnitude ) / ( self->client->ps.gravity * 2 );

	//prepare for trace
	playerMins[2] += jumpMagnitude;
	playerMaxs[2] += jumpMagnitude;

	//check if jumping will clear us of entity
	trap_Trace( &tr2, origin, playerMins, playerMaxs, end, self->s.number, MASK_PLAYERSOLID, 0 );

	classAttributes_t const* pcl = BG_Class( pClass );
	bool ladder = ( pcl->abilities & SCA_CANUSELADDERS )
		&& ( ( tr1.entityNum == ENTITYNUM_WORLD && tr1.surfaceFlags & SURF_LADDER )
		|| ( tr2.entityNum == ENTITYNUM_WORLD && tr2.surfaceFlags & SURF_LADDER ) )
		;

	if ( ladder )
	{
		return OBSTACLE_LADDER;
	}

	if ( blocker->s.eType == entityType_t::ET_BUILDABLE
			&& blocker->s.modelindex == BA_A_BARRICADE
			&& G_OnSameTeam( self, blocker ) )
	{
		glm::vec3 mins, maxs;
		BG_BoundingBox( static_cast<buildable_t>( blocker->s.modelindex ), &mins, &maxs );
		maxs.z *= BARRICADE_SHRINKPROP;
		glm::vec3 pos = VEC2GLM( blocker->s.origin );
		BG_BoundingBox( pClass, &playerMins, &playerMaxs, nullptr, nullptr, nullptr );
		if ( origin.z + playerMins.z >= pos.z + maxs.z )
		{
			return OBSTACLE_JUMPABLE;
		}
	}

	//if we can jump over it, then jump
	return tr2.fraction == 1.0f ? OBSTACLE_JUMPABLE : OBSTACLE_NONE;
}

// Determine a new direction, which may be unchanged (forward) or sideway. It
// does try the following every second: one time left, one time right and one
// time forward until they find their way.
static void BotFindDefaultSteerTarget( gentity_t *self, glm::vec3 &dir )
{
	// turn this into a 2D problem
	dir[2] = 0;
	// a vector 90° from dir, pointing right and of same magnitude
	glm::vec3 right = glm::vec3(dir[1], -dir[0], 0);

	// We add a delta determined from clientNum to avoid bots
	// dancing in sync
	int time = (110 * self->num() + self->client->time10000) % 1600;

	bool invert = time < 800;
	// part of the time we go straight instead
	// 600 instead of 800 because we want some left/right
	// asymetry in case it helps
	bool sideways = std::abs(time - 600) < 500;

	if ( sideways )
	{
		// 2*right + 1*dir means an angle of atan(2/1) = 63°. This
		// seems about right
		dir += right * (invert ? -1.0f : 1.0f)
			     * 2.0f;
	}
}

// Try to find a path around the obstacle by projecting 5
// traces in 15° steps in both directions.
// Return true if a path could be found.
// This function will always set "dir", even if it fails.
static bool BotFindSteerTarget( gentity_t *self, glm::vec3 &dir )
{
	glm::vec3 forward;
	glm::vec3 testPoint1, testPoint2;
	glm::vec3 playerMins, playerMaxs;
	float yaw1, yaw2;
	trace_t trace1, trace2;
	int i;
	glm::vec3 angles;

	if ( !( self && self->client ) )
	{
		return false;
	}

	//get bbox
	class_t pclass = static_cast<class_t>( self->client->ps.stats[STAT_CLASS] );
	BG_BoundingBox( pclass, &playerMins, &playerMaxs, nullptr, nullptr, nullptr );

	//account for stepsize
	playerMins[2] += STEPSIZE;
	playerMaxs[2] += STEPSIZE;

	//get the yaw (left/right) we dont care about up/down
	angles = vectoangles( dir );
	yaw1 = yaw2 = angles[ YAW ];

	//directly infront of us is blocked, so dont test it
	yaw1 -= 15;
	yaw2 += 15;

	//forward vector is 2D
	forward[ 2 ] = 0;

	//find an unobstructed position
	//we check the full 180 degrees in front of us
	glm::vec3 origin = VEC2GLM( self->s.origin );
	for ( i = 0; i < 5; i++, yaw1 -= 15 , yaw2 += 15 )
	{
		//compute forward for right
		forward[0] = cosf( DEG2RAD( yaw1 ) );
		forward[1] = sinf( DEG2RAD( yaw1 ) );
		//forward is already normalized
		//try the right
		testPoint1 = origin + BOT_OBSTACLE_AVOID_RANGE * forward;

		//test it
		trap_Trace( &trace1, origin, playerMins, playerMaxs, testPoint1, self->s.number,
		            MASK_PLAYERSOLID, 0 );

		//check if unobstructed
		if ( trace1.fraction >= 1.0f )
		{
			dir = forward;
			return true;
		}

		//compute forward for left
		forward[0] = cosf( DEG2RAD( yaw2 ) );
		forward[1] = sinf( DEG2RAD( yaw2 ) );
		//forward is already normalized
		//try the left
		testPoint2 = origin + BOT_OBSTACLE_AVOID_RANGE * forward;

		//test it
		trap_Trace( &trace2, origin, playerMins, playerMaxs, testPoint2, self->s.number,
		            MASK_PLAYERSOLID, 0 );

		//check if unobstructed
		if ( trace2.fraction >= 1.0f )
		{
			dir = forward;
			return true;
		}
	}

	// We couldnt find a "smart" new position, try going forward or sideways
	BotFindDefaultSteerTarget( self, dir );
	return false;
}

// This function tries to detect obstacles and to find a way
// around them. It always sets dir as the output value.
// Returns true on error
static bool BotAvoidObstacles( gentity_t *self, glm::vec3 &dir, bool ignoreGeometry, obstacle_t &obst )
{
	dir = self->botMind->nav().dir;
	gentity_t const *blocker = BotGetPathBlocker( self, dir );

	if ( !blocker )
	{
		return false;
	}

	// check for crouching
	if ( G_Team( self ) == TEAM_HUMANS && g_bot_autocrouch.Get() )
	{
		glm::vec3 playerMins;
		glm::vec3 playerCMaxs;
		trace_t trace;

		class_t pClass = static_cast<class_t>( self->client->ps.stats[STAT_CLASS] );
		BG_BoundingBox( pClass, &playerMins, nullptr, &playerCMaxs, nullptr, nullptr );

		glm::vec3 origin = VEC2GLM( self->s.origin );
		glm::vec3 end = origin + BOT_OBSTACLE_AVOID_RANGE * dir;

		//make sure we are moving into a block
		trap_Trace( &trace, origin, playerMins, playerCMaxs, end, self->s.number, MASK_PLAYERSOLID, 0 );
		if ( trace.fraction >= 1.0f )
		{
			self->botMind->cmdBuffer.upmove = -127;
			return false;
		}
	}

	obst = BotDetectObstacle( self, blocker, dir );
	if ( obst == OBSTACLE_JUMPABLE )
	{
		BotJump( self );
		return false;
	}
	else if ( obst == OBSTACLE_LADDER )
	{
		// look up to climb the ladder
		dir.z += 200.f;
		return false;
	}

	if ( BotFindSteerTarget( self, dir ) )
	{
		return false;
	}

	// ignore some stuff like geometry, movers...
	switch( blocker->s.eType )
	{
		case entityType_t::ET_GENERAL:
		case entityType_t::ET_MOVER:
			if ( ignoreGeometry )
			{
				return false;
			}
		default:
			break;
	}

	return true;
}

// `dir` does not need to be normalized
static void BotDirectionToUsercmd( gentity_t *self, const glm::vec3 &dir, usercmd_t *cmd )
{
	if ( glm::length2( glm::vec2(dir) ) < 1.0e-5f )
	{
		cmd->forwardmove = 0;
		cmd->rightmove = 0;
		return;
	}

	glm::vec3 forward;
	glm::vec3 right;

	float forwardmove;
	float rightmove;
	signed char speed = BotGetMaxMoveSpeed( self );

	AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, &right, nullptr );

	// get direction and non-optimal magnitude
	forwardmove = speed * Alignment2D( forward, dir ); // Alignment2D is cos() so we have forwardmove²+rightmove² = 1
	rightmove = speed * Alignment2D( right, dir );

	// find optimal magnitude to make speed as high as possible
	if ( fabsf( forwardmove ) > fabsf( rightmove ) )
	{
		float highestforward = forwardmove < 0 ? -speed : speed;

		float highestright = highestforward * rightmove / forwardmove;

		cmd->forwardmove = ClampChar( highestforward );
		cmd->rightmove = ClampChar( highestright );
	}
	else
	{
		float highestright = rightmove < 0 ? -speed : speed;

		float highestforward = highestright * forwardmove / rightmove;

		cmd->forwardmove = ClampChar( highestforward );
		cmd->rightmove = ClampChar( highestright );
	}
}

// Makes bot aim more or less slowly in a direction
static void BotSeek( gentity_t *self, const glm::vec3 &direction )
{
	glm::vec3 viewOrigin = BG_GetClientViewOrigin( &self->client->ps );

	// move directly toward the target
	BotDirectionToUsercmd( self, direction, &self->botMind->cmdBuffer );

	glm::vec3 seekPos = viewOrigin + 100.f * direction;

	// slowly change our aim to point to the target
	BotSlowAim( self, seekPos, 0.6 );
	BotAimAtLocation( self, seekPos );
}

/*
=========================
Global Bot Navigation
=========================
*/

static Cvar::Cvar<int> g_bot_upwardNavconMinHeight("g_bot_upwardNavconMinHeight", "minimal height difference for bots to use special upward movement.", Cvar::NONE, 100);
static Cvar::Cvar<int> g_bot_upwardLeapAngleCorr("g_bot_upwardLeapAngleCorr", "is added to the angle for mantis and dragoon when leaping upward (in degrees).", Cvar::NONE, 20);

// Wallwalk-capable classes (dretch, mantis, advanced granger) reach a target that
// is above them by climbing a wall/ceiling when they are at its base. These cvars
// gate and tune that (experimental) behaviour without a rebuild.
static Cvar::Cvar<bool> g_bot_wallwalkClimb(
	"g_bot_wallwalkClimb", "allow wallwalk-capable bots to climb walls/ceilings to reach a target above them", Cvar::NONE, true );
static Cvar::Cvar<float> g_bot_wallwalkClimbMinHeight(
	"g_bot_wallwalkClimbMinHeight", "minimum height difference (quake units) above the bot before it climbs to a target; 0 (default) uses the class's own jump apex", Cvar::NONE, 0.f );
static Cvar::Cvar<float> g_bot_wallwalkClimbRange(
	"g_bot_wallwalkClimbRange", "horizontal distance within which a bot's body counts as being against a climbable wall's base so it bites it (the engine's climb-attach trace reaches only a short distance into the move direction, so the body must be at the face); 0 (default) uses the bot's own body width so it adapts to each class -- a larger value lets it bite from farther but risks spinning in place", Cvar::NONE, 0.f );
static Cvar::Cvar<float> g_bot_wallwalkSeekRange(
	"g_bot_wallwalkSeekRange", "how far a wallwalk-capable bot will walk toward a climb point (nearest wall base, or the target's own XZ when that is the best route) to reach a target above it; 0 disables this and falls back to jumping", Cvar::NONE, 2048 );
static Cvar::Cvar<float> g_bot_wallwalkDetectRange(
	"g_bot_wallwalkDetectRange", "horizontal radius within which a wallwalk-capable bot detects a wall it is physically against and bites it directly, even when no generated marker is that close; 0 disables the direct detect", Cvar::NONE, 64.f );
static Cvar::Cvar<float> g_bot_wallwalkHeightReach(
	"g_bot_wallwalkHeightReach", "when reaching for a target above the bot, how strongly a wallwalk-capable bot prefers a wall whose top reaches the target over a nearer but shorter one (squared height-shortfall weight; 0 = plain nearest wall)", Cvar::NONE, 4.f );
// The shortfall penalty above only discourages a wall whose top falls short of the
// target; an overshoot (a wall whose top is above the target) is currently free, so the
// bot happily climbs a wall far taller than the target and ends up far above and far from
// its XZ -- the "climbs a 3x-tall wall, loops on the ceiling, never drops down on the
// target" symptom. This is the squared weight for that overshoot: it makes a wall whose top
// is near the target's height win over a much taller one, so the bot lands close to the
// target's level (and XZ) instead of overshooting. Over-weighted it would avoid every tall
// wall even when it is the only way up, so the default is lighter than the shortfall weight
// (overshoot is recoverable -- the bot can drop down -- whereas a shortfall cannot be).
static Cvar::Cvar<float> g_bot_wallwalkOvershootReach(
	"g_bot_wallwalkOvershootReach", "when reaching for a target above the bot, how strongly a wallwalk-capable bot dislikes a wall whose top is above the target (squared height-overshoot weight; 0 = no overshoot penalty, tall walls never dispreferred)", Cvar::NONE, 1.f );
static Cvar::Cvar<bool> g_bot_wallwalkDebug(
	"g_bot_wallwalkDebug", "log wallwalk climb-trigger diagnostics for wallwalk-capable bots (throttled)", Cvar::NONE, false );

// The minimum height difference above the bot that warrants a wall climb.
//
// By default this tracks the class's own maximum jump apex, computed the same
// way BotDetectObstacle does (Square(jumpMagnitude) / ( 2 * gravity )). A target
// within a single jump is cheaply reached by jumping, so the bot only bites a
// wall for a target above its jump reach -- which is exactly the case where the
// target sits on a raised platform the bot cannot simply jump onto. A positive
// g_bot_wallwalkClimbMinHeight pins an explicit threshold instead, so the
// (experimental) behaviour can be tuned or disabled; 0 selects the jump apex.
static float BotClimbMinHeight( gentity_t *self )
{
	const float pinned = g_bot_wallwalkClimbMinHeight.Get();
	if ( pinned > 0.f )
	{
		return pinned;
	}

	const class_t classNum = static_cast<class_t>( self->client->ps.stats[ STAT_CLASS ] );
	const float gravity = self->client->ps.gravity;
	// Match BotDetectObstacle's "actual height of our jump" so the threshold is
	// exactly what a full jump can reach for this class.
	return gravity > 0.f ? Square( BG_Class( classNum )->jumpMagnitude ) / ( gravity * 2.f ) : 0.f;
}

// How long (ms) a wallwalk-capable bot must be stuck (no progress toward its goal, per the
// same stuck accounting BotMoveToGoal uses) before it treats a target that is only slightly
// above it as still worth reaching by a wall detour, instead of assuming an ordinary
// jump/pounce gets there. A small vertical gap normally implies a jump reaches the target,
// but that is only true when the target's XZ is reachable on the ground; when the bot is
// stuck under a platform/ledge it cannot reach that XZ without climbing, so we relax the
// below-min-height gate and detour to a nearby wall. Shared by BotWallwalkClimbTowardGoal
// (the climb decision) and BotIsAttemptingWallBite (the jump/pounce suppression that keeps
// the bot on the wall while it does so).
constexpr int BOT_WALLWALK_STUCK_THRESHOLD = 2500;

// How long (ms) a wallwalk-capable bot stays committed to a wall detour after it first
// detects it is stuck with a nearby wall that reaches the target. Walking toward the wall
// resets the stuck timer (any movement beyond BOT_STUCK_RADIUS unsticks the bot), which
// would otherwise make the below-min-height gate re-evaluate and bail the bot back under
// the target before it reaches the wall -- the "climbs a bit then returns under me" symptom.
// The window is long enough to walk to the wall base and bite it; it is refreshed only while
// the bot is both stuck and a reaching wall is in range, so an ordinary same-level approach
// (never stuck) is left alone. Shared by BotWallwalkClimbTowardGoal and BotIsAttemptingWallBite.
constexpr int BOT_WALLWALK_DETOUR_COMMIT_WINDOW = 4000;

// How far (quake units, horizontal) the bot must get *closer* to the wall base for the
// walk-to-base phase to count as progress. Below this the bot is treated as making no
// progress -- the margin filters out the jitter of sliding along a surface so a bot that
// is genuinely closing in does not keep resetting its own timer.
constexpr float BOT_WALLWALK_PROGRESS_MARGIN = 8.f;

// How long (ms) the bot may press into a wall base that a platform/ledge blocks between it
// and the wall before the walk-to-base phase stops pressing straight in and starts arcing
// around the blocker -- a sideways component blended into the to-wall direction so the bot
// spirals off the obstacle toward the wall instead of circling in place under it. This is
// the "it can try a wall even if the detour first moves it away" behaviour: a straight line
// to the base is blocked by the platform the bot is under, so it has to go around it.
constexpr int BOT_WALLWALK_DEFLECT_DELAY = 800;

static int WallclimbStopTime( gentity_t *self )
{
	// experiments suggest an appropriate value of 4 milliseconds per distance
	// unit, add 500 milliseconds as safety margin
	// if the bot is moving downwards from the navcon start, lastNavconDistance
	// will be negative
	// TODO: take the different speeds of the classes into account
	return self->botMind->lastNavconTime + self->botMind->lastNavconDistance * 4 + 500;
}

// check if a wall climbing bot is running into an attackable human entity, if so: attack it
static void BotMaybeAttackWhileClimbing( gentity_t *self )
{
	int ownClass = self->client->ps.stats[ STAT_CLASS ];
	glm::vec3 playerMins, playerMaxs;
	BG_BoundingBox( static_cast<class_t>( ownClass ), &playerMins, &playerMaxs, nullptr, nullptr, nullptr );

	glm::vec3 origin = VEC2GLM( self->s.origin );

	// A bite is horizontal and must reach the enemy we are climbing toward, but the climb
	// aim points up-into-the-wall, so a trace along viewangles would whiff an enemy that
	// sits at our level beside or just above us on the wall/ceiling -- this is the
	// "bot climbs to the enemy's level but never bites, just cycles" symptom. Trace toward
	// the goal's XZ instead (a melee bite has no vertical aim), falling back to viewangles
	// when the enemy is directly overhead (no horizontal direction to use).
	glm::vec3 goalPos = self->botMind->goal.getPos();
	glm::vec3 toGoalXZ( goalPos.x - origin.x, goalPos.y - origin.y, 0.f );
	glm::vec3 dir;
	const float toGoalLen = glm::length( toGoalXZ );
	if ( toGoalLen > 1.f )
	{
		dir = toGoalXZ / toGoalLen;
	}
	else
	{
		glm::vec3 forward;
		AngleVectors( VEC2GLM( self->client->ps.viewangles ), &forward, nullptr, nullptr );
		dir = glm::vec3( forward.x, forward.y, 0.f );
	}

	// Each wallwalk-capable class has its own melee range for the trace; a bot
	// that isn't one of them can never reach this point, so defaulting to a
	// no-op keeps the trace out of scope for anything unexpected.
	float range = 0.f;
	switch ( ownClass )
	{
		case PCL_ALIEN_LEVEL0:
			range = LEVEL0_BITE_RANGE;
			break;
		case PCL_ALIEN_LEVEL1:
			range = LEVEL1_CLAW_RANGE;
			break;
		case PCL_ALIEN_BUILDER0_UPG:
			// The advanced granger's claw (WP_ABUILD2 secondary fire) reaches this far.
			range = ABUILDER_CLAW_RANGE;
			break;
		default:
			return;
	}

	trace_t trace;
	glm::vec3 end = origin + range * dir;
	trap_Trace( &trace, origin, playerMins, playerMaxs, end, self->s.number, MASK_PLAYERSOLID, 0 );

	if ( trace.fraction < 1.0f )
	{
		gentity_t const* hit = &g_entities[ trace.entityNum ];
		bool hitEnemy = not ( G_OnSameTeam( self, hit ) || G_Team( hit ) == TEAM_NONE );

		if ( hitEnemy )
		{
			int distanceExtension = 30;  // roughly corresponds to 100 milliseconds
			// upper bound for extensions, in case a bot loops on a wall:
			int extendedDistance = std::max( self->botMind->lastNavconDistance + distanceExtension, 4000 );
			usercmd_t *botCmdBuffer = &self->botMind->cmdBuffer;

			switch ( ownClass )
			{
				case PCL_ALIEN_LEVEL0:
					// The dretch bite is an auto-hit, so just keep the climb alive.
					if ( G_DretchCanDamageEntity( hit ) )
					{
						self->botMind->lastNavconDistance = extendedDistance;
					}
					break;
				case PCL_ALIEN_LEVEL1:
					BotFireWeapon( WPM_PRIMARY, botCmdBuffer );
					self->botMind->lastNavconDistance = extendedDistance;
					break;
				case PCL_ALIEN_BUILDER0_UPG:
					// The advanced granger's claw is its secondary fire (swipe).
					BotFireWeapon( WPM_SECONDARY, botCmdBuffer );
					self->botMind->lastNavconDistance = extendedDistance;
					break;
			}
		}
	}
}

// Aim at `targetPos` and issue the class-specific climb command (the dretch /
// mantis / advanced granger wall-walk), then attack anything in range while
// climbing. Shared by the corner-driven climb (below) and the goal-driven climb
// for reaching a target that is above the bot on a wall or ceiling.
static void BotClimbToward( gentity_t *self, const glm::vec3 &targetPos )
{
	BotAimAtLocation( self, targetPos );
	self->botMind->cmdBuffer.upmove = -127;
	self->botMind->cmdBuffer.forwardmove = 127;
	self->botMind->cmdBuffer.rightmove = 0;
	// Stamp the climb so the hold window keeps the bot biting and suppresses jumps
	// until the target is reached (see BotIsWallClimbing).
	self->botMind->lastWallClimbTime = level.time;
	BotMaybeAttackWhileClimbing( self );
}

// could use BG_ClassHasAbility( pcl, SCA_WALLCLIMBER ) to check if calling
// this is a good idea
static void BotClimbToGoal( gentity_t *self )
{
	glm::vec3 ownPos = VEC2GLM( self->s.origin );
	glm::vec3 dir = self->botMind->nav().dir;
	BotClimbToward( self, ownPos + 100.f * dir );
}

// Point the bot up-and-into-the-wall it is about to climb, so the engine's short
// attach trace (PM_GroundClimbTrace) grabs the face and the wallwalk starts. The
// horizontal component points into the wall -- from the bot toward the marker base,
// which sits on the wall face -- and a fixed up-tilt carries it up the face. A
// near-vertical look would make the attach trace miss, and a pure horizontal look
// would never progress, so the two are combined. This only *starts* a climb; once the
// bot is on the wall, the caller keeps it biting toward the actual target (see
// BotWallwalkClimbTowardGoal's in-progress branch).
//
// `markerPos` is the base of the nearest wallwalk connection (quake space). When the
// bot is pressed right against it the horizontal offset is tiny, so fall back to
// aiming straight up -- the engine's fallback attach points still grab an adjacent
// face.
static glm::vec3 BotWallwalkClimbAim( const glm::vec3 &ownPos, const glm::vec3 &markerPos )
{
	const glm::vec2 toWallXZ( markerPos.x - ownPos.x, markerPos.y - ownPos.y );
	if ( glm::length( toWallXZ ) < 1.f )
	{
		return ownPos + glm::vec3( 0.f, 0.f, 128.f );
	}

	const glm::vec2 intoWall = glm::normalize( toWallXZ );
	return glm::vec3( ownPos.x + intoWall.x * 128.f, ownPos.y + intoWall.y * 128.f, ownPos.z + 128.f );
}

// Detects a climbable wall the bot is physically against -- independent of any
// generated marker -- by tracing a short horizontal distance out of the bot in a
// few candidate directions and accepting the first steep, world surface it hits.
// This is what lets a wallwalk-capable class bite the wall it is already standing
// next to (the case where the floor navmesh is right below and the nearest marker
// sits far away, which previously made the bot gather under the target). It
// returns true and sets `outDir` to the horizontal direction to aim into the wall
// when such a surface is within `dist`; otherwise false.
//
// The trace uses the class bounding box and skips the bot itself, and only
// accepts steep surfaces (|normal.z| < MIN_WALK_NORMAL) on the world, so a floor,
// a low ledge or a ceiling directly above does not register as a climbable wall.
static bool BotAdjacentClimbWall( gentity_t *self, const glm::vec3 &ownPos, const glm::vec3 &targetPos, float dist, glm::vec3 &outDir )
{
	if ( dist <= 0.f )
	{
		return false;
	}

	// Candidate horizontal directions: toward the target first (the wall we would climb
	// to reach something above us), then eight evenly-spaced directions, so a wall the
	// bot is against -- even when the target is not directly beyond it and even for a
	// diagonal face -- is found. When the target has no horizontal offset (directly
	// overhead) the primary falls back to +X.
	const glm::vec2 toTarget( targetPos.x - ownPos.x, targetPos.y - ownPos.y );
	const glm::vec2 primary = glm::length( toTarget ) < 1.f ? glm::vec2( 1.f, 0.f ) : glm::normalize( toTarget );
	constexpr int NUM_DIRS = 8;
	glm::vec2 dirs[ NUM_DIRS + 1 ];
	dirs[ 0 ] = primary;
	for ( int i = 0; i < NUM_DIRS; ++i )
	{
		const float ang = 360.f * i / NUM_DIRS;
		dirs[ 1 + i ] = glm::vec2( cosf( DEG2RAD( ang ) ), sinf( DEG2RAD( ang ) ) );
	}

	// Trace as a *point* ray (no bounding box). When the bot is pressed against a wall
	// its bbox overlaps the solid, which makes a bbox-expanded trace start embedded in
	// the surface and degenerate (fraction 1, a bogus normal) -- the reason the old
	// five-way pre-check missed a wall the bot was literally touching. A point ray from
	// a couple of heights above the feet catches the face the bot is against instead.
	// The |normal.z| test rejects floors, low ledges and the ceiling directly overhead.
	const float heights[ 2 ] = { 8.f, 40.f };
	for ( const float h : heights )
	{
		const glm::vec3 origin = ownPos + glm::vec3( 0.f, 0.f, h );
		for ( const glm::vec2 &c : dirs )
		{
			const glm::vec3 dir( c.x, 0.f, c.y );
			const glm::vec3 end = origin + dir * dist;

			trace_t trace;
			// A zero bbox is a point trace, which (unlike a class-bbox-expanded trace)
			// does not degenerate when the bot is pressed against the face.
			const glm::vec3 zero( 0.f, 0.f, 0.f );
			trap_Trace( &trace, origin, zero, zero, end, self->s.number, MASK_SOLID, 0 );

			if ( trace.fraction < 1.f
					&& trace.entityNum == ENTITYNUM_WORLD
					&& fabsf( trace.plane.normal[ 2 ] ) < MIN_WALK_NORMAL )
			{
				outDir = dir;
				return true;
			}
		}
	}

	return false;
}

// True when a solid surface (a platform, ceiling or bridge) sits directly above the bot, so
// it cannot reach a target that is above it by jumping or pouncing -- it must detour to a
// nearby wall and climb up to it instead. This is the *positional* replacement for the old
// time-based "stuck under a platform" trigger: it fires the instant the bot is boxed in under
// the target, so the bot steers to a wall immediately instead of circling under the target for
// 2.5 s (a circling bot keeps moving, which never let the time-based stuck flag fire, so the
// bot only detoured after a long, erratic delay -- or not at all).
//
// A point trace (zero bounding box) straight up from just above the bot's *feet*, to a point
// just above the target's feet, is used: a bounding-box trace degenerates when the bot is
// pressed against the blocking surface. Starting near the feet (not the head) is what lets a
// low platform/bridge register -- for a target only a few units up, the bot's head already
// clears the surface, so a trace from the head would start above it and miss. trap_Trace skips
// the bot itself (via self->s.number), so only a real world/buildable surface (or the target it
// stands on) above the bot stops the trace. A target at or below the bot is an ordinary
// same-level approach (the bot reaches it by walking or a plain jump), so the trace is gated on
// targetPos.z > ownPos.z and only the vertical gap is probed -- the target being off to the side
// does not by itself mean the bot is boxed in.
static bool BotBlockedFromTarget( gentity_t *self, const glm::vec3 &ownPos, const glm::vec3 &targetPos )
{
	if ( targetPos.z <= ownPos.z )
	{
		return false;
	}

	// Probe the whole gap: start just above the bot's feet and end just above the target's
	// feet. A small offset keeps the trace just off the floor the bot stands on, while still
	// spanning a low platform a few units up (starting at the head would clear it and miss).
	const float gapStart = ownPos.z + 2.f;
	const float gapEnd = targetPos.z + 2.f;
	trace_t trace;
	const glm::vec3 start( ownPos.x, ownPos.y, gapStart );
	const glm::vec3 end( ownPos.x, ownPos.y, gapEnd );
	const glm::vec3 zero( 0.f, 0.f, 0.f );
	trap_Trace( &trace, start, zero, zero, end, self->s.number, MASK_SOLID, 0 );

	return trace.fraction < 1.0f;
}

// True when a wallwalk-capable bot is trying to bite an enemy that sits above it: its goal
// is a valid enemy, that enemy is above the bot by at least the climb min height, and the bot
// is close to it horizontally (at a wall next to the enemy, within the seek range). In that
// window the bot should keep climbing/biting rather than jump or pounce -- a jump/pounce would
// pop it off the wall and whiff the enemy above, whereas a human wallwalker stays on the wall
// until it reaches the target. This extends the jump/pounce suppression (see BotIsWallClimbing)
// beyond the climb hold window, so the bot does not launch off the wall the instant it reaches
// the enemy near an edge. The height test is in the quake frame (Z up), where both positions
// arrive, so a target that is at or below the bot (an ordinary pounce) never matches.
static bool BotIsAttemptingWallBite( gentity_t *self )
{
	const int classNum = self->client->ps.stats[ STAT_CLASS ];
	if ( !BG_ClassHasAbility( classNum, SCA_WALLCLIMBER ) )
	{
		return false;
	}

	const botTarget_t &goal = self->botMind->goal;
	if ( !goal.targetsValidEntity() )
	{
		return false;
	}

	glm::vec3 ownPos = VEC2GLM( self->s.origin );
	glm::vec3 targetPos = goal.getPos();

	// The enemy must be above the bot by at least the climb min height; a target at or below
	// us is an ordinary jump/pounce situation, not a wall bite. When the bot is boxed in under
	// the target (a surface sits above it, so a jump cannot reach the target -- its XZ may be on
	// a platform/bridge only a wall climb can reach), however, a small gap does NOT mean a jump
	// reaches the target, so we relax the height test and keep the bot biting instead of letting
	// it launch off and whiff the enemy above. A committed detour (see
	// BotWallwalkClimbTowardGoal) keeps the bot on the wall for the same window, so it does
	// not pounce off a wall it is mid-way up toward the target. The time-based `stuck` flag is
	// kept as a fallback for the rare case the trace misses, but the positional boxedIn test is
	// the primary signal so the bot never waits out the stuck timer first.
	const bool boxedIn = BotBlockedFromTarget( self, ownPos, targetPos );
	const bool stuck = level.time - self->botMind->stuckTime > BOT_WALLWALK_STUCK_THRESHOLD;
	const bool detourCommit = self->botMind->wallwalkDetourTime > 0
		&& level.time - self->botMind->wallwalkDetourTime < BOT_WALLWALK_DETOUR_COMMIT_WINDOW;
	if ( targetPos.z - ownPos.z < BotClimbMinHeight( self ) && !boxedIn && !stuck && !detourCommit )
	{
		return false;
	}

	// And close enough horizontally that the bot is at a wall next to the enemy, so it is in
	// the middle of climbing up to it. The seek range is a generous horizontal bound: a bot
	// climbing the last of the way toward an enemy above it is within it.
	const float seekRange = g_bot_wallwalkSeekRange.Get();
	const float dx = targetPos.x - ownPos.x;
	const float dy = targetPos.y - ownPos.y;
	return dx * dx + dy * dy <= seekRange * seekRange;
}

// Wallwalk-capable classes (dretch, mantis, advanced granger) reach a target that
// is above them by climbing a wall/ceiling. The decision (gated behind
// g_bot_wallwalkClimb, tuned by g_bot_wallwalkClimbMinHeight / g_bot_wallwalkSeekRange /
// g_bot_wallwalkDetectRange so the experimental behaviour can be tuned or disabled
// without a rebuild) has three phases, all of which return true so the caller skips
// ordinary movement:
//
//   1. At the base of a generated wall (within g_bot_wallwalkDetectRange of its base
//      marker, or physically against a face by a point trace): bite it and let the
//      engine's attach trace climb the face.
//   2. Target above the bot's jump apex but not yet at a wall: walk horizontally toward
//      a climb point -- the nearest wall base when it is at least as close as the target,
//      otherwise the target's own XZ (so a target on a platform is reached by getting
//      under it, next to the platform's own walls) -- bounded by g_bot_wallwalkSeekRange.
//   3. Target at or below the bot (an overshoot, or the bot stuck on a wall/ceiling
//      above its target): the bot is higher than its target, so instead of biting (the
//      horizontal reach cannot hit a target below it) it walks over the target's XZ and
//      then jumps off the surface -- upmove 127 releases the grip -- letting gravity drop
//      it onto the target, the move a human wallwalker makes. Once over the target within
//      its own melee reach it leaps down; until then it walks toward the target's XZ.
//
// g_bot_wallwalkClimbRange is retained for tuning but is not wired into the combat path
// any longer (the walk-to-climb-point phase above uses g_bot_wallwalkSeekRange instead).
bool BotWallwalkClimbTowardGoal( gentity_t *self )
{
	if ( !g_bot_wallwalkClimb.Get() )
	{
		return false;
	}

	const int classNum = self->client->ps.stats[ STAT_CLASS ];
	if ( !BG_ClassHasAbility( classNum, SCA_WALLCLIMBER ) )
	{
		return false;
	}

	const botTarget_t &goal = self->botMind->goal;
	if ( !goal.targetsValidEntity() )
	{
		return false;
	}

	glm::vec3 ownPos = VEC2GLM( self->s.origin );
	glm::vec3 targetPos = goal.getPos();

	// Throttled diagnostics so the climb path can be observed without a rebuild.
	static int s_nextLog = 0;
	const bool wantDebug = g_bot_wallwalkDebug.Get();
	const float seekRange = g_bot_wallwalkSeekRange.Get();
	const float detectRange = g_bot_wallwalkDetectRange.Get();

	// The vertical gap to the target (quake frame, Z up): positive when the target is
	// above the bot. This is what gates the whole decision, and it is a *gap* (not an
	// absolute height) so it is stable regardless of the map's floor level.
	const float heightDiff = targetPos.z - ownPos.z;
	const float climbMin = BotClimbMinHeight( self );

	// When the bot is boxed in under the target -- a surface sits directly above it, so an
	// ordinary jump/pounce cannot reach the target's XZ (which is on a platform/bridge only a
	// wall climb can reach) -- a small vertical gap does NOT imply a jump reaches it. So we
	// relax the below-min-height gate below and fall through to the wall search, so the bot
	// detours to a nearby wall and climbs even for a gap smaller than its jump apex. This is the
	// "bot gathers under a platform with walls at its ends and just circles" fix, now positional:
	// it fires the instant the bot is boxed in, so the bot steers to a wall immediately instead
	// of circling under the target for the 2.5 s stuck timer. The time-based `stuck` flag is
	// kept only as a fallback for the rare case the trace misses.
	const bool boxedIn = BotBlockedFromTarget( self, ownPos, targetPos );
	const bool stuck = level.time - self->botMind->stuckTime > BOT_WALLWALK_STUCK_THRESHOLD;
	const bool needDetour = boxedIn || stuck;

	// A climb is in progress (within the hold window or physically attached) and the
	// target is still above us: keep biting on the wall so we finish reaching it instead
	// of being popped off by a jump/pounce. When the target drops to or below our height
	// the climb is done, so stop and let ordinary combat take over.
	if ( BotIsWallClimbing( self ) && targetPos.z > ownPos.z )
	{
		// Press up along the wall the bot is on: aim up-and-into its current facing (the
		// wall it bit), NOT straight at the target. A target on the far side of the wall
		// would make forwardmove push the bot off the face and it would hang at the base
		// instead of climbing -- the "stuck at the bottom of the wall, not moving" symptom.
		// The nav direction points into the wall the bot is moving along, so a small
		// up-into-it aim (as in BotWallwalkClimbToGoal / BotClimbToGoal) keeps the climb
		// progressing; a degenerate direction falls back to straight up. The up-tilt gives
		// the engine's short attach trace a face to grab; BotClimbToward stamps the hold
		// window and attacks anything in range on the way up. Once the bot is level with
		// the target the height gate below drops it down onto it.
		const glm::vec3 faceDir( self->botMind->nav().dir.x, self->botMind->nav().dir.y, 0.f );
		glm::vec3 aim = ownPos + glm::vec3( 0.f, 0.f, 128.f );
		if ( glm::length( faceDir ) > 1.f )
		{
			aim += faceDir * 64.f;
		}
		BotClimbToward( self, aim );
		return true;
	}

	// Height gates first, before any wall search or bite, so a bot that has climbed above
	// its target (an overshoot, or one stuck on a wall/ceiling above it) stops re-biting
	// and instead gets down. A target that is *below* us is not a climb: a wallwalker that
	// has overshot must jump off the surface it is on -- the horizontal bite cannot reach a
	// target below it -- so gravity drops it onto the target. We only do this when the bot
	// is actually attached to a surface (EF_WALLCLIMB/EF_WALLCLIMBCEILING) so ordinary
	// ground combat -- a target a few units below on the same floor -- is left to the normal
	// movement path; when the bot is not attached we fall through to ordinary combat rather
	// than climbing. This is the fix for a bot that climbs a wall or ceiling and then sits
	// above its target doing nothing. With the target above us (heightDiff >= 0) we instead
	// fall through to the wall search below and detour to a nearby wall.
	if ( heightDiff < 0.f )
	{
		// A target that sits *below* the bot is not a climb: the wall/ceiling the bot is on
		// is taller than the target, so the bot has overshot and ended up above it. The bite
		// (BotMaybeAttackWhileClimbing) is horizontal -- it traces toward the target's XZ -- so
		// it cannot reach a target that is below the bot. To get down onto it the bot must
		// release the grip and fall, which is what a human wallwalker does: jump off the wall/
		// ceiling and drop onto the target. This is why a bot that climbs a tall wall or a
		// ceiling over a platform otherwise sits above its target and can never hit it.
		if ( self->client->ps.eFlags & ( EF_WALLCLIMB | EF_WALLCLIMBCEILING ) )
		{
			glm::vec3 toTarget = targetPos - ownPos;
			toTarget.z = 0.f;
			const float targetDist = glm::length( toTarget );

			// The horizontal drop threshold is the class's own melee reach: once the bot is
			// over the target's XZ within that distance, the fall from a jump lands inside bite
			// range, so the bot gets the hit instead of undershooting and re-climbing. Each
			// wallwalk class has its own range, matching BotMaybeAttackWhileClimbing.
			float dropRange = 0.f;
			switch ( classNum )
			{
				case PCL_ALIEN_LEVEL0:        dropRange = LEVEL0_BITE_RANGE; break;
				case PCL_ALIEN_LEVEL1:        dropRange = LEVEL1_CLAW_RANGE; break;
				case PCL_ALIEN_BUILDER0_UPG:  dropRange = ABUILDER_CLAW_RANGE; break;
			}

			// Over the target's XZ (within melee reach): jump off the surface. upmove 127 is
			// the jump that pops the bot off a wall/ceiling -- it releases the grip so gravity
			// drops the bot down onto the target, which a horizontal walk (upmove 0) can never
			// do because it only slides along the surface. We leap toward the target's XZ so the
			// fall lands near it. Clearing lastWallClimbTime keeps the climb hold window inactive,
			// so while airborne the in-progress climb branch (BotIsWallClimbing) does not re-bite
			// and pull the bot back onto the wall mid-fall; it lands and re-evaluates from the
			// ground instead.
			if ( targetDist <= dropRange )
			{
				if ( wantDebug && level.time >= s_nextLog )
				{
					s_nextLog = level.time + 1000;
					Log::Notice( "Wallwalk: client %d (%s) target %.0f below self on wall/ceiling -> jumping down to drop onto it",
						self->s.number, BG_Class( classNum )->name, heightDiff );
				}
				BotSeek( self, toTarget );
				self->botMind->cmdBuffer.upmove = 127;
				self->botMind->lastWallClimbTime = 0;
				return true;
			}

			// Not yet over the target's XZ: walk horizontally toward it so the next tick drops
			// the bot from there, rather than sliding aimlessly along the surface. While doing so
			// still bite if the target comes within melee range -- a bot dropping onto an adjacent
			// ledge/overhang would otherwise just cycle under a target it could already reach.
			if ( targetDist > 1.f && ( seekRange <= 0.f || targetDist <= seekRange ) )
			{
				if ( wantDebug && level.time >= s_nextLog )
				{
					s_nextLog = level.time + 1000;
					Log::Notice( "Wallwalk: client %d (%s) target %.0f below self, walking to its XZ then dropping (%.0f away)",
						self->s.number, BG_Class( classNum )->name, heightDiff, targetDist );
				}
				BotSeek( self, toTarget );
				self->botMind->cmdBuffer.upmove = 0;
				BotMaybeAttackWhileClimbing( self );
				return true;
			}
		}
		return false;
	}

	// Locate the wall whose base best reaches the target: when the target is above the bot
	// the genuinely nearest wall (by horizontal distance) is preferred over one merely on
	// the way to it, with a squared penalty for how far that wall's top falls short of the
	// target (g_bot_wallwalkHeightReach) so a nearer wall that actually reaches the target
	// wins over a far tall one, and a short wall is only chosen when it is close enough
	// (see G_BotNearestWallConnection). There may be none in this mesh, in which case
	// climbing is impossible and we fall back to the ordinary upward movement below.
	// Done *before* the min-height gate so the gate can use it: a wall whose top reaches the
	// target is a stable reason to detour and climb, independent of the momentary stuck flag.
	glm::vec3 markerPos;
	float markerDist = 0.f;
	int wallCount = 0;
	float markerTopZ = 0.f;
	// Fresh search for the nearest reachable wall this tick. The mesh is static, so while a
	// detour is committed (below) the walk-to-base and bite phases instead drive to the wall
	// the bot locked on, rather than re-picking the nearest wall every tick as the target moves.
	bool hasWall = G_BotNearestWallConnection( self->s.number, ownPos, targetPos, markerPos, markerDist, wallCount, markerTopZ, g_bot_wallwalkHeightReach.Get(), g_bot_wallwalkOvershootReach.Get() );

	// A wall whose top reaches (or passes) the target's height is a signal that the target is
	// on a surface the bot can only get to by climbing -- e.g. a bridge or platform the bot is
	// standing under, whose XZ is right on top of the bot so an ordinary walk to it moves
	// nowhere, but whose side walls reach the top. It is reported in the diagnostics below so the
	// climb path can be observed without a rebuild.
	const bool wallReachesTarget = hasWall && markerTopZ >= targetPos.z;

	// A climbable wall within the seek range is a way up to an elevated target even for a gap
	// smaller than the jump apex: a wallwalker reaches it by climbing, not by a ground jump
	// that would miss the surface. heightDiff > 0 keeps an exactly same-level enemy (reached
	// by walking) from making the bot detour to a far wall. Shared with the min-height gate
	// below (see preferClimb).
	const bool wallInSeek = hasWall && ( seekRange <= 0.f || markerDist <= seekRange );

	// Commit to the detour while the bot is pursuing a climb -- boxed in under the target, or
	// steering to a climbable wall in range -- and (as a fallback) still within the commit
	// window from a previous tick. Because the trigger is positional (boxedIn / wallInSeek) it
	// holds for the whole approach, so the bot walks to the wall and bites it instead of bailing
	// back under the target before it reaches the wall -- the "climbs a bit then returns under
	// me" symptom. A value of 0 means "never committed" (the initial state), so a freshly spawned
	// bot is not treated as mid-detour.
	const bool pursuingClimb = needDetour || ( wallInSeek && heightDiff > 0.f );
	const bool withinCommitWindow = self->botMind->wallwalkDetourTime > 0
		&& level.time - self->botMind->wallwalkDetourTime < BOT_WALLWALK_DETOUR_COMMIT_WINDOW;

	// On the tick a detour begins, lock the wall we are heading to so the bot commits to *that*
	// wall for the window instead of re-picking the nearest wall every tick as the target moves
	// -- the erratic "walk to one wall, bail, turn to the other" behaviour. The window is
	// refreshed every tick while pursuing, so the bot stays committed to the same wall (and
	// finishes the climb) for the whole approach rather than flipping walls.
	if ( pursuingClimb && hasWall )
	{
		if ( !withinCommitWindow )
		{
			self->botMind->wallwalkDetourPos = markerPos;
			// Start progress tracking fresh for this detour: the base we just locked on is the
			// reference the walk-to-base phase measures progress against.
			self->botMind->wallwalkWalkBestDist = markerDist;
			self->botMind->wallwalkWalkNoProgressTime = level.time;
		}
		self->botMind->wallwalkDetourTime = level.time;
	}

	// Within the commit window the walk-to-base and bite phases drive to the *locked* wall (a
	// stable point) instead of the freshly searched marker, so the bot finishes the climb it
	// started rather than oscillating between two near walls as the target moves. The locked
	// marker is a real mesh wall, so force hasWall true even if this tick's fresh search found
	// nothing, keeping the walk/bite phases active.
	const bool detourCommit = withinCommitWindow;
	if ( detourCommit )
	{
		const glm::vec3 locked = self->botMind->wallwalkDetourPos;
		markerPos = locked;
		markerDist = glm::length( glm::vec3( locked.x - ownPos.x, locked.y - ownPos.y, 0.f ) );
		hasWall = true;
	}

	// Throttled diagnostic of the positional boxed-in trigger, so the user can see the bot
	// commit to a wall detour immediately (boxedIn) rather than after the 2.5 s stuck timer.
	if ( wantDebug && boxedIn && level.time >= s_nextLog )
	{
		s_nextLog = level.time + 1000;
		Log::Notice( "Wallwalk: client %d (%s) boxed in under target %.0f above%s%s -> nearest wall top %.0f (%.0f away), detouring",
			self->s.number, BG_Class( classNum )->name, heightDiff,
			hasWall ? "" : " (no wall marker in mesh)",
			wallReachesTarget ? " reaches target" : "", markerTopZ, markerDist );
	}

	// The target is above us but within the jump apex (a positive gap smaller than
	// climbMin): normally a jump/pounce reaches it, so leave it to the ordinary upward
	// movement below and don't climb. We bypass this -- and fall through to the wall search
	// so the bot steers to a wall and climbs -- whenever climbing is the right way up, which
	// for a wallwalker means an elevated target it cannot get onto by a ground jump: a low
	// platform/bridge it is boxed in under (the "bot circles directly under me" case), a
	// climbable wall within the seek range, or a detour it is already committed to. This is
	// the "prefer wallwalking as the primary approach" behaviour; the heightDiff > 0 guard on
	// the wall branch keeps an exactly same-level enemy (reached by walking) from making the
	// bot detour to a far wall. The time-based stuck flag is kept only as a fallback.
	const bool preferClimb = needDetour || detourCommit || ( wallInSeek && heightDiff > 0.f );
	if ( heightDiff < climbMin && !preferClimb )
	{
		if ( wantDebug && level.time >= s_nextLog )
		{
			s_nextLog = level.time + 1000;
			Log::Notice( "Wallwalk: client %d (%s) target only %.0f above (self z %.0f, target z %.0f), below min height %.0f, no wall in range -> not climbing",
				self->s.number, BG_Class( classNum )->name, heightDiff, ownPos.z, targetPos.z, climbMin );
		}
		return false;
	}
	else if ( heightDiff < climbMin && preferClimb && wantDebug && level.time >= s_nextLog )
	{
		s_nextLog = level.time + 1000;
		Log::Notice( "Wallwalk: client %d (%s) target only %.0f above%s%s%s -> detouring to a wall (top %.0f, target %.0f)",
			self->s.number, BG_Class( classNum )->name, heightDiff,
			boxedIn ? " boxedIn" : ( wallInSeek ? " wallInSeek" : "" ),
			stuck ? " stuck" : "",
			detourCommit ? " committed" : "",
			markerTopZ, targetPos.z );
	}

	// The engine's climb-attach trace (PM_GroundClimbTrace) reaches only a couple of units
	// into the move direction, so the bot's body must actually be *against* the wall face to
	// attach -- it can never get closer to a face than its own body width. So the bite only
	// fires when the base marker is within the bot's reach; farther than that the bot *walks*
	// to the base on the ground (upmove cleared, so it doesn't try to climb from a distance
	// and spin in place -- the "gathers under me and rotates" symptom) and bites once its body
	// reaches the face. The reach is the bot's own XZ half-width plus a small margin, so it
	// adapts to each class's size; g_bot_wallwalkClimbRange > 0 pins a fixed value for tuning.
	glm::vec3 cmins, cmaxs;
	BG_BoundingBox( static_cast<class_t>( classNum ), &cmins, &cmaxs, nullptr, nullptr, nullptr );
	const float botReach = std::max(
		std::max( cmins.x, cmins.y ),
		std::max( cmaxs.x, cmaxs.y ) );
	const float cvarBite = g_bot_wallwalkClimbRange.Get();
	const float biteDistance = cvarBite > 0.f ? cvarBite : botReach + 8.f;

	// Phase 1 -- bite: when the bot's body is within reach of a generated wall's base marker,
	// bite it and let the engine's attach trace grab the face and the climb take hold. This is
	// the fix for the "gathers under the platform and spins in place" symptom: the bot now gets
	// its body to the face first (the walk phase below) instead of biting from 20-40 units off,
	// where the attach trace cannot reach and the up-into-wall aim produced no horizontal
	// progress. Aiming up-and-into-the-wall (BotWallwalkClimbAim) gives that attach trace a
	// face to grab; a near-vertical or purely horizontal look would miss.
	if ( hasWall && markerDist <= biteDistance )
	{
		if ( wantDebug && level.time >= s_nextLog )
		{
			s_nextLog = level.time + 1000;
			Log::Notice( "Wallwalk: client %d (%s) target %.0f above, body at wall base (%.0f away, reach %.0f) -> climbing into it",
				self->s.number, BG_Class( classNum )->name, heightDiff, markerDist, biteDistance );
		}
		BotClimbToward( self, BotWallwalkClimbAim( ownPos, markerPos ) );
		return true;
	}

	// Phase 1 fallback -- a wall with no generated marker this close (rare: the mesh now emits
	// one per wall): if the bot's body is against a climbable face -- detected by a point trace
	// out to g_bot_wallwalkDetectRange, independent of any marker -- bite it the same way, aiming
	// into it.
	{
		glm::vec3 wallDir;
		if ( BotAdjacentClimbWall( self, ownPos, targetPos, detectRange, wallDir ) )
		{
			if ( wantDebug && level.time >= s_nextLog )
			{
				s_nextLog = level.time + 1000;
				Log::Notice( "Wallwalk: client %d (%s) target %.0f above, body against a wall (no near marker) -> climbing into it",
					self->s.number, BG_Class( classNum )->name, heightDiff );
			}
			// Aim horizontally into the wall the trace hit, with a fixed up-tilt so the
			// climb-attach trace (which reaches only a short distance into the move
			// direction) has a face to grab; wallDir points into that face.
			BotClimbToward( self, glm::vec3( ownPos.x + wallDir.x * 128.f, ownPos.y + wallDir.y * 128.f, ownPos.z + 128.f ) );
			return true;
		}
	}

	// Phase 2 -- walk to the base: the target is above our jump apex and the nearest wall's base
	// is within the seek range but not yet within the bot's reach, so walk horizontally toward
	// the base (upmove cleared) and bite it on a later tick once the body reaches the face. We
	// walk to the *nearest wall base* within the seek range -- that is the surface the bot can
	// actually climb, and for a target on a platform it is the platform's own edge wall, not
	// some far-off wall. This is what makes a bot that has gathered *directly under* a platform
	// move off to its edge and climb up, rather than standing still under the target (whose own
	// XZ is right on top of the bot, so heading there moves nothing). Only when no wall is within
	// the seek range do we fall back to heading for the target's own XZ (to get under a larger
	// structure), still bounded by g_bot_wallwalkSeekRange so we don't chase across the whole map;
	// 0 disables both and falls through to the ordinary upward movement (jump).
	glm::vec3 toTarget = targetPos - ownPos;
	toTarget.z = 0.f;
	const float targetDist = glm::length( toTarget );

	if ( hasWall && ( seekRange <= 0.f || markerDist <= seekRange ) )
	{
		// Walk horizontally to the wall base; the z is zeroed so we reach the base and the
		// bite (phase 1) takes us up on the next tick. upmove is cleared so the bot walks on
		// the ground to the face instead of trying to climb from a distance -- the up-into-wall
		// aim produces no horizontal progress and the attach trace cannot reach from 20-40 units
		// off, which is what spun the bot in place under the target.
		glm::vec3 toMarker = markerPos - ownPos;
		toMarker.z = 0.f;
		const float distToBase = glm::length( toMarker );

		// Progress toward the wall base, measured against the closest we have been this detour.
		// The generic stuck timer (any movement) is useless here: a bot sliding in a circle under
		// a platform keeps resetting it, so it never aborts. We instead track horizontal progress
		// toward *this* wall base specifically.
		if ( distToBase < self->botMind->wallwalkWalkBestDist - BOT_WALLWALK_PROGRESS_MARGIN )
		{
			self->botMind->wallwalkWalkBestDist = distToBase;
			self->botMind->wallwalkWalkNoProgressTime = level.time;
		}
		const int noProgress = level.time - self->botMind->wallwalkWalkNoProgressTime;

		// No progress for a while means a platform/ledge sits between us and the base, so the
		// straight line is blocked. Arc around it: blend a sideways (perpendicular) component into
		// the to-wall direction, alternating sides every second so the bot spirals off the obstacle
		// toward the wall instead of pressing into it and circling under it. The 2:1 sideways bias
		// (as in BotFindDefaultSteerTarget) keeps a pull toward the base so the arc still closes on
		// it. This is the "try a wall even if the detour first moves us away" behaviour.
		if ( noProgress > BOT_WALLWALK_DEFLECT_DELAY )
		{
			const float sideSign = ( ( level.time / 1000 + self->s.number ) & 1 ) ? 1.f : -1.f;
			glm::vec3 side( -toMarker.y, 0.f, toMarker.x );
			if ( glm::length( side ) > 1.f )
			{
				toMarker = glm::normalize( toMarker + side * 2.f * sideSign );
			}
		}

		if ( wantDebug && level.time >= s_nextLog )
		{
			s_nextLog = level.time + 1000;
			Log::Notice( "Wallwalk: client %d (%s) walking to wall base %.0f away (best %.0f, no-progress %dms%s, top %.0f, target %.0f)",
				self->s.number, BG_Class( classNum )->name, distToBase,
				self->botMind->wallwalkWalkBestDist, noProgress,
				noProgress > BOT_WALLWALK_DEFLECT_DELAY ? " deflecting" : "",
				markerTopZ, targetPos.z );
		}

		BotSeek( self, toMarker );
		self->botMind->cmdBuffer.upmove = 0;
		return true;
	}

	if ( seekRange <= 0.f || targetDist <= seekRange )
	{
		if ( wantDebug && level.time >= s_nextLog )
		{
			s_nextLog = level.time + 1000;
			Log::Notice( "Wallwalk: client %d (%s) target %.0f above at (%.0f,%.0f,%.0f), walking under it (%.0f away)",
				self->s.number, BG_Class( classNum )->name, heightDiff,
				targetPos.x, targetPos.y, targetPos.z, targetDist );
		}
		BotSeek( self, toTarget );
		self->botMind->cmdBuffer.upmove = 0;
		return true;
	}

	if ( wantDebug && level.time >= s_nextLog )
	{
		s_nextLog = level.time + 1000;
		if ( !hasWall )
		{
			Log::Notice( "Wallwalk: client %d (%s) target %.0f above, no wall marker in mesh (wall markers present: %d)",
				self->s.number, BG_Class( classNum )->name, heightDiff, wallCount );
		}
		else
		{
			Log::Notice( "Wallwalk: client %d (%s) target at (%.0f,%.0f,%.0f) is %.0f above self (%.0f,%.0f,%.0f); nearest of %d wall marker(s) base at (%.0f,%.0f,%.0f) top %.0f is %.0f away (target %.0f away), beyond seek range %.0f",
				self->s.number, BG_Class( classNum )->name,
				targetPos.x, targetPos.y, targetPos.z, heightDiff,
				ownPos.x, ownPos.y, ownPos.z,
				wallCount, markerPos.x, markerPos.y, markerPos.z, markerTopZ, markerDist, targetDist, seekRange );
		}
	}

	return false;
}

// Drives a wallwalk-capable builder (advanced granger) toward an elevated coordinate
// goal -- the top of a wall or a ceiling, set by g_bot_wallwalkBuild -- instead of
// the ordinary ground route. When the bot is at the base of a wall below the goal it
// issues the wall-walk; otherwise it walks horizontally toward the nearest wall base.
// Returns true while still making progress (so the caller keeps this branch active),
// and false once the bot has reached or passed the goal's height, so the caller can
// hand control back to the build flow. Only meaningful for a wallwalk-capable class,
// which is why the caller (BotActionRoamInRadius) gates it on SCA_WALLCLIMBER.
bool BotWallwalkBuildTowardGoal( gentity_t *self )
{
	const botTarget_t &goal = self->botMind->goal;
	if ( !goal.targetsCoordinates() )
	{
		return false;
	}

	glm::vec3 ownPos = VEC2GLM( self->s.origin );
	glm::vec3 targetPos = goal.getPos();

	const float detectRange = g_bot_wallwalkDetectRange.Get();

	// While a climb is in progress (physically attached to the wall/ceiling, or within
	// its hold window) keep biting up toward the elevated spot instead of dropping off or
	// handing off: the build flow's G_CanBuild only accepts a near-horizontal surface, so
	// the bot must actually reach the top before we yield control -- a handoff while still
	// on the vertical face would just fail the build there.
	if ( BotIsWallClimbing( self ) )
	{
		BotClimbToward( self, targetPos );
		return true;
	}

	// Only once off the wall: at or above the goal's height the elevated spot is reached,
	// so let the build flow take over on the next tick.
	if ( targetPos.z <= ownPos.z + 40.f )
	{
		return false;
	}

	// Locate the wall whose base best reaches the goal: when the goal is above the bot the
	// nearest wall by horizontal distance is preferred over one merely on the way to it
	// (see G_BotNearestWallConnection). There may be none in this mesh, in which case
	// there is nothing to climb onto.
	glm::vec3 markerPos;
	float markerDist = 0.f;
	int wallCount = 0;
	float markerTopZ = 0.f;
	// Fresh search for the nearest reachable wall this tick. As in the combat path, while a
	// detour is committed the builder drives to the wall it locked on rather than re-picking the
	// nearest wall every tick as it approaches the elevated build spot.
	bool hasWall = G_BotNearestWallConnection( self->s.number, ownPos, targetPos, markerPos, markerDist, wallCount, markerTopZ, g_bot_wallwalkHeightReach.Get(), g_bot_wallwalkOvershootReach.Get() );
	( void )wallCount;
	( void )markerTopZ;

	// Lock the wall the builder heads to so it commits to one wall to reach the elevated build
	// spot instead of re-picking the nearest wall every tick. The window is refreshed while a
	// wall is in range, so the builder stays with the same wall for the whole approach.
	const bool withinCommitWindow = self->botMind->wallwalkDetourTime > 0
		&& level.time - self->botMind->wallwalkDetourTime < BOT_WALLWALK_DETOUR_COMMIT_WINDOW;
	if ( hasWall )
	{
		if ( !withinCommitWindow )
		{
			self->botMind->wallwalkDetourPos = markerPos;
		}
		self->botMind->wallwalkDetourTime = level.time;
	}
	if ( withinCommitWindow )
	{
		const glm::vec3 locked = self->botMind->wallwalkDetourPos;
		markerPos = locked;
		markerDist = glm::length( glm::vec3( locked.x - ownPos.x, locked.y - ownPos.y, 0.f ) );
		hasWall = true;
	}

	// Primary trigger: at the base of a generated wall (within the detect range of its
	// base marker), bite it and climb to the elevated spot. Same fix as the combat path --
	// trust the engine's attach trace over a bot-side pre-check, which is why a builder
	// used to mill around a wall base instead of climbing it. Aiming up-and-into-the-wall
	// (BotWallwalkClimbAim) gives the attach trace a face to grab.
	if ( hasWall && markerDist <= detectRange )
	{
		BotClimbToward( self, BotWallwalkClimbAim( ownPos, markerPos ) );
		return true;
	}

	// Fallback: a wall physically against the bot with no near marker -- bite it.
	{
		glm::vec3 wallDir;
		if ( BotAdjacentClimbWall( self, ownPos, targetPos, detectRange, wallDir ) )
		{
			BotClimbToward( self, glm::vec3( ownPos.x + wallDir.x * 128.f, ownPos.y + wallDir.y * 128.f, ownPos.z + 128.f ) );
			return true;
		}
	}

	if ( !hasWall )
	{
		// No wall in this mesh to climb: walk horizontally toward the goal's base and
		// let the ordinary (ground) route take it the rest of the way.
		glm::vec3 dir = targetPos - ownPos;
		dir.z = 0.f;
		BotSeek( self, dir );
		return true;
	}

	// Not yet at the wall base: walk horizontally to the marker's XZ (on the wall face at
	// the wall's base) and let the bite above take us up on the next tick. Crouching a
	// wall that is still a few units away would just spin in place.
	glm::vec3 toMarker = markerPos - ownPos;
	toMarker.z = 0.f;
	BotSeek( self, toMarker );
	return true;
}

void BotMoveUpward( gentity_t *self, glm::vec3 nextCorner )
{
	const playerState_t& ps  = self->client->ps;
	weaponMode_t wpm = WPM_NONE;
	int magnitude = 0;
	switch ( ps.stats [ STAT_CLASS ] )
	{
	case PCL_ALIEN_BUILDER0_UPG:
	case PCL_ALIEN_LEVEL0:
		BotClimbToGoal( self );
		break;
	case PCL_ALIEN_LEVEL1:
		// Keep biting the wall (rather than pounce and pop off) while a climb is in
		// progress, during its hold window, or while the bot is trying to reach an enemy that
		// sits above it from the wall -- a pounce there would pop it off and whiff the enemy
		// above, so it keeps climbing until it reaches the target, as a human wallwalker does.
		if ( level.time < WallclimbStopTime( self ) || BotIsWallClimbing( self )
				|| BotIsAttemptingWallBite( self ) )
		{
			BotClimbToGoal( self );
			return;
		}
		if ( ps.weaponCharge <= 50 ) // I don't remember why 50
		{
			wpm = WPM_SECONDARY;
			magnitude = LEVEL1_POUNCE_DISTANCE;
		}
		break;
	case PCL_ALIEN_LEVEL2:
	case PCL_ALIEN_LEVEL2_UPG:
		{
			self->botMind->cmdBuffer.forwardmove = 127;
			self->botMind->cmdBuffer.rightmove = 0;
			self->botMind->cmdBuffer.angles[ PITCH ] = ANGLE2SHORT( -60.f ) - self->client->ps.delta_angles[ PITCH ];
			BotJump( self );
			break;
		}
	case PCL_ALIEN_LEVEL3:
		if ( ps.weaponCharge < LEVEL3_POUNCE_TIME )
		{
			wpm = WPM_SECONDARY;
			magnitude = LEVEL3_POUNCE_JUMP_MAG;
		}
		break;
	case PCL_ALIEN_LEVEL3_UPG:
		if ( ps.weaponCharge < LEVEL3_POUNCE_TIME_UPG )
		{
			wpm = WPM_SECONDARY;
			magnitude = LEVEL3_POUNCE_JUMP_MAG_UPG;
		}
		break;
	default:
		return;
	}
	if ( wpm != WPM_NONE )
	{
		usercmd_t &botCmdBuffer = self->botMind->cmdBuffer;

		float pitch = Math::Clamp(
			-CalcAimPitch( self, nextCorner, magnitude ) - g_bot_upwardLeapAngleCorr.Get(),
			-90.f, 90.f );
		self->botMind->cmdBuffer.angles[ PITCH ] =
			ANGLE2SHORT( pitch ) - self->client->ps.delta_angles[ PITCH ];

		BotFireWeapon( wpm, &botCmdBuffer );
	}
}

static Cvar::Cvar<float>  g_bot_jetpackLandingZ("g_bot_jetpackLandingZ", "how far below the desired height the landing manouver is initiated", Cvar::NONE, 100);
static Cvar::Cvar<float>  g_bot_jetpackOvershootZ("g_bot_jetpackOvershootZ", "how far above the desired height the jetpack is stopped", Cvar::NONE, 0);

static bool BotFlyUpward( gentity_t *self, glm::vec3 &ownPos, glm::vec3 &nextCorner )
{
	switch ( self->botMind->jetpackState )
	{
	case BOT_JETPACK_NONE:
		if ( level.time - self->botMind->lastNavconTime < 500 && self->botMind->lastNavconDistance > 50 )
		{
			self->botMind->jetpackState = BOT_JETPACK_NAVCON_WAITING;
			return true;
		}
		return false;
	case BOT_JETPACK_NAVCON_WAITING:
		{
			// firing a full fuel tank will reach a relative height of about 1500 quake units
			// use a little less here for safety
			float maxDistance = 1200.f;
			float actualDistance = static_cast<float>( self->botMind->lastNavconDistance );
			float maxFuel = static_cast<float>( JETPACK_FUEL_MAX );
			int requiredFuel = static_cast<int>( maxFuel * actualDistance / maxDistance );
			if ( self->client->ps.stats[ STAT_FUEL ] >= requiredFuel && level.time - self->botMind->lastNavconTime > 500 )
			{
				self->botMind->jetpackState = BOT_JETPACK_NAVCON_FLYING;
			}
		}
		BotResetStuckTime( self );
		BotStandStill( self );
		return true;
	case BOT_JETPACK_NAVCON_FLYING:
		if ( nextCorner.z < ownPos.z + g_bot_jetpackLandingZ.Get() )
		{
			self->botMind->jetpackState = BOT_JETPACK_NAVCON_LANDING;
		}
		BotStandStill( self );
		self->botMind->cmdBuffer.upmove = 127;
		return true;
	case BOT_JETPACK_NAVCON_LANDING:
		if ( nextCorner.z + g_bot_jetpackOvershootZ.Get() < ownPos.z )
		{
			self->botMind->jetpackState = BOT_JETPACK_NONE;
			return false;
		}
		self->botMind->cmdBuffer.upmove = 127;
		return true;
	default:
		return false;
	}
}

static bool BotTryMoveUpward( gentity_t *self )
{
	int selfClientNum = self->client->num();
	glm::vec3 ownPos = VEC2GLM( self->s.origin );
	glm::vec3 nextCorner;
	bool hasNextCorner = G_BotPathNextCorner( selfClientNum, nextCorner );

	if ( !hasNextCorner )
	{
		nextCorner = self->botMind->nav().tpos;
	}

	// A wallwalk-capable class (dretch, mantis, advanced granger) whose route
	// leads onto a wall/ceiling polygon must climb onto it. A horizontal wall
	// has no upward component, so this bypasses the height-difference check
	// below and drives the class-specific climb via BotMoveUpward().
	if ( BG_ClassHasAbility( self->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER )
			&& G_BotNextCornerIsWallwalk( selfClientNum ) )
	{
		BotMoveUpward( self, nextCorner );
		return true;
	}

	// if not trying to move upward
	if ( nextCorner.z - ownPos.z < g_bot_upwardNavconMinHeight.Get() )
	{
		switch ( self->client->ps.stats [ STAT_CLASS ] )
		{
		case PCL_HUMAN_NAKED:
		case PCL_HUMAN_LIGHT:
		case PCL_HUMAN_MEDIUM:
			// might have a jetpack, that case is handled below
			break;
		case PCL_ALIEN_BUILDER0_UPG:
		case PCL_ALIEN_LEVEL0:
		case PCL_ALIEN_LEVEL1:
			if ( level.time > WallclimbStopTime( self ) )
			{
				return false;
			}
			break;
		default:
			return false;
		}
	}

	int diff = level.time - self->botMind->lastNavconTime;
	int limit = LEVEL3_POUNCE_TIME_UPG * 3 / 2;  // seems to be reasonable for all classes
	switch ( self->client->ps.stats [ STAT_CLASS ] )
	{
	case PCL_HUMAN_NAKED:
	case PCL_HUMAN_LIGHT:
	case PCL_HUMAN_MEDIUM:
		if ( !BG_InventoryContainsUpgrade( UP_JETPACK, self->client->ps.stats ) )
		{
			return false;
		}
		return BotFlyUpward( self, ownPos, nextCorner );
	case PCL_ALIEN_LEVEL0:
	case PCL_ALIEN_LEVEL1:
	case PCL_ALIEN_LEVEL2:
	case PCL_ALIEN_LEVEL2_UPG:
		if ( level.time > WallclimbStopTime( self ) )
		{
			return false;
		}
		break;
	case PCL_ALIEN_LEVEL3:
	case PCL_ALIEN_LEVEL3_UPG:
		if ( diff < 0 || diff > limit )
		{
			return false;
		}
		BotStandStill( self );
		break;
	default:
		break;
	}

	BotMoveUpward( self, nextCorner );
	return true;
}

// This function makes the bot move and aim at it's goal, trying
// to move faster if the skill is high enough, depending on it's
// class.
// Returns false on failure.
bool BotMoveToGoal( gentity_t *self )
{
	glm::vec3 dir;
	constexpr int ignoreGeometryThreshold = 1700; // 1.7s, we seem stuck
	constexpr int softStuckThreshold = 5000; // 5s, something's fishy
	int stuckTime = level.time - self->botMind->stuckTime;

	// At start, ignore geometry, unless we seem stuck for too long.
	// This avoids the following two bugs:
	//
	//  * https://github.com/Unvanquished/Unvanquished/issues/2358
	//    Maybe this issue would be better avoided by treating open door
	//    panes as obstacle, but in the current state this would make bot
	//    "loose focus" because their path was deemed invalid.
	//
	//  * https://github.com/Unvanquished/Unvanquished/issues/2263
	//    Maybe this issue would be better avoided by building thighter
	//    navmesh, so that the corners would be less susceptible to be
	//    blocking. A quick try didn't seem to show that other solution to
	//    be satisfying.

	bool ignoreGeometry = stuckTime < ignoreGeometryThreshold;
	obstacle_t obstacle = OBSTACLE_NONE;

	// Wallwalk-capable classes reach a target that is above them by climbing a
	// wall/ceiling when they are at its base. This bypasses the ground route and
	// must win over the ordinary upward check below (whose next corner is on the
	// floor and can never trigger the climb). Gated behind g_bot_wallwalkClimb.
	if ( BotWallwalkClimbTowardGoal( self ) )
	{
		return true;
	}

	if ( BotAvoidObstacles( self, dir, ignoreGeometry, obstacle ) )
	{
		if ( BotTryMoveUpward( self ) )
		{
			return true;
		}

		BotSeek( self, dir );
		bool softStuck = stuckTime > softStuckThreshold;
		// if we have not moved much for some time, let's propagate
		// a failure status, so we can find another goal
		return softStuck;
	}

	BotSeek( self, dir );

	team_t targetTeam = TEAM_NONE;
	if ( self->botMind->goal.targetsValidEntity() )
	{
		targetTeam = G_Team( self->botMind->goal.getTargetedEntity() );
	}

	// if target is friendly, let's go there quickly (heal, poison) by using trick moves
	// when available (still need to implement wall walking, but that will be more complex)
	if ( G_Team( self ) == TEAM_ALIENS && G_Team( self ) != targetTeam )
	{
		BotTryMoveUpward( self );
		return true;
	}
	else if ( BotTryMoveUpward( self ) )
	{
		return true;
	}

	weaponMode_t wpm = WPM_NONE;
	int magnitude = 0;
	const playerState_t& ps  = self->client->ps;
	if ( ( G_Team( self ) == TEAM_HUMANS && !self->botMind->skillSet[BOT_H_FAST_FLEE] )
			|| ( G_Team( self ) == TEAM_ALIENS && !self->botMind->skillSet[BOT_A_FAST_FLEE] ) )
	{
		if ( G_Team( self ) == TEAM_HUMANS )
		{
			BotWalkIfStaminaLow( self );
		}
		return true;
	}
	switch ( ps.stats [ STAT_CLASS ] )
	{
		case PCL_HUMAN_NAKED:
		case PCL_HUMAN_LIGHT:
		case PCL_HUMAN_MEDIUM:
		case PCL_HUMAN_BSUIT:
			BotSprint( self, true );
			BotWalkIfStaminaLow( self );
			break;
		//those classes do not really have capabilities allowing them to be
		//significantly faster while fleeing (except jumps, but that also
		//makes them easier to hit I'd say)
		case PCL_ALIEN_BUILDER0:
		case PCL_ALIEN_BUILDER0_UPG:
		case PCL_ALIEN_LEVEL0:
			break;
		case PCL_ALIEN_LEVEL1:
			if ( ps.weaponCharge <= 50 ) // I don't remember why 50
			{
				wpm = WPM_SECONDARY;
				magnitude = LEVEL1_POUNCE_DISTANCE;
			}
			break;
		case PCL_ALIEN_LEVEL2:
		case PCL_ALIEN_LEVEL2_UPG:
		{
			// make marauder jump randomly to be harder to hit
			// don't do this on every frame though, we would loose
			// a lot of maneuverability
			int msec = level.time - level.previousTime;
			constexpr float jumpChance = 0.2f; // chance per second
			if ( (jumpChance / 1000.0f) * msec > BG_random() )
			{
				BotJump( self );
			}
			break;
		}
		case PCL_ALIEN_LEVEL3:
			// flee by repeatedly charging and releasing the pounce
			// however, charging the pounce prevents jumping
			// if we are facing an obstacle we can jump over:
			// stop charging (or do not start charging) until we have
			// jumped over the obstacle
			if ( ps.weaponCharge < LEVEL3_POUNCE_TIME
				 && obstacle != OBSTACLE_JUMPABLE )
			{
				wpm = WPM_SECONDARY;
				magnitude = LEVEL3_POUNCE_JUMP_MAG;
			}
		break;
		case PCL_ALIEN_LEVEL3_UPG:
			// see the comment at case PCL_ALIEN_LEVEL3
			if ( ps.weaponCharge < LEVEL3_POUNCE_TIME_UPG
				 && obstacle != OBSTACLE_JUMPABLE )
			{
				wpm = WPM_SECONDARY;
				magnitude = LEVEL3_POUNCE_JUMP_MAG_UPG;
			}
			break;
		case PCL_ALIEN_LEVEL4:
			wpm = WPM_SECONDARY;
		break;
	}
	if ( wpm != WPM_NONE )
	{
		usercmd_t &botCmdBuffer = self->botMind->cmdBuffer;
		if ( magnitude )
		{
			glm::vec3 target = self->botMind->nav().tpos;
			float pitch = -CalcAimPitch( self, target, magnitude ) / 3;
			self->botMind->cmdBuffer.angles[ PITCH ] =
				ANGLE2SHORT( pitch ) - self->client->ps.delta_angles[ PITCH ];
		}
		BotFireWeapon( wpm, &botCmdBuffer );
	}
	return true;
}

bool FindRouteToTarget( gentity_t *self, botTarget_t target, bool allowPartial )
{
	botRouteTarget_t routeTarget;
	BotTargetToRouteTarget( self, target, &routeTarget );
	return G_BotFindRoute( self->s.number, &routeTarget, allowPartial );
}
