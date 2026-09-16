//////////////////////////////////////////////////////////////////
// File: bccTetScene.cpp
// Author: Court Cutting
// Date: 3/4/2019
// Purpose: Bcc tet based projective dynamics physics library interface to surgical simulator code.
//    This version uses the ShapeOp subroutine library of Sofien Bouaziz ( http://ShapeOp.org )
//    to do the physics.  So far have experimented with mass-springs, physX(NVIDIA),
//    physBAM(Fedkiw, Teran, Sifakis, et al.), corotated linear elasticity(Teran, Sifakis, Mitchell) and
//    the shape matching code of Rivers and James.
//    Copyright 2019 - All rights reserved at the present time.
// Revised: 11/1/2024
// Description: This revision uses the multiresolution bcc tet classes to drop tet count.
///////////////////////////////////////////////////////////////////

#include "bccTetScene.h"
#include <string>
#include <fstream>
#include <algorithm>
#include "gl3wGraphics.h"
#include "surgicalActions.h"
#include "boundingBox.h"
#include "json.h"
#include "closestPointOnTriangle.h"
#include "remapTetPhysics.h"
#include <iostream>
#include <cstdint>
#include <chrono>
#include <ctime>
#include <stdexcept>
#include <memory>
#include <unordered_map>
#include <cmath>

static int s_solveCount = 0;

