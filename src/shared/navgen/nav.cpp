/*
   ===========================================================================
   Copyright (C) 2012 Unvanquished Developers

   This file is part of Daemon source code.

   Daemon source code is free software; you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; either version 2 of the License,
   or (at your option) any later version.

   Daemon source code is distributed in the hope that it will be
   useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Daemon source code; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
   ===========================================================================
 */
// nav.cpp -- navigation mesh generator interface

#include "common/Common.h"

#include <iostream>
#include <vector>
#include <queue>
#include <map>
#include <unordered_set>
#include <unordered_map>
#include <algorithm>
#include <cstdint>

#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/hash.hpp>

#include "engine/qcommon/qcommon.h"
#include "shared/CommonProxies.h"
#include "common/cm/cm_patch.h"
#include "common/cm/cm_local.h"
#include "common/FileSystem.h"
#include "shared/bg_gameplay.h"
#include "sgame/botlib/bot_convert.h"
#include "navgen.h"

// disable suppression in case multiple threads finishing together lead to a burst of output
static auto LOG = Log::Logger( VM_STRING_PREFIX "navgen", "", Log::Level::NOTICE ).WithoutSuppression();

void UnvContext::doLog(rcLogCategory category, const char* msg, int len)
{
	std::string line(msg, len);
	switch ( category )
	{
		case RC_LOG_WARNING:
		case RC_LOG_ERROR:
			RunOnMainThread( [line] { LOG.Warn( line ); } );
			break;
		case RC_LOG_PROGRESS:
		default:
			RunOnMainThread( [line] { LOG.Notice( line ); } );
			break;
	}
}

void UnvContext::RunOnMainThread( std::function<void()> f )
{
	mainThreadTasks_.push_back( std::move( f ) );
}

void UnvContext::DoMainThreadTasks()
{
	for ( auto &f : mainThreadTasks_ )
	{
		f();
	}

	mainThreadTasks_.clear();
}

static NavgenStatus::Code CodeForFailedDtStatus(dtStatus status)
{
	if ( dtStatusDetail( status, DT_OUT_OF_MEMORY ) )
	{
		return NavgenStatus::TRANSIENT_FAILURE;
	}
	return NavgenStatus::PERMANENT_FAILURE;
}

static int tileSize = 64;

void NavmeshGenerator::WriteFile( const NavgenTask &t ) {
	if ( t.status.code == NavgenStatus::OK )
	{
		LOG.Notice( "Finished generating navmesh for %s", BG_ClassModelConfig( t.species )->humanName );
	}
	else
	{
		LOG.Warn( "Navmesh generation for %s failed: %s", BG_ClassModelConfig( t.species )->humanName, t.status.message );
	}

	if ( t.status.code == NavgenStatus::TRANSIENT_FAILURE )
	{
		return; // Don't write anything
	}

	// there are 22 bits to store a tile and its polys
	int tileBits = rcMin( ( int ) dtIlog2( dtNextPow2( t.tcparams.maxTiles ) ), 14 );
	int polyBits = 22 - tileBits;

	dtNavMeshParams params;
	dtVcopy( params.orig, t.tcparams.orig );
	params.tileHeight = t.tcparams.height * t.cfg.cs;
	params.tileWidth = t.tcparams.width * t.cfg.cs;
	params.maxTiles = 1 << tileBits;
	params.maxPolys = 1 << polyBits;

	std::string filename = NavmeshFilename( mapName_, t.species );

	NavMeshSetHeader header;
	int maxTiles;

	if ( t.status.code == NavgenStatus::OK )
	{
		maxTiles = t.tileCache->getTileCount();
		int numTiles = 0;

		for ( int i = 0; i < maxTiles; i++ )
		{
			const dtCompressedTile *tile = t.tileCache->getTile( i );
			if ( !tile || !tile->header || !tile->dataSize ) {
				continue;
			}
			numTiles++;
		}
		header.numTiles = numTiles;
		header.cacheParams = *t.tileCache->getParams();
		header.params = params;
	}
	else
	{
		header.params = {};
		header.cacheParams = {};
		header.numTiles = -1;
	}

	header.magic = NAVMESHSET_MAGIC;
	header.version = NAVMESHSET_VERSION;
	header.productVersionHash = ProductVersionHash();
	header.headerSize = sizeof(header);
	header.mapId = GetNavgenMapId( mapName_ );
	header.config = config_;

	SwapNavMeshSetHeader( header );

	qhandle_t file;
	trap_FS_FOpenFile( filename.c_str(), &file, fsMode_t::FS_WRITE );

	if ( !file ) {
		LOG.Warn( "Error opening %s", filename );
		return;
	}

	auto Write = [file, &filename](const void* data, size_t len) -> bool {
		if (len != static_cast<size_t>(trap_FS_Write(data, len, file)))
		{
			LOG.Warn( "Error writing navmesh file %s", filename );
			trap_FS_FCloseFile( file );
			std::error_code err;
			FS::HomePath::DeleteFile( filename, err );
			return false;
		}
		return true;
	};

	if ( !Write( &header, sizeof( header ) ) ) return;

	if ( t.status.code == NavgenStatus::PERMANENT_FAILURE )
	{
		// rest of file is the error message
		if ( !Write( t.status.message.data(), t.status.message.size() ) ) return;
		trap_FS_FCloseFile( file );
		return;
	}
	ASSERT_EQ(t.status.code, NavgenStatus::OK);

	for ( int i = 0; i < maxTiles; i++ )
	{
		const dtCompressedTile *tile = t.tileCache->getTile( i );

		if ( !tile || !tile->header || !tile->dataSize ) {
			continue;
		}

		NavMeshTileHeader tileHeader;
		tileHeader.tileRef = t.tileCache->getTileRef( tile );
		tileHeader.dataSize = tile->dataSize;

		SwapNavMeshTileHeader( tileHeader );
		if ( !Write( &tileHeader, sizeof( tileHeader ) ) ) return;

		std::unique_ptr<unsigned char[]> data( new unsigned char[tile->dataSize] );

		memcpy( data.get(), tile->data, tile->dataSize );
		if ( LittleLong( 1 ) != 1 ) {
			dtTileCacheHeaderSwapEndian( data.get(), tile->dataSize );
		}

		if ( !Write( data.get(), tile->dataSize ) ) return;
	}
	trap_FS_FCloseFile( file );

	// Write the off-mesh connections (ground <-> wall layers) to a .navcon file
	// so that wallwalk-capable agents can route onto the generated wall layers.
	// Only wallwalk-capable classes produce any (see GenerateWallLayers), so
	// non-wallwalk meshes and their navcon files are unaffected.
	if ( t.status.code == NavgenStatus::OK && !t.wallConnections.empty() )
	{
		// Match the text format BotLoadOffMeshConnections expects.
		static const int NAVCON_VERSION = 3;
		static const char *NAVCON_HEADER_PREFIX = "navcon";
		// Endpoint snap radius for the connection (cf. DEFAULT_CONNECTION_SIZE).
		static const float WALLCON_RADIUS = 50.f;

		std::string conName = Str::Format( "maps/%s-%s.navcon", mapName_, BG_Class( t.species )->name );
		qhandle_t conFile;
		trap_FS_FOpenFile( conName.c_str(), &conFile, fsMode_t::FS_WRITE );

		if ( conFile )
		{
			LOG.Notice( "Writing %zu off-mesh connection(s) to %s",
				t.wallConnections.size(), conName.c_str() );

			char buffer[ 256 ];
			int len = snprintf( buffer, sizeof( buffer ), "%s %d\n", NAVCON_HEADER_PREFIX, NAVCON_VERSION );
			trap_FS_Write( buffer, len, conFile );

			for ( const WallConnection &con : t.wallConnections )
			{
				len = snprintf( buffer, sizeof( buffer ),
					"%.0f %.0f %.0f %.0f %.0f %.0f %.0f %d %d %d\n",
					con.start[ 0 ], con.start[ 1 ], con.start[ 2 ],
					con.end[ 0 ], con.end[ 1 ], con.end[ 2 ],
					WALLCON_RADIUS, navPolyFlags::POLYFLAGS_WALLWALK,
					DT_TILECACHE_WALKABLE_AREA, DT_OFFMESH_CON_BIDIR );
				trap_FS_Write( buffer, len, conFile );
			}

			trap_FS_FCloseFile( conFile );
		}
		else
		{
			LOG.Warn( "Failed to open navcon file %s", conName.c_str() );
		}
	}
}

