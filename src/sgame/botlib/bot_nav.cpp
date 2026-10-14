/*
===========================================================================

Daemon BSD Source Code
Copyright (c) 2013 Daemon Developers
All rights reserved.

This file is part of the Daemon BSD Source Code (Daemon Source Code).

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Daemon developers nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL DAEMON DEVELOPERS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS

===========================================================================
*/

#include "common/Common.h"
#include <cmath>
#include "bot_local.h"
#include "bot_api.h"
#include "sgame/sg_local.h"

Bot_t agents[ MAX_CLIENTS ];

/*
====================
bot_nav.cpp

All vectors used as inputs and outputs to functions here use the engine's coordinate system
====================
*/

#if 0
static void BotSetPolyFlags(
	const glm::vec3 &origin, const glm::vec3 &mins, const glm::vec3 &maxs, unsigned short flags )
{
	if ( !numNavData )
	{
		return;
	}

	// convert possibly lopsided extents to centered ones
	glm::vec3 qCenter = origin + 0.5f * ( mins + maxs );
	glm::vec3 qExtents = 0.5f * ( maxs - mins );

	// convert to recast coordinates
	rVec center(qCenter);
	rVec extents(qExtents);

	// setup a filter so our queries include disabled polygons
	// FIXME: that doesn't make sense! POLYFLAGS_DISABLED is 0
	dtQueryFilter filter;
	filter.setIncludeFlags( POLYFLAGS_WALK | POLYFLAGS_DISABLED );
	filter.setExcludeFlags( 0 );

	for ( int i = 0; i < numNavData; i++ )
	{
		const int maxPolys = 20;
		int polyCount = 0;
		dtPolyRef polys[ maxPolys ];

		dtNavMeshQuery *query = BotNavData[ i ].query;
		dtNavMesh *mesh = BotNavData[ i ].mesh;

		query->queryPolygons( center, extents, &filter, polys, &polyCount, maxPolys );

		for ( int j = 0; j < polyCount; j++ )
		{
			mesh->setPolyFlags( polys[ j ], flags );
		}
	}
}
#endif

// must be called on class or upgrade change
// sets navmesh and polyflag filter
void G_BotSetNavMesh( gentity_t *ent )
{
	class_t newClass = NavmeshForClass( static_cast<class_t>( ent->client->ps.stats[ STAT_CLASS ] ),
	                                    g_bot_navmeshReduceTypes.Get() );

	NavData_t *nav = nullptr;

	for ( int i = 0; i < numNavData; i++ )
	{
		if ( BotNavData[ i ].species == newClass )
		{
			nav = &BotNavData[ i ];
			break;
		}
	}

	Bot_t &bot = agents[ ent->num() ];

	if ( !nav )
	{
		Log::Warn( "G_BotSetNavMesh: no navmesh for %s", BG_Class( newClass )->name );

		if ( bot.nav == nullptr )
		{
			// Drop to prevent the imminent crash
			Sys::Drop( "bot spawned as %s without a navmesh", BG_Class( newClass )->name );
		}

		return;
	}

	// should only init the corridor once
	if ( !bot.corridor.getPath() )
	{
		if ( !bot.corridor.init( MAX_BOT_PATH ) )
		{
			Sys::Drop( "Out of memory (bot corridor init)" );
		}
	}

	int polyflags = POLYFLAGS_WALK;
	if ( BG_InventoryContainsUpgrade( UP_JETPACK, ent->client->ps.stats ) )
	{
		polyflags |= POLYFLAGS_JETPACK;
	}
	// Wallwalk-capable classes (dretch, mantis, advanced granger) may also traverse
	// polygons tagged POLYFLAGS_WALLWALK. We no longer generate extra wall layers to
	// route onto (a recast tile cache's single global up-vector makes rotated wall
	// tiles unroutable), so this only matters for the climb trigger, which detects
	// proximity to the base-of-wall markers rather than routing onto a wall poly.
	// Non-wallwalk classes are unaffected because their filter never includes
	// POLYFLAGS_WALLWALK.
	const bool classWallwalk = BG_ClassHasAbility( ent->client->ps.stats[ STAT_CLASS ], SCA_WALLCLIMBER );
	if ( classWallwalk )
	{
		polyflags |= POLYFLAGS_WALLWALK;
	}
	bot.filter.setIncludeFlags( polyflags );

	bot.nav = nav;

	// Report how many base-of-wall markers this class's mesh carries, so the climb
	// path can be observed to actually have data to work with. A zero means no
	// markers were generated/loaded and the bot can never climb, but we stay quiet
	// on that case (the trigger's own "no nearby wall marker" diagnostic covers it)
	// to avoid logging on every class change.
	if ( classWallwalk )
	{
		const OffMeshConnections &con = nav->process.con;
		int wallCon = 0;
		for ( int i = 0; i < con.offMeshConCount; ++i )
		{
			if ( con.flags[ i ] & POLYFLAGS_WALLWALK )
			{
				++wallCon;
			}
		}
		if ( wallCon > 0 )
		{
			Log::Notice( "Wallwalk: %s navmesh has %d of %d off-mesh connection marker(s) at wall bases",
				BG_Class( newClass )->name, wallCon, con.offMeshConCount );
		}
	}
	float clearVec[3]{};
	bot.corridor.reset( 0, clearVec );
	bot.clientNum = ent->num();
	bot.needReplan = true;
	bot.offMesh = false;
	bot.numCorners = 0;
	memset( bot.routeResults, 0, sizeof( bot.routeResults ) );
}