bool bccTetScene::loadScene(const char *dataDirectory, const char *sceneFileName)
{
	_physicsPaused = true;
	std::string path(dataDirectory);
	path.append(sceneFileName);
	std::ifstream istr(path.c_str());
	std::string jsonStr;
	if (!istr.is_open()) {
		path = std::string("Unable to load: ") + path;
		_surgAct->sendUserMessage(path.c_str(), "Error Message");
		istr.close();
		return false;
	}
	else {
		char ch;
		while (istr.get(ch))
			jsonStr.push_back(ch);

	}
	istr.close();
	json::Value my_data = json::Deserialize(jsonStr);  // will trim leading and trailing white space from {} pair
	if (my_data.GetType() != json::ObjectVal) {
		_surgAct->sendUserMessage("Module file not in correct JSON format-", "Error Message");
		return false;
	}
	json::Object scnObj = my_data.ToObject();
	json::Object::ValueMap::iterator oit, suboit, suboit2;
	// get texture files first
	std::map<int, GLuint> txMap;
	std::string nrm, tex;
	if ((oit = scnObj.find("textureFiles")) == scnObj.end()) {
		_surgAct->sendUserMessage("No texture files in scene file-", "Error Message");
		return false;
	}
	else {
		json::Object txObj = oit->second.ToObject();
		for (suboit = txObj.begin(); suboit != txObj.end(); ++suboit) {
			path = dataDirectory + suboit->first;
			GLuint txNow = _gl3w->getTextures()->loadTexture(suboit->second.ToInt(), path.c_str());
			if (txNow > 0xfffffffe) {
				path = "Unable to load bitmap .bmp input file: " + path;
				_surgAct->sendUserMessage(path.c_str(), "Error Message");
				return false;
			}
			int txNum = suboit->second.ToInt();
			txMap.insert(std::make_pair(txNum, txNow));
		}
	}
	if ((oit = scnObj.find("staticObjects")) != scnObj.end()) {
		json::Object statObj = oit->second.ToObject();
		for (suboit = statObj.begin(); suboit != statObj.end(); ++suboit) {
			path = dataDirectory + suboit->first;
			std::map<int, GLuint>::iterator tit;
			std::vector<int> txIds;
			json::Object tmapObj = suboit->second.ToObject();
			for (suboit2 = tmapObj.begin(); suboit2 != tmapObj.end(); ++suboit2) {
				if (suboit2->first == "textureMap")
					txIds.push_back(suboit2->second.ToInt());
				else if (suboit2->first == "normalMap")
					txIds.push_back(suboit2->second.ToInt());
				else {
					_surgAct->sendUserMessage("Incorrect static object section in .smd input file-", "Error Message");
					return false;
				}
			}
			// this is a staticTriangle, not elastic so put on graphics card and clean up
			if ( _gl3w->loadStaticObjFile(path.c_str(), txIds, true) == NULL)
			{
				_surgAct->sendUserMessage("Unable to load fixed triangle .obj input file-", "Error Message");
				return false;
			}
		}
	}
	std::string deepBedFilepath;
	deepBedFilepath.clear();
	if ((oit = scnObj.find("dynamicObjects")) == scnObj.end()) {
		_surgAct->sendUserMessage("No dynamic objects in this scene file-", "Error Message");
		return false;
	}
	else {
		json::Object dynObj = oit->second.ToObject();
		std::vector<int> txIds;
		for (suboit = dynObj.begin(); suboit != dynObj.end(); ++suboit) {
			path = dataDirectory + suboit->first;
			std::map<int, GLuint>::iterator tit;
			json::Object tmapObj = suboit->second.ToObject();
			for (suboit2 = tmapObj.begin(); suboit2 != tmapObj.end(); ++suboit2) {
				if (suboit2->first == "textureMaps") {
					json::Array txArr;
					txArr = suboit2->second.ToArray();
					for (int i = 0; i < txArr.size(); ++i) {
						txIds.push_back(txArr[i].ToInt());
						if (!_gl3w->getTextures()->textureExists(txIds.back())) {
							_surgAct->sendUserMessage("Missing texture or normal map in dynamic triangle section in .smd input file-", "Error Message");
							return false;
						}
					}
				}
			}
			_mt = _surgAct->getSurgGraphics()->getMaterialTriangles();
			if (_mt->readObjFile(path.c_str())) {
				_surgAct->sendUserMessage("Unable to load fixed materialTriangle .obj input file-", "Error Message");
				return false;
			}
			// same material texture seams processed in graphics,
			// may want to create hard texture & normal seams between materials here.
			_surgAct->getSurgGraphics()->setGl3wGraphics(_gl3w);
			std::string vtxShd(dataDirectory), frgShd(dataDirectory);
			vtxShd.append("mtVertexShader.txt");
			frgShd.append("mtFragmentShader.txt");
			_surgAct->getSurgGraphics()->setTextureFilesCreateProgram(txIds, vtxShd.c_str(), frgShd.c_str());  // openGL buffers ceated here
			_surgAct->getSurgGraphics()->setNewTopology();
			_surgAct->getSurgGraphics()->updatePositionsNormalsTangents();
			_surgAct->getSurgGraphics()->computeLocalBounds();
			path = suboit->first;
			size_t pos = path.rfind(".obj");
			path.erase(pos);
			_mt->setName(path.c_str());
			_surgAct->getSurgGraphics()->getSceneNode()->setName(path.c_str());
			// input new deep bed file here
			deepBedFilepath.clear();
			deepBedFilepath.append(dataDirectory);
			deepBedFilepath.append(path);
			deepBedFilepath.append(".bed");
		}
	}
	if ((oit = scnObj.find("fixedGeometry")) != scnObj.end()) {
		// now using fixedCollisionSets instead
		throw(std::logic_error("Model .smd file sent to simulator uses an old fixedGeometry specifier that is no longer supported.\n"));
	}
	if ((oit = scnObj.find("fixedCollisionSets")) != scnObj.end()) {
		json::Object hullObj = oit->second.ToObject();
		std::string lsPath;
		for (suboit = hullObj.begin(); suboit != hullObj.end(); ++suboit) {
			lsPath = dataDirectory + suboit->first;
			json::Array polyArr;
			polyArr = suboit->second.ToArray();
			std::vector<int> vIdx;
			vIdx.reserve(polyArr.size());
			for (int i = 0; i < polyArr.size(); ++i)
				vIdx.push_back( polyArr[i].ToInt());
			_tetCol.addFixedCollisionSet(lsPath, vIdx);
		}
	}
	int nTetSizeLevels = 4, maxDimMegatetSubdivs = 31;  // Multires settings initial tet count 11,587 tets while old single res was0.5 million tets for cleft model.  Now loaded in properties below.
	if ((oit = scnObj.find("tetrahedralProperties")) != scnObj.end()) {
		json::Object hullObj = oit->second.ToObject();
		float lowTetWeight, highTetWeight, TJunctionWeight, strainMin, strainMax, collisionWeight, fixedWeight, periferalWeight, hookWeight, sutureWeight, autoSutureSpacing, selfCollisionWeight;
		for (suboit = hullObj.begin(); suboit != hullObj.end(); ++suboit) {
			if (suboit->first == "minStrain")
				strainMin = suboit->second.ToFloat();
			else if (suboit->first == "maxStrain")
				strainMax = suboit->second.ToFloat();
			else if (suboit->first == "lowTetWeight")
				_lowTetWeight = lowTetWeight = suboit->second.ToFloat();
			else if (suboit->first == "highTetWeight")
				highTetWeight = suboit->second.ToFloat();
			else if (suboit->first == "TJunctionWeight")
				TJunctionWeight = suboit->second.ToFloat();
			else if (suboit->first == "collisionWeight")
				collisionWeight = suboit->second.ToFloat();
			else if (suboit->first == "selfCollisionWeight")
				selfCollisionWeight = suboit->second.ToFloat();
			else if (suboit->first == "fixedWeight")
				fixedWeight = suboit->second.ToFloat();
			else if (suboit->first == "periferalWeight")
				periferalWeight = suboit->second.ToFloat();
			else if (suboit->first == "sutureWeight")
				sutureWeight = suboit->second.ToFloat();
			else if (suboit->first == "hookWeight")
				hookWeight = suboit->second.ToFloat();
			else if (suboit->first == "autoSutureSpacing")
				autoSutureSpacing = suboit->second.ToFloat();
			else if (suboit->first == "maxDimMegatetSubdivs")
				maxDimMegatetSubdivs = suboit->second.ToInt();
			else if (suboit->first == "nTetSizeLevels")
				nTetSizeLevels = suboit->second.ToInt();
			else
				_surgAct->sendUserMessage("Unknown tetrahedral property in scene file-", "File Error Message");
		}
		_ptp.setTetProperties(lowTetWeight, highTetWeight, TJunctionWeight, strainMin, strainMax, collisionWeight, selfCollisionWeight, fixedWeight, periferalWeight);
		_ptp.setHookSutureWeights(hookWeight, sutureWeight, 0.3f);
		_surgAct->getSutures()->setAutoSutureSpacing(autoSutureSpacing);
	}
	struct tetSubset {
		std::string objFile;
		float lowTetWeight;
		float highTetWeight;
		float strainMin;
		float strainMax;
	};
	std::list<tetSubset> tetSubsets;
	if ((oit = scnObj.find("tetrahedralSubsets")) != scnObj.end()) {
		json::Object tetSubObj = oit->second.ToObject();
		tetSubset ts;
		for (suboit = tetSubObj.begin(); suboit != tetSubObj.end(); ++suboit) {
			path = dataDirectory + suboit->first;
			ts.objFile = path;
			json::Object tetSubData = suboit->second.ToObject();
			for (auto dataoit = tetSubData.begin(); dataoit != tetSubData.end(); ++dataoit) {
				if (dataoit->first == "minStrain")
					ts.strainMin = dataoit->second.ToFloat();
				else if (dataoit->first == "maxStrain")
					ts.strainMax = dataoit->second.ToFloat();
				else if (dataoit->first == "lowTetWeight")
					ts.lowTetWeight = dataoit->second.ToFloat();
				else if (dataoit->first == "highTetWeight")
					ts.highTetWeight = dataoit->second.ToFloat();
				else;
			}
			tetSubsets.push_back(ts);
		}
	}
	else
		;
	createNewPhysicsLattice(maxDimMegatetSubdivs, nTetSizeLevels);  // now creating operable lattice on load
	_surgAct->getDeepCutPtr()->setMaterialTriangles(_mt);
	if (!_surgAct->getDeepCutPtr()->setDeepBed(_mt, deepBedFilepath.c_str(), &_vnTets)){
		_surgAct->sendUserMessage("Undermine layer .bed file could not be found-", "Error Message");
	}
	if (!tetSubsets.empty()) {
		for (auto& ts : tetSubsets)
			_tetSubsets.createSubset(&_vnTets, ts.objFile, ts.lowTetWeight, ts.highTetWeight, ts.strainMin, ts.strainMax);
	}
	_gl3w->frameScene(true);  // computes bounding spheres
	return true;
}