void NavmeshGenerator::LoadBSP()
{
	// copied from beginning of CM_LoadMap
	std::string mapFile = "maps/" + mapName_ + ".bsp";
	mapData_ = FS::PakPath::ReadFile(mapFile);
	dheader_t* header = reinterpret_cast<dheader_t*>(&mapData_[0]);

	// hacky byte swapping for lumps of interest
	// assume all fields size 4 except the string at the beginning of dshader_t
	for (unsigned i = 0; i < sizeof(dheader_t) / 4; i++)
	{
		((int*)header)[i] = LittleLong(((int*)header)[i]);
	}
	for (int lump: {LUMP_MODELS, LUMP_BRUSHES, LUMP_SHADERS, LUMP_BRUSHSIDES, LUMP_PLANES, LUMP_SURFACES, LUMP_DRAWVERTS})
	{
		if ((lump == LUMP_SURFACES || lump == LUMP_DRAWVERTS) && !config_.generatePatchTris)
		{
			continue;
		}
		unsigned nBytes = header->lumps[lump].fileofs;
		char *base = &mapData_[0] + header->lumps[lump].filelen;
		for (unsigned i = 0; i < nBytes; i += 4)
		{
			if (lump == LUMP_SHADERS && i % sizeof(dshader_t) < offsetof(dshader_t, surfaceFlags))
				continue;
			// There is one more exception to 4-byte fields, byte color[4] in drawVert_t, but we don't use it
#define color DO_NOT_USE_BYTE_SWAPPED

			int* p = reinterpret_cast<int*>(base + i);
			*p = LittleLong(*p);
		}
	}
}

//need this to get the windings for brushes
bool FixWinding( winding_t* w );

static void AddVert( std::vector<float> &verts, std::vector<int> &tris, vec3_t vert ) {
	rVec recastVert(VEC2GLM( vert ));
	int index = 0;
	for ( int i = 0; i < 3; i++ ) {
		verts.push_back( recastVert[i] );
	}
	index = ( verts.size() - 3 ) / 3;
	tris.push_back( index );
}

static void AddTri( std::vector<float> &verts, std::vector<int> &tris, vec3_t v1, vec3_t v2, vec3_t v3 ) {
	AddVert( verts, tris, v1 );
	AddVert( verts, tris, v2 );
	AddVert( verts, tris, v3 );
}

static bool SkipContent( const dshader_t *shader ) {
	const int solidFlags = CONTENTS_SOLID | CONTENTS_PLAYERCLIP;

	return !( shader->contentFlags & solidFlags );
}

static bool SkipSurface( const NavgenConfig *config, const dshader_t *shader ) {
	if ( config->excludeSky && shader->surfaceFlags & SURF_SKY )
	{
		return true;
	}

	if ( config->excludeCaulk && !Q_stricmp( shader->shader, "textures/common/caulk" ) )
	{
		return true;
	}

	if ( !Q_stricmp( shader->shader, "textures/common/outside" ) )
	{
		return true;
	}

	return false;
}

// TODO: Can this stuff be done using the already-loaded CM data structures
// (e.g. cbrush_t instead of dbrush_t) instead of reopening the BSP?
void NavmeshGenerator::LoadTris( std::vector<float> &verts, std::vector<int> &tris ) {
	const byte* const cmod_base = reinterpret_cast<const byte*>(mapData_.data());
	auto& header = *reinterpret_cast<const dheader_t*>(mapData_.data());

	// Each one of these lumps should be byte-swapped in LoadBSP
	const lump_t& modelsLump = header.lumps[LUMP_MODELS];
	const lump_t& brushesLump = header.lumps[LUMP_BRUSHES];
	const lump_t& shadersLump = header.lumps[LUMP_SHADERS];
	const lump_t& sidesLump = header.lumps[LUMP_BRUSHSIDES];
	const lump_t& planesLump = header.lumps[LUMP_PLANES];

	/* get model, index 0 is worldspawn entity */
	if ( modelsLump.filelen / sizeof(dmodel_t) <= 0 ) {
		initStatus_ = { NavgenStatus::PERMANENT_FAILURE, "No brushes found" };
		return;
	}
	auto* model = reinterpret_cast<const dmodel_t*>(cmod_base + modelsLump.fileofs);
	auto* bspBrushes = reinterpret_cast<const dbrush_t*>(cmod_base + brushesLump.fileofs);
	auto* bspShaders = reinterpret_cast<const dshader_t*>(cmod_base + shadersLump.fileofs);
	auto* bspBrushSides = reinterpret_cast<const dbrushside_t*>(cmod_base + sidesLump.fileofs);
	auto* bspPlanes = reinterpret_cast<const dplane_t*>(cmod_base + planesLump.fileofs);

	/*
	 * Load brush tris
	 */

	//go through the brushes
	for ( int i = model->firstBrush, m = 0; m < model->numBrushes; i++, m++ ) {
		int numSides = bspBrushes[i].numSides;
		int firstSide = bspBrushes[i].firstSide;
		const dshader_t* brushShader = &bspShaders[bspBrushes[i].shaderNum];

		if ( SkipContent( brushShader ) )
		{
			continue;
		}

		/* walk the list of brush sides */
		for ( int p = 0; p < numSides; p++ )
		{
			/* get side and plane */
			const dbrushside_t *side = &bspBrushSides[p + firstSide];
			const dplane_t *plane = &bspPlanes[side->planeNum];
			const dshader_t* shader = &bspShaders[side->shaderNum];

			if ( SkipSurface( &config_, shader ) )
			{
				continue;
			}

			/* make huge winding */
			winding_t *w = BaseWindingForPlane( plane->normal, plane->dist );

			/* walk the list of brush sides */
			for ( int j = 0; j < numSides && w != nullptr; j++ )
			{
				const dbrushside_t *chopSide = &bspBrushSides[j + firstSide];
				if ( chopSide == side ) {
					continue;
				}
				if ( chopSide->planeNum == ( side->planeNum ^ 1 ) ) {
					continue;       /* back side clipaway */

				}
				const dplane_t *chopPlane = &bspPlanes[chopSide->planeNum ^ 1];

				ChopWindingInPlace( &w, chopPlane->normal, chopPlane->dist, 0 );

				/* ydnar: fix broken windings that would generate trifans */
				FixWinding( w );
			}

			if ( w ) {
				for ( int j = 2; j < w->numpoints; j++ ) {
					AddTri( verts, tris, w->p[0], w->p[j - 1], w->p[j] );
				}

				FreeWinding( w );
			}
		}
	}

	if ( !config_.generatePatchTris )
	{
		return;
	}
	LOG.Debug( "Generated %d brush tris", tris.size() / 3 );

	/*
	 * Load patch tris
	 */
	const lump_t& surfacesLump = header.lumps[LUMP_SURFACES];
	const lump_t& vertsLump = header.lumps[LUMP_DRAWVERTS];

	auto* bspSurfaces = reinterpret_cast<const dsurface_t*>(cmod_base + surfacesLump.fileofs);
	auto* bspVerts = reinterpret_cast<const drawVert_t*>(cmod_base + vertsLump.fileofs);

	/*
	    Patches are not used during the bsp building process where
	    the generated portals are flooded through from all entity positions
	    if even one entity reaches the void, the map will not be compiled
	    Therefore, we can assume that any patch which lies outside the collective
	    bounds of the brushes cannot be reached by entitys
	 */

	// calculate bounds of all verts
	rVec rmins, rmaxs;
	rcCalcBounds( &verts[ 0 ], verts.size() / 3, rmins, rmaxs );

	// convert from recast to quake3 coordinates
	glm::vec3 mins = rmins.ToQuake();
	glm::vec3 maxs = rmaxs.ToQuake();

	for ( int k = model->firstSurface, n = 0; n < model->numSurfaces; k++, n++ )
	{
		// Surface is what is named Patch in NetRadiant editor.
		const dsurface_t *surface = &bspSurfaces[ k ];
		const dshader_t *surfaceShader = &bspShaders[surface->shaderNum];

		/* Patches don't have any content but their shader may
		have contentparm. */
		if ( SkipContent( surfaceShader ) )
		{
			continue;
		}

		if ( SkipSurface( &config_, surfaceShader ) )
		{
			continue;
		}

		if ( surface->surfaceType != mapSurfaceType_t::MST_PATCH ) {
			continue;
		}

		if ( !surface->patchWidth ) {
			continue;
		}

		cGrid_t grid;
		grid.width = surface->patchWidth;
		grid.height = surface->patchHeight;
		grid.wrapHeight = false;
		grid.wrapWidth = false;

		const drawVert_t *curveVerts = &bspVerts[surface->firstVert];

		// make sure the patch intersects the bounds of the brushes
		vec3_t tmin, tmax;
		ClearBounds( tmin, tmax );

		for ( int x = 0; x < grid.width; x++ )
		{
			for ( int y = 0; y < grid.height; y++ )
			{
				AddPointToBounds( curveVerts[ y * grid.width + x ].xyz, tmin, tmax );
			}
		}

		if ( !BoundsIntersect( tmin, tmax, GLM4READ( mins ), GLM4READ( maxs ) ) ) {
			// we can safely ignore this patch surface
			continue;
		}

		for ( int x = 0; x < grid.width; x++ )
		{
			for ( int y = 0; y < grid.height; y++ )
			{
				VectorCopy( curveVerts[ y * grid.width + x ].xyz, grid.points[ x ][ y ] );
			}
		}

		// subdivide the grid
		CM_SetGridWrapWidth( &grid );
		CM_SubdivideGridColumns( &grid );
		CM_RemoveDegenerateColumns( &grid );

		CM_TransposeGrid( &grid );

		CM_SetGridWrapWidth( &grid );
		CM_SubdivideGridColumns( &grid );
		CM_RemoveDegenerateColumns( &grid );

		for ( int x = 0; x < ( grid.width - 1 ); x++ )
		{
			for ( int y = 0; y < ( grid.height - 1 ); y++ )
			{
				/* set indexes */
				float *p1 = grid.points[ x ][ y ];
				float *p2 = grid.points[ x + 1 ][ y ];
				float *p3 = grid.points[ x + 1 ][ y + 1 ];
				AddTri( verts, tris, p1, p2, p3 );

				p1 = grid.points[ x + 1 ][ y + 1 ];
				p2 = grid.points[ x ][ y + 1 ];
				p3 = grid.points[ x ][ y ];
				AddTri( verts, tris, p1, p2, p3 );
			}
		}
	}
}