static void GetEntPosition( int num, rVec &pos )
{
	pos = rVec( VEC2GLM( g_entities[ num ].s.origin ) );
}

bool G_BotFindRoute( int botClientNum, const botRouteTarget_t *target, bool allowPartial )
{
	rVec start;
	Bot_t *bot = &agents[ botClientNum ];

	GetEntPosition( botClientNum, start );
	return FindRoute( bot, start, *target, allowPartial );
}

static bool withinRadiusOfOffMeshConnection( const Bot_t *bot, rVec pos, rVec off, dtPolyRef conPoly )
{
	const dtOffMeshConnection *con = bot->nav->mesh->getOffMeshConnectionByRef( conPoly );
	if ( !con )
	{
		return false;
	}

	if ( dtVdist2DSqr( pos, off ) < con->rad * con->rad )
	{
		return true;
	}

	return false;
}

static bool overOffMeshConnectionStart( const Bot_t *bot, rVec pos )
{
	if ( bot->numCorners < 1 )
	{
		return false;
	}
	int corner = bot->numCorners - 1;
	const bool offMeshConnection = ( bot->cornerFlags[ corner ] & DT_STRAIGHTPATH_OFFMESH_CONNECTION ) ? true : false;

	if ( offMeshConnection )
	{
		return withinRadiusOfOffMeshConnection(
			bot, pos, rVec::Load( bot->cornerVerts + corner * 3 ), bot->cornerPolys[ corner ] );
	}
	return false;
}

bool G_IsBotOverNavcon( int botClientNum )
{
	rVec spos;
	Bot_t *bot = &agents[ botClientNum ];
	GetEntPosition( botClientNum, spos );
	return overOffMeshConnectionStart( bot, spos );
}

// Returns true when the bot's immediate next path corner lies on a polygon that
// was generated as a wall/ceiling layer (tagged POLYFLAGS_WALLWALK). This lets
// a wallwalk-capable class decide it must climb onto the wall rather than walk
// across the ground to the corner.
bool G_BotNextCornerIsWallwalk( int botClientNum )
{
	Bot_t *bot = &agents[ botClientNum ];
	if ( !bot->nav || bot->numCorners < 1 )
	{
		return false;
	}

	dtPolyRef poly = bot->cornerPolys[ bot->numCorners - 1 ];
	if ( !poly )
	{
		return false;
	}

	unsigned short flags = 0;
	if ( dtStatusFailed( bot->nav->mesh->getPolyFlags( poly, &flags ) ) )
	{
		return false;
	}

	return ( flags & navPolyFlags::POLYFLAGS_WALLWALK ) != 0;
}