void bccTetScene::updateOldPhysicsLattice()
{
	++s_latticeGeneration;  // the lattice is about to change
	_rtp.getOldPhysicsData(&_vnTets);  // must be done before any new incisions.  Worst case example < 0.02 seconds - not worth multithreading.
	_tc.addNewMultiresIncision();
	rehomeFlapWallBottoms();

#ifdef NO_PHYSICS
	_firstSpatialCoords.assign(_vnTets.nodeNumber(), Vec3f());
	_vnTets.setNodeSpatialCoordinatePointer(&_firstSpatialCoords[0]);  // for no physics debug
#else
	std::vector<uint8_t> tetSizeMult;
	tetSizeMult.reserve(_vnTets.tetNumber());
	for (int n = _vnTets.tetNumber(), i = 0; i < n; ++i) {
		uint8_t sizeBit = 1;
		auto& c = _vnTets.tetCentroid(i);
		unsigned short ored = c[0] | c[1] | c[2];
		while (true) {
			if (ored & sizeBit)
					break;
			sizeBit <<= 1;
		}
		tetSizeMult.push_back(sizeBit);
	}
	std::array<float, 3>* nodeSpatialCoords = _ptp.createBccTetStructure_multires(_vnTets.getTetNodeArray(), tetSizeMult, (float)_vnTets.getTetUnitSize());
	_vnTets.setNodeSpatialCoordinatePointer(nodeSpatialCoords);  // vector created in _ptp
#endif
	_rtp.remapNewPhysicsNodePositions(&_vnTets);  // requires node spatial coordinate array pointer. Worst case example < 0.02 seconds - not worth multithreading.
	std::vector<int> subNodes;
	std::vector<std::vector<int> > macroNodes;
	std::vector<std::vector<float> > macroBarys;
	_vnTets.getTJunctionConstraints(subNodes, macroNodes, macroBarys);
	_ptp.addInterNodeConstraints(subNodes, macroNodes, macroBarys);
	_tetSubsets.sendTetSubsets(&_vnTets, _mt, &_ptp);

	if (_forcesApplied) {  // _tetsModified not necessary as implied by calling this routine
		initPdPhysics();
		_tetsModified = true;
	}
	_physicsPaused = false;
}

void bccTetScene::createNewPhysicsLattice(int maxDimMegatetSubdivs, int nTetSizeLevels)
{
	++s_latticeGeneration;  // new lattice
	_vertexTetOverride.clear();
	try {
		_tetsModified = false;
		_tc.setRemapTetPhysics(&_rtp);
		_tc.createFirstMacroTets(_mt, &_vnTets, nTetSizeLevels, maxDimMegatetSubdivs);
		_surgAct->getDeepCutPtr()->setVnBccTetrahedra(&_vnTets);
		_surgAct->getDeepCutPtr()->setMaterialTriangles(_mt);

//		std::cout << "Tet number at this time is " << _vnTets.tetNumber() << "\n";

		_surgAct->getHooks()->setSpringConstant(_lowTetWeight * 1.5f);  // COURT fix me after macrotet issue resolved

#ifdef NO_PHYSICS
		_firstSpatialCoords.assign(_vnTets.nodeNumber(), Vec3f());
		_vnTets.setNodeSpatialCoordinatePointer(&_firstSpatialCoords[0]);  // for no physics debug
#else
		std::vector<uint8_t> tetSizeMult;
		tetSizeMult.reserve(_vnTets.tetNumber());
		for (int n = _vnTets.tetNumber(), i = 0; i < n; ++i) {
			// COURT may do faster with just first 2 nodes
			uint8_t sizeBit = 1;
			auto& c = _vnTets.tetCentroid(i);
			while (true) {
				if (c[0] & sizeBit || c[1] & sizeBit || c[2] & sizeBit)
					break;
				sizeBit <<= 1;
			}
			tetSizeMult.push_back(sizeBit);
		}
		std::array<float, 3>* nodeSpatialCoords = _ptp.createBccTetStructure_multires(_vnTets.getTetNodeArray(), tetSizeMult, (float)_vnTets.getTetUnitSize());
		_vnTets.setNodeSpatialCoordinatePointer(nodeSpatialCoords);  // vector created in _ptp
#endif
		_vnTets.materialCoordsToNodeSpatialVector();

		std::vector<int> subNodes;
		std::vector<std::vector<int> > macroNodes;
		std::vector<std::vector<float> > macroBarys;
		_vnTets.getTJunctionConstraints(subNodes, macroNodes, macroBarys);
		_ptp.addInterNodeConstraints(subNodes, macroNodes, macroBarys);

		_tetsModified = false;
		_physicsPaused = false;
	}  // end try block
	catch (...) {
		_surgAct->taskThreadError = true;
		_surgAct->taskThreadErrorStr = "Couldn't create the initial physics lattice. Probable model error.";
	}
}