void NavmeshGenerator::LoadGeometry()
{
	std::vector<float> verts;
	std::vector<int> tris;

	LOG.Debug( "loading geometry..." );
	int numVerts, numTris;

	//count surfaces
	LoadTris( verts, tris );
	if ( initStatus_.code != NavgenStatus::OK ) return;

	numTris = tris.size() / 3;
	numVerts = verts.size() / 3;

	LOG.Debug( "Using %d triangles", numTris );

	geo_.init( &verts[ 0 ], numVerts, &tris[ 0 ], numTris );

	rVec mins = rVec::Load( geo_.getMins() );
	rVec maxs = rVec::Load( geo_.getMaxs() );

	LOG.Debug( "Recast world bounds: min (%.0f %.0f %.0f) max (%.0f %.0f %.0f)",
	           mins[0], mins[1], mins[2], maxs[0], maxs[1], maxs[2] );
}

// Modified version of Recast's rcErodeWalkableArea that uses an AABB instead of a cylindrical radius
static bool rcErodeWalkableAreaByBox( rcContext* ctx, int boxRadius, rcCompactHeightfield& chf ){
	rcAssert( ctx );

	const int w = chf.width;
	const int h = chf.height;

	ctx->startTimer( RC_TIMER_ERODE_AREA );

	unsigned char* dist = (unsigned char*)rcAlloc( sizeof( unsigned char ) * chf.spanCount, RC_ALLOC_TEMP );
	if ( !dist ) {
		ctx->log( RC_LOG_ERROR, "erodeWalkableArea: Out of memory 'dist' (%d).", chf.spanCount );
		return false;
	}

	// Init distance.
	memset( dist, 0xff, sizeof( unsigned char ) * chf.spanCount );

	// Mark boundary cells.
	for ( int y = 0; y < h; ++y )
	{
		for ( int x = 0; x < w; ++x )
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for ( int i = (int)c.index, ni = (int)( c.index + c.count ); i < ni; ++i )
			{
				if ( chf.areas[i] == RC_NULL_AREA ) {
					dist[i] = 0;
				}
				else
				{
					const rcCompactSpan& s = chf.spans[i];
					int nc = 0;
					for ( int dir = 0; dir < 4; ++dir )
					{
						if ( rcGetCon( s, dir ) != RC_NOT_CONNECTED ) {
							const int nx = x + rcGetDirOffsetX( dir );
							const int ny = y + rcGetDirOffsetY( dir );
							const int nidx = (int)chf.cells[nx + ny * w].index + rcGetCon( s, dir );
							if ( chf.areas[nidx] != RC_NULL_AREA ) {
								nc++;
							}
						}
					}
					// At least one missing neighbour.
					if ( nc != 4 ) {
						dist[i] = 0;
					}
				}
			}
		}
	}

	unsigned char nd;

	// Pass 1
	for ( int y = 0; y < h; ++y )
	{
		for ( int x = 0; x < w; ++x )
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for ( int i = (int)c.index, ni = (int)( c.index + c.count ); i < ni; ++i )
			{
				const rcCompactSpan& s = chf.spans[i];

				if ( rcGetCon( s, 0 ) != RC_NOT_CONNECTED ) {
					// (-1,0)
					const int ax = x + rcGetDirOffsetX( 0 );
					const int ay = y + rcGetDirOffsetY( 0 );
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon( s, 0 );
					const rcCompactSpan& as = chf.spans[ai];
					nd = (unsigned char)rcMin( (int)dist[ai] + 2, 255 );
					if ( nd < dist[i] ) {
						dist[i] = nd;
					}

					// (-1,-1)
					if ( rcGetCon( as, 3 ) != RC_NOT_CONNECTED ) {
						const int aax = ax + rcGetDirOffsetX( 3 );
						const int aay = ay + rcGetDirOffsetY( 3 );
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon( as, 3 );
						nd = (unsigned char)rcMin( (int)dist[aai] + 2, 255 );
						if ( nd < dist[i] ) {
							dist[i] = nd;
						}
					}
				}
				if ( rcGetCon( s, 3 ) != RC_NOT_CONNECTED ) {
					// (0,-1)
					const int ax = x + rcGetDirOffsetX( 3 );
					const int ay = y + rcGetDirOffsetY( 3 );
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon( s, 3 );
					const rcCompactSpan& as = chf.spans[ai];
					nd = (unsigned char)rcMin( (int)dist[ai] + 2, 255 );
					if ( nd < dist[i] ) {
						dist[i] = nd;
					}

					// (1,-1)
					if ( rcGetCon( as, 2 ) != RC_NOT_CONNECTED ) {
						const int aax = ax + rcGetDirOffsetX( 2 );
						const int aay = ay + rcGetDirOffsetY( 2 );
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon( as, 2 );
						nd = (unsigned char)rcMin( (int)dist[aai] + 2, 255 );
						if ( nd < dist[i] ) {
							dist[i] = nd;
						}
					}
				}
			}
		}
	}

	// Pass 2
	for ( int y = h - 1; y >= 0; --y )
	{
		for ( int x = w - 1; x >= 0; --x )
		{
			const rcCompactCell& c = chf.cells[x + y * w];
			for ( int i = (int)c.index, ni = (int)( c.index + c.count ); i < ni; ++i )
			{
				const rcCompactSpan& s = chf.spans[i];

				if ( rcGetCon( s, 2 ) != RC_NOT_CONNECTED ) {
					// (1,0)
					const int ax = x + rcGetDirOffsetX( 2 );
					const int ay = y + rcGetDirOffsetY( 2 );
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon( s, 2 );
					const rcCompactSpan& as = chf.spans[ai];
					nd = (unsigned char)rcMin( (int)dist[ai] + 2, 255 );
					if ( nd < dist[i] ) {
						dist[i] = nd;
					}

					// (1,1)
					if ( rcGetCon( as, 1 ) != RC_NOT_CONNECTED ) {
						const int aax = ax + rcGetDirOffsetX( 1 );
						const int aay = ay + rcGetDirOffsetY( 1 );
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon( as, 1 );
						nd = (unsigned char)rcMin( (int)dist[aai] + 2, 255 );
						if ( nd < dist[i] ) {
							dist[i] = nd;
						}
					}
				}
				if ( rcGetCon( s, 1 ) != RC_NOT_CONNECTED ) {
					// (0,1)
					const int ax = x + rcGetDirOffsetX( 1 );
					const int ay = y + rcGetDirOffsetY( 1 );
					const int ai = (int)chf.cells[ax + ay * w].index + rcGetCon( s, 1 );
					const rcCompactSpan& as = chf.spans[ai];
					nd = (unsigned char)rcMin( (int)dist[ai] + 2, 255 );
					if ( nd < dist[i] ) {
						dist[i] = nd;
					}

					// (-1,1)
					if ( rcGetCon( as, 0 ) != RC_NOT_CONNECTED ) {
						const int aax = ax + rcGetDirOffsetX( 0 );
						const int aay = ay + rcGetDirOffsetY( 0 );
						const int aai = (int)chf.cells[aax + aay * w].index + rcGetCon( as, 0 );
						nd = (unsigned char)rcMin( (int)dist[aai] + 2, 255 );
						if ( nd < dist[i] ) {
							dist[i] = nd;
						}
					}
				}
			}
		}
	}

	const unsigned char thr = (unsigned char)( boxRadius * 2 );
	for ( int i = 0; i < chf.spanCount; ++i ) {
		if ( dist[i] < thr ) {
			chf.areas[i] = RC_NULL_AREA;
		}
	}

	rcFree( dist );

	ctx->stopTimer( RC_TIMER_ERODE_AREA );

	return true;
}