// Finds the base-of-wall off-mesh connection (tagged POLYFLAGS_WALLWALK) closest to
// the bot and reports it, so a wallwalk-capable class knows where the nearest
// climbable wall is even when it is not yet standing at a wall base.
//
// Returns true and sets `markerPos` to the connection's start (the wall base, at the
// map floor) and `markerDist` to the horizontal distance from `ownPos` to it, when at
// least one such connection exists. Returns false when there are none, which means
// climbing is impossible for this bot and the caller should fall back to ordinary
// upward (jumping) movement.
//
// `ownPos` and `markerPos` are both in the quake coordinate system (Z up), which is
// what the caller (BotWallwalkClimbTowardGoal) expects. The stored marker coordinates
// are in the recast frame (Y up), so the start is converted with ToQuake() before being
// handed back. Only the bridge connections emitted for wallwalk-capable classes carry
// POLYFLAGS_WALLWALK, so this never matches hand-authored navcons.
bool G_BotNearestWallConnection( int botClientNum, const glm::vec3 &ownPos, glm::vec3 &markerPos, float &markerDist )
{
	Bot_t *bot = &agents[ botClientNum ];
	if ( !bot->nav )
	{
		return false;
	}

	const OffMeshConnections &con = bot->nav->process.con;
	if ( con.offMeshConCount == 0 )
	{
		return false;
	}

	rVec spos( ownPos );
	bool found = false;
	float bestDistSq = 1e18f;
	for ( int i = 0; i < con.offMeshConCount; ++i )
	{
		if ( ( con.flags[ i ] & navPolyFlags::POLYFLAGS_WALLWALK ) == 0 )
		{
			continue; // only generated wall/ceiling bridge connections
		}

		rVec start = rVec::Load( &con.verts[ i * 6 + 0 ] );
		// The marker start and spos are both in the recast frame (Y up), so the
		// horizontal separation (recast x/z, which is quake x/y) is compared directly.
		// A bot at the base of a tall wall still matches because only the horizontal
		// distance is used.
		const float dx = start[ 0 ] - spos[ 0 ];
		const float dz = start[ 2 ] - spos[ 2 ];
		const float d2 = dx * dx + dz * dz;
		if ( d2 < bestDistSq )
		{
			bestDistSq = d2;
			found = true;
			// Hand the caller a quake-space (Z up) position: the stored start is in the
			// recast frame, so convert it. This is the base of the wall the bot should
			// walk toward and climb from.
			markerPos = start.ToQuake();
		}
	}

	if ( !found )
	{
		return false;
	}

	markerDist = std::sqrt( bestDistSq );
	return true;
}

static void G_UpdatePathCorridor( Bot_t *bot, rVec spos, botRouteTargetInternal target )
{
	bot->corridor.movePosition( spos, bot->nav->query, &bot->filter );

	if ( target.type == botRouteTargetType_t::BOT_TARGET_DYNAMIC )
	{
		bot->corridor.moveTargetPosition( target.pos, bot->nav->query, &bot->filter );
	}

	if ( !bot->corridor.isValid( MAX_PATH_LOOKAHEAD, bot->nav->query, &bot->filter ) )
	{
		bot->corridor.trimInvalidPath( bot->corridor.getFirstPoly(), spos, bot->nav->query, &bot->filter );
		bot->needReplan = true;
	}

	FindWaypoints( bot, bot->cornerVerts, bot->cornerFlags, bot->cornerPolys, &bot->numCorners, MAX_CORNERS );
}

// return false if the bot has no next corner in mind, and true otherwise
// currently, this uses closestPointOnPolyBoundary as an approximation
bool G_BotPathNextCorner( int botClientNum, glm::vec3 &result )
{
	Bot_t *bot = &agents[ botClientNum ];
	gentity_t *ent = &g_entities[ botClientNum ];
	if ( bot->numCorners <= 0 )
	{
		return false;
	}
	dtPolyRef firstPoly = bot->corridor.getFirstPoly();
	rVec corner{};
	rVec ownPos( VEC2GLM( ent->s.origin ) );
	bot->nav->query->closestPointOnPolyBoundary( firstPoly, ownPos, corner ); // FIXME: check error
	result = corner.ToQuake();
	return true;
}