void bccTetScene::initPdPhysics()
{  // called after each new tet lattice created
	fixPeriostealPeriferalVertices();  // doesn't throw
	if (!_tetCol.empty()) {
		_tetCol.updateFixedCollisions(_mt, &_vnTets);
		_tetCol.initSoftCollisions(_mt, &_vnTets);
	}
#ifndef NO_PHYSICS
//	if (_surgAct->getHooks()->getNumberOfHooks() < 1 && _surgAct->getSutures()->getNumberOfSutures() < 1)
//		throw(std::logic_error("Trying to initialize physics without applying any forces.\n"));
	_surgAct->getHooks()->setGroupPhysicsInit(true);  // instead of individual inits, add all hooks and sutures then initialize physics only once.
	_surgAct->getSutures()->setGroupPhysicsInit(true);
	_surgAct->getHooks()->updateHookPhysics();
	_surgAct->getSutures()->updateSuturePhysics();
	reportLatticeAnchors("before physics init");  // before the factorization, so a severed piece is named even when the solver refuses it
	_heldTets.clear();  // a new lattice: its pins are added below
	holdSeveredPieces();
	_ptp.initializePhysics();
	_surgAct->getHooks()->setGroupPhysicsInit(false);
	_surgAct->getSutures()->setGroupPhysicsInit(false);
#endif
}

// A periosteal release re-initializes the solver here rather than through
// initPdPhysics(), so the anchors report and the hold are repeated on this
// path: the release can leave slivers of tets with no anchor at all (no shared
// node, no surface vertex), which make the system singular and drift to NaN at
// the first hook move.  A released element that stays connected to the lattice
// is not a separate component and is never pinned.
void bccTetScene::nonTetPhysicsUpdate()
{
	reportLatticeAnchors(_ptp.solverInitialized() ? "before physics re-init (periosteal release)" : "before the solver's first init (periosteal release before any hook)");
	holdSeveredPieces();
	_ptp.initializePhysics();
}

void bccTetScene::updatePhysics()
{
	if (_vnTets.empty())
		return;
//	if (!_tetsModified && _forcesApplied) {  // don't do this here anymore
//		_tetsModified = true;
//		initPdPhysics();
//	}

#ifndef NO_PHYSICS
	if (_tetsModified || _forcesApplied) {
		_tetCol.findSoftCollisionPairs();
		_ptp.solve();
		++s_solveCount;
		checkNodesInModel();
	}
#endif

#ifdef WRITE_FOR_RENDER
	RenderHelper<float>::writeMesh(*_mt);
	RenderHelper<float>::frame++;
#endif
}
 
void bccTetScene::setVisability(char surface, char physics)
{  // 0=off, 1=on, 2=don't change
	if (surface < 1)
		_surgAct->getSurgGraphics()->getSceneNode()->visible = false;
	if (surface == 1)
		_surgAct->getSurgGraphics()->getSceneNode()->visible = true;
	if (physics < 1)
		_gl3w->getLines()->setLinesVisible(false);
	if (physics == 1) {
		if (!_gl3w->getLines()->getSceneNode()) {
			createTetLatticeDrawing();
			drawTetLattice();
		}
		else
			_gl3w->getLines()->setLinesVisible(true);
	}
}

// Lattice nodes that are non-finite or lie farther than half the model's extent
// outside the lattice box.  A lifted or dragged flap never gets there; a runaway
// solve does, growing over successive solves until the positions overflow, and
// the solver's own check only sees a non-finite right-hand side.
int bccTetScene::nodesOutsideModel(int& firstNode, Vec3f& firstPos, float marginFraction)
{
	firstNode = -1;
	const int nn = _vnTets.nodeNumber();
	if (nn < 1)
		return 0;
	const Vec3f lo = _vnTets.getMinimumCorner(), hi = _vnTets.getMaximumCorner();
	const Vec3f ext = hi - lo;
	const float margin = marginFraction * std::max(ext.X, std::max(ext.Y, ext.Z));
	int count = 0;
	for (int i = 0; i < nn; ++i) {
		const Vec3f& p = _vnTets.nodeSpatialCoordinate(i);
		const bool finite = p.X == p.X && p.Y == p.Y && p.Z == p.Z;
		if (!finite || p.X < lo.X - margin || p.X > hi.X + margin || p.Y < lo.Y - margin || p.Y > hi.Y + margin || p.Z < lo.Z - margin || p.Z > hi.Z + margin) {
			if (firstNode < 0) { firstNode = i; firstPos = p; }
			++count;
		}
	}
	return count;
}

void bccTetScene::checkNodesInModel()
{
	int first;
	Vec3f fp;
	const int n = nodesOutsideModel(first, fp);
	if (n > 0) {
		char buf[256];
		snprintf(buf, sizeof buf, "Tissue flew out of the model after the last move (%d lattice nodes; the first, node %d, at %.1f %.1f %.1f)-", n, first, fp.X, fp.Y, fp.Z);
		throw std::runtime_error(buf);
	}
}

int bccTetScene::solveCount() { return s_solveCount; }