/*
   rcFilterGaps

   Does a super simple sampling of ledges to detect and fix
   any "gaps" in the heightfield that are narrow enough for us to walk over
   because of our AABB based collision system
 */
static void rcFilterGaps( rcContext *ctx, int walkableRadius, int walkableClimb, int walkableHeight, rcHeightfield &solid ) {
	const int h = solid.height;
	const int w = solid.width;
	const int MAX_HEIGHT = 0xffff;
	std::vector<int> spanData;
	spanData.reserve( 500 * 3 );
	std::vector<int> data;
	data.reserve( ( walkableRadius * 2 - 1 ) * 3 );

	//for every span in the heightfield
	for ( int y = 0; y < h; ++y ) {
		for ( int x = 0; x < w; ++x ) {
			//check each span in the column
			for ( rcSpan *s = solid.spans[x + y * w]; s; s = s->next ) {
				//bottom and top of the "base" span
				const int sbot = s->smax;

				//if the span is walkable
				if ( s->area != RC_NULL_AREA ) {
					//check all neighbor connections
					for ( int dir = 0; dir < 4; dir++ ) {
						const int dirx = rcGetDirOffsetX( dir );
						const int diry = rcGetDirOffsetY( dir );
						int dx = x;
						int dy = y;
						bool freeSpace = false;
						bool stop = false;

						if ( dx < 0 || dy < 0 || dx >= w || dy >= h ) {
							continue;
						}

						//keep going the direction for walkableRadius * 2 - 1 spans
						//because we can walk as long as at least part of our bbox is on a solid surface
						for ( int i = 1; i < walkableRadius * 2; i++ ) {
							dx = dx + dirx;
							dy = dy + diry;
							if ( dx < 0 || dy < 0 || dx >= w || dy >= h ) {
								i--;
								freeSpace = false;
								stop = false;
								break;
							}

							//tells if there is space here for us to stand
							freeSpace = false;

							//go through the column
							for ( rcSpan *ns = solid.spans[dx + dy * w]; ns; ns = ns->next ) {
								int nsbot = ns->smax;
								int nstop = ( ns->next ) ? ( ns->next->smin ) : MAX_HEIGHT;

								//if there is a span within walkableClimb of the base span, we have reached the end of the gap (if any)
								if ( rcAbs( sbot - nsbot ) <= walkableClimb && ns->area != RC_NULL_AREA ) {
									//set flag telling us to stop
									stop = true;

									//only add spans if we have gone for more than 1 iteration
									//if we stop at the first iteration, it means there was no gap to begin with
									if ( i > 1 ) {
										freeSpace = true;
									}
									break;
								}

								if ( nsbot < sbot && nstop >= sbot + walkableHeight ) {
									//tell that we found walkable space within reach of the previous span
									freeSpace = true;
									//add this span to a temporary storage location
									data.push_back( dx );
									data.push_back( dy );
									data.push_back( sbot );
									break;
								}
							}

							//stop if there is no more freespace, or we have reached end of gap
							if ( stop || !freeSpace ) {
								break;
							}
						}
						//move the spans from the temp location to the storage
						//we check freeSpace to make sure we don't add a
						//span when we stop at the first iteration (because there is no gap)
						//checking stop tells us if there was a span to complete the "bridge"
						if ( freeSpace && stop ) {
							const int N = data.size();
							for ( int i = 0; i < N; i++ )
								spanData.push_back( data[i] );
						}
						data.clear();
					}
				}
			}
		}
	}

	//add the new spans
	//we cant do this in the loop, because the loop would then iterate over those added spans
	for ( std::vector<int>::size_type i = 0; i < spanData.size(); i += 3 ) {
		rcAddSpan( ctx, solid, spanData[i], spanData[i + 1], spanData[i + 2] - 1, spanData[i + 2], RC_WALKABLE_AREA, walkableClimb );
	}
}