void G_BotUpdatePath( int botClientNum, const botRouteTarget_t *target, botNavCmd_t *cmd )
{
	rVec spos;
	rVec epos;
	Bot_t *bot = &agents[ botClientNum ];
	botRouteTargetInternal rtarget;

	if ( !cmd || !target )
	{
		return;
	}

	GetEntPosition( botClientNum, spos );

	rtarget = *target;
	epos = rtarget.pos;

	G_UpdatePathCorridor( bot, spos, rtarget );

	if ( !bot->offMesh )
	{
		if ( bot->needReplan && FindRoute( bot, spos, rtarget, false ) )
		{
			bot->needReplan = false;
		}

		cmd->havePath = !bot->needReplan;

		if ( overOffMeshConnectionStart( bot, spos ) )
		{
			dtPolyRef refs[ 2 ];
			rVec start;
			rVec end;
			// numCorners is guaranteed to be >= 1 here
			dtPolyRef con = bot->cornerPolys[ bot->numCorners - 1 ];

			if ( bot->corridor.moveOverOffmeshConnection( con, refs, start, end, bot->nav->query ) )
			{
				bot->offMesh = true;
				bot->offMeshPoly = con;
				bot->offMeshEnd = end;
				bot->offMeshStart = start;

				// save some characteristics about the connection in the bot's mind, for later use
				// this happens quite rarely, so we can afford a square root for the
				// distance here
				gentity_t *self = &g_entities[ botClientNum ];
				self->botMind->lastNavconTime = level.time;
				glm::vec3 nextCorner = glm::vec3( end[ 0 ], end[ 2 ], end[ 1 ] );
				glm::vec3 ownPos = VEC2GLM( self->s.origin );
				// HACK: if the bot wants to move upward, make the distance
				// positive, make it negative otherwise
				self->botMind->lastNavconDistance = glm::distance( ownPos , nextCorner );
				if ( nextCorner.z - ownPos.z < 0 )
				{
					self->botMind->lastNavconDistance = -self->botMind->lastNavconDistance;
				}
			}
		}

		dtPolyRef firstPoly = bot->corridor.getFirstPoly();
		dtPolyRef lastPoly = bot->corridor.getLastPoly();

		if ( !PointInPoly( bot, firstPoly, spos ) ||
		   ( rtarget.type == botRouteTargetType_t::BOT_TARGET_DYNAMIC
		     && !PointInPolyExtents( bot, lastPoly, epos, rtarget.polyExtents ) ) )
		{
			bot->needReplan = true;
		}

		rVec rdir;
		BotCalcSteerDir( bot, rdir );
		cmd->dir = rdir.ToQuake();

		cmd->directPathToGoal = bot->numCorners <= 1;

		cmd->pos = rVec::Load( bot->corridor.getPos() ).ToQuake();

		// if there are no corners, we have reached the goal
		// FIXME: this must be done because of a weird bug where the target is not reachable even if
		// the path was checked for a partial path beforehand
		if ( bot->numCorners == 0 )
		{
			cmd->tpos = cmd->pos;
		}
		else
		{
			rVec tpos = rVec::Load( bot->corridor.getTarget() );

			float height;
			if ( dtStatusSucceed( bot->nav->query->getPolyHeight( bot->corridor.getLastPoly(), tpos, &height ) ) )
			{
				tpos[ 1 ] = height;
			}
			cmd->tpos = tpos.ToQuake();
		}
	}

	if ( bot->offMesh )
	{
		glm::vec3 start = bot->offMeshStart.ToQuake();
		glm::vec3 end = bot->offMeshEnd.ToQuake();

		glm::vec3 pos = VEC2GLM( g_entities[ botClientNum ].s.origin );
		start[ 2 ] = pos[ 2 ];
		end[ 2 ] = pos[ 2 ];

		cmd->pos = ProjectPointOntoVectorBounded( pos, start, end );

		cmd->directPathToGoal = false;
		cmd->dir = end - pos;
		VectorNormalize( cmd->dir );

		rVec tpos = rVec::Load( bot->corridor.getTarget() );
		float height;
		if ( dtStatusSucceed( bot->nav->query->getPolyHeight( bot->corridor.getLastPoly(), tpos, &height ) ) )
		{
			tpos[ 1 ] = height;
		}
		cmd->tpos = tpos.ToQuake();

		cmd->havePath = true;

		if ( withinRadiusOfOffMeshConnection( bot, spos, bot->offMeshEnd, bot->offMeshPoly ) )
		{
			bot->offMesh = false;
		}
	}
}

bool BotFindRandomPointInRadius( int botClientNum, const glm::vec3 &origin, glm::vec3 &point, float radius )
{
	rVec rorigin(origin);
	rVec nearPoint;
	dtPolyRef nearPoly;

	point = { 0, 0, 0 };

	Bot_t *bot = &agents[ botClientNum ];

	if ( !BotFindNearestPoly( bot, rorigin, &nearPoly, nearPoint ) )
	{
		return false;
	}

	dtPolyRef randRef;
	dtStatus status = bot->nav->query->findRandomPointAroundCircle( nearPoly, rorigin, radius, &bot->filter, BG_random, &randRef, nearPoint );

	if ( dtStatusFailed( status ) )
	{
		return false;
	}

	point = nearPoint.ToQuake();
	return true;
}