// Connected components of the lattice (nodes joined by tets) that hold no tet
// constraint at all, i.e. no periosteal or peripheral fixed point, no hook and no
// suture.  Nothing holds such a component in place.  Reported before every
// physics initialization; the unanchored components' tets are kept for
// holdSeveredPieces().
void bccTetScene::reportLatticeAnchors(const char* when)
{
	const int nn = _vnTets.nodeNumber(), nt = _vnTets.tetNumber();
	if (nn < 1 || nt < 1)
		return;
	std::vector<int> parent(nn);
	for (int i = 0; i < nn; ++i) parent[i] = i;
	auto find = [&](int a) { while (parent[a] != a) { parent[a] = parent[parent[a]]; a = parent[a]; } return a; };
	auto unite = [&](int a, int b) { a = find(a); b = find(b); if (a != b) parent[a] = b; };
	for (int i = 0; i < nt; ++i) {
		const int* tn = _vnTets.tetNodes(i);
		if (tn[0] < 0 || tn[0] >= nn) continue;
		for (int k = 1; k < 4; ++k)
			if (tn[k] >= 0 && tn[k] < nn)
				unite(tn[0], tn[k]);
	}
	// The lattice is multiresolution: a fine patch shares no node with the coarse tets around it and
	// hangs on T-junction (inter-node) constraints instead.  Those bind it as firmly as shared nodes
	// do, so they join components here.
	size_t tJunctions = 0;
	{
		std::vector<int> subNodes;
		std::vector<std::vector<int> > macroNodes;
		std::vector<std::vector<float> > macroBarys;
		_vnTets.getTJunctionConstraints(subNodes, macroNodes, macroBarys);
		tJunctions = subNodes.size();
		for (size_t i = 0; i < subNodes.size() && i < macroNodes.size(); ++i)
			for (int mnode : macroNodes[i])
				if (subNodes[i] >= 0 && subNodes[i] < nn && mnode >= 0 && mnode < nn)
					unite(subNodes[i], mnode);
	}
	std::vector<char> anchored(nn, 0);
	size_t nPrimary = 0, nFake = 0;
	{
		std::vector<decltype(_ptp)::ConstraintExport> primary, fake, collision;
		std::vector<double> muLow, muHigh;
		_ptp.exportConstraintData(primary, fake, collision, muLow, muHigh);
		nPrimary = primary.size(); nFake = fake.size();
		for (auto* v : { &primary, &fake })
			for (auto& c : *v)
				for (int k = 0; k < 4; ++k)
					if (c.element[k] >= 0 && c.element[k] < nn)
						anchored[c.element[k]] = 1;
	}
	struct Comp { int nodes = 0, tets = 0, anchoredNodes = 0, surfaceVertices = 0, firstNode = -1; };
	std::unordered_map<int, Comp> comps;
	for (int i = 0; i < nn; ++i) {
		Comp& c = comps[find(i)];
		++c.nodes;
		if (anchored[i]) ++c.anchoredNodes;
		if (c.firstNode < 0) c.firstNode = i;
	}
	for (int i = 0; i < nt; ++i) {
		const int* tn = _vnTets.tetNodes(i);
		if (tn[0] >= 0 && tn[0] < nn) ++comps[find(tn[0])].tets;
	}
	for (int v = 0, nv = _mt ? _mt->numberOfVertices() : 0; v < nv; ++v) {
		const int t = _vnTets.getVertexTetrahedron(v);
		if (t < 0 || t >= nt) continue;
		const int* tn = _vnTets.tetNodes(t);
		if (tn[0] >= 0 && tn[0] < nn) ++comps[find(tn[0])].surfaceVertices;
	}
	int unanchored = 0, withSurface = 0;
	for (auto& kv : comps)
		if (kv.second.anchoredNodes == 0) { ++unanchored; if (kv.second.surfaceVertices > 0) ++withSurface; }
	_unanchoredTets.clear();
	{
		std::unordered_map<int, size_t> slot;
		for (int i = 0; i < nt; ++i) {
			const int* tn = _vnTets.tetNodes(i);
			if (tn[0] < 0 || tn[0] >= nn) continue;
			const int root = find(tn[0]);
			if (comps[root].anchoredNodes > 0) continue;
			auto it = slot.find(root);
			if (it == slot.end()) { it = slot.insert(std::make_pair(root, _unanchoredTets.size())).first; _unanchoredTets.emplace_back(); }
			_unanchoredTets[it->second].push_back(i);
		}
	}
	fprintf(stderr, "[lattice-anchors] %s: %d nodes, %d tets, %zu T-junctions, %zu components, %zu tet constraints (%zu fake sutures); unanchored components: %d (%d carry surface vertices)\n",
		when, nn, nt, tJunctions, comps.size(), nPrimary, nFake, unanchored, withSurface);
	int listed = 0;
	for (auto& kv : comps) {
		const Comp& c = kv.second;
		if (c.anchoredNodes > 0) continue;
		if (listed++ >= 12) { fprintf(stderr, "[lattice-anchors]   ... and %d more unanchored components\n", unanchored - 12); break; }
		const Vec3f& p = _vnTets.nodeSpatialCoordinate(c.firstNode);
		fprintf(stderr, "[lattice-anchors]   unanchored: nodes=%d tets=%d surface vertices=%d first node %d at (%.2f %.2f %.2f)\n", c.nodes, c.tets, c.surfaceVertices, c.firstNode, p.X, p.Y, p.Z);
	}
	fflush(stderr);
}

// A both-open deep cut can detach a piece of tissue completely (the wedge between
// the cut and a rim).  Once forces have been applied the cut re-initializes the
// physics at once, the piece has no constraint, the system is singular and the
// solver refuses it ("Eigen CHOLMOD: A11 factorization failed").  Such a piece,
// and only such a piece (no shared node, no T-junction, no fixed point, hook or
// suture), is held where it is by strong hook constraints on a few of its tets
// until the next lattice rebuild (the excise that follows clears them).
int bccTetScene::holdSeveredPieces()
{
	if (_unanchoredTets.empty())
		return 0;
	int held = 0;
	for (auto& tets : _unanchoredTets) {
		if (tets.empty()) continue;
		const size_t n = tets.size();
		const size_t picks = std::min<size_t>(8, n);
		if (_heldTets.count(tets[0]))
			continue;  // already pinned on this lattice (a periosteal re-init)
		for (size_t k = 0; k < picks; ++k) {
			const int tet = tets[(k * n) / picks];
			_heldTets.insert(tet);
			const int* tn = _vnTets.tetNodes(tet);
			Vec3f c = _vnTets.nodeSpatialCoordinate(tn[0]) + _vnTets.nodeSpatialCoordinate(tn[1]) + _vnTets.nodeSpatialCoordinate(tn[2]) + _vnTets.nodeSpatialCoordinate(tn[3]);
			c *= 0.25f;
			const std::array<float, 3> bw = { 0.25f, 0.25f, 0.25f }, pos = { c.X, c.Y, c.Z };
			_ptp.addHook(tet, bw, pos, true);
			++held;
		}
		_heldTets.insert(tets[0]);
		fprintf(stderr, "[lattice-anchors] a piece of tissue with no anchor left (a cut severed it, or the periosteum under it was released) is held in place (%zu tets, %d hold points): excise it, close one end of the cut, or hook it before releasing its periosteum\n", n, (int)picks);
	}
	return held;
}