// Most Recast error returns here are translated as transient failures because inspection of the source shows
// that the only failure mode is insufficient memory
static NavgenStatus rasterizeTileLayers( Geometry& geo, rcContext &context, int tx, int ty, const rcConfig &mcfg,
                                         TileCacheData *data, int maxLayers, bool filterGaps, int *ntiles )
{
	rcConfig cfg;

	FastLZCompressor comp;
	RasterizationContext rc;

	const float tcs = mcfg.tileSize * mcfg.cs;

	cfg = mcfg;

	// find tile bounds
	// easy optimisation here: avoid recalculating things Y * X times
	cfg.bmin[ 0 ] = mcfg.bmin[ 0 ] + tx * tcs;
	cfg.bmin[ 1 ] = mcfg.bmin[ 1 ];
	cfg.bmin[ 2 ] = mcfg.bmin[ 2 ] + ty * tcs;

	cfg.bmax[ 0 ] = mcfg.bmin[ 0 ] + ( tx + 1 ) * tcs;
	cfg.bmax[ 1 ] = mcfg.bmax[ 1 ];
	cfg.bmax[ 2 ] = mcfg.bmin[ 2 ] + ( ty + 1 ) * tcs;

	// expand bounds by border size
	// easy optimisation here: borderSize and cs (cs: The xz-plane cell size to use for fields)
	// are constants (coming directly from mcfg, previously named cfg, you follow?).
	cfg.bmin[ 0 ] -= cfg.borderSize * cfg.cs;
	cfg.bmin[ 2 ] -= cfg.borderSize * cfg.cs;

	cfg.bmax[ 0 ] += cfg.borderSize * cfg.cs;
	cfg.bmax[ 2 ] += cfg.borderSize * cfg.cs;

	rc.solid = rcAllocHeightfield();

	if ( !rcCreateHeightfield( &context, *rc.solid, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch ) ) {
		return { NavgenStatus::TRANSIENT_FAILURE, "Failed to create heightfield for navigation mesh" };
	}

	//I understand that using std::vector prevents NiH's proudness.
	const float *verts = geo.getVerts();
	const int nverts = geo.getNumVerts();
	const rcChunkyTriMesh *chunkyMesh = geo.getChunkyMesh();

	rc.triareas = new unsigned char[ chunkyMesh->maxTrisPerChunk ];

	float tbmin[ 2 ], tbmax[ 2 ];

	tbmin[ 0 ] = cfg.bmin[ 0 ];
	tbmin[ 1 ] = cfg.bmin[ 2 ];
	tbmax[ 0 ] = cfg.bmax[ 0 ];
	tbmax[ 1 ] = cfg.bmax[ 2 ];

	int *cid = new int[ chunkyMesh->nnodes ];

	//do not search for this in libraries, you won't find it. It's in RecastDemo's code.
	//It " Creates partitioned triangle mesh (AABB tree), where each node contains at max trisPerChunk triangles."
	//why? No idea.
	const int ncid = rcGetChunksOverlappingRect( chunkyMesh, tbmin, tbmax, cid, chunkyMesh->nnodes );
	if ( !ncid ) {
		delete[] cid;
		*ntiles = 0;
		return {};
	}

	for ( int i = 0; i < ncid; i++ )
	{
		const rcChunkyTriMeshNode &node = chunkyMesh->nodes[ cid[ i ] ];
		const int *tris = &chunkyMesh->tris[ node.i * 3 ];
		const int ntris = node.n;

		memset( rc.triareas, 0, ntris * sizeof( unsigned char ) );

		rcMarkWalkableTriangles( &context, cfg.walkableSlopeAngle, verts, nverts, tris, ntris, rc.triareas );
		rcRasterizeTriangles( &context, verts, nverts, tris, rc.triareas, ntris, *rc.solid, cfg.walkableClimb );
	}

	delete[] cid;

	//makes them walkable (unlike the other filters, would probably kill some people to write meaningful code)
	rcFilterLowHangingWalkableObstacles( &context, cfg.walkableClimb, *rc.solid );

	//dont filter ledge spans since characters CAN walk on ledges due to using a bbox for movement collision
	//makes them un-walkable
	//rcFilterLedgeSpans (&context, cfg.walkableHeight, cfg.walkableClimb, *rc.solid);

	//makes them un-walkable
	rcFilterWalkableLowHeightSpans( &context, cfg.walkableHeight, *rc.solid );

	// Make gaps walkable: as clients have cubic BBox, they can walk a bit
	// over a ledge as long as a part of their bbox stays over ground.
	//
	// This piece of code seems to handle thin walls correctly, and only
	// marks voids walkable.
	if ( filterGaps ) {
		rcFilterGaps( &context, cfg.walkableRadius, cfg.walkableClimb, cfg.walkableHeight, *rc.solid );
	}

	rc.chf = rcAllocCompactHeightfield();

	if ( !rcBuildCompactHeightfield( &context, cfg.walkableHeight, cfg.walkableClimb, *rc.solid, *rc.chf ) ) {
		return { NavgenStatus::TRANSIENT_FAILURE, "Failed to create compact heightfield for navigation mesh" };
	}

	if ( !rcErodeWalkableAreaByBox( &context, cfg.walkableRadius, *rc.chf ) ) {
		return { NavgenStatus::TRANSIENT_FAILURE, "Unable to erode walkable surfaces" };
	}

	rc.lset = rcAllocHeightfieldLayerSet();

	if ( !rc.lset ) {
		return { NavgenStatus::TRANSIENT_FAILURE, "Out of memory heightfield layer set" };
	}

	if ( !rcBuildHeightfieldLayers( &context, *rc.chf, cfg.borderSize, cfg.walkableHeight, *rc.lset ) ) {
		// This could be an out-of-memory error or a real algorithm failure
		return { NavgenStatus::PERMANENT_FAILURE, "Could not build heightfield layers" };
	}

	rc.ntiles = 0;

	for ( int i = 0; i < rcMin( rc.lset->nlayers, MAX_LAYERS ); i++ )
	{
		TileCacheData *tile = &rc.tiles[ rc.ntiles++ ];
		const rcHeightfieldLayer *layer = &rc.lset->layers[ i ];

		dtTileCacheLayerHeader header;
		header.magic = DT_TILECACHE_MAGIC;
		header.version = DT_TILECACHE_VERSION;

		header.tx = tx;
		header.ty = ty;
		header.tlayer = i;
		dtVcopy( header.bmin, layer->bmin );
		dtVcopy( header.bmax, layer->bmax );

		header.width = ( unsigned char ) layer->width;
		header.height = ( unsigned char ) layer->height;
		header.minx = ( unsigned char ) layer->minx;
		header.maxx = ( unsigned char ) layer->maxx;
		header.miny = ( unsigned char ) layer->miny;
		header.maxy = ( unsigned char ) layer->maxy;
		header.hmin = ( unsigned short ) layer->hmin;
		header.hmax = ( unsigned short ) layer->hmax;

		dtStatus status = dtBuildTileCacheLayer( &comp, &header, layer->heights, layer->areas, layer->cons, &tile->data, &tile->dataSize );

		if ( dtStatusFailed( status ) ) {
			return { CodeForFailedDtStatus( status ), "dtBuildTileCacheLayer failed" };
		}
	}

	// transfer tile data over to caller
	int n = 0;
	for ( int i = 0; i < rcMin( rc.ntiles, maxLayers ); i++ )
	{
		data[ n++ ] = rc.tiles[ i ];
		rc.tiles[ i ].data = 0;
		rc.tiles[ i ].dataSize = 0;
	}

	*ntiles = n;
	return {};
}

void NavmeshGenerator::LoadMapAndEnqueueTasks(
	Str::StringRef mapName, std::bitset<PCL_NUM_CLASSES> classes )
{
	// The NavmeshGenerator object is not designed to be used more than once
	ASSERT( mapName_.empty() );

	// never divide by 0
	// This fails to include map loading time but we don't update progress during that anyway
	int amountOfWork = 1;

	if ( classes.any() )
	{
		LoadMap( mapName );
		std::string names;
		for ( int i = PCL_NUM_CLASSES; --i != PCL_NONE; )
		{
			if ( !classes[ i ] )
			{
				continue;
			}

			taskQueue_.push_back( StartGeneration( Util::enum_cast<class_t>( i ) ) );
			const NavgenTask& task = *taskQueue_.back();
			if ( task.status.code == NavgenStatus::OK )
			{
				amountOfWork += task.tw * task.th; // This correlates pretty well with how long it takes
			}

			names = ' ' + ( BG_Class(i)->name + names );
		}
		LOG.Notice("Navgen requested for:%s", names);
	}
	else
	{
		LOG.Notice( "Nothing requested for navgen" );
	}

	fractionCompleteDenominator_ = amountOfWork;
}

