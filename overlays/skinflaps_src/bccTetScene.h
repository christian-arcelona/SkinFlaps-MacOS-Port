//////////////////////////////////////////////////////////////////
// File: bccTetScene.h
// Author: Court Cutting
// Date: 3/4/2019
// Purpose: Bcc tet based projective dynamics physics library interface to surgical simulator code.
//    This version uses the ShapeOp subroutine library of Sofien Bouaziz ( http://ShapeOp.org )
//    to do the physics.  So far have experimented with mass-springs, physX(NVIDIA),
//    physBAM(Fedkiw, Teran, Sifakis, et al.), corotated linear elasticity(Teran, Sifakis, Mitchell) and
//    the shape matching code of Rivers and James.
//    Copyright 2019 - All rights reserved at the present time.
///////////////////////////////////////////////////////////////////

#ifndef __BCC_TET_SCENE__
#define __BCC_TET_SCENE__

#include "surgGraphics.h"
#include "vnBccTetrahedra.h"
#include "vnBccTetCutter_tbb.h"
#include "tetCollisions.h"
#include "tetSubset.h"
#include "remapTetPhysics.h"
#include "pdTetPhysics.h"
#include <memory>
#include <cstdint>
#include <array>
#include <set>
#include <unordered_map>

// forward declarations
class gl3wGraphics;
class surgicalActions;

class bccTetScene
{
public:
	bool loadScene(const char *dataDirectory, const char *sceneFileName);
	void createNewPhysicsLattice(int maxDimMegatetSubdivs, int nTetSizeLevels);
	void updateOldPhysicsLattice();
	void nonTetPhysicsUpdate();  // solver re-init without a lattice change (periosteal release): reports the anchors, holds a freed piece, then initializes
	void initPdPhysics();
	void updatePhysics();
	int nodesOutsideModel(int& firstNode, Vec3f& firstPos, float marginFraction = 0.5f);  // lattice nodes non-finite or far outside the model box
	void checkNodesInModel();  // throws after a solve that sent nodes out of the model
	void reportLatticeAnchors(const char* when);  // lattice components with no fixed point, hook or suture
	int holdSeveredPieces();  // pin a fully severed piece so the physics can run until it is excised
	int rehomeFlapWallBottoms();  // a flap-side wall bottom left in its bed twin's tet is drawn from a flap-side tet
	static int solveCount();  // physics solves so far in this session
	void fixPeriostealPeriferalVertices();
	void updateSurfaceDraw();
	pdTetPhysics* getPdTetPhysics_2(){ return &_ptp; }
	inline void setForcesAppliedFlag(){ _forcesApplied = true; }
	inline void promoteSutures() { _ptp.promoteAllSutures(); _ptp.initializePhysics(); }
	vnBccTetrahedra* getVirtualNodedBccTetrahedra() { return &_vnTets; }
	void setVisability(char surface, char physics);	// 0=off, 1=on, 2=don't change
	void setGl3wGraphics(gl3wGraphics *gl3w) { _gl3w = gl3w; }
	void createTetLatticeDrawing();
	void drawTetLattice();
	void eraseTetLattice();
	void setSurgicalActions(surgicalActions *sa) { _surgAct = sa; }
	void setPhysicsPause(bool pause) { _physicsPaused = pause; }
	inline bool isPhysicsPaused(){ return  _physicsPaused; }
	inline bool forcesApplied() { return  _forcesApplied; }
	// Whole-state undo: everything needed to reinstate the physics lattice
	// exactly, without recutting.  The cutter is included because its incision
	// bookkeeping is incremental (it assumes the surface only grows); the remap
	// state because the next topology change reads it.
	// The lattice, cutter bookkeeping and remap state only change on a topology
	// rebuild, so snapshots taken between rebuilds share one immutable core
	// (keyed by a generation counter); node positions are per snapshot.
	struct latticeCore {
		std::unique_ptr<vnBccTetrahedra> vnTets;
		std::unique_ptr<vnBccTetCutter_tbb> tc;
		std::unique_ptr<remapTetPhysics> rtp;
	};
	struct latticeState {
		std::shared_ptr<const latticeCore> core;
		unsigned generation = 0;
		std::vector<std::array<float, 3>> nodePos;
		bool forcesApplied = false;
	};
	void saveLatticeState(latticeState& s);
	void restoreLatticeState(const latticeState& s);
	bccTetScene();
	~bccTetScene();

private:
	gl3wGraphics *_gl3w;
	surgicalActions *_surgAct;
	materialTriangles* _mt;  // pointer from surgGraphics.
	vnBccTetrahedra _vnTets;
	remapTetPhysics _rtp;
	tetCollisions _tetCol;
	tetSubset _tetSubsets;
	vnBccTetCutter_tbb _tc;  // multithreaded version using Intel threaded building blocks.  Much faster, but indices of nodes and tets different each run as nondeterministic.
	pdTetPhysics _ptp;
	bool _forcesApplied, _tetsModified, _physicsPaused;
	float _lowTetWeight;
	struct boundingBox3{
		float corners[6];
	};
	std::vector<GLfloat> _nodeGraphicsPositions;  // homogeneous coords[4]

	std::vector<Vec3f> _firstSpatialCoords;

	std::vector<std::vector<int> > _unanchoredTets;  // tets of each unanchored component at the last anchors report
	std::set<int> _heldTets;  // tets already pinned by holdSeveredPieces on this lattice (a periosteal re-init must not pin them again)
	std::unordered_map<int, std::pair<int, Vec3f> > _vertexTetOverride;  // vertex -> (tet, weights) used for its drawn position only; the cutter's assignment is untouched
	static unsigned s_latticeGeneration;  // bumped by every rebuild
	static std::weak_ptr<const latticeCore> s_lastCore;
	static unsigned s_lastCoreGeneration;
	static uint64_t s_lastCoreVertexKey;  // hash of the vertex->tet table: cuts extend it without a rebuild
	uint64_t vertexTableKey();
};

#endif // __BCC_TET_SCENE__