// When an undermine doubles the bottom vertices of an incision wall, each flap-side
// copy is created with its bed-side twin's tet and weights, and the re-cut that
// follows is expected to move it into a flap-side copy of the cell.  At a cell the
// cutter does not split (the corner where an incision T's into a wound, the free end
// of an incision), the copy stays in the bed's tet.  Nothing in the physics holds the
// flap there (a surface vertex is a passive interpolation of its tet's nodes), but
// the vertex is DRAWN from the bed's nodes, so the flap lifts everywhere except that
// point and the wall triangles there stretch from the flap down to the wound.  The
// signature is exact: a vertex whose triangles are only incision wall (3) and flap
// bottom (4), coincident with a vertex that has bed (5) triangles, in the same tet
// with the same weights.  For such a vertex the POSITION is taken from the tet of its
// nearest neighbour across a flap-bottom triangle (a flap-side tet that contains it
// well enough), with weights from its own material position.  The cutter's assignment
// (_vertexTets) is not touched: the cutter assumes a vertex lies inside its recorded
// tet, and rewriting it makes a later undermine fail its topology check.  Rebuilt
// after every re-cut and every restore.
int bccTetScene::rehomeFlapWallBottoms()
{
	_vertexTetOverride.clear();
	if (!_mt)
		return 0;
	const int nv = _mt->numberOfVertices(), nt = _mt->numberOfTriangles();
	if (nv < 1 || _vnTets.vertexTetCount() < nv)
		return 0;
	std::vector<uint16_t> mats(nv, 0);
	for (int t = 0; t < nt; ++t) {
		const int m = _mt->triangleMaterial(t);
		if (m < 0 || m > 15) continue;
		const int* tr = _mt->triangleVertices(t);
		for (int k = 0; k < 3; ++k)
			if (tr[k] >= 0 && tr[k] < nv) mats[tr[k]] |= (uint16_t)(1u << m);
	}
	const uint16_t M3 = 1u << 3, M4 = 1u << 4, M5 = 1u << 5;
	std::vector<int> candidates;
	for (int v = 0; v < nv; ++v)
		if (mats[v] == (M3 | M4) && _vnTets.getVertexTetrahedron(v) >= 0)
			candidates.push_back(v);
	if (candidates.empty())
		return 0;
	// coincident vertices: hash by rounded position
	auto key = [](const float* p) { return std::to_string((long long)std::llround(p[0] * 1e4)) + "," + std::to_string((long long)std::llround(p[1] * 1e4)) + "," + std::to_string((long long)std::llround(p[2] * 1e4)); };
	std::unordered_map<std::string, std::vector<int> > cells;
	for (int v = 0; v < nv; ++v) {
		if (!(mats[v] & M5)) continue;
		float p[3]; _mt->getVertexCoordinate(v, p);
		cells[key(p)].push_back(v);
	}
	// flap-bottom neighbours of the candidates
	std::unordered_map<int, std::vector<int> > m4nei;
	for (int v : candidates) m4nei[v];
	for (int t = 0; t < nt; ++t) {
		if (_mt->triangleMaterial(t) != 4) continue;
		const int* tr = _mt->triangleVertices(t);
		for (int k = 0; k < 3; ++k) {
			auto it = m4nei.find(tr[k]);
			if (it == m4nei.end()) continue;
			it->second.push_back(tr[(k + 1) % 3]);
			it->second.push_back(tr[(k + 2) % 3]);
		}
	}
	int moved = 0;
	for (int v : candidates) {
		const int tv = _vnTets.getVertexTetrahedron(v);
		const Vec3f wv = *_vnTets.getVertexWeight(v);
		float pv[3]; _mt->getVertexCoordinate(v, pv);
		auto cit = cells.find(key(pv));
		if (cit == cells.end()) continue;
		int twin = -1;
		for (int u : cit->second) {
			if (u == v || _vnTets.getVertexTetrahedron(u) != tv) continue;
			const Vec3f& wu = *_vnTets.getVertexWeight(u);
			if (std::fabs(wu.X - wv.X) > 1e-6f || std::fabs(wu.Y - wv.Y) > 1e-6f || std::fabs(wu.Z - wv.Z) > 1e-6f) continue;
			float pu[3]; _mt->getVertexCoordinate(u, pu);
			if (std::fabs(pu[0] - pv[0]) > 1e-5f || std::fabs(pu[1] - pv[1]) > 1e-5f || std::fabs(pu[2] - pv[2]) > 1e-5f) continue;
			twin = u; break;
		}
		if (twin < 0) continue;
		// candidate flap-side tets: those of the neighbours across flap-bottom triangles (no bed
		// triangles of their own, not the shared tet).  Take the one that contains the vertex best
		// (largest smallest barycentric weight); refuse a tet the vertex lies well outside of, since
		// far-outside weights would amplify the tet's node motion into the vertex.
		Vec3f gl;
		_vnTets.vertexGridLocus(v, gl);  // material position from the current tet: exact, the twin tets share material geometry
		int best = -1; float bestScore = -1e9f; Vec3f bestW;
		std::vector<int> tried;
		for (int n : m4nei[v]) {
			if (n < 0 || n >= nv || n == v) continue;
			const int tn = _vnTets.getVertexTetrahedron(n);
			if (tn < 0 || tn == tv || (mats[n] & M5)) continue;
			if (std::find(tried.begin(), tried.end(), tn) != tried.end()) continue;
			tried.push_back(tn);
			Vec3f w;
			_vnTets.gridLocusToBarycentricWeight(gl, _vnTets.tetCentroid(tn), w);
			const float score = std::min(std::min(w.X, w.Y), std::min(w.Z, 1.0f - w.X - w.Y - w.Z));
			if (score > bestScore) { bestScore = score; best = tn; bestW = w; }
		}
		if (best < 0 || bestScore < -0.25f)
			continue;
		_vertexTetOverride[v] = std::make_pair(best, bestW);
		++moved;
	}
	return moved;
}