std::unique_ptr<NavgenTask> NavmeshGenerator::StartGeneration( class_t species )
{
	classAttributes_t const& agent = *BG_Class( species );
	auto t = Util::make_unique<NavgenTask>();
	t->species = species;
	t->status = initStatus_;
	if ( t->status.code != NavgenStatus::OK )
	{
		return t;
	}

	const float *bmin = geo_.getMins();
	const float *bmax = geo_.getMaxs();
	int gw = 0, gh = 0;

	const classModelConfig_t* dimensions = BG_ClassModelConfig(species);
	//radius of agent (BBox maxs[0] or BBox maxs[1])
	float radius = dimensions->maxs[0];
	//height of agent (BBox maxs[2] - BBox mins[2])
	float height = ( config_.crouchSupport ? dimensions->crouchMaxs[2] : dimensions->maxs[2] ) - dimensions->mins[2];

	const float cellSize = radius * config_.cellSizeFactor;

	rcCalcGridSize( bmin, bmax, cellSize, &gw, &gh );

	const int ts = tileSize;
	t->tw = ( gw + ts - 1 ) / ts;
	t->th = ( gh + ts - 1 ) / ts;

	float climb = config_.stepSize;
	if ( config_.autojumpSecurity > 0.f )
	{
		glm::vec3 mins, maxs;
		BG_BoundingBox( species, &mins, &maxs, nullptr, nullptr, nullptr );
		float safety = ( maxs.z - mins.z ) * config_.autojumpSecurity;
		//FIXME use g_gravity
		float jump = Square( agent.jumpMagnitude ) / 1600; //( g_gravity.Get() * 2 );

		climb = std::min( safety, climb + jump );

		// keep the STEPSIZE as minimum, just in case
		climb = std::max( config_.stepSize, climb );
	}

	// We are actually still running on the main thread, but put the message in there
	// so that it will be grouped with other messages about the same class
	std::string msg = Str::Format( "generating agent %s with stepsize of %d, grid %dx%d", agent.name, climb, t->tw, t->th );
	t->context.RunOnMainThread( [msg] { LOG.Verbose( msg ); } );

	t->cfg.cs = cellSize;
	t->cfg.ch = cellHeight_;
	t->cfg.walkableSlopeAngle = RAD2DEG( acosf( MIN_WALK_NORMAL ) );
	t->cfg.walkableHeight = ( int ) ceilf( height / t->cfg.ch );
	t->cfg.walkableClimb = ( int ) floorf( climb / t->cfg.ch );
	t->cfg.walkableRadius = ( int ) ceilf( radius * config_.walkableRadiusFactor / t->cfg.cs );
	t->cfg.maxEdgeLen = 0;
	t->cfg.maxSimplificationError = 1.3f;
	t->cfg.minRegionArea = rcSqr( 25 );
	t->cfg.mergeRegionArea = rcSqr( 50 );
	t->cfg.maxVertsPerPoly = 6;
	t->cfg.tileSize = ts;
	t->cfg.borderSize = t->cfg.walkableRadius * 2;
	t->cfg.width = t->cfg.tileSize + t->cfg.borderSize * 2;
	t->cfg.height = t->cfg.tileSize + t->cfg.borderSize * 2;
	t->cfg.detailSampleDist = t->cfg.cs * 6.0f;
	t->cfg.detailSampleMaxError = t->cfg.ch * 1.0f;

	rcVcopy( t->cfg.bmin, bmin );
	rcVcopy( t->cfg.bmax, bmax );

	rcVcopy( t->tcparams.orig, bmin );
	t->tcparams.cs = cellSize;
	t->tcparams.ch = cellHeight_;
	t->tcparams.width = ts;
	t->tcparams.height = ts;
	t->tcparams.walkableHeight = height;
	t->tcparams.walkableRadius = radius * config_.walkableRadiusFactor;
	t->tcparams.walkableClimb = climb;
	t->tcparams.maxSimplificationError = 1.3;
	t->tcparams.maxTiles = t->tw * t->th * EXPECTED_LAYERS_PER_TILE;
	// Keep extra tile-cache headroom even though wallwalk no longer rasterizes
	// rotated layers into the cache (those were dropped as unroutable; see
	// GenerateWallLayers): it is harmless capacity and spares a change if a future
	// revision ever adds walkable wall polys back.
	if ( IsWallwalkSupported() )
	{
		t->tcparams.maxTiles *= 5;
	}
	t->tcparams.maxObstacles = 256;

	t->tileCache.reset(new dtTileCache);
	dtStatus status = t->tileCache->init( &t->tcparams, &t->alloc, &t->comp, &t->proc );

	if ( dtStatusFailed( status ) ) {
		std::string message = dtStatusDetail( status, DT_INVALID_PARAM ) ? "Could not init tile cache: Invalid parameter" : "Could not init tile cache";
		t->status = { CodeForFailedDtStatus( status ), message };
	}

	return t;
}

// May be called from a worker thread, thus must not use trap calls.
std::unique_ptr<NavgenTask> NavmeshGenerator::PopTask()
{
	if ( taskQueue_.empty() )
	{
		return nullptr;
	}

	auto ret = std::move( taskQueue_.back() );
	taskQueue_.pop_back();
	return ret;
}

float NavmeshGenerator::FractionComplete() const
{
	return float( fractionCompleteNumerator_ ) / float( fractionCompleteDenominator_ );
}

bool NavmeshGenerator::IsWallwalkSupported() const
{
	return config_.wallwalkOrientationMask != 0;
}