bool G_BotNavTrace( int botClientNum, botTrace_t *trace, const glm::vec3& start, const glm::vec3& end )
{
	dtPolyRef startRef;
	dtStatus status;
	rVec extents( 75, 96, 75 );
	rVec spos(start);
	rVec epos(end);

	*trace = {};

	Bot_t *bot = &agents[ botClientNum ];

	status = bot->nav->query->findNearestPoly( spos, extents, &bot->filter, &startRef, nullptr );
	if ( dtStatusFailed( status ) || startRef == 0 )
	{
		//try larger extents
		extents[ 1 ] += 500;
		status = bot->nav->query->findNearestPoly( spos, extents, &bot->filter, &startRef, nullptr );
		if ( dtStatusFailed( status ) || startRef == 0 )
		{
			return false;
		}
	}

	rVec unusedNormal;
	status = bot->nav->query->raycast( startRef, spos, epos, &bot->filter, &trace->frac, unusedNormal, nullptr, nullptr, 0 );
	return !dtStatusFailed( status );
}

std::map<int, saved_obstacle_t> savedObstacles;
std::map<int, std::array<dtObstacleRef, MAX_NAV_DATA>> obstacleHandles; // handles of detour's obstacles, if any

void G_BotAddObstacle( const glm::vec3 &qmins, const glm::vec3 &qmaxs, int obstacleNum )
{
	savedObstacles[obstacleNum] = { navMeshLoaded == navMeshStatus_t::LOADED, { qmins, qmaxs } };
	if ( navMeshLoaded != navMeshStatus_t::LOADED )
	{
		return;
	}

	std::array<dtObstacleRef, MAX_NAV_DATA> handles;
	std::fill(handles.begin(), handles.end(), (unsigned int)-1);
	for ( int i = 0; i < numNavData; i++ )
	{
		NavData_t *nav = &BotNavData[ i ];

		const dtTileCacheParams *params = nav->cache->getParams();
		float offset = params->walkableRadius;

		rVec rmins(qmins);
		rVec rmaxs(qmaxs);

		// offset bbox by agent radius like the navigation mesh was originally made
		rmins[ 0 ] -= offset;
		rmins[ 2 ] -= offset;

		rmaxs[ 0 ] += offset;
		rmaxs[ 2 ] += offset;

		// offset mins down by agent height so obstacles placed on ledges are handled correctly
		rmins[ 1 ] -= params->walkableHeight;

		// FIXME: check error of addBoxObstacle
		nav->cache->addBoxObstacle( rmins, rmaxs, &handles[i] );
	}
	auto result = obstacleHandles.insert({obstacleNum, std::move(handles)});
	if ( !result.second )
	{
		Log::Warn("Insertion of obstacle %i failed. Was an obstacle of this number inserted already?", obstacleNum);
	}
}

// We do lazy load navmesh when bots are added. The downside is that this means
// map entities are loaded before the navmesh are. This workaround does keep
// those obstacle (such as doors and buildables) in mind until when navmesh is
// finally loaded, or generated.
void BotAddSavedObstacles()
{
	for ( auto &obstacle : savedObstacles )
	{
		if ( !obstacle.second.added )
			G_BotAddObstacle(obstacle.second.bbox.mins, obstacle.second.bbox.maxs, obstacle.first);
		obstacle.second.added = true;
	}
}

void G_BotRemoveObstacle( int obstacleNum )
{
	auto obstacle = savedObstacles.find(obstacleNum);
	if (obstacle != savedObstacles.end()) {
		savedObstacles.erase(obstacle);
	}

	auto iterator = obstacleHandles.find(obstacleNum);
	if (iterator != obstacleHandles.end()) {
		for ( int i = 0; i < numNavData; i++ )
		{
			NavData_t *nav = &BotNavData[ i ];
			std::array<dtObstacleRef, MAX_NAV_DATA> &handles = iterator->second;
			if ( nav->cache->getObstacleCount() <= 0 )
			{
				continue;
			}
			if ( handles[i] != (unsigned int)-1 )
			{
				nav->cache->removeObstacle( handles[i] );
			}
		}
		obstacleHandles.erase(iterator);
	}
}

void G_BotUpdateObstacles()
{
	for ( int i = 0; i < numNavData; i++ )
	{
		NavData_t *nav = &BotNavData[ i ];
		nav->cache->update( 0, nav->mesh );
	}
}