void bccTetScene::updateSurfaceDraw()
{
	// A draw must never run on a GL topology older than the mesh.  Every restore
	// and every cut sets newTopology for the frame loop, but the draw update is
	// also called directly by the action handlers and by the hold/revert flows;
	// a mesh replaced under a stale vertex map reads past the position array and
	// hands mismatched buffers to the driver.  Rebuild first when pending.
	if (_surgAct && _surgAct->newTopology) {
		_surgAct->getSurgGraphics()->setNewTopology();
		_surgAct->newTopology = false;
	}
	int nv;
	auto pArr = _mt->getPositionArrayPtr();
	nv = pArr->size();
	const int nt = _vnTets.tetNumber();
	for (int i = 0; i < nv; ++i) {
		const int tet = _vnTets.getVertexTetrahedron(i);
		if (tet < 0)  // an excision may have occurred leaving an empty vertex
			continue;
		if (tet >= nt)  // a tet index from a lattice this vertex no longer belongs to would be read past the tet array
			continue;
		auto ov = _vertexTetOverride.find(i);  // a flap wall bottom the re-cut left in the bed's tet is drawn from its flap tet
		if (ov != _vertexTetOverride.end() && ov->second.first >= 0 && ov->second.first < nt)
			_vnTets.getBarycentricTetPosition(ov->second.first, ov->second.second, pArr->at(i));
		else
			_vnTets.getBarycentricTetPosition(tet, *(_vnTets.getVertexWeight(i)), pArr->at(i));
	}
	_surgAct->getSurgGraphics()->updatePositionsNormalsTangents();
	if (_gl3w->getLines()->linesVisible())
		drawTetLattice();
}

 void bccTetScene::fixPeriostealPeriferalVertices()
{  // this routine should be done only once after original lattice constructed
	struct anchorPoint {
		bool isPeriferal;
		std::array<float, 3> baryWeight, pos;
	}ap;
	std::unordered_map<int, anchorPoint> fixPoints;  // key is tet index. At present only oneper tet.
	// fixed nodes take precedence over periferal nodes
	// Fix all nodes surrounding a periosteal or periferalvertex.
	auto enterFixPoint = [&](int vId, bool periferal) {
		const Vec3f* vp = _vnTets.getVertexWeight(vId);
		ap.baryWeight[0] = vp->X;
		ap.baryWeight[1] = vp->Y;
		ap.baryWeight[2] = vp->Z;
		_vnTets.vertexMaterialCoordinate(vId, ap.pos);
		ap.isPeriferal = periferal;
		fixPoints.insert(std::make_pair(_vnTets.getVertexTetrahedron(vId), ap));
	};
	for (int n = _mt->numberOfTriangles(), i = 0; i < n; ++i) {
		if (_mt->triangleMaterial(i) == 7) {  // periosteal triangle
			for (int k = 0; k < 3; ++k) {
				int vIdx = _mt->triangleVertices(i)[k];
				enterFixPoint(vIdx, false);
			}
		}
		if (_mt->triangleMaterial(i) == 1) {  // periosteal triangle
			for (int k = 0; k < 3; ++k) {
				int vIdx = _mt->triangleVertices(i)[k];
				enterFixPoint(vIdx, true);
			}
		}
	}
	size_t n = fixPoints.size();
	std::vector<int> fixedTets, peripheralTets;
	fixedTets.reserve(n);
	peripheralTets.reserve(n);
	std::vector<std::array<float, 3> > fixedWeights, peripheralWeights, fixedPos, peripheralPos;
	fixedWeights.reserve(n);
	peripheralWeights.reserve(n);
	fixedPos.reserve(n);
	peripheralPos.reserve(n);
	for (auto &fp : fixPoints) {
		if (fp.second.isPeriferal) {
			peripheralTets.push_back(fp.first);
			peripheralWeights.push_back(fp.second.baryWeight);
			peripheralPos.push_back(fp.second.pos);
		}
		else {
			fixedTets.push_back(fp.first);
			fixedWeights.push_back(fp.second.baryWeight);
			fixedPos.push_back(fp.second.pos);
		}
	}
#ifndef NO_PHYSICS
	_ptp.setFixedVertices(fixedTets, fixedWeights, fixedPos, peripheralTets, peripheralWeights, peripheralPos);
#endif
}

void bccTetScene::createTetLatticeDrawing()
{
	if (_nodeGraphicsPositions.size() == _vnTets.nodeNumber())
		return;
	_nodeGraphicsPositions.clear();
	_nodeGraphicsPositions.assign(_vnTets.nodeNumber() << 2, 1.0f);
	GLfloat *ngp = &_nodeGraphicsPositions[0];
	for (int n = _vnTets.nodeNumber(), i = 0; i < n; ++i){
		const float *fp = _vnTets.nodeSpatialCoordinatePtr(i);
		*(ngp++) = fp[0];
		*(ngp++) = fp[1];
		*(ngp++) = fp[2];
		++ngp;
	}
	std::set<std::pair<int, int> > segs;
	for (int n = _vnTets.tetNumber(), i=0; i<n; ++i){  // numberOfMegaTets()
		std::pair<int, int> ll;
		const int* tetNodes = _vnTets.tetNodes(i);
		for (int j = 0; j < 3; ++j){
			for (int k = j+1; k < 4; ++k){
				ll.first = std::min(tetNodes[j], tetNodes[k]);
				ll.second = std::max(tetNodes[j], tetNodes[k]);
				segs.insert(ll);
			}
		}
	}
	std::vector<GLuint> lines;
	lines.reserve(segs.size() * 3);
	for (auto &s : segs){
		lines.push_back(s.first);
		lines.push_back(s.second);
		lines.push_back(0xffffffff);
	}
	_gl3w->getLines()->setGl3wGraphics(_gl3w);
	float white2[4] = {1.0f, 1.0f, 1.0f, 1.0f};
	_gl3w->getLines()->addLines(_nodeGraphicsPositions, lines);
	_gl3w->getLines()->getSceneNode()->setColor(white2);
}