// Emits off-mesh connection markers at the base of every on-axis wall/ceiling of
// the map, tagged POLYFLAGS_WALLWALK, so that a wallwalk-capable class knows it
// can climb from its current spot toward a target that is above it (see
// G_BotNearWallConnection and BotWallwalkClimbTowardGoal in sg_bot_nav.cpp).
// It is a no-op unless wallwalk was requested via navcfg
// (wallwalkOrientationMask != 0), so the default navigation mesh (and its
// navcon) is unchanged. No rotated tiles are rasterized: a recast tile cache
// has a single global "up" vector, so a wall rotated to lie flat cannot be
// routed onto (see the body for the full rationale).
void NavmeshGenerator::GenerateWallLayers( NavgenTask &t )
{
	if ( !IsWallwalkSupported() || t.status.code != NavgenStatus::OK )
	{
		return;
	}

	// Only wallwalk-capable classes produce the bridge markers (their
	// POLYFLAGS_WALLWALK flag is filtered out for everyone else anyway), so skip
	// the pass for other classes to keep their navcon empty/unchanged. We no
	// longer rasterize rotated wall/ceiling tiles into the shared cache: a recast
	// tile cache assumes a single global "up" vector, so a wall rotated to lie
	// flat lands on the ground cache's grid at a position that does not match its
	// real world location and findNearestPoly() of a world position never finds
	// it, so routing a bot ONTO such a tile does not work. Instead we emit off-mesh
	// connection markers at the base of each wall and let a wallwalk-capable bot
	// climb toward a target that is above it (see G_BotNearWallConnection and
	// BotWallwalkClimbTowardGoal in sg_bot_nav.cpp).
	if ( !BG_ClassHasAbility( t.species, SCA_WALLCLIMBER ) )
	{
		return;
	}

	const float *worldVerts = geo_.getVerts();
	const int worldNumVerts = geo_.getNumVerts();
	const rcChunkyTriMesh *worldMesh = geo_.getChunkyMesh();
	if ( !worldVerts || worldNumVerts == 0 || !worldMesh || worldMesh->ntris == 0 )
	{
		return;
	}

	// A wall/ceiling triangle counts as "on axis" for this pass when its normal
	// points roughly along one of the enabled wall directions. The squared test
	// (a component exceeds half the normal's length, i.e. steeper than 60 deg
	// from that axis) rejects diagonal surfaces and avoids a sqrt.
	const int mask = config_.wallwalkOrientationMask;
	const float floorY = geo_.getMins()[ 1 ];
	// Marker grid spacing in quake units. A finer grid places a base-of-wall
	// marker closer to any point on a wall so a wallwalk-capable bot standing
	// anywhere at a wall base finds one within g_bot_wallwalkClimbRange. The cap
	// (MAX_WALL_CON) is the only reason the grid can't go arbitrarily fine: a large
	// map with more cells than the budget has the surplus sampled out (below).
	const float grid = 128.f;

	// Cluster on-axis wall triangles into world XZ grid cells so the connection
	// markers are spread across the whole map. The previous code stride-sampled
	// the raw triangle list, which scattered (and often clustered) the markers and
	// left large stretches of wall uncovered, so a bot standing at a wall base
	// could find no nearby marker and never climb. Each non-empty cell contributes
	// at most one marker at that wall's base.
	struct WallCluster { float minY, maxY; };
	std::unordered_map< std::uint64_t, WallCluster > clusters;

	for ( int ti = 0; ti < worldMesh->ntris; ++ti )
	{
		const int i0 = worldMesh->tris[ ti * 3 + 0 ];
		const int i1 = worldMesh->tris[ ti * 3 + 1 ];
		const int i2 = worldMesh->tris[ ti * 3 + 2 ];

		const float ax = worldVerts[ i0 * 3 + 0 ], ay = worldVerts[ i0 * 3 + 1 ], az = worldVerts[ i0 * 3 + 2 ];
		const float bx = worldVerts[ i1 * 3 + 0 ], by = worldVerts[ i1 * 3 + 1 ], bz = worldVerts[ i1 * 3 + 2 ];
		const float cx = worldVerts[ i2 * 3 + 0 ], cy = worldVerts[ i2 * 3 + 1 ], cz = worldVerts[ i2 * 3 + 2 ];

		const float e1x = bx - ax, e1y = by - ay, e1z = bz - az;
		const float e2x = cx - ax, e2y = cy - ay, e2z = cz - az;
		const float nx = e1y * e2z - e1z * e2y;
		const float ny = e1z * e2x - e1x * e2z;
		const float nz = e1x * e2y - e1y * e2x;
		const float n2 = nx * nx + ny * ny + nz * nz;
		if ( n2 < 1e-6f )
		{
			continue; // degenerate triangle
		}

		bool onAxis = false;
		if ( mask & navPolyOrientations::NAV_WALL_XPOS && nx > 0.f && nx * nx > 0.25f * n2 )
			onAxis = true;
		else if ( mask & navPolyOrientations::NAV_WALL_XNEG && nx < 0.f && nx * nx > 0.25f * n2 )
			onAxis = true;
		else if ( mask & navPolyOrientations::NAV_WALL_YPOS && ny > 0.f && ny * ny > 0.25f * n2 )
			onAxis = true;
		else if ( mask & navPolyOrientations::NAV_WALL_YNEG && ny < 0.f && ny * ny > 0.25f * n2 )
			onAxis = true;

		if ( !onAxis )
		{
			continue;
		}

		// World-frame centroid (X/Z place it horizontally, Y is up).
		const float px = ( ax + bx + cx ) / 3.f;
		const float py = ( ay + by + cy ) / 3.f;
		const float pz = ( az + bz + cz ) / 3.f;

		// Bucket by world XZ; one cluster per cell tracks the wall's vertical
		// extent so the marker can aim up the top of the wall.
		const int gx = int( floorf( px / grid ) );
		const int gz = int( floorf( pz / grid ) );
		const std::uint64_t key = ( ( std::uint64_t ) static_cast<std::uint32_t>( gx ) << 32 )
			| static_cast<std::uint32_t>( gz );

		auto it = clusters.find( key );
		if ( it == clusters.end() )
		{
			clusters.emplace( key, WallCluster{ py, py } );
		}
		else
		{
			WallCluster &c = it->second;
			if ( py < c.minY ) c.minY = py;
			if ( py > c.maxY ) c.maxY = py;
		}
	}

	// Flatten the clusters and cap at MAX_WALL_CON, sampling evenly across the
	// grid when there are more cells than the budget (the mesh's off-mesh-
	// connection storage cap, OffMeshConnections::MAX_CON) so the markers stay
	// spread over the map instead of clustering in the early cells.
	struct Cell { int gx, gz; float minY, maxY; };
	std::vector< Cell > cells;
	cells.reserve( clusters.size() );
	for ( auto &kv : clusters )
	{
		Cell c;
		c.gx = static_cast<int32_t>( kv.first >> 32 );
		c.gz = static_cast<int32_t>( kv.first & 0xffffffffu );
		c.minY = kv.second.minY;
		c.maxY = kv.second.maxY;
		cells.push_back( c );
	}
	std::sort( cells.begin(), cells.end(), []( const Cell &a, const Cell &b ) {
		return a.gx != b.gx ? a.gx < b.gx : a.gz < b.gz;
	} );

	const int total = static_cast<int>( cells.size() );
	int stride = 1;
	if ( total > MAX_WALL_CON )
	{
		stride = ( total + MAX_WALL_CON - 1 ) / MAX_WALL_CON;
	}

	int added = 0;
	for ( int i = 0; i < total; i += stride )
	{
		const Cell &c = cells[ i ];

		// The marker sits at the wall base (dropped to the map floor, at the
		// cell's centre X/Z) and points up the wall's top. The proximity test
		// only reads the start, so a bot detects it is "at a wall"; the end is
		// where a future route-to-wall could aim.
		const float px = c.gx * grid + 0.5f * grid;
		const float pz = c.gz * grid + 0.5f * grid;

		WallConnection con;
		con.start[ 0 ] = px;
		con.start[ 1 ] = floorY;
		con.start[ 2 ] = pz;
		con.end[ 0 ] = px;
		con.end[ 1 ] = c.maxY;
		con.end[ 2 ] = pz;
		t.wallConnections.push_back( con );
		++added;
	}

	if ( added > 0 )
	{
		t.context.RunOnMainThread( [added, total] {
			LOG.Notice( "Wallwalk: %d wall cell(s) clustered into %d off-mesh connection marker(s)", total, added );
		} );
	}
}

// May be called from a worker thread, thus must not use trap calls.
bool NavmeshGenerator::Step( NavgenTask &t )
{
	if ( t.status.code != NavgenStatus::OK || t.y >= t.th || t.tw == 0 )
	{
		// Add the wall/ceiling layers after all ground tiles so that, should the
		// shared tile cache ever overflow, the ground navigation mesh keeps
		// priority. No-op unless wallwalk was requested via navcfg.
		if ( IsWallwalkSupported() && !t.wallLayersGenerated )
		{
			GenerateWallLayers( t );
			t.wallLayersGenerated = true;
		}
		t.context.RunOnMainThread( [this, &t] { WriteFile( t ); } );
		return true;
	}

	TileCacheData tiles[ MAX_LAYERS ]{};

	int ntiles;
	NavgenStatus status = rasterizeTileLayers(geo_, t.context, t.x, t.y, t.cfg, tiles, MAX_LAYERS, !!config_.filterGaps, &ntiles);
	if ( status.code != NavgenStatus::OK )
	{
		t.status = status;
		return false;
	}

	for ( int i = 0; i < ntiles; i++ )
	{
		TileCacheData *tile = &tiles[ i ];
		dtStatus tileStatus = t.tileCache->addTile( tile->data, tile->dataSize, DT_COMPRESSEDTILE_FREE_DATA, 0 );
		if ( dtStatusFailed( tileStatus ) ) {
			dtFree( tile->data );
			tile->data = 0;
			continue;
		}
	}

	//iterate over all tiles (number is determined by rcCalcGridSize)
	if ( ++t.x == t.tw )
	{
		t.x = 0;
		++t.y;
	}

	++fractionCompleteNumerator_;
	return false;
}