void bccTetScene::eraseTetLattice()
{
	_nodeGraphicsPositions.clear();
	_gl3w->getLines()->clear();
	_gl3w->getLines()->getSceneNode()->visible = false;
}

void bccTetScene::drawTetLattice()
{
	if (_nodeGraphicsPositions.empty())
		return;
	GLfloat *ngp = &_nodeGraphicsPositions[0];
	for (int n = _vnTets.nodeNumber(), i = 0; i < n; ++i){
		const float *fp = _vnTets.nodeSpatialCoordinatePtr(i);
		*(ngp++) = fp[0];
		*(ngp++) = fp[1];
		*(ngp++) = fp[2];
		++ngp;
	}
	_gl3w->getLines()->updatePoints(_nodeGraphicsPositions);
}

bccTetScene::bccTetScene() : _physicsPaused(false), _forcesApplied(false), _tetsModified(false)
{
	_tetCol.setPdTetPhysics(&_ptp); // Qisi:set ptp for tetCol so things of ptp are accessible inside of tetCol
}


bccTetScene::~bccTetScene()
{
}

// ---- whole-state undo support ----------------------------------------------

unsigned bccTetScene::s_latticeGeneration = 0;
std::weak_ptr<const bccTetScene::latticeCore> bccTetScene::s_lastCore;
unsigned bccTetScene::s_lastCoreGeneration = 0;
uint64_t bccTetScene::s_lastCoreVertexKey = 0;

// A knife cut on un-undermined skin adds vertices (and their tet assignments
// and weights) to the lattice without any rebuild, so the rebuild generation
// alone cannot decide whether a snapshot may share the last lattice core.
uint64_t bccTetScene::vertexTableKey()
{
	uint64_t h = 1469598103934665603ULL ^ (uint64_t)_vnTets.vertexNumber();
	for (int n = _vnTets.vertexNumber(), i = 0; i < n; ++i) {
		const int tet = _vnTets.getVertexTetrahedron(i);
		const unsigned char* b = reinterpret_cast<const unsigned char*>(&tet);
		for (int k = 0; k < 4; ++k) { h ^= b[k]; h *= 1099511628211ULL; }
	}
	return h;
}

void bccTetScene::saveLatticeState(latticeState& s)
{
	std::shared_ptr<const latticeCore> core = s_lastCore.lock();
	const uint64_t vkey = vertexTableKey();
	if (!core || s_lastCoreGeneration != s_latticeGeneration || s_lastCoreVertexKey != vkey) {
		auto fresh = std::make_shared<latticeCore>();
		fresh->vnTets = std::make_unique<vnBccTetrahedra>(_vnTets);
		fresh->tc = std::make_unique<vnBccTetCutter_tbb>(_tc);
		fresh->rtp = std::make_unique<remapTetPhysics>(_rtp);
		core = fresh;
		s_lastCore = core;
		s_lastCoreGeneration = s_latticeGeneration;
		s_lastCoreVertexKey = vkey;
	}
	s.core = core;
	s.generation = s_latticeGeneration;
	s.nodePos.resize(_vnTets.nodeNumber());
	for (int n = _vnTets.nodeNumber(), i = 0; i < n; ++i) {
		const Vec3f& p = _vnTets.nodeSpatialCoordinate(i);
		s.nodePos[i] = { p[0], p[1], p[2] };
	}
	s.forcesApplied = _forcesApplied;
}

// Mirrors updateOldPhysicsLattice() from the point where the lattice exists,
// except that node positions come from the snapshot instead of a remap.
void bccTetScene::restoreLatticeState(const latticeState& s)
{
	_vnTets = *s.core->vnTets;
	_tc = *s.core->tc;
	_tc.setRemapTetPhysics(&_rtp);
	_rtp = *s.core->rtp;
	// the live lattice now equals this core: later snapshots can share it
	++s_latticeGeneration;
	s_lastCore = s.core;
	s_lastCoreGeneration = s_latticeGeneration;
	s_lastCoreVertexKey = vertexTableKey();
	std::vector<uint8_t> tetSizeMult;
	tetSizeMult.reserve(_vnTets.tetNumber());
	for (int n = _vnTets.tetNumber(), i = 0; i < n; ++i) {
		uint8_t sizeBit = 1;
		auto& c = _vnTets.tetCentroid(i);
		unsigned short ored = c[0] | c[1] | c[2];
		while (true) {
			if (ored & sizeBit)
				break;
			sizeBit <<= 1;
		}
		tetSizeMult.push_back(sizeBit);
	}
	std::array<float, 3>* nodeSpatialCoords = _ptp.createBccTetStructure_multires(_vnTets.getTetNodeArray(), tetSizeMult, (float)_vnTets.getTetUnitSize());
	_vnTets.setNodeSpatialCoordinatePointer(nodeSpatialCoords);
	if ((int)s.nodePos.size() != _vnTets.nodeNumber())
		throw(std::logic_error("Undo snapshot node positions do not match its lattice.\n"));
	std::copy(s.nodePos.begin(), s.nodePos.end(), nodeSpatialCoords);
	std::vector<int> subNodes;
	std::vector<std::vector<int> > macroNodes;
	std::vector<std::vector<float> > macroBarys;
	_vnTets.getTJunctionConstraints(subNodes, macroNodes, macroBarys);
	_ptp.addInterNodeConstraints(subNodes, macroNodes, macroBarys);
	_tetSubsets.sendTetSubsets(&_vnTets, _mt, &_ptp);
	rehomeFlapWallBottoms();  // the drawing override belongs to this lattice
	_forcesApplied = s.forcesApplied;
	if (_forcesApplied) {
		initPdPhysics();
		_tetsModified = true;
	}
	_physicsPaused = false;
}