void NavmeshGenerator::LoadMap(Str::StringRef mapName)
{
	config_ = ReadNavgenConfig( mapName );
	mapName_ = mapName;
	initStatus_ = {};
	LoadBSP();
	LoadGeometry();
	if ( initStatus_.code != NavgenStatus::OK ) return;

	cellHeight_ = config_.requestedCellHeight;
	float height = rcAbs( geo_.getMaxs()[1] ) + rcAbs( geo_.getMins()[1] );
	if ( height / cellHeight_ > RC_SPAN_MAX_HEIGHT )
	{
		LOG.Warn("Map geometry is too tall for specified cell height. Increasing cell height to compensate. This may cause a less accurate navmesh.");
		cellHeight_ = height / RC_SPAN_MAX_HEIGHT;
	}

	if ( cellHeight_ > config_.stepSize )
	{
		initStatus_ = { NavgenStatus::PERMANENT_FAILURE, "Map is too tall to generate a navigation mesh, cell height can't be greater than the step size" };
	}
}

void NavmeshGenerator::StartBackgroundThreads( int numBackgroundThreads )
{
	numBackgroundThreads = std::max( numBackgroundThreads, 1 );
	numBackgroundThreads = std::min( numBackgroundThreads, static_cast<int>( taskQueue_.size() ) );
	LOG.Notice( "Using %d worker thread(s) for navmesh generation", numBackgroundThreads );
	numActiveThreads_ = numBackgroundThreads;

	for ( ; numBackgroundThreads > 0; numBackgroundThreads-- )
	{
		threads_.emplace_back( &NavmeshGenerator::BackgroundThreadMain, this );
	}
}

void NavmeshGenerator::BackgroundThreadMain()
{
	std::unique_lock<std::mutex> lock(taskQueueMutex_);

	while ( true )
	{
		std::unique_ptr<NavgenTask> task;
		task = PopTask();

		if ( !task )
		{
			--numActiveThreads_;
			return;
		}

		lock.unlock();
		int start = Sys::Milliseconds();

		do
		{
			if ( canceled_ )
			{
				return;
			}
		}
		while ( !Step( *task ) );

		std::string msg = Str::Format( "Navgen for %s took %d ms",
			BG_Class( task->species )->name, Sys::Milliseconds() - start );
		task->context.RunOnMainThread( [msg] { LOG.Verbose( msg ); } );
		lock.lock();
		finishedTasks_.push_back( std::move( task ) );
	}

	--numActiveThreads_;
}

void NavmeshGenerator::WaitInMainThread( std::function<void(float)> progressCallback )
{
	while ( !ThreadsDone() )
	{
		bool somethingFinished = HandleFinishedTasks();
		progressCallback( FractionComplete() );
		if ( !somethingFinished )
		{
			Cvar::GetValue( "x" ); // prevent being killed by engine after 2 seconds of idleness
			Sys::SleepFor( std::chrono::milliseconds( 300 ) ); // TODO std::condition_variable?
		}
	}

	HandleFinishedTasks();
}

bool NavmeshGenerator::ThreadsDone()
{
	std::lock_guard<std::mutex> lock(taskQueueMutex_);
	return numActiveThreads_ == 0;
}

// returns true if there were any finished
bool NavmeshGenerator::HandleFinishedTasks()
{
	std::vector<std::unique_ptr<NavgenTask>> finished;
	{
		std::lock_guard<std::mutex> lock(taskQueueMutex_);
		std::swap( finished, finishedTasks_ );
	}

	for ( auto &task : finished )
	{
		task->context.DoMainThreadTasks();
	}

	return !finished.empty();
}

NavmeshGenerator::~NavmeshGenerator()
{
	canceled_ = true;
	for ( std::thread &thread : threads_ )
	{
		thread.join();
	}
}

#ifdef BUILD_SGAME // TODO move this somewhere else?
class LadderCmd : public Cmd::StaticCmd
{
public:
	LadderCmd() : StaticCmd("printLadders", "print coordinates of ladders in navcon format") {}

	void Run( const Cmd::Args& ) const override
	{
		// This iterates all brushes included ones attached to movers (e.g. the ladder that
		// goes down when a button is pressed on Chasm)
		for ( int b = 0; b < cm.numBrushes; b++ )
		{
			const cbrush_t &brush = cm.brushes[ b ];
			std::vector<glm::vec3> ladderVerts;

			for ( int s = 0; s < brush.numsides; s++ )
			{
				if ( !( brush.sides[ s ].surfaceFlags & SURF_LADDER ) )
				{
					continue;
				}

				winding_t *w = GetWinding( brush, s );

				if ( w )
				{
					for ( int i = 0; i < w->numpoints; i++ )
					{
						ladderVerts.push_back( VEC2GLM( w->p[ i ] ) );
					}

					FreeWinding( w );
				}
			}

			if ( !ladderVerts.empty() )
			{
				EmitLadder( ladderVerts );
			}
		}
	}

private:
	static winding_t *GetWinding( const cbrush_t &brush, int s )
	{
		const cplane_t &plane = *brush.sides[ s ].plane;
		winding_t *w = BaseWindingForPlane( plane.normal, plane.dist );
		for ( int s2 = 0; s2 < brush.numsides && w != nullptr; s2++ )
		{
			if ( s2 == s )
			{
				continue;
			}


			cplane_t chopPlane = *brush.sides[ s2 ].plane;
			VectorNegate( chopPlane.normal, chopPlane.normal );
			chopPlane.dist = -chopPlane.dist;

			ChopWindingInPlace( &w, chopPlane.normal, chopPlane.dist, 0 );

			/* ydnar: fix broken windings that would generate trifans */
			FixWinding( w );
		}

		return w;
	}

	void EmitLadder( const std::vector<glm::vec3> &verts ) const
	{
		float zMin = verts[ 0 ].z, zMax = verts[ 0 ].z;
		for ( const glm::vec3 &vert : verts ) {
			zMin = std::min( zMin, vert.z );
			zMax = std::max( zMax, vert.z );
		}
		constexpr float MIN_HEIGHT = 10;
		constexpr float END_TOLERANCE = 4;

		if ( zMax - zMin < MIN_HEIGHT )
		{
			Log::Debug( "ladder too short" );
			return;
		}

		glm::vec3 bottom{}, top{};
		int nBottom = 0, nTop = 0;
		for ( const glm::vec3 &vert : verts )
		{
			if ( vert.z < zMin + END_TOLERANCE )
			{
				nBottom++;
				bottom += vert;
			}
			else if ( vert.z > zMax - END_TOLERANCE )
			{
				nTop++;
				top += vert;
			}
		}
		bottom /= nBottom;
		top /= nTop;

		//TODO: use a non-hardcoded radius
		Print("%.1f %.1f %.1f %.1f %.1f %.1f 30 1 63 0", bottom.x, bottom.z, bottom.y, top.x, top.z, top.y);
	}
};
static LadderCmd ladderCmdRegistration;
#endif // BUILD_SGAME
