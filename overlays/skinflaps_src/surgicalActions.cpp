#include "gl3wGraphics.h"
#include "Vec3f.h"
#include "Mat2x2f.h"
#include <sstream>
#include <fstream>
#include <algorithm>
#include <exception>
#include <chrono>
#include <thread>
#include <assert.h>
#include "insidePolygon.h"
#include "prettyPrintJSON.h"
#include "surgGraphics.h"
#include <tbb/task_arena.h>
#include "FacialFlapsGui.h"
#include "surgicalActions.h"
#include <memory>
#include <list>
#include <map>
#include <stdexcept>

// Whole-state undo/redo. Every forward move — a post placed or re-angled, an
// undermine or periosteal point, a hook placed / grabbed / deleted, a suture
// placed / deleted, and every Enter commit or excise — records the complete
// solving state at its start; Cmd+Z steps back through those records and
// Cmd+Shift+Z steps forward again. A record holds the surface mesh, the tet
// lattice (its core shared between records while the topology is unchanged)
// with every node position, the cutter's incremental bookkeeping, the remap
// state, the STATIC deep-bed map, the incision object, hooks, sutures, pending
// fence posts, pending undermine / periosteal marks, the selection, the active
// tool and the history. Restore assigns all of it back and rebuilds physics
// from the restored lattice (bccTetScene::restoreLatticeState), so the solve
// simply continues from the captured positions: the solver is quasi-static, so
// positions plus constraints are its whole dynamic state.
struct undoFencePost { Vec3f xyz, nrm, spherePos; int triangle = -1; bool connectToEdge = false, openEnd = false, hasSphere = false; };
struct undoFenceAccess {
	static std::vector<undoFencePost> capture(fence& f) {
		std::vector<undoFencePost> out;
		for (auto& fp : f._posts) {
			undoFencePost u; u.xyz = fp.xyz; u.nrm = fp.nrm; u.spherePos = fp.spherePos; u.triangle = fp.triangle;
			u.connectToEdge = fp.connectToEdge; u.openEnd = fp.openEnd; u.hasSphere = (bool)fp.sphereShape;
			out.push_back(u);
		}
		return out;
	}
	static void restore(fence& f, materialTriangles* tr, gl3wGraphics* gl3w, float fenceSize, const std::vector<undoFencePost>& posts) {
		f.clear();
		if (posts.empty())
			return;
		if (!f.isInitialized()) {
			f.setFenceSize(fenceSize);
			f.setGl3wGraphics(gl3w);
		}
		for (size_t i = 0; i < posts.size(); ++i) {
			float xyz[3] = { posts[i].xyz[0], posts[i].xyz[1], posts[i].xyz[2] }, nrm[3] = { posts[i].nrm[0], posts[i].nrm[1], posts[i].nrm[2] };
			f.addPost(tr, posts[i].triangle, xyz, nrm, posts[i].connectToEdge, posts[i].hasSphere, posts[i].openEnd);
			if (posts[i].hasSphere) { Vec3f sp = posts[i].spherePos; f.setSpherePos((int)i, sp); }
		}
	}
};

struct surgicalActions::UndoSnapshot {
	std::unique_ptr<materialTriangles> mesh;
	std::unique_ptr<bccTetScene::latticeState> lattice;
	std::unique_ptr<deepCut> incisions;
	std::unique_ptr<skinCutUndermineTets::deepBedState> deepBed;  // the cutter's STATIC deep-bed map
	std::unique_ptr<hooks> hooksCopy;
	std::unique_ptr<sutures> suturesCopy;
	std::vector<undoFencePost> fencePosts;
	std::vector<undermineTriangle> undermineMarks;
	std::list<perioTri> periostealMarks;
	std::string selectedObject;
	int toolState = 0;
	json::Array history;
	size_t historyPos = (size_t)-1;  // iterator offset into history at snapshot time; -1 = end (interactive)
	std::string action;
};

namespace {
std::vector<surgicalActions::UndoSnapshot> g_undoStack;  // records of the state BEFORE each forward move, newest last
std::vector<surgicalActions::UndoSnapshot> g_redoStack;  // records of the state an undo left, newest last
std::vector<surgicalActions::UndoSnapshot> g_redoCleared;  // the redo stack the latest forward move cleared; a withdrawn attempt (deep cut refused before any change) puts it back
constexpr size_t kUndoDepth = 256;

// Full trail: every action committed in this session plus an "undo"/"redo"
// record for each restore, so a saved trail replays the session INCLUDING its
// undos (the plain history keeps only the actions still in effect). Synced
// lazily from the live history by executed position.
json::Array g_fullTrail;
size_t g_trailSynced = 0;
// The wall clock of every state change (forward move, undo, redo, error revert)
// with the physics solves since the previous one, so a replay can be paced like
// the session. Logged as [action-clock]; written next to every saved history as
// <name>.clock.json.
namespace {
	struct ActionClock { size_t trailIndex; std::string what; double t, dt; int solves; };
	std::vector<ActionClock> g_actionClock;
	std::chrono::steady_clock::time_point g_clockStart;
	bool g_clockStarted = false;
	double g_clockLastT = 0.0;
	int g_clockLastSolves = 0;
}
static void noteActionClockImpl(const char* what)
{
	const auto now = std::chrono::steady_clock::now();
	if (!g_clockStarted) { g_clockStart = now; g_clockStarted = true; }
	const double t = std::chrono::duration<double>(now - g_clockStart).count();
	const int solves = bccTetScene::solveCount();
	ActionClock c{ g_fullTrail.size(), what ? what : "", t, g_actionClock.empty() ? 0.0 : t - g_clockLastT, g_actionClock.empty() ? 0 : solves - g_clockLastSolves };
	g_clockLastT = t;
	g_clockLastSolves = solves;
	g_actionClock.push_back(c);
	fprintf(stderr, "[action-clock] #%zu %s at %.2fs (dt %.2fs, %d solves since the previous)\n", g_actionClock.size(), c.what.c_str(), t, c.dt, c.solves);
}
// cleanIndices: the sidecar beside a plain history (the actions in effect) must
// index that history's records, not the full trail's positions, or a paced
// replay times the wrong records.  The mapping walks the trail: an action
// record survives until an undo takes it back (a redo brings it back); reverts
// and the undo/redo notes of record-less moves are skipped.
static void writeActionClock(const char* historyPath, bool cleanIndices = false)
{
	if (g_actionClock.empty() || !historyPath) return;
	std::string p(historyPath);
	if (p.size() > 4 && p.compare(p.size() - 4, 4, ".hst") == 0) p.resize(p.size() - 4);
	p += ".clock.json";
	std::map<size_t, int> cleanPos;
	if (cleanIndices) {
		std::vector<size_t> live, redo;
		for (size_t t = 0; t < g_fullTrail.size(); ++t) {
			const json::Value& v = g_fullTrail[t];
			if (v.HasKey("revert")) continue;
			if (v.HasKey("undo") || v.HasKey("redo")) {
				const bool isUndo = v.HasKey("undo");
				json::Object u = (isUndo ? v["undo"] : v["redo"]).ToObject();
				if (u.HasKey("noRecord")) continue;
				if (isUndo) { if (!live.empty()) { redo.push_back(live.back()); live.pop_back(); } }
				else { if (!redo.empty()) { live.push_back(redo.back()); redo.pop_back(); } }
				continue;
			}
			live.push_back(t);
			redo.clear();
		}
		for (size_t i = 0; i < live.size(); ++i) cleanPos[live[i]] = (int)i;
	}
	std::ofstream o(p);
	if (!o.is_open()) return;
	std::vector<std::string> lines;
	for (size_t i = 0; i < g_actionClock.size(); ++i) {
		const ActionClock& c = g_actionClock[i];
		int idx = (int)c.trailIndex;
		if (cleanIndices) {
			if (c.what.rfind("undo:", 0) == 0 || c.what.rfind("redo:", 0) == 0 || c.what.rfind("error revert:", 0) == 0) continue;
			auto it = cleanPos.find(c.trailIndex);
			if (it == cleanPos.end()) continue;  // a move that was later undone: not in this history
			idx = it->second;
		}
		std::string w; for (char ch : c.what) { if (ch == '"' || ch == '\\') w += '\\'; w += ch; }
		std::ostringstream os;
		os << "  {\"i\": " << idx << ", \"action\": \"" << w << "\", \"t\": " << c.t << ", \"dt\": " << c.dt << ", \"solves\": " << c.solves << "}";
		lines.push_back(os.str());
	}
	o << "[\n";
	for (size_t i = 0; i < lines.size(); ++i) o << lines[i] << (i + 1 < lines.size() ? ",\n" : "\n");
	o << "]\n";
}
void trailSync(json::Array& history, size_t executed) {
	if (executed > history.size()) executed = history.size();
	if (executed < g_trailSynced) g_trailSynced = executed;  // history shrank without a recorded undo (e.g. a load)
	auto it = history.begin();
	std::advance(it, g_trailSynced);
	for (size_t i = g_trailSynced; i < executed; ++i, ++it) g_fullTrail.push_back(*it);
	g_trailSynced = executed;
}
void trailNote(const char* kind, const std::string& action, size_t restoredLength, bool noRecord = false) {
	json::Object u; u["action"] = action; u["historyLength"] = (int)restoredLength;
	if (noRecord) u["noRecord"] = true;  // the move undone/redone never reached the history (a post, a mark, a grab): a replay skips this note
	json::Object rec; rec[kind] = u;
	g_fullTrail.push_back(rec);
	g_trailSynced = restoredLength;
}
}  // namespace

void surgicalActions::noteActionClock(const char* what)
{
	noteActionClockImpl(what);
}
bool skinflapsUndoRestoreSucceeded = false;
namespace { bool g_quietUndo = false; }  // undoRedoSteps: one message for the whole gesture
// What the last state change was, so an error hold raised after an undo
// reverts to the state that undo left (the redo stack's top) rather than one step further back
namespace { enum class MoveKind { forward, undo, redo }; MoveKind g_lastMoveKind = MoveKind::forward; }
bool skinflapsLastMoveWasUndo() { return g_lastMoveKind == MoveKind::undo; }
bool skinflapsHoldRevertAvailable() { return g_lastMoveKind == MoveKind::undo ? !g_redoStack.empty() : !g_undoStack.empty(); }

std::vector<std::string> surgicalActions::undoActionNames(int maxCount) const
{
	std::vector<std::string> v;
	for (auto it = g_undoStack.rbegin(); it != g_undoStack.rend() && (int)v.size() < maxCount; ++it) v.push_back(it->action);
	return v;
}
std::vector<std::string> surgicalActions::redoActionNames(int maxCount) const
{
	std::vector<std::string> v;
	for (auto it = g_redoStack.rbegin(); it != g_redoStack.rend() && (int)v.size() < maxCount; ++it) v.push_back(it->action);
	return v;
}
void surgicalActions::undoRedoSteps(int key, int steps)
{
	const bool redo = (key == SKINFLAPS_REDO_KEY);
	std::string last;
	g_quietUndo = true;
	int done = 0;
	try {
		for (; done < steps && !(redo ? g_redoStack : g_undoStack).empty(); ++done) {
			last = (redo ? g_redoStack : g_undoStack).back().action;
			undoRedo(key);
		}
	}
	catch (...) { g_quietUndo = false; throw; }
	g_quietUndo = false;
	if (done > 0) {
		std::string msg = (redo ? "Redid " : "Undid ") + std::to_string(done) + (done == 1 ? " move" : " moves") + (redo ? " through: " : " back to before: ") + last;
		sendUserMessage(msg.c_str(), redo ? "Redo" : "Undo");
	}
}
bool skinflapsUndoAvailable() { return !g_undoStack.empty(); }

surgicalActions::UndoSnapshot surgicalActions::captureSnapshot(const char* what, size_t historyPos)
{
	UndoSnapshot s;
	s.mesh = std::make_unique<materialTriangles>(*_sg.getMaterialTriangles());
	s.lattice = std::make_unique<bccTetScene::latticeState>();
	_bts.saveLatticeState(*s.lattice);
	s.incisions = std::make_unique<deepCut>(_incisions);
	s.deepBed = skinCutUndermineTets::saveDeepBedState();
	s.hooksCopy = std::make_unique<hooks>(_hooks);
	s.suturesCopy = std::make_unique<sutures>(_sutures);
	s.fencePosts = undoFenceAccess::capture(_fence);
	s.undermineMarks = _undermineTriangles;
	s.periostealMarks = _periostealUndermineTriangles;
	s.selectedObject = _selectedSurgObject;
	s.toolState = _toolState;
	s.history = _historyArray;
	s.historyPos = historyPos;
	s.action = what;
	return s;
}

void surgicalActions::restoreSnapshot(const UndoSnapshot& snap)
{
	materialTriangles* mt = _sg.getMaterialTriangles();
	*mt = *snap.mesh;
	_incisions = *snap.incisions;
	skinCutUndermineTets::restoreDeepBedState(*snap.deepBed);  // static: every undone cut/undermine/excise edited it in place
	_hooks.restoreState(*snap.hooksCopy);
	if (snap.suturesCopy) {
		auto sn = _sg.getSceneNode();
		if (_sutures.getNumberOfSutures() < 1 && snap.suturesCopy->getNumberOfSutures() > 0) {  // first-use initialization, as the suture tool does
			_sutures.setSutureSize(sn->getRadius() * 0.003f);
			_sutures.setShapes(_gl3w->getShapes());
			_sutures.setGLmatrices(_gl3w->getGLmatrices());
			_sutures.setPhysicsLattice(_bts.getPdTetPhysics_2());
			_sutures.setVnBccTetrahedra(_bts.getVirtualNodedBccTetrahedra());
			_sutures.setSurgicalActions(this);
		}
		_sutures.restoreState(*snap.suturesCopy);
	}
	// The lattice is reinstated from the snapshot rather than recut: the
	// rebuild is incremental (it assumes the surface only grows). Physics is
	// rebuilt from the restored lattice with the snapshot's node positions, then
	// hooks and sutures are rebound to it.
	_bts.restoreLatticeState(*snap.lattice);
	_historyArray = snap.history;
	_historyIt = (snap.historyPos < _historyArray.size()) ? _historyArray.begin() + snap.historyPos : _historyArray.end();
	_undermineTriangles = snap.undermineMarks;
	_periostealUndermineTriangles = snap.periostealMarks;
	_selectedSurgObject = snap.selectedObject;
	auto sn = _sg.getSceneNode();
	undoFenceAccess::restore(_fence, mt, _gl3w, sn ? sn->getRadius() * 0.02f : 1.0f, snap.fencePosts);
	_hooks.selectHook(-1);
	_sutures.selectSuture(-1);
	if (_selectedSurgObject.compare(0, 2, "H_") == 0)
		_hooks.selectHook(atoi(_selectedSurgObject.c_str() + 2));
	else if (_selectedSurgObject.compare(0, 2, "S_") == 0)
		_sutures.selectSuture(atoi(_selectedSurgObject.c_str() + 2));
	else if (_selectedSurgObject.compare(0, 3, "NP_") == 0) {
		// _selectedSurgObject is not cleared between tools, so a snapshot can carry a
		// post selection from an earlier deep cut while its fence holds knife
		// posts (no spheres) or fewer posts; fence::selectPost() colours every
		// post's sphere unguarded. Re-select only a post that exists in this
		// state with a sphere; otherwise drop the stale selection.
		const int k = atoi(_selectedSurgObject.c_str() + 3);
		bool allSpheres = !snap.fencePosts.empty();
		for (auto& fp : snap.fencePosts) allSpheres = allSpheres && fp.hasSphere;
		if (allSpheres && k >= 0 && k < _fence.numberOfPosts())
			_fence.selectPost(k);
		else
			_selectedSurgObject.clear();
	}
	_ffg->setToolState(snap.toolState);
	setToolState(snap.toolState);  // pauses physics while a surgical tool is active, as a tool selection does
	// No updateSurfaceDraw() here: it would overwrite the exact restored vertex
	// positions with a lattice reconstruction. The main loop rebuilds the GL
	// topology and refreshes positions from newTopology.
	newTopology = true;
	physicsDone = true;
}

void surgicalActions::snapshotForwardMove(const char* what, size_t historyPos)
{
	// A GUI forward move made part-way through a loaded history (Next pressed
	// N times, then a tool used) must record the iterator position, or a restore
	// would put the iterator at the end of the still-untruncated array and the
	// unexecuted tail would count as executed.  The replay path passes it.
	if (historyPos == (size_t)-1 && _historyArray.size() > 0 && _historyIt != _historyArray.end())
		historyPos = (size_t)(_historyIt - _historyArray.begin());
	trailSync(_historyArray, historyPos == (size_t)-1 ? _historyArray.size() : historyPos);
	noteActionClock(what);
	const bool wasPaused = _bts.isPhysicsPaused();
	_bts.setPhysicsPause(true);
	while (!physicsDone)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	// a new forward move invalidates what was undone; the deep-cut commit alone
	// keeps it aside, because its refusal path may withdraw the move (see there)
	if (std::string(what) == "deep cut")
		g_redoCleared = std::move(g_redoStack);
	else
		g_redoCleared.clear();
	g_redoStack.clear();
	if (g_undoStack.size() >= kUndoDepth)
		g_undoStack.erase(g_undoStack.begin());
	g_undoStack.push_back(captureSnapshot(what, historyPos));
	g_lastMoveKind = MoveKind::forward;
	_bts.setPhysicsPause(wasPaused);
}

void surgicalActions::undoRedo(int key)
{
	const bool afterError = (key == SKINFLAPS_UNDO_AFTER_ERROR_KEY);
	const bool redo = (key == SKINFLAPS_REDO_KEY);
	skinflapsUndoRestoreSucceeded = false;
	// A hold raised right after an undo (a runaway solve on the restored lattice)
	// reverts to the state that undo left, which sits on the redo stack
	const bool revertOfUndo = afterError && g_lastMoveKind == MoveKind::undo;
	std::vector<UndoSnapshot>& from = (redo || revertOfUndo) ? g_redoStack : g_undoStack;
	if (from.empty()) {
		if (!afterError)
			sendUserMessage(redo ? "Nothing to redo-" : "Nothing to undo-", redo ? "Redo" : "Undo");
		return;
	}
	const bool wasPaused = _bts.isPhysicsPaused();
	_bts.setPhysicsPause(true);
	while (!physicsDone)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	try {
		UndoSnapshot target = std::move(from.back());
		from.pop_back();
		const size_t executed = _historyIt == _historyArray.end() ? _historyArray.size() : (size_t)(_historyIt - _historyArray.begin());
		trailSync(_historyArray, executed);
		if (!afterError) {
			// the state being left becomes the top of the opposite stack
			(redo ? g_undoStack : g_redoStack).push_back(captureSnapshot(target.action.c_str(), executed));
		}
		// the executed position after the restore, not the array length: a move made
		// part-way through a loaded history restores the iterator to where it was
		noteActionClock((std::string(afterError ? "error revert: " : redo ? "redo: " : "undo: ") + target.action).c_str());
		// An error revert of a move that never reached the history (a hook drag, a
		// post placement) is written as a "revert" record, which a replay skips:
		// written as "undo" it would make the replay pop the previous move instead.
		// A revert of a move that IS in the history stays an "undo" record, since
		// the replay has to take that record back.
		const size_t restoredLength = target.historyPos < target.history.size() ? target.historyPos : target.history.size();
		if (afterError && restoredLength == executed)
			trailNote("revert", target.action, restoredLength);
		else
			trailNote(redo ? "redo" : "undo", target.action, restoredLength, restoredLength == executed);  // noRecord: a post, a mark or a grab was undone/redone, nothing a replay holds
		restoreSnapshot(target);
		skinflapsUndoRestoreSucceeded = true;
		g_lastMoveKind = afterError ? MoveKind::forward : (redo ? MoveKind::redo : MoveKind::undo);
		if (!afterError) {
			if (!g_quietUndo) {
				std::string msg = (redo ? "Redid: " : "Undid: ") + target.action;
				sendUserMessage(msg.c_str(), redo ? "Redo" : "Undo");
			}
		}
	}
	catch (std::exception& e) {
		physicsDone = true;
		_bts.setPhysicsPause(wasPaused);
		skinflapsUndoRestoreSucceeded = false;
		if (!afterError)
			throw;
	}
}

surgicalActions::surgicalActions() : _toolState(0), _originalTriangleNumber(0), _sceneDir("0"), _historyDir("0"), _strongHooks(false), physicsDone(true), newTopology(false), taskThreadError(false)
{
	_bts.setSurgicalActions(this);
	_historyArray.Clear();
	_historyIt = _historyArray.begin();
	_undermineTriangles.clear();
	_periostealUndermineTriangles.clear();
	_x=0.0f; _y=0.0f; _z=0.0f; _u=0.0f; _f=0.0f; _r=0.0f;
}

surgicalActions::~surgicalActions()
{
}

// The full trail (see g_fullTrail): the actions still in effect plus every
// undone action and an "undo"/"redo"/"revert" record where each restore happened.
bool surgicalActions::saveFullHistoryTrail(const char *fullFilePath)
{
	trailSync(_historyArray, _historyIt == _historyArray.end() ? _historyArray.size() : (size_t)(_historyIt - _historyArray.begin()));
	std::ofstream outf(fullFilePath);
	if (!outf.is_open())
		return false;
	outf << Serialize(g_fullTrail);
	outf.close();
	writeActionClock(fullFilePath);  // the clock's indices are trail positions, so the sidecar belongs here too
	return true;
}

// A hook drag reaches the history only at its release (rightMouseUp writes the
// moveHook record).  When the physics fails during the drag, the hold saves the
// error files before the release ever comes, so the move that caused the failure
// would be missing from them and from the full trail.  Called at the start of
// an error hold: when the last forward move is a hook grab that has no record
// yet, the hook's current position is written as that record, exactly as the
// release would have written it.  The revert then takes it back (the trail
// keeps it with an "undo").
bool surgicalActions::recordInFlightHookMove()
{
	if (g_undoStack.empty() || g_undoStack.back().action != "hook move")
		return false;
	if (_selectedSurgObject.compare(0, 2, "H_") != 0)
		return false;
	const size_t executed = _historyIt == _historyArray.end() ? _historyArray.size() : (size_t)(_historyIt - _historyArray.begin());
	const UndoSnapshot& top = g_undoStack.back();
	const size_t grabLength = top.historyPos < top.history.size() ? top.historyPos : top.history.size();
	if (executed != grabLength)
		return false;  // the move is already in the history (a replayed record, or a drag that was released)
	const int hookNum = atoi(_selectedSurgObject.c_str() + 2);
	Vec3f xyz;
	if (!_hooks.getHookPosition(hookNum, xyz.xyz))
		return false;
	if (_historyIt != _historyArray.end()) {  // as rightMouseUp: the unexecuted tail of a loaded history goes
		json::Array tarr;
		for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
			tarr.push_back(*it);
		_historyArray.Clear();
		_historyArray = tarr;
	}
	json::Array hArr;
	hArr.push_back(hookNum);
	hArr.push_back((double)xyz.xyz[0]);
	hArr.push_back((double)xyz.xyz[1]);
	hArr.push_back((double)xyz.xyz[2]);
	json::Object mObj;
	mObj["moveHook"] = hArr;
	_historyArray.push_back(mObj);
	_historyIt = _historyArray.end();
	return true;
}

// A suture reaches the history at its release, after setSecondEdge and the
// auto-row; a throw inside that release (the row, the physics init) saves the
// error files without the suture.  When both points are on the suture object,
// write the record exactly as the release would have.
bool surgicalActions::recordInFlightSuture()
{
	if (_toolState != 4 || _selectedSurgObject.compare(0, 2, "S_") != 0)
		return false;
	const int i = atoi(_selectedSurgObject.c_str() + 2);
	materialTriangles* tr0 = nullptr; materialTriangles* tr1 = nullptr;
	int tri0 = -1, tri1 = -1, edge0 = -1, edge1 = -1;
	float p0 = -1.0f, p1 = -1.0f;
	if (!_sutures.getEdgeAttachment(i, true, tr0, tri0, edge0, p0) || tr0 == nullptr || tri0 < 0 || edge0 < 0 || edge0 > 2)
		return false;
	if (!_sutures.getEdgeAttachment(i, false, tr1, tri1, edge1, p1) || tr1 == nullptr || tri1 < 0 || tri1 >= tr1->numberOfTriangles() || edge1 < 0 || edge1 > 2) {
		return false;  // only the first point is placed: nothing to record
	}
	const int sNum = _sutures.baseToUserSutureNumber(i);
	if (sNum < 0)
		return false;
	auto uvOf = [](int edge, float param, float (&uv)[2]) {
		param += (param < 0.002f) ? 0.001f : -0.001f;
		if (edge < 1) { uv[0] = param; uv[1] = 0.001f; }
		else if (edge > 1) { uv[0] = 0.001f; uv[1] = 1.0f - param; }
		else { uv[1] = param; uv[0] = 0.999f - param; }
	};
	float uv[2], hTx[2];
	int material;
	Vec3f hVec;
	json::Array hArr;
	json::Object pObj, sutureTitle;
	uvOf(edge1, p1, uv);
	if (!setHistoryAttachPoint(tri1, uv, material, hTx, hVec))
		return false;
	pObj["sutureNum"] = sNum;
	pObj["linked"] = _sutures.isLinked(i);
	pObj["material1"] = material;
	hArr.push_back(hTx[0]); hArr.push_back(hTx[1]); pObj["historyTexture1"] = hArr; hArr.Clear();
	hArr.push_back(hVec[0]); hArr.push_back(hVec[1]); hArr.push_back(hVec[2]); pObj["displacement1"] = hArr; hArr.Clear();
	uvOf(edge0, p0, uv);
	if (!setHistoryAttachPoint(tri0, uv, material, hTx, hVec))
		return false;
	pObj["material0"] = material;
	hArr.push_back(hTx[0]); hArr.push_back(hTx[1]); pObj["historyTexture0"] = hArr; hArr.Clear();
	hArr.push_back(hVec[0]); hArr.push_back(hVec[1]); hArr.push_back(hVec[2]); pObj["displacement0"] = hArr;
	sutureTitle["addSuture"] = pObj;
	if (_historyIt != _historyArray.end()) {
		json::Array tarr;
		for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
			tarr.push_back(*it);
		_historyArray.Clear();
		_historyArray = tarr;
	}
	_historyArray.push_back(sutureTitle);
	_historyIt = _historyArray.end();
	return true;
}

bool surgicalActions::saveSurgicalHistory(const char *fullFilePath)
{
	std::string ppStr, hstStr;
	if (_historyIt != _historyArray.end()) {
		json::Array htmp;
		size_t idx = 0;
		auto hit = _historyArray.begin();
		if (hit == _historyIt)
			htmp.insert(idx++, *hit);
		else {
			do {
				htmp.insert(idx++, *hit);
				++hit;
			} while (hit != _historyIt);
		}
		hstStr = Serialize(htmp);
	}
	else
		hstStr = Serialize(_historyArray);
	std::ofstream outf(fullFilePath);
	if (!outf.is_open()) {
		_ffg->sendUserMessage("Can't save to this filename (demos are read only).\n\nPlease create another name for your history file-\n", "History Save Error");
		return false;
	}
	prettyPrintJSON pp;
	pp.convert(hstStr.c_str(), ppStr);
	outf.write(ppStr.c_str(), ppStr.size());
    outf.close();
	trailSync(_historyArray, _historyIt == _historyArray.end() ? _historyArray.size() : (size_t)(_historyIt - _historyArray.begin()));
	writeActionClock(fullFilePath, true);  // <name>.clock.json beside the history, indexed by this history's records
	return true;
}

void surgicalActions::sendUserMessage(const char *message, const char *title, bool closeProgram)
{
	_ffg->sendUserMessage(message, title);
}

// Guard for skinCutUndermineTets::excise. When the surface flood from the
// excise seed exceeds half the model, excise assumes an enclosed-but-not-
// undermined subpatch, marks the surrounding skin for undermining, and calls
// undermineSkin() — whose border-ring walk requires the marked patch to be
// bounded. A seed whose material-2 patch spans most of the skin sheet (uncut
// tissue, or an enclosure whose closure left an unsevered edge) has no such
// bound and the walk runs off the ring, so the excise is refused before any
// state or history mutation. Knife-incision boundaries are adjacent
// material-3 wall triangles with intact adjacency, not the ==3 no-adjacency
// sentinel, so boundedness is judged by patch size alone. The floods below
// mirror excise's own two flood fills, read-only.
static bool exciseRegionIsBounded(materialTriangles *tr, int triangle)
{
	int nTris = tr->numberOfTriangles();
	std::vector<bool> visited(nTris, false);
	std::list<int> rList;
	rList.push_back(triangle);
	visited[triangle] = true;
	int nConnected = 1, halfTris = nTris >> 1;
	while (!rList.empty()) {
		unsigned int *adjs = tr->triAdjs(rList.front());
		rList.pop_front();
		for (int j = 0; j < 3; ++j) {
			if (adjs[j] == 3 || visited[adjs[j] >> 2])
				continue;
			visited[adjs[j] >> 2] = true;
			rList.push_back(adjs[j] >> 2);
			if (++nConnected <= halfTris)
				continue;
			int mat2Total = 0;
			for (int t2 = 0; t2 < nTris; ++t2)
				if (tr->triangleMaterial(t2) == 2)
					++mat2Total;
			std::vector<bool> v2(nTris, false);
			std::list<int> r2;
			r2.push_back(triangle);
			v2[triangle] = true;
			int n2 = 1;
			while (!r2.empty()) {
				unsigned int *a2 = tr->triAdjs(r2.front());
				r2.pop_front();
				for (int k = 0; k < 3; ++k) {
					if (a2[k] == 3)
						continue;
					int at = a2[k] >> 2;
					if (v2[at] || tr->triangleMaterial(at) != 2)
						continue;
					v2[at] = true;
					if (++n2 * 2 > mat2Total)
						return false;
					r2.push_back(at);
				}
			}
			return true;
		}
	}
	return true;
}

bool surgicalActions::rightMouseDown(std::string objectHit, float (&position)[3], int triangle)
{	// returns true if a surgical action is taken, false if this is a simple viewer command
	if((_toolState==0 && objectHit[1]!='_') || (_toolState>0 && (objectHit.substr(0,2)=="H_" || objectHit.substr(0,2)=="S_")))
		return false;
	// staticTriangle objects are only scenery. If user selects one, just ignore it. pick() should ignore it
	int hookNum=-1;
	if (_toolState != 7 && !_periostealUndermineTriangles.empty()){  // forgot to finish periosteal undermining so do it now
		int newTs = _toolState;
		_toolState = 7;
		// onKeyDown takes GLFW key codes; ENTER completes the pending
		// periosteal undermining before the new tool acts.
		onKeyDown(GLFW_KEY_ENTER);
		setToolState(newTs);
	}
	if (_toolState > 0) {  // active tool requested by user
		_bts.setPhysicsPause(true);  // stop doing physics updates
		// prevent user from doing a new op until previous one is finished
		while (!physicsDone)  // physics update thread must be complete before doing next op.
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	if(_toolState==0)	//viewer
	{
		if(objectHit.substr(0,2)=="H_")	// user picked a hook
		{
			snapshotForwardMove("hook move");
			_bts.setPhysicsPause(false);
			_selectedSurgObject = objectHit;
			hookNum = atoi(_selectedSurgObject.c_str()+2);
			_sutures.selectSuture(-1);
			_hooks.selectHook(hookNum);
		}
		else if(objectHit.substr(0,2)=="S_")	// user picked a suture
		{
			_selectedSurgObject = objectHit;
			hookNum = atoi(_selectedSurgObject.c_str()+2);
			_hooks.selectHook(-1);
			int userNum = _sutures.baseToUserSutureNumber(hookNum);
			if(userNum < 0){
				sendUserMessage("User can't select or delete an automatic suture.\nDelete a user applied suture before or after\nto remove the automatic row.", "Usage error", false);
				_sutures.selectSuture(-1);
			}
			else
				_sutures.selectSuture(hookNum);
		}
		else
			;
	}
	else if (_toolState == 1) {	// create hook mode
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		float uv[2] = { 0.0f, 0.0f };
		tr->getBarycentricProjection(triangle, position, uv);
		int material;
		float hTx[2];
		Vec3f hVec;
		if (!setHistoryAttachPoint(triangle, uv, material, hTx, hVec))
			return false;  // try again
		if (_hooks.getNumberOfHooks() < 1) {	// initialize hooks
			_hooks.setHookSize(sn->getRadius() * 0.02f);
			_hooks.setShapes(_gl3w->getShapes());
			_hooks.setGLmatrices(_gl3w->getGLmatrices());
			_hooks.setPhysicsLattice(_bts.getPdTetPhysics_2());
			_hooks.setVnBccTetrahedra(_bts.getVirtualNodedBccTetrahedra());
		}

		// COURT visual debug use
/*				uv[0] = 0.0f; uv[1] = 0.0f;
				for (int j, i = 0; i < tr->numberOfTriangles(); ++i) {
					int* trp = tr->triangleVertices(i);
					for (j = 0; j < 3; ++j)
						if (trp[j] == 8736)
							break;
					if (j < 3) {
						triangle = i;
						if (j == 1)
							uv[0] = 1.0f;
						if (j == 2)
							uv[1] = 1.0f;
						_toolState = 0;
						break;
					}
				} */
//				triangle = 7646;
//				uv[0] = 0.33;
//				uv[1] = 0.33;

		snapshotForwardMove("hook placement");
		_bts.setPhysicsPause(false);
		if ((hookNum = _hooks.addHook(tr, triangle, uv, _strongHooks)) > -1)
		{

//						return true;  // for above debug use

			if (!_bts.getPdTetPhysics_2()->solverInitialized()) {  // solver must be initialized to add a hook
				_bts.setForcesAppliedFlag();
				physicsDone = false;
				_ffg->physicsDrag = true;
				tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
					try {
						_bts.initPdPhysics();
						physicsDone = true;
					}
					catch (...) {
						physicsDone = true;
						_ffg->physicsDrag = false;
						taskThreadErrorStr = "Couldn't initialize physics after adding hook.";
						taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
						taskThreadError = true;
					}
					}
				);
			}
			_sutures.selectSuture(-1);
			_hooks.selectHook(hookNum);
			char s[80];
			sprintf(s, "H_%d", hookNum);
			_selectedSurgObject = s;
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			json::Object hookObj, hookTitle;
			hookObj["hookNum"] = hookNum;
			hookObj["material"] = material;
			if (_strongHooks)
				hookObj["strongHook"] = true;
			json::Array vArr;
			vArr.push_back(hTx[0]);
			vArr.push_back(hTx[1]);
			hookObj["historyTexture"] = vArr;
			vArr.Clear();
			vArr.push_back(hVec[0]);
			vArr.push_back(hVec[1]);
			vArr.push_back(hVec[2]);
			hookObj["displacement"] = vArr;
			hookTitle["addHook"] = hookObj;
			_historyArray.push_back(hookTitle);
			_historyIt = _historyArray.end();
			// don't _frame->setToolState(0) or will get unnecessary hook move on mouse up or motion.  Fix there.
		}
		_bts.setPhysicsPause(false);
	}
	else if (_toolState == 2)	// incision mode
	{
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		if (!_fence.isInitialized()) {	// initialize fence
			_fence.setFenceSize(sn->getRadius() * 0.02f);
			_fence.setGl3wGraphics(_gl3w);
		}
		bool endConn = false;
		Vec3f vtx(position), nrm;
		float uv[2] = { 0.0f,0.0f };  // pos[3], norm[3],
		auto ToutToFirstPoint = [&]() {
			_fence.getPostPos(0, vtx);
			_fence.getPostNormal(0, nrm);
			snapshotForwardMove("post placement");
			_fence.addPost(tr, _fence.getPostTriangle(0), vtx.xyz, nrm.xyz, endConn, false, false);
			_hooks.selectHook(-1);
			_sutures.selectSuture(-1);
			onKeyDown(GLFW_KEY_ENTER);	// press enter key for user
		};
		if (_ffg->CtrlOrShiftKeyIsDown()) {
			endConn = true;
			int edg, oldTriangle = triangle;
			float param, closeIncisionDistance = _incisions.closestSkinIncisionPoint(vtx, triangle, edg, param);
			if (closeIncisionDistance < FLT_MAX) {
				if (_fence.numberOfPosts() > 2) {  // possible Tout to the first incision point
					_fence.getPostPos(0, vtx);
					float len = (vtx - Vec3f(position)).length();
					if (len < closeIncisionDistance) {
						ToutToFirstPoint();
						return true;
					}
				}
			}
			else {
				if (_fence.numberOfPosts() < 1) {
					sendUserMessage("There is no existing skin incision edge to T in to.", "Usage error", false);
					return false;
				}
				else if (_fence.numberOfPosts() < 3) {
					sendUserMessage("Can only self T out if there are three incision points already present.  Try again-", "Usage error", false);
					return false;
				}
				else {  // can only Tout to the first point
					ToutToFirstPoint();
					return true;
				}
			}
			position[0] = vtx.X; position[1] = vtx.Y; position[2] = vtx.Z;
			if (edg < 1)
				uv[0] = param;
			else if (edg > 1)
				uv[1] = 1.0f - param;
			else {
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
		}
		else
			tr->getBarycentricProjection(triangle, position, uv);
		if (tr->triangleMaterial(triangle) != 2) {
			sendUserMessage("With this tool you can only incise from top side of skin.", "USER ERROR");
			return true;
		}
		tr->getBarycentricPosition(triangle, uv, vtx.xyz);
		tr->getBarycentricNormal(triangle, uv, nrm.xyz);
		snapshotForwardMove("post placement");
		_fence.addPost(tr, triangle, vtx.xyz, nrm.xyz, endConn, false, false);
		_hooks.selectHook(-1);
		_sutures.selectSuture(-1);
		if (_fence.numberOfPosts() > 1 && endConn)	// this must finish an incision
			onKeyDown(GLFW_KEY_ENTER);	// press enter key for user
	}
	else if (_toolState == 3){	// start undermine tool
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		if (tr->triangleMaterial(triangle) != 2 && tr->triangleMaterial(triangle) != 10) {
			sendUserMessage("With this tool you can only undermine from top side of skin.", "USER ERROR");
			return true;
		}
		undermineTriangle ut;
		ut.triangle = triangle;
		ut.incisionConnect = !_ffg->CtrlOrShiftKeyIsDown();
		_undermineTriangles.push_back(ut);
		_bts.updateSurfaceDraw();
		snapshotForwardMove("undermine point");
		if (!_incisions.addUndermineTriangle(triangle, 2, ut.incisionConnect)) {
			// ignore false return as it does no harm
			_undermineTriangles.pop_back();
		}
	}
	else if (_toolState == 4){	// create suture mode
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		if (_sutures.getNumberOfSutures() < 1) {	// initialize sutures
			_sutures.setSutureSize(sn->getRadius()*0.003f);
			_sutures.setShapes(_gl3w->getShapes());
			_sutures.setGLmatrices(_gl3w->getGLmatrices());
			_sutures.setPhysicsLattice(_bts.getPdTetPhysics_2());
			_sutures.setVnBccTetrahedra(_bts.getVirtualNodedBccTetrahedra());
			_sutures.setSurgicalActions(this);
		}
		int i = 0, edg, triMat = tr->triangleMaterial(triangle);
		int eTri = triangle;
		float param, uv[2];
		tr->getBarycentricProjection(triangle, position, uv);
		if (triMat == 2) {
			_sutures.nearestSkinIncisionEdge(uv, eTri, edg, param);
			if (edg < 1) {
				uv[0] = param;
				uv[1] = 0.0f;
			}
			else if (edg > 1){
				uv[1] = 1.0f - param;
				uv[0] = 0.0f;
			}
			else {
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
		}
		else if (triMat == 3) {
			int aTE = tr->triAdjs(triangle)[0];
			if (tr->triangleMaterial(aTE >> 2) > 3 && tr->triangleMaterial(aTE >> 2) < 7)
				aTE = tr->triAdjs(triangle - 1)[0];  // incision convention
			else
				assert(tr->triangleMaterial(aTE >> 2) == 2);
			eTri = aTE >> 2;
			edg = aTE & 3;
			Vec3f V0, V1, P(position);
			int* tp = tr->triangleVertices(eTri);
			tr->getVertexCoordinate(tp[edg], V0.xyz);
			tr->getVertexCoordinate(tp[(edg + 1) % 3], V1.xyz);
			V1 -= V0;
			float denom = (V1 * V1);
			if (denom < 1e-8f)
				param = 0.0;
			else {
				param = (P - V0) * V1 / denom;
				if (param >= 1.0f)
					param = 1.0f;
				if (param < 0.0f)
					param = 0.0f;
			}
			if (edg < 1) {
				uv[0] = param;
				uv[1] = 0.0f;
			}
			else if (edg > 1) {
				uv[1] = 1.0f - param;
				uv[0] = 0.0f;
			}
			else {
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
			triMat = 2;  // corrected
		}
		else if (triMat == 6) {
			sendUserMessage("Currently can't suture within cut muscle or fat. Please suture above or below ths point-", "USER ERROR");
			return true;
		}
		else {  // suturing to a non-skin surface object.
			if (uv[0] + uv[1] > 0.67f) {  // force to an edge
				edg = 1;
				param = uv[1] / (uv[0] + uv[1]);
			}
			else if (uv[0] > uv[1]) {
				edg = 0;
				param = uv[0];
			}
			else {
				edg = 2;
				param = 1.0f - uv[1];
			}
		}
		tr->getBarycentricPosition(eTri, uv, _dragXyz);
		snapshotForwardMove("suture placement");
		i = _sutures.addUserSuture(tr, eTri, edg, param);
		if (_ffg->CtrlOrShiftKeyIsDown()) {
			int prevMat = _sutures.previousUserSuture(i);
			if (prevMat > -1)
				prevMat = _sutures.firstVertexMaterial(prevMat);
			if (prevMat != 2 || triMat != 2) {
				sendUserMessage("Can only create an automatic suture line on a skin/mucosal edges-", "USER ERROR");
				_sutures.deleteSuture(i);
				return true;
			}
			else
			_sutures.setLinked(i, true);
		}
		else
		_sutures.setLinked(i, false);
		_sutures.setSecondVertexPosition(i, _dragXyz);
		char s[10];
		sprintf(s, "S_%d", i);
		_selectedSurgObject = s;
	}
	else if (_toolState == 5) {	// excise mode
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		int mat = tr->triangleMaterial(triangle);
		if (mat == 3 || mat == 6) {
			sendUserMessage("Can't excise from a skin/mucosal edge or a cut muscle belly,  Try again-", "USER ERROR");
			return true;
		}
		if (!exciseRegionIsBounded(tr, triangle)) {
			sendUserMessage("Excise failed. The region must be enclosed by incisions.  Please try again-", "USER ERROR");
			return true;
		}
		float uv[2], hTx[2];
		tr->getBarycentricProjection(triangle, position, uv);
		int material;
		Vec3f hVec;
		if (!setHistoryAttachPoint(triangle, uv, material, hTx, hVec))
			return true;
		// The forward-move snapshot goes BEFORE the record is written, as every
		// other commit does: taken after it, a reverted excise would leave its
		// record in the restored history.
		snapshotForwardMove("excise");
		if (_historyIt != _historyArray.end()) {
			json::Array tarr;
			for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
				tarr.push_back(*it);
			_historyArray.Clear();
			_historyArray = tarr;
		}
		json::Object exciseObj, exciseTitle;
		exciseObj["material"] = material;
		json::Array vArr;
		vArr.push_back(hTx[0]);
		vArr.push_back(hTx[1]);
		exciseObj["historyTexture"] = vArr;
		vArr.Clear();
		vArr.push_back(hVec[0]);
		vArr.push_back(hVec[1]);
		vArr.push_back(hVec[2]);
		exciseObj["displacement"] = vArr;
		exciseTitle["excise"] = exciseObj;
		_historyArray.push_back(exciseTitle);
		_historyIt = _historyArray.end();
		_incisions.excise(triangle);
		physicsDone = false;
		_ffg->physicsDrag = true;
		tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
			try {
				_bts.updateOldPhysicsLattice();
				newTopology = true;
				physicsDone = true;
			}
			catch (std::exception& e) {  // name the underlying failure in the dialog
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadErrorStr = std::string("Topological error following excision: ") + e.what();
				taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
				taskThreadError = true;
			}
			catch (...) {
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadErrorStr = "Topological error following excision.";
				taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
				taskThreadError = true;
			}
			}
		);
		_bts.setPhysicsPause(false);
		_hooks.selectHook(-1);
		_sutures.selectSuture(-1);
		_selectedSurgObject = "";
		_ffg->setToolState(0);
		setToolState(0);
	}
	else if (_toolState == 6)	// deep cut mode
	{
		if (objectHit.substr(0, 3) == "NP_") {	// user picked a fence handle
			_selectedSurgObject = objectHit;
			hookNum = atoi(_selectedSurgObject.c_str() + 3);
			_sutures.selectSuture(-1);
			_hooks.selectHook(-1);
			snapshotForwardMove("post adjustment");
			_fence.selectPost(hookNum);
			Vec3f pos(position[0], position[1], position[2]);
			_fence.setSpherePos(hookNum, pos);
			return true;
		}
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		if (tr->triangleMaterial(triangle) != 2 && tr->triangleMaterial(triangle) != 5) {
			sendUserMessage("Can only deep cut from unelevated skin top or deep bed.  Try again-", "USER ERROR");
			return true;
		}
		if (!_fence.isInitialized()) {	// initialize fence
			_fence.setFenceSize(sn->getRadius() * 0.02f);
			_fence.setGl3wGraphics(_gl3w);
		}
		bool closedEnd = true;
		if (_ffg->CtrlOrShiftKeyIsDown())
			closedEnd = false;
		Vec3f norm;
		float pos[3], uv[2] = { 0.0f, 0.0f };
		tr->getBarycentricProjection(triangle, position, uv);
		// if triangle selected is material 2 which has been undermined, xRay through it to its corresponding deep bed triangle
		if (tr->triangleMaterial(triangle) == 2 && _incisions.triangleUndermined(triangle)) {
			// The X-ray resolves by TEXTURE, i.e. to the bed at the
			// flap's ORIGINAL site. That is right while the flap lies in its bed and
			// no longer holds once it has been elevated or rotated (the post lands at
			// the donor site). Measure displacement with the flap UNDERSIDE point
			// beneath the click: its material-4 vertices were created coincident
			// with the bed and separate only when the flap moves, so the in-place
			// baseline is ~0 regardless of skin thickness.
			Vec3f underPos(0.0f, 0.0f, 0.0f);
			bool underKnown = true;
			{
				const int* tv = tr->triangleVertices(triangle);
				const float w[3] = { 1.0f - uv[0] - uv[1], uv[0], uv[1] };
				for (int i = 0; i < 3; ++i) {
					const int dv = skinCutUndermineTets::deepBedVertex(tv[i]);
					if (dv < 0) { underKnown = false; break; }
					const float* q = tr->vertexCoordinate(dv);
					underPos += Vec3f(q[0], q[1], q[2]) * w[i];
				}
			}
			float tx[2];
			tr->getBarycentricTexture(triangle, uv, tx);
			Vec3f displ(0.0f, 0.0f, 0.0f);
			if (!getHistoryAttachPoint(5, tx, displ, triangle, uv, false)) {
				std::string msg = "Can't Xray through top to a deep bed location.";
				historyAttachFailure(msg);
				return false;
			}
			if (underKnown) {
				float bedPos[3];
				tr->getBarycentricPosition(triangle, uv, bedPos);
				const float sep = (Vec3f(bedPos) - underPos).length();
				const float tol = 2.0f * (float)_bts.getVirtualNodedBccTetrahedra()->getTetUnitSize();
				if (sep > tol) {
					sendUserMessage("Can't deep cut through an elevated flap.\nClick the deep bed or unelevated skin-", "USER ERROR");
					return true;  // no post, no undo record (the snapshot comes with addPost below)
				}
			}
		}
		tr->getBarycentricPosition(triangle, uv, pos);
		// Every post takes the normal of its own clicked triangle. Inheriting the
		// previous post's normal (which limits wall crossover between posts) makes
		// later posts tangential to their local surface on curved anatomy, so the
		// walls graze a single sheet and the interpost level stacks mismatch.
		tr->getTriangleNormal(triangle, norm, true);
		if (_fence.numberOfPosts() > 0 && _fence.getPostTriangle(_fence.numberOfPosts() - 1) == triangle) {
			// Two consecutive posts in one surface triangle can never be connected
			// (the commit's top probe throws); refuse the click, no post and no
			// undo step.
			sendUserMessage("Post too close to the previous one.\nClick farther along the line-", "USER ERROR");
			return false;  // no surgical action: the drag that follows must not re-aim the still-selected previous post
		}
		snapshotForwardMove("post placement");
		_fence.addPost(tr, triangle, pos, norm.xyz, false, true, !closedEnd);  // never connect to nearest hard edge
		hookNum = _fence.numberOfPosts() - 1;
		_fence.selectPost(hookNum);
		char s[80];
		sprintf(s, "NP_%d", hookNum);
		_selectedSurgObject = s;
		_hooks.selectHook(-1);
		_sutures.selectSuture(-1);
	}
	else if (_toolState == 7){	// periosteal undermine mode
		auto sn = _gl3w->getNodePtr(objectHit);
		if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
			return false;
		materialTriangles* tr = _sg.getMaterialTriangles();
		Vec3f cameraPos, dir;
		_gl3w->getTrianglePickLine(cameraPos.xyz, dir.xyz);  // this routine only used here as of 3/22/2022
		_bts.updateSurfaceDraw();
		perioTri pt;
		pt.incisionConnect = !_ffg->CtrlOrShiftKeyIsDown();
		snapshotForwardMove("periosteal point");
		pt.periostealTriangle = _incisions.addPeriostealUndermineTriangle(triangle, dir, pt.incisionConnect);
		if (pt.periostealTriangle > 0x7ffffffe){
			sendUserMessage("No periosteal triangle hit.  Try again-", "USER ERROR");
			return true;
		}
		_periostealUndermineTriangles.push_back(pt);
	}
	else
		;
	return true;
}

bool surgicalActions::rightMouseUp(std::string objectHit, float (&position)[3], int triangle)
{  // this routine only called when terminating a surgical drag operation
	std::string hStr;
	if((_toolState==2 || _toolState==0) && _selectedSurgObject.substr(0,2)=="P_")	// fence post selected in viewer or incision mode
		_selectedSurgObject = "";
	else if (_toolState == 4)	{	// finish applying a suture
		// prevent user from doing a new op until previous one is finished
		assert(physicsDone);  // physics update thread must be complete before doing next op.
		assert(_selectedSurgObject.substr(0,2)=="S_");
		materialTriangles *tr = NULL;
		int i = atoi(_selectedSurgObject.c_str()+2);
		surgGraphics *sg = NULL;
		if (objectHit != "") {
			auto sn = _gl3w->getNodePtr(objectHit);
			if (sn->getType() != sceneNode::nodeType::MATERIAL_TRIANGLES)
				return false;
			tr = _sg.getMaterialTriangles();
		}
		int eTri = triangle;
		auto invalidate = [&]() {
			// prevent user from doing a new op until previous one is finished
			if (!physicsDone)  // physics update thread must be complete before doing next op.
				throw(std::logic_error("Trying to invalidate a suture while a physics thread is active.\n"));
			_sutures.deleteSuture(i);
			_bts.setPhysicsPause(false);
			_selectedSurgObject = "";
			_hooks.selectHook(-1);
			_sutures.selectSuture(-1);
			setToolState(0);
			_ffg->setToolState(0);
		};
		// A release that lands off the model leaves tr null; the material
		// lookup must not run before that test.
		if (tr == NULL){
			invalidate();
			return true;
		}
		int edge, triMat = tr->triangleMaterial(triangle);
		float param, uv[2];
		if (triMat == 2){
			if (_sutures.firstVertexMaterial(i) != 2){
				sendUserMessage("A skin/mucosal edge can only be sutured to another skin/mucosal edge-", "USER ERROR");
				invalidate();
				return true;
			}
			tr->getBarycentricProjection(triangle, position, uv);
			_sutures.nearestSkinIncisionEdge(uv, eTri, edge, param);
			if (edge < 1) {
				uv[0] = param;
				uv[1] = 0.0f;
			}
			else if (edge > 1) {
				uv[1] = 1.0f - param;
				uv[0] = 0.0f;
			}
			else {
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
			if (eTri < 0) {
				sendUserMessage("Couldn't find a suitable edge point for this suture.  Try again-", "PROGRAM ERROR");
				invalidate();
				return true;
			}
		}
		else if (triMat == 3) {
			int aTE = tr->triAdjs(triangle)[0];
			if (tr->triangleMaterial(aTE >> 2) > 3 && tr->triangleMaterial(aTE >> 2) < 7) 
				aTE = tr->triAdjs(triangle - 1)[0];  // incision convention
			else
				assert(tr->triangleMaterial(aTE >> 2) == 2);
			eTri = aTE >> 2;
			edge = aTE & 3;
			Vec3f V0, V1, P(position);
			int* tp = tr->triangleVertices(eTri);
			tr->getVertexCoordinate(tp[edge], V0.xyz);
			tr->getVertexCoordinate(tp[(edge + 1) % 3], V1.xyz);
			V1 -= V0;
			float denom = (V1 * V1);
			if (denom < 1e-8f)
				param = 0.0;
			else {
				param = (P - V0) * V1 / denom;
				if (param >= 1.0f)
					param = 1.0f;
				if (param < 0.0f)
					param = 0.0f;
			}
			if (edge < 1) {
				uv[0] = param;
				uv[1] = 0.0f;
			}
			else if (edge > 1) {
				uv[1] = 1.0f - param;
				uv[0] = 0.0f;
			}
			else {
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
		}
		else if (triMat == 6){
			sendUserMessage("Currently can't suture in the middle of cut muscle belly or fat. Please suture above or below this point-", "USER ERROR");
			invalidate();
			return true;
		}
		else{
			if (_sutures.firstVertexMaterial(i) == 2){
				sendUserMessage("A deep tissue can only be sutured to deep tissue and not a skin/mucosal edge-", "USER ERROR");
				invalidate();
				return true;
			}
			tr->getBarycentricProjection(triangle, position, uv);
			// suturing to a non-skin surface object.
			if (uv[0] + uv[1] > 0.67f){  // force to an edge
				edge = 1;
				param = uv[1] / (uv[0] + uv[1]);
			}
			else if (uv[0] > uv[1]){
				edge = 0;
				param = uv[0];
			}
			else{
				edge = 0;
				param = 1.0f - uv[1];
			}
		}
		int sRet = _sutures.setSecondEdge(i, tr, eTri, edge, param);
		_bts.setForcesAppliedFlag();
		if (!_bts.getPdTetPhysics_2()->solverInitialized()) {  // solver must be initialized to add a suture
			physicsDone = false;
			_ffg->physicsDrag = true;
			tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
				try {
					_bts.initPdPhysics();
					physicsDone = true;
				}
				catch (...) {
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadErrorStr = "Couldn't initialize physics after adding hook.";
					taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
					taskThreadError = true;
				}
				}
			);
		}
		if (sRet < 1){
			float pos[3], uv[2] = {0.0f, 0.0f};
			if (edge < 1)
				uv[0] = param;
			else if (edge > 1)
				uv[1] = 1.0f - param;
			else{
				uv[0] = 1.0f - param;
				uv[1] = param;
			}
			tr->getBarycentricPosition(eTri, uv, pos);
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			_sutures.setSecondVertexPosition(i, pos);
			if (_sutures.isLinked(i)) {
				// laySutureLine builds the intermediate sutures' graphics, and
				// OpenGL calls are main-thread-only on macOS — dispatched to a
				// worker the shader-uniform lookup faults. The worker lambda
				// would also capture the local suture index by reference,
				// which dangles once this callback returns. The history-replay
				// path already runs this call synchronously on the main
				// thread; the interactive path matches it.
				physicsDone = false;
				_ffg->physicsDrag = true;
				bool rowLaid = false;
				try {
					rowLaid = _sutures.laySutureLine(i);
				}
				catch (...) {
					_ffg->physicsDrag = false;
					taskThreadError = true;
					taskThreadErrorStr = "Error in placing a linked suture line";
				}
				physicsDone = true;
				if (!rowLaid && !taskThreadError) {
					// The row could not be completed (an intermediate suture found no
					// clean edge or tet); the engine already removed the partial row and
					// left this suture as a single one. Report it and carry on.
					_ffg->physicsDrag = false;
					sendUserMessage("Automatic suture row not completed; kept as a single suture.\nPlace it closer to the previous suture-", "Automatic sutures");
				}
			}
		}
		else if (sRet < 2){
			sendUserMessage("Trying to suture to same side of incision is not allowed-", "USER ERROR");
			invalidate();
			return false;
		}
		else
			assert(false);
		_bts.setPhysicsPause(false);

		if (_historyIt != _historyArray.end()) {
			json::Array tarr;
			for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
				tarr.push_back(*it);
			_historyArray.Clear();
			_historyArray = tarr;
		}
		auto getSutureUv = [&]() {
			param += (param < 0.002f) ? 0.001f : -0.001f;
			if (edge < 1) {
				uv[0] = param;
				uv[1] = 0.001f;
			}
			else if (edge > 1) {
				uv[0] = 0.001f;
				uv[1] = 1.0f - param;
			}
			else {
				uv[1] = param;
				uv[0] = 0.999f - param;
			}
		};
		getSutureUv();
		int material;
		float hTx[2];
		Vec3f hVec;
		if (!setHistoryAttachPoint(eTri, uv, material, hTx, hVec))
			invalidate();
		json::Array hArr;
		json::Object pObj, sutureTitle;
		int sNum = _sutures.baseToUserSutureNumber(i);
		if(sNum < 0) {
			sendUserMessage("Trying to finish a non-user suture manually-", "PROGRAM ERROR");
			invalidate();
			return false;
		}
		pObj["sutureNum"] = sNum;
		pObj["linked"] = _sutures.isLinked(i);
		pObj["material1"] = material;  // should probably allow multiple material suturing
		hArr.Clear();
		hArr.push_back(hTx[0]);
		hArr.push_back(hTx[1]);
		pObj["historyTexture1"] = hArr;
		hArr.Clear();
		hArr.push_back(hVec[0]);
		hArr.push_back(hVec[1]);
		hArr.push_back(hVec[2]);
		pObj["displacement1"] = hArr;
		int tri;
		_sutures.getEdgeAttachment(i, true, tr, tri, edge, param);
		getSutureUv();
		if (!setHistoryAttachPoint(tri, uv, material, hTx, hVec))
			invalidate;
		pObj["material0"] = material;
		hArr.Clear();
		hArr.push_back(hTx[0]);
		hArr.push_back(hTx[1]);
		pObj["historyTexture0"] = hArr;
		hArr.Clear();
		hArr.push_back(hVec[0]);
		hArr.push_back(hVec[1]);
		hArr.push_back(hVec[2]);
		pObj["displacement0"] = hArr;
		sutureTitle["addSuture"] = pObj;
		_historyArray.push_back(sutureTitle);
		_historyIt = _historyArray.end();
		_hooks.selectHook(-1);
		_sutures.selectSuture(i);
		_ffg->setToolState(0);
		_bts.setPhysicsPause(false);
		setToolState(0);
	}
	else if (_selectedSurgObject.substr(0, 2) == "H_")	// hook selected. Can only drag hooks.
	{
		if (_toolState == 1) {
			_bts.setPhysicsPause(false);
			setToolState(0);
			_ffg->setToolState(0);
			return true;
		}
		if (_toolState == 0) {  // Too many spurius hook moves recorded due to zoom releases. Fixed in cleftSimViewer.
			Vec3f xyz, selXyz;
			int hookNum = atoi(_selectedSurgObject.c_str() + 2);
			_hooks.getHookPosition(hookNum, xyz.xyz);
			_hooks.getSelectPosition(hookNum, selXyz.xyz);
			selXyz -= xyz;
//			if (selXyz.length2() < 0.01f)  // ignore small movements to unclutter history file
//				return true;
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			json::Array hArr;
			hArr.push_back(hookNum);
			hArr.push_back((double)xyz.xyz[0]);
			hArr.push_back((double)xyz.xyz[1]);
			hArr.push_back((double)xyz.xyz[2]);
			json::Object mObj;
			mObj["moveHook"] = hArr;
			_historyArray.push_back(mObj);
			_historyIt = _historyArray.end();
			setToolState(0);
		}
	}
	else if (_toolState == 6){
		if (_selectedSurgObject.substr(0, 3) == "NP_") {	// fence post selected in viewer or incision mode
			int postNum = atoi(_selectedSurgObject.c_str() + 3);
			_fence.selectPost(postNum);
			// changed to correcting fence problems on <enter> key
		}
	}
	else
		return false;
	return true;
}

bool surgicalActions::mouseMotion(float dScreenX, float dScreenY)
{
	Vec3f xyz, dv;
	if(_toolState==6 && _selectedSurgObject.substr(0,3)=="NP_")
	{
		int postNum = atoi(_selectedSurgObject.c_str()+3);
		_fence.getSpherePos(postNum, xyz);
		_gl3w->getGLmatrices()->getDragVector(dScreenX, dScreenY, xyz.xyz, dv.xyz);
		xyz += dv;
		_fence.setSpherePos(postNum, xyz);
	}
	else if(_toolState==4)	{
		assert(_selectedSurgObject.substr(0,2)=="S_");
		int sutNum = atoi(_selectedSurgObject.c_str()+2);
		_gl3w->getGLmatrices()->getDragVector(dScreenX,dScreenY,_dragXyz,dv.xyz);
		_dragXyz[0]+=dv.xyz[0]; _dragXyz[1]+=dv.xyz[1]; _dragXyz[2]+=dv.xyz[2];
		const float *mm=_gl3w->getGLmatrices()->getFrameAndRotationMatrix();
		transformVector3(_dragXyz,mm,xyz.xyz);
		xyz *= 0.7f;
		xyz.xyz[0]-=mm[12]; xyz.xyz[1]-=mm[13]; xyz.xyz[2]-=mm[14];
		dv.xyz[0] = mm[0]*xyz.xyz[0] + mm[1]*xyz.xyz[1] + mm[2]*xyz.xyz[2];
		dv.xyz[1] = mm[4]*xyz.xyz[0] + mm[5]*xyz.xyz[1] + mm[6]*xyz.xyz[2];
		dv.xyz[2] = mm[8]*xyz.xyz[0] + mm[9]*xyz.xyz[1] + mm[10]*xyz.xyz[2];
		_sutures.setSecondVertexPosition(sutNum, dv.xyz);
	}
	else if (_toolState != 1 && _selectedSurgObject.substr(0, 2) == "H_")	// hook selected.
	{
		int hookNum = atoi(_selectedSurgObject.c_str() + 2);
		_hooks.getHookPosition(hookNum, xyz.xyz);
		_gl3w->getGLmatrices()->getDragVector(dScreenX, dScreenY, xyz.xyz, dv.xyz);
		xyz += dv;
		_bts.setForcesAppliedFlag();  // this is a hook move so forces are applied
		if (!_bts.isPhysicsPaused() || !physicsDone) {
			_bts.setPhysicsPause(true);  // stop doing physics updates
			// prevent user from doing a new op until previous one is finished
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		_hooks.setHookPosition(hookNum, xyz.xyz);
		_bts.setPhysicsPause(false);
	}
	else
		;
	return true;
}

void surgicalActions::onKeyDown(int key)
{
	std::string hStr;
	if (key == SKINFLAPS_UNDO_KEY || key == SKINFLAPS_UNDO_AFTER_ERROR_KEY || key == SKINFLAPS_REDO_KEY) {
		undoRedo(key);
		return;
	}
	// ctrl and shift keys now handled by frame calls
	if(key == GLFW_KEY_DELETE)	// delete key
	{
		// Posts, undermine points and periosteal points are not deletable: they
		// are forward moves that Cmd+Z steps back one at a time. Delete keeps its
		// clinical meaning for a selected hook or suture (itself a forward move).
		if (_toolState == 2 || _toolState == 6) {
			sendUserMessage("Use Cmd+Z to remove the last post-", "Undo instead");
			return;  // don't reset toolstate
		}
		else if (_toolState == 3 || _toolState == 7) {
			sendUserMessage("Use Cmd+Z to remove the last point-", "Undo instead");
			return;
		}
		else if (_selectedSurgObject.substr(0, 2) == "H_")
		{
			int hookNum = atoi(_selectedSurgObject.c_str()+2);
			// prevent user from doing a new op until previous one is finished
			_bts.setPhysicsPause(true);  // don't spawn another physics update till complete
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			snapshotForwardMove("hook removal");
			_hooks.deleteHook(hookNum);
			_bts.setPhysicsPause(false);
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			json::Object dObj;
			dObj["deleteHook"] = hookNum;
			_historyArray.push_back(dObj);
			_historyIt = _historyArray.end();
		}
		else if(_selectedSurgObject.substr(0,2)=="S_")
		{
			_bts.setPhysicsPause(true);  // don't spawn another physics update till complete
			// prevent user from doing a new op until previous one is finished
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			snapshotForwardMove("suture deletion");  // before the truncation below, as the hook branch does: it reads _historyIt
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			json::Object sObj;
			int sutNum = atoi(_selectedSurgObject.c_str() + 2);
			int userNum = _sutures.baseToUserSutureNumber(sutNum);
			int linkNum = _sutures.deleteSuture(sutNum);
			if (userNum < 0) {
				json::Object lObj;
				lObj["autoSuturesFor"] = _sutures.baseToUserSutureNumber(linkNum);
				sObj["deleteSuture"] = lObj;
			}
			else
				sObj["deleteSuture"] = userNum;
			_historyArray.push_back(sObj);
			_historyIt = _historyArray.end();
		}
		else
			;
		_ffg->setToolState(0);
		setToolState(0);
		_bts.setPhysicsPause(false);
	}
	else if (key == GLFW_KEY_ENTER)	// <enter> key
	{
		// prevent user from doing a new op until previous one is finished
		if (_toolState == 7){	//periosteal undermine mode
			_bts.setPhysicsPause(true);  // should already be done
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			materialTriangles *mt = _sg.getMaterialTriangles();
			snapshotForwardMove("periosteal undermine");
			for (int n = mt->numberOfTriangles(), i = 0; i < n; ++i){
				if (mt->triangleMaterial(i) == 10)
					mt->setTriangleMaterial(i, 8);  // 8 is a periosteal triangle that has been undermined
			}
			_bts.updateSurfaceDraw();
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			physicsDone = false;
			_ffg->physicsDrag = true;
			tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
				try {
					_bts.fixPeriostealPeriferalVertices();
					_bts.nonTetPhysicsUpdate();
					newTopology = true;
					physicsDone = true;
				}
				catch (...) {
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadErrorStr = "Periosteal undermine error.";
					taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
					taskThreadError = true;
				}
				}
			);
			_bts.setPhysicsPause(false);
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			float hTx[2], uv[2] = { 0.333f, 0.333f };
			int material;
			Vec3f hVec;
			json::Array uArr;
			json::Object uObj, pObj;
			auto ptit = _periostealUndermineTriangles.begin();
			while (ptit != _periostealUndermineTriangles.end()) {
				uObj.Clear();
				if (!setHistoryAttachPoint(ptit->periostealTriangle, uv, material, hTx, hVec)) {
					sendUserMessage("Couldn't record a periosteal point for the history file.  Please try again-", "PROGRAM ERROR");
					return;
				}
				uObj["material"] = material;
				json::Array sArr;
				sArr.push_back(hTx[0]);
				sArr.push_back(hTx[1]);
				uObj["historyTexture"] = sArr;
				sArr.Clear();
				sArr.push_back(hVec[0]);
				sArr.push_back(hVec[1]);
				sArr.push_back(hVec[2]);
				uObj["displacement"] = sArr;
				uObj["incisionConnect"] = (bool)ptit->incisionConnect;
				pObj["periostealTriangle"] = uObj;
				uArr.push_back(pObj);
				++ptit;
			}
			uObj.Clear();
			uObj["periostealUndermine"] = uArr;
			_historyArray.push_back(uObj);
			_incisions.clearCurrentUndermine(8);  // set all periosteal undermined triangles to material 8 and reset.
			_periostealUndermineTriangles.clear();
			_historyIt = _historyArray.end();
			_hooks.selectHook(-1);
			_sutures.selectSuture(-1);
			_selectedSurgObject = "";
		}
		else if (_toolState == 2)	//incision mode
		{
			snapshotForwardMove("incision");
			std::vector<Vec3f> positions, normals;
			std::vector<float> postUvs;
			std::vector<int> postTriangles;
			bool edgeStart = false, edgeEnd = false, Tout = false, nukeThis = false, sOpen, eOpen;
			int n = _fence.getPostData(positions, normals, postTriangles, postUvs, edgeStart, edgeEnd, sOpen, eOpen);
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			json::Object iObj;
			iObj["incisedObject"] = 0;	// for now only one object incisable
			iObj["Tin"] = edgeStart;
			iObj["Tout"] = edgeEnd;
			iObj["pointNumber"] = n;
			json::Array iArr;
			iArr.push_back(iObj);
			materialTriangles *tri=_sg.getMaterialTriangles();
			float uv[2];  //  , minParam = 1.0e15f;
			int material;
			float hTx[2];
			Vec3f hVec;
			json::Array hArr;
			for (int i = 0; i<n; ++i)	{
				uv[0] = postUvs[i << 1];
				uv[1] = postUvs[(i << 1) + 1];
				if (!setHistoryAttachPoint(postTriangles[i], uv, material, hTx, hVec)) {
					sendUserMessage("Couldn't record an incision point for the history file.  Please try again-", "PROGRAM ERROR");
					_bts.setPhysicsPause(false);
					return;
				}
				json::Object pObj;
				pObj["material"] = material;
				hArr.Clear();
				hArr.push_back(hTx[0]);
				hArr.push_back(hTx[1]);
				pObj["historyTexture"] = hArr;
				hArr.Clear();
				hArr.push_back(hVec[0]);
				hArr.push_back(hVec[1]);
				hArr.push_back(hVec[2]);
				pObj["displacement"] = hArr;
				iObj.Clear();
				iObj["incisionPoint"] = pObj;
				iArr.push_back(iObj);
			}
			iObj.Clear();
			iObj["makeIncision"] = iArr;
			_historyArray.push_back(iObj);
			_historyIt = _historyArray.end();
			if (!nukeThis) {
				if (!_incisions.skinCut(positions, normals, edgeStart, edgeEnd)) {
					// The refusal comes from inside topDeepSplit, after every cut point
					// was already inserted into the surface: the mesh is left partially
					// cut. With a snapshot available, treat it like a throw: show the
					// reason and let Close revert.
					const std::string why = skinCutUndermineTets::lastRefusal.empty()
						? std::string("Incision tool error: incision left partially cut.")
						: "Incision failed: " + skinCutUndermineTets::lastRefusal;
					if (!FacialFlapsGui::undoAfterError(why.c_str()))
						sendUserMessage("Incision tool error.  Please save history file for debugging-", "Error Message");
				}
				else {
					if (_incisions.physicsRecutRequired()){
						_bts.setPhysicsPause(true);  // don't spawn another physics update till complete
						while (!physicsDone)  // physics update thread must be complete before doing next op.
							std::this_thread::sleep_for(std::chrono::milliseconds(20));

						physicsDone = false;
						_ffg->physicsDrag = true;
						tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
							try {
								_bts.updateOldPhysicsLattice();
								newTopology = true;
								physicsDone = true;
							}
							catch (...) {
								physicsDone = true;
								_ffg->physicsDrag = false;
								taskThreadErrorStr = "An incision requiring physics recut failed.";
								taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
								taskThreadError = true;
							}
							}
						);
					}
					else {
						newTopology = true;
					}
				}
			}
			_bts.setPhysicsPause(false);
			_fence.clear();
		}
		else if (_toolState == 3) {	// undermine mode
			snapshotForwardMove("undermine");
			if (_historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			float hTx[2], uv[2] = {0.333f, 0.333f};
			int material;
			Vec3f hVec;
			json::Array uArr;
			json::Object uObj, pObj;
			auto uit = _undermineTriangles.begin();
			while (uit != _undermineTriangles.end()) {
				uObj.Clear();
				if (!setHistoryAttachPoint(uit->triangle, uv, material, hTx, hVec)) {
					sendUserMessage("Couldn't record an undermine point for the history file.  Please try again-", "PROGRAM ERROR");
					return;
				}
				uObj["material"] = 2;  // at time executed all set to 10, but they came in as 2
				uObj["incisionConnect"] = (bool)uit->incisionConnect;
				json::Array sArr;
				sArr.push_back(hTx[0]);
				sArr.push_back(hTx[1]);
				uObj["historyTexture"] = sArr;
				sArr.Clear();
				sArr.push_back(hVec[0]);
				sArr.push_back(hVec[1]);
				sArr.push_back(hVec[2]);
				uObj["displacement"] = sArr;
				pObj["underminePoint"] = uObj;
				uArr.push_back(pObj);
				++uit;
			}
			uObj.Clear();
			uObj["undermine"] = uArr;
			_historyArray.push_back(uObj);
			_historyIt = _historyArray.end();
			_bts.setPhysicsPause(true);  // should already be done
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			_bts.updateSurfaceDraw();
			_incisions.undermineSkin();
			_undermineTriangles.clear();

			physicsDone = false;
			_ffg->physicsDrag = true;
			tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
				try {
					_bts.updateOldPhysicsLattice();
					newTopology = true;
					physicsDone = true;
				}
				catch (...) {
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadErrorStr = "Topology error after undermine operation.";
					taskThreadErrorHoldable = true;  // a forward move's snapshot can take this back (main loop error hold)
					taskThreadError = true;
				}
				}
			);
			_bts.setPhysicsPause(false);
		}
		else if (_toolState == 6)	// deep cut mode
		{
			if (!_incisions.inputCorrectFence(&_fence, _ffg))
				return;
			snapshotForwardMove("deep cut");
			std::vector<Vec3f> positions, rays;
			std::vector<float> postUvs;
			std::vector<int> postTriangles;
			bool edgeStart, edgeEnd, startOpen, endOpen;  //  , Tout = false, nukeThis = false;
			if (_historyArray.size()>0 && _historyIt != _historyArray.end()) {
				json::Array tarr;
				for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
					tarr.push_back(*it);
				_historyArray.Clear();
				_historyArray = tarr;
			}
			int n = _fence.getPostData(positions, rays, postTriangles, postUvs, edgeStart, edgeEnd, startOpen, endOpen); // bools not relevant
			if (n >= 2 && startOpen && !endOpen) {
				// The cutter walks a one-end-open cut from the closed post toward the
				// open one (deepCut::inputCorrectFence reverses these internally).
				// Record the history in that walked order, so the file replays the
				// working direction on every platform.
				std::reverse(positions.begin(), positions.end());
				std::reverse(rays.begin(), rays.end());
				std::reverse(postTriangles.begin(), postTriangles.end());
				for (int i = 0, j = n - 1; i < j; ++i, --j) {
					std::swap(postUvs[i << 1], postUvs[j << 1]);
					std::swap(postUvs[(i << 1) + 1], postUvs[(j << 1) + 1]);
				}
				std::swap(startOpen, endOpen);
			}
			materialTriangles *tri = _sg.getMaterialTriangles();
			float hTx[2], uv[2];
			int material;
			Vec3f hVec;
			json::Array iArr;
			json::Object iObj, dObj;
			iObj["deepCutObject"] = 0;	// for now only one object incisable
			iObj["openIn"] = startOpen;
			iObj["openOut"] = endOpen;
			iObj["pointNumber"] = n;
			iArr.push_back(iObj);
			json::Array pArr;
			for (int i = 0; i<n; ++i)	{
				iObj.Clear();
				dObj.Clear();
				uv[0] = postUvs[i << 1];
				uv[1] = postUvs[(i << 1) + 1];
				if (!setHistoryAttachPoint(postTriangles[i], uv, material, hTx, hVec)) {
					sendUserMessage("Couldn't record a deep cut point for the history file.  Please try again-", "PROGRAM ERROR");
					return;
				}
				dObj["material"] = material;
				json::Array sArr;
				sArr.push_back(hTx[0]);
				sArr.push_back(hTx[1]);
				dObj["historyTexture"] = sArr;
				sArr.Clear();
				sArr.push_back(hVec[0]);
				sArr.push_back(hVec[1]);
				sArr.push_back(hVec[2]);
				dObj["displacement"] = sArr;
				sArr.Clear();
				sArr.push_back(rays[i].X);
				sArr.push_back(rays[i].Y);
				sArr.push_back(rays[i].Z);
				dObj["postNormal"] = sArr;
				iObj["deepCutPoint"] = dObj;
				iArr.push_back(iObj);
			}
			iObj.Clear();
			iObj["makeDeepCut"] = iArr;
			_historyArray.push_back(iObj);
			_historyIt = _historyArray.end();
			if (!_bts.isPhysicsPaused())
				throw(std::logic_error("Physics must be paused before deep cut."));
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			_bts.updateSurfaceDraw();
			const int nvBeforeCut = _sg.getMaterialTriangles()->numberOfVertices(), ntBeforeCut = _sg.getMaterialTriangles()->numberOfTriangles();
			if (!_incisions.cutDeep()) {
				// The fence, tool state 6 and the paused physics are left as they
				// are, so the posts can be adjusted and the cut retried (a designed
				// refusal, kept).  But the makeDeepCut record pushed above
				// stays in the history, where a replay stops on it, and the
				// forward-move snapshot stays on the undo stack as a dead step.
				// First keep the attempt on disk (the message asks for the
				// history to debug), then take it back out of the live history.
				// When the cutter had already altered the surface before giving up
				// (a false return after the post punches), revert to the snapshot,
				// which brings the posts back with it; without a snapshot that
				// state stays as the cutter left it, and the record stays so the saved history
				// still documents it.
				materialTriangles* mt = _sg.getMaterialTriangles();
				const bool mutated = mt->numberOfVertices() != nvBeforeCut || mt->numberOfTriangles() != ntBeforeCut;
				// The versioned save syncs the full trail, which appends the record
				// about to be withdrawn; remember where the trail stood so the
				// withdrawal can roll it back (the reverted case records the
				// attempt plus an "undo" note there, like every other error hold).
				const size_t trailLenBefore = g_fullTrail.size(), trailSyncedBefore = g_trailSynced;
				const std::string savedAs = FacialFlapsGui::saveVersionedErrorHistories();
				std::string msg = "Attempted deepCut failed.";  // the saved attempt's path is in the log ([error-history])
				if (savedAs.empty())
					msg += " Save history to debug.";
				bool reverted = false, restoreTried = false;
				if (!g_undoStack.empty() && g_undoStack.back().action == "deep cut") {
					if (mutated) {
						restoreTried = true;
						undoRedo(SKINFLAPS_UNDO_AFTER_ERROR_KEY);  // pops the snapshot and restores it: surface, history, posts, tool state
						reverted = skinflapsUndoRestoreSucceeded;
					}
					else
						g_undoStack.pop_back();
					if (!mutated || reverted) {
						if (g_redoStack.empty())
							g_redoStack = std::move(g_redoCleared);  // the attempt changed nothing: what was undone before is still redoable
						g_redoCleared.clear();
					}
				}
				if (restoreTried && !reverted) {
					// A snapshot restore that failed part-way is not a state to continue
					// from; finishErrorHold treats it the same way: save-and-exit.
					FacialFlapsGui::handleThrow("Deep cut refused after the surface was altered, and the state before the attempt could not be restored.");
					return;
				}
				if (mutated && !reverted)
					msg += "\nSave the history and reload it before continuing-";  // the record stays
				else {
					if (!reverted && _historyArray.size() > 0) {  // json::Array has no pop_back: rebuild without the last record, as the truncation above does
						json::Array tarr;
						auto last = _historyArray.end();
						--last;
						for (auto it = _historyArray.begin(); it != last; ++it)
							tarr.push_back(*it);
						_historyArray.Clear();
						_historyArray = tarr;
						_historyIt = _historyArray.end();
						if (g_fullTrail.size() > trailLenBefore) {  // roll the trail back to before the save appended the withdrawn record
							json::Array t2;
							size_t k = 0;
							for (auto it = g_fullTrail.begin(); it != g_fullTrail.end() && k < trailLenBefore; ++it, ++k)
								t2.push_back(*it);
							g_fullTrail.Clear();
							g_fullTrail = t2;
						}
						g_trailSynced = std::min(trailSyncedBefore, _historyArray.size());
					}
					msg += "\nAdjust the posts and try again-";
				}
				sendUserMessage(msg.c_str(), "PROGRAM ERROR");
				return;
			}
			g_redoCleared.clear();  // the cut went through: the redo stack it cleared is gone for good
			_ffg->user_message_flag = false;

			physicsDone = false;
			_ffg->physicsDrag = true;
			tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
				try {
					_bts.updateOldPhysicsLattice();
					newTopology = true;
					physicsDone = true;
				}
				catch (std::exception& e) {
					// Keep the reason (a catch(...) would lose it: e.g. the
					// solver's "Eigen CHOLMOD: A11 factorization failed" when a cut
					// severs an unanchored piece of tissue) and mark the failure as
					// one the pre-cut snapshot can take back (main loop error hold).
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadErrorStr = std::string("Deep cut failure: ") + e.what();
					taskThreadErrorHoldable = true;
					taskThreadError = true;
				}
				catch (...) {
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadErrorStr = "Deep cut failure.";
					taskThreadErrorHoldable = true;
					taskThreadError = true;
				}
				}
			);
			_fence.clear();
			_incisions.clearDeepCutter();
			_bts.setPhysicsPause(false);
		}
		else
			;
		_ffg->setToolState(0);
		setToolState(0);
	}
	else
		;
}

void surgicalActions::onKeyUp(int key)
{  // ctrl and shift key now handled by frame call
}

bool surgicalActions::loadScene(const char *modelDirectory, const char *sceneFilename)
{
	bool ret = _bts.loadScene(modelDirectory, sceneFilename);  // computes bounding spheres
	_sceneDir.assign(modelDirectory);
	_originalTriangleNumber = _sg.getMaterialTriangles()->numberOfTriangles();
	if(ret && _historyArray.size() < 1) {
		std::string dstr(modelDirectory),fstr(sceneFilename);
		_historyArray.Clear();
		std::size_t n;
		while ((n = dstr.find("\\")) < dstr.npos)
			dstr.replace(n, 1, "/");
		json::Object loadObj;
		loadObj["loadSceneFile"] = fstr;
		_historyArray.push_back(loadObj);
		_historyIt = _historyArray.end();
	}
	_gl3w->zeroViewRotations();
	return ret;
}

bool surgicalActions::setHistoryAttachPoint(const int triangle, const float(&uv)[2], int &material, float(&historyTexture)[2], Vec3f &historyVec)
{  // Input an attach point in current environment. Outputs a historyTriangle, historyUv, and historyVec for storage in a history file.
	// This attachment point is created by program with variable physics state at time of incisions.  For this reason move away a safe distance to an original triangle and use
	// historyVec to find closest original location.  historyVec is in material coords so less sensitive to physics state.
	materialTriangles *mtp = _sg.getMaterialTriangles();
	material = mtp->triangleMaterial(triangle);
	if (material == 3 || material == 6) {
		sendUserMessage("Can't attach to side of skin incision or middle of cut muscle. Try again-", "USER ERROR", false);
		return false;
	}
	// attachments on an edge must be moved inside triangle
	float tp[2];
	if (uv[0] < 1e-5f) {
		if (uv[1] < 1e-5f) {
			tp[0] = 0.001f;
			tp[1] = 0.001f;
		}
		else {
			tp[0] = 0.001f;
			tp[1] = uv[1] * 0.996f;
		}
	}
	else if (uv[1] < 1e-5f) {
		tp[0] = uv[0] * 0.996f;
		tp[1] = 0.001f;
	}
	else if (uv[0] + uv[1] > 0.998f) {
		tp[0] = uv[0] *0.996f;
		tp[1] = uv[1] * 0.996f;
	}
	else {
		tp[0] = uv[0];
		tp[1] = uv[1];
	}
	auto isBorderTriangle = [mtp, material](int tri) ->bool {  // on incision edge?
		int at[3], ae[3];
		mtp->triangleAdjacencies(tri, at, ae);
		for (int i = 0; i < 3; ++i) {
			int mat = mtp->triangleMaterial(at[i]);
			if ((mat > 2 && mat < 4) || mat == 6)  // hard intermaterial cut edge to move away from
				return true;
		}
		return false;
	};
	historyVec.set(0.0f, 0.0f, 0.0f);
	if (!isBorderTriangle(triangle)) {
		Vec2f tx;
		int *tr = mtp->triangleTextures(triangle);
		float *fp = mtp->getTexture(tr[0]);
		tx.set(fp[0], fp[1]);
		tx *= 1.0f - tp[0] - tp[1];
		fp = mtp->getTexture(tr[1]);
		tx += Vec2f(fp[0], fp[1]) * tp[0];
		fp = mtp->getTexture(tr[2]);
		tx += Vec2f(fp[0], fp[1]) * tp[1];
		historyTexture[0] = tx[0];
		historyTexture[1] = tx[1];
		return true;
	}
	auto vbt = _bts.getVirtualNodedBccTetrahedra();
	auto triangleNormal = [mtp, vbt](const int tri) -> const Vec3f {
		Vec3f N, vM[3];
		int *tr = mtp->triangleVertices(tri);
		for (int j = 0; j < 3; ++j)
			vbt->vertexGridLocus(tr[j], vM[j]);
		vM[1] -= vM[0];
		vM[2] -= vM[0];
		N = vM[1] ^ vM[2];
		N.normalize();
		return N;
	};
	// get material coord displacement direction
	Vec3f planeN, edgeN;
	int at[3], ae[3];
	mtp->triangleAdjacencies(triangle, at, ae);
	int eNum = 0;
	edgeN.set(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < 3; ++i) {
		int mat = mtp->triangleMaterial(at[i]);
		if ((mat > 2 && mat < 4) || mat == 6) {  // hard intermaterial cut edge to move away from
			edgeN += triangleNormal(at[i]);
			++eNum;
		}
	}
	assert(eNum < 3 && eNum>0);
	if(eNum > 1)
		edgeN.normalize();
	planeN = edgeN ^ triangleNormal(triangle);
	int *tr = mtp->triangleVertices(triangle);
	vbt->vertexGridLocus(tr[0], historyVec);
	historyVec *= 1.0f - tp[0] - tp[1];
	for (int i = 0; i<2; ++i){
		Vec3f v;
		vbt->vertexGridLocus(tr[i+1], v);
		historyVec += v*tp[i];
	}
	float d = planeN * historyVec, tetSizeSq = (float)vbt->getTetUnitSize();
	tetSizeSq *= tetSizeSq;
	int nextTri = triangle;
	int lastEdge = -1;
	do {
		tr = mtp->triangleVertices(nextTri);
		Vec3f now, nextV;  // last, 
		vbt->vertexGridLocus(tr[0], nextV);
		float dNext = planeN * nextV - d;
		for (int i = 2; i > -1; --i) {
			vbt->vertexGridLocus(tr[i], now);
			float dNow = planeN * now - d;
			if (i != lastEdge && std::signbit(dNext) != std::signbit(dNow)) {
				Vec3f vI;
				vI = nextV * dNow + now * -dNext;
				vI /= dNow - dNext;
				vI -= historyVec;
				if (vI*edgeN < 0.0) {
					lastEdge = (i + 2) % 3;
					// COURT - what size to use? Could be a physics/model specific value based on variability in number of iterations to stability.
					// creator of a history file will usually have lots of physics iterations before applying a suture, but someone playing back history quickly may have very few.
					if (vI.length2()*tetSizeSq > 0.0001f && !isBorderTriangle(nextTri)) {
						// pull inside this triangle to ensure an insideTest() texture find on retrieval
						float edgeParam = -dNext / (dNow - dNext);
						if (lastEdge < 1) {
							if (edgeParam < 0.005f) {
								tp[0] = 0.001f;
								tp[1] = 0.001f;
							}
							else if (edgeParam > 0.995f) {
								tp[0] = 0.998f;
								tp[1] = 0.001f;
							}
							else {
								tp[0] = edgeParam * 0.996f;
								tp[1] = 0.001f;
							}
						}
						 else if (lastEdge < 2) {
							if (edgeParam < 0.005f) {
								tp[0] = 0.998f;
								tp[1] = 0.001f;
							}
							else if (edgeParam > 0.995f) {
								tp[1] = 0.998f;
								tp[0] = 0.001f;
							}
							else {
								tp[1] = edgeParam * 0.996f;
								tp[0] = (1.0f - edgeParam) * 0.996f;
							}
						}
						 else {
							if (edgeParam > 0.998f) {
								tp[0] = 0.001f;
								tp[1] = 0.001f;
							}
							else if (edgeParam < .005f) {
								tp[1] = 0.998f;
								tp[0] = 0.001f;
							}
							else {
								tp[1] = (1.0f - edgeParam) * 0.996f;
								tp[0] = 0.001f;
							}

						}
						int* triTx = mtp->triangleTextures(nextTri);
						Vec2f tx;
						float *fp = mtp->getTexture(triTx[0]);
						tx.set(fp[0], fp[1]);
						tx *= 1.0f - tp[0] - tp[1];
						fp = mtp->getTexture(triTx[1]);
						tx += Vec2f(fp[0], fp[1]) * tp[0];
						fp = mtp->getTexture(triTx[2]);
						tx += Vec2f(fp[0], fp[1]) * tp[1];
						historyVec = -vI;
						historyTexture[0] = tx[0];
						historyTexture[1] = tx[1];
						return true;
					}
					else {
						int at[3], ae[3];
						mtp->triangleAdjacencies(nextTri, at, ae);
						nextTri = at[lastEdge];
						lastEdge = ae[lastEdge];
						break;
					}
				}
			}
			if (i > 1)
				material = 40;  // send error
			nextV = now;
			dNext = dNow;
		}
	} while (mtp->triangleMaterial(nextTri) == material);
	// have just failed to find a suitable attach point using a search perpendicular to the wound edge.  This is usually due to trying to suture
	// to a flap corner point where that corner is an acute angle.  Instead will look for the nearest non-border triangle.
	material = mtp->triangleMaterial(triangle);
	struct nearTri {
		bool borderTriangle;
		Vec3f centroid;
		int triangle;
	} nt;
	nt.borderTriangle = false;
	nt.triangle = triangle;
	std::multimap<float, nearTri> nearTris;
	nearTris.insert(std::make_pair(0.0f, nt));
	auto ntit = nearTris.begin();
	std::set<int> trisDone;
	while (ntit != nearTris.end()) {
		ntit->second.borderTriangle = false;
		for (int i = 0; i < 3; ++i) {
			int at[3], ae[3];
			mtp->triangleAdjacencies(ntit->second.triangle, at, ae);
			int mat = mtp->triangleMaterial(at[i]);
			if ((mat > 2 && mat < 4) || mat == 6) {  // hard intermaterial cut edge to move away from
				ntit->second.borderTriangle = true;
				continue;
			}
			else if (!trisDone.insert(at[i]).second)
				continue;
			else {
				nt.triangle = at[i];
				int *tr = mtp->triangleVertices(at[i]);
				Vec3f v;
				nt.centroid = { 0.0f, 0.0f, 0.0f };
				for (int j = 0; j < 3; ++j) {
					vbt->vertexGridLocus(tr[j], v);
					nt.centroid += v * 0.33333f;
				}
				nearTris.insert(std::make_pair((historyVec - nt.centroid).length2(), nt));
			}
		}
		if (!ntit->second.borderTriangle) {
			historyVec -= ntit->second.centroid;
			int *ttx = mtp->triangleTextures(ntit->second.triangle);
			Vec2f tx, vt;
			tx.set(0.0f, 0.0f);
			for (int i = 0; i < 3; ++i) {
				mtp->getTexture(ttx[i], vt.xy);
				tx += vt * 0.33333f;
			}
			historyTexture[0] = tx[0];
			historyTexture[1] = tx[1];
			return true;
		}
		else {
			nearTris.erase(ntit);
			ntit = nearTris.begin();
		}
	}
	sendUserMessage("No suitable attachment triangle for this point. Try again-", "PROGRAM LIMITATION", false);
	return false;
}

void surgicalActions::historyAttachFailure(std::string& errorDescription) {
	json::Array tarr;
	for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
		tarr.push_back(*it);
	_historyArray.Clear();
	_historyArray = tarr;
	std::string msg = errorDescription;
	msg.append("\nSetting history back one step and truncating further forward.");
	sendUserMessage(msg.c_str(), "Program error");
}

bool surgicalActions::getHistoryAttachPoint(const int material, const float(&historyTexture)[2], const Vec3f &displacement, int &triangle, float(&uv)[2], bool findEdge)
{  // Input a history attach point from history file. Outputs a triangle, and parametric uv coord in current environment.
	materialTriangles *mtp = _sg.getMaterialTriangles();
	std::vector<Vec2f> triTex;
	triTex.assign(3, Vec2f());
	Vec2f txIn(historyTexture[0], historyTexture[1]);
	int k, n = mtp->numberOfTriangles(), matIn = material;
	if (matIn == 8)  // make all periosteal materials 7
		matIn = 7;
	insidePolygon ip;
	for (k = 0; k < n; ++k) {
		if (material > 6) {
			if (mtp->triangleMaterial(k) < 7)  // in an undermine periosteum may have already been labelled as 7, 8, or 10.
				continue;
		}
		else {
			if (mtp->triangleMaterial(k) != material && mtp->triangleMaterial(k) != 10)  // in an undermine may already have been labelled as 10
				continue;
		}
		int *tr = mtp->triangleTextures(k);
		float *fp;
		for (int j = 0; j < 3; ++j) {
			fp = mtp->getTexture(tr[j]);
			triTex[j].set(fp[0], fp[1]);
		}
		if (ip.insidePolygon2f(txIn, triTex)) {  // COURT texture seams may screw this up. See following backup strategy
			Mat2x2f M;
			M.Initialize_With_Column_Vectors(triTex[1] - triTex[0], triTex[2] - triTex[0]);
			Vec2f R = M.Robust_Solve_Linear_System(txIn - triTex[0]);
			uv[0] = R.X;
			uv[1] = R.Y;
			break;
		}
	}
	if (k >= n) {  // this section to handle texture seam case
		triangle = -1;
		float dsq, minDsq = FLT_MAX;
		for (k = 0; k < n; ++k) {
			if (material > 6) {
				if (mtp->triangleMaterial(k) < 7)  // in an undermine periosteum may have already been labelled as 7, 8, or 10.
					continue;
			}
			else {
				if (mtp->triangleMaterial(k) != material && mtp->triangleMaterial(k) != 10)  // in an undermine may already have been labelled as 10
					continue;
			}
			int* tr = mtp->triangleTextures(k);
			float* fp;
			for (int j = 0; j < 3; ++j) {
				fp = mtp->getTexture(tr[j]);
				triTex[0].set(fp[0], fp[1]);
				dsq = (triTex[0] - txIn).length2();
				if (minDsq > dsq) {
					minDsq = dsq;
					triangle = k;
					uv[0] = j==1 ? 1.0f : 0.0f;
					uv[1] = j > 1 ? 1.0f : 0.0f;
				}
			}
		}
		k = triangle;
		if (triangle < 0)
			throw(std::logic_error("Program error in finding history attach point."));
		return true;
	}
	if (displacement[0] == 0.0f && displacement[1] == 0.0f &&displacement[2] == 0.0f) {
		triangle = k;
		return true;
	}
	// attachments on an edge must be moved inside
	if (uv[0] < 1e-5f) {
		if (uv[1] < 1e-5f) {
			uv[0] = 0.001f;
			uv[1] = 0.001f;
		}
		else
			uv[0] = 0.001f;
	}
	else if (uv[1] < 1e-5f)
		uv[1] = 0.001f;
	else if (uv[0] + uv[1] > 0.998f) {
		uv[0] = uv[0] * 0.996f;
		uv[1] = uv[1] * 0.996f;
	}
	else
		;
	Vec3f triV[3], N, V, startV;
	// switch to material coords to minimize effect of varying physics state
	auto vbt = _bts.getVirtualNodedBccTetrahedra();
	int *tr = mtp->triangleVertices(k);
	for (int j = 0; j < 3; ++j)
		vbt->vertexGridLocus(tr[j], triV[j]);
	startV = triV[0] * (1.0f - uv[0] - uv[1]) + triV[1] * uv[0] + triV[2] * uv[1];
	V = (triV[1] - triV[0])^(triV[2] - triV[0]);
	N = displacement ^ V;
	float d = N * startV, dsqFinal = displacement.length2();
	int nextTri = k;
	int lastEdge = -1;
	do {
		tr = mtp->triangleVertices(nextTri);
		Vec3f now, nextV;  // last, 
		vbt->vertexGridLocus(tr[0], nextV);
		float dNext = N * nextV - d;
		int i;
		for (i = 2; i > -1; --i) {
			vbt->vertexGridLocus(tr[i], now);
			float dNow = N * now - d;
			if (i != lastEdge && std::signbit(dNext) != std::signbit(dNow)) {
				Vec3f vI;
				vI = nextV * dNow + now * -dNext;
				vI /= dNow - dNext;
				vI -= startV;
				if (vI*displacement > 0.0) {
					lastEdge = i;
					if (!findEdge && vI.length2() > dsqFinal) {  // overshoot displacement
						// pull inside this triangle to ensure an insideTest() texture find on retrieval
						for (int j = 0; j < 3; ++j)
							vbt->vertexGridLocus(tr[j], triV[j]);
						triV[1] -= triV[0];
						triV[2] -= triV[0];
						V = startV + displacement - triV[0];
						Mat2x2f M2;
						M2.Initialize_With_Column_Vectors(Vec2f(triV[1]*triV[1], triV[1]*triV[2]), Vec2f(triV[2]*triV[1], triV[2]*triV[2]));
						Vec2f R, B = {V*triV[1], V*triV[2]};
						R = M2.Robust_Solve_Linear_System(B);
						uv[0] = R[0];
						uv[1] = R[1];
						triangle = nextTri;
						return true;
					}
					else {
						int at[3], ae[3];
						mtp->triangleAdjacencies(nextTri, at, ae);
						int nextMaterial = mtp->triangleMaterial(at[lastEdge]);
						if (nextMaterial == 8)
							nextMaterial = 7;
						if (nextMaterial != matIn && nextMaterial != 10) {  // crossed incision edge
							triangle = nextTri;
							float edgeParam = dNow / (dNow - dNext);
							if (lastEdge < 1) {
								uv[0] = edgeParam;
								uv[1] = 0.0f;
							}
							else if (lastEdge < 2) {
								uv[1] = edgeParam;
								uv[0] = 1.0f - edgeParam;
							}
							else {
								uv[1] = 1.0f - edgeParam;
								uv[0] = 0.0f;
							}
							return true;
						}
						nextTri = at[lastEdge];
						lastEdge = ae[lastEdge];
						break;
					}
				}
			}
			nextV = now;
			dNext = dNow;
		}
		if (i < 0) {  // no way out
			triangle = -1;
			return false;
		}
	} while (true);
	return false;
}

bool surgicalActions::loadHistory(const char *historyDir, const char *historyFile)
{
	if (_historyArray.begin() != _historyArray.end())
		return false;
	_historyDir.assign(historyDir);
	// set scene dir elsewhere. Don't lock to history location.
	std::size_t found = _historyDir.rfind("History");
	if (found == _historyDir.size())
		sendUserMessage("History directory specified incorrectly.", "Program error", false);
	_historyArray.Clear();
	std::string hPath(_historyDir);
	hPath.append(historyFile);
	std::ifstream is(hPath.c_str());
	if(!is.is_open())
		return false;
	std::stringstream buffer;
	buffer << is.rdbuf();
	std::string str = buffer.str();
	json::Value hstData = json::Deserialize(str);
	if(hstData.GetType() != json::ArrayVal)
		return false;
	_historyArray = hstData.ToArray();
	_historyIt = _historyArray.begin();
	nextHistoryAction();  // loads scene in history file
	return true;
}

void surgicalActions::promoteFakeSutures()
{
	// The only menu action that reinitializes physics; wait out any
	// in-flight solve first, as every other physics-mutating entry does.
	// The menu handler's setToolState(0) afterward unpauses.
	_bts.setPhysicsPause(true);  // stop doing physics updates
	while (!physicsDone)  // physics update thread must be complete before doing next op.
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	if (_historyIt != _historyArray.end()) {
		json::Array tarr;
		for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
			tarr.push_back(*it);
		_historyArray.Clear();
		_historyArray = tarr;
	}
	json::Object title;
	title["promoteSutureApproximations"] = 0;
	_historyArray.push_back(title);
	_historyIt = _historyArray.end();
	_bts.promoteSutures();
}

void surgicalActions::pausePhysics()
{
	if (_historyIt != _historyArray.end()) {
		json::Array tarr;
		for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
			tarr.push_back(*it);
		_historyArray.Clear();
		_historyArray = tarr;
	}
	json::Object title;
	title["pausePhysics"] = 0;
	_historyArray.push_back(title);
	_historyIt = _historyArray.end();
	_bts.setPhysicsPause(true);
}

void surgicalActions::nextHistoryAction()
{
	if (_historyIt == _historyArray.end()) {
		sendUserMessage("There are no more actions found in this history file-", "SURGICAL HISTORY INFORMATION", false);
		return;
	}
	_bts.setPhysicsPause(true);  // don't spawn another physics update till complete
	// prevent user from doing a new op until previous one is finished; a task
	// thread that failed ends the wait too, so a failure cannot hang the replay
	while (!physicsDone && !taskThreadError)
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	_gl3w->drawAll();
	if (_historyIt->HasKey("loadSceneFile"))
	{
		const json::Object& fObj = _historyIt->ToObject();
		if (!loadScene(_sceneDir.c_str(), fObj.begin()->second.ToString().c_str())) {
			sendUserMessage("The scene file in the history file can't be loaded-", "SURGICAL HISTORY INFORMATION", false);
			_historyArray.Clear();
		}
		else {
			_ffg->setModelFile(fObj.begin()->second.ToString());
			++_historyIt;
		}
	}
	else if (_historyIt->HasKey("addHook"))
	{
		materialTriangles *tr = _sg.getMaterialTriangles();
		if (tr == NULL)
			return;
		json::Object hookObj = (*_historyIt)["addHook"].ToObject();
		int hookNum, triangle;
		int material;
		float uv[2], historyTx[2];
		assert(hookObj.HasKey("material"));
		material = hookObj["material"].ToInt();
		hookNum = hookObj["hookNum"].ToInt();
		assert(hookObj.HasKey("historyTexture"));
		json::Array vArr = hookObj["historyTexture"];
		historyTx[0] = vArr[0];
		historyTx[1] = vArr[1];
		Vec3f V;
		vArr.Clear();
		vArr = hookObj["displacement"];
		V[0] = vArr[0].ToFloat();
		V[1] = vArr[1].ToFloat();
		V[2] = vArr[2].ToFloat();
		if (!getHistoryAttachPoint(material, historyTx, V, triangle, uv, false)) {
			std::string msg = "History file attachment failure at hook number ";
			msg.append(std::to_string(hookNum));
			historyAttachFailure(msg);
			return;
		}
		if (_hooks.getNumberOfHooks() < 1) {	// initialize hooks
			_hooks.setHookSize(_sg.getSceneNode()->getRadius()*0.02f);
			_hooks.setShapes(_gl3w->getShapes());
			_hooks.setGLmatrices(_gl3w->getGLmatrices());
			_hooks.setPhysicsLattice(_bts.getPdTetPhysics_2());
			_hooks.setVnBccTetrahedra(_bts.getVirtualNodedBccTetrahedra());
		}
		hookNum = -1;
		bool strongHook = false;
		if(hookObj.HasKey("strongHook"))
			strongHook = true;
		int newHookNum;
		snapshotForwardMove("hook placement", _historyIt - _historyArray.begin());
		_bts.setPhysicsPause(false);
		if ((newHookNum = _hooks.addHook(tr, triangle, uv, strongHook)) > -1)
		{
			if (!_bts.getPdTetPhysics_2()->solverInitialized()) {  // solver must be initialized to add a hook. Done once.
				_bts.setForcesAppliedFlag();
				physicsDone = false;
				_ffg->physicsDrag = true;
				tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
					try {
						_bts.initPdPhysics();
						physicsDone = true;
					}
					catch (...) {
						physicsDone = true;
						_ffg->physicsDrag = false;
						taskThreadError = true;
						taskThreadErrorStr = "Couldn't initialize physics after adding hook.";
					}
					}
				);
			}
			_sutures.selectSuture(-1);
			_hooks.selectHook(hookNum);
			char s[20];
#ifdef _WINDOWS
			sprintf_s(s, 19, "H_%d", hookNum);
#else
			sprintf(s, "H_%d", hookNum);
#endif
			_selectedSurgObject = s;
		}
		++_historyIt;
	}
	else if (_historyIt->HasKey("moveHook"))
	{
		Vec3f xyz;
		int hookNum;
		const json::Array& hArr = (*_historyIt)["moveHook"];
		hookNum = hArr[0].ToInt();
		xyz.xyz[0] = hArr[1].ToFloat();
		xyz.xyz[1] = hArr[2].ToFloat();
		xyz.xyz[2] = hArr[3].ToFloat();
		snapshotForwardMove("hook move", _historyIt - _historyArray.begin());
		_bts.setPhysicsPause(false);
		_hooks.setHookPosition(hookNum, xyz.xyz);
		_bts.setForcesAppliedFlag();
		char s[20];
		sprintf(s, "H_%d", hookNum);
		_sutures.selectSuture(-1);
		_hooks.selectHook(hookNum);	// deselect hooks
		_selectedSurgObject = s;
		++_historyIt;
	}
	else if (_historyIt->HasKey("deleteHook"))
	{
		int hookNum = (*_historyIt)["deleteHook"].ToInt();
		snapshotForwardMove("hook deletion", _historyIt - _historyArray.begin());
		_bts.setPhysicsPause(false);
		_hooks.deleteHook(hookNum);
		_sutures.selectSuture(-1);
		_hooks.selectHook(-1);
		++_historyIt;
	}
	else if (_historyIt->HasKey("makeIncision"))
	{
		json::Array iArr = (*_historyIt)["makeIncision"].ToArray();
		json::Object iObj = iArr[0].ToObject();
		int i, incisPointNum;
		bool startIncis, endIncis;
		// for now ignore "incisedObject"] as there is only one incisable object.  May change this later.
		startIncis = iObj["Tin"].ToBool();
		endIncis = iObj["Tout"].ToBool();
		incisPointNum = iObj["pointNumber"].ToInt();
		std::vector<Vec3f> positions, normals;
		positions.assign(incisPointNum, Vec3f());
		normals.assign(incisPointNum, Vec3f());
		materialTriangles *mtp = _sg.getMaterialTriangles();
		for (i = 0; i < incisPointNum; ++i)
		{
			iObj = iArr[i + 1].ToObject();
			if (!iObj.HasKey("incisionPoint")) {
				sendUserMessage("There is an error in this history file.  Truncating from this point forward-", "", false);
				if (_historyIt != _historyArray.end()) {
					json::Array tarr;
					for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
						tarr.push_back(*it);
					_historyArray.Clear();
					_historyArray = tarr;
					_historyIt = _historyArray.end();
				}
				_bts.setPhysicsPause(false);
				return;
			}
			json::Object ipObj = iObj["incisionPoint"].ToObject();
			int tri;
			int material;
			float hTx[2], uv[2];
			material = ipObj["material"].ToInt();
			json::Array sArr = ipObj["historyTexture"].ToArray();
			hTx[0] = sArr[0].ToFloat();
			hTx[1] = sArr[1].ToFloat();
			Vec3f hV;
			sArr.Clear();
			sArr = ipObj["displacement"];
			hV[0] = sArr[0].ToFloat();
			hV[1] = sArr[1].ToFloat();
			hV[2] = sArr[2].ToFloat();
			if (!getHistoryAttachPoint(material, hTx, hV, tri, uv, false)) {
				std::string msg = "History file incision point location failure.";
				historyAttachFailure(msg);
				return;
			}
			assert(mtp->triangleMaterial(tri) == 2);
			mtp->getBarycentricPosition(tri, uv, positions[i].xyz);
			mtp->getBarycentricNormal(tri, uv, normals[i].xyz);
		}
		snapshotForwardMove("incision", _historyIt - _historyArray.begin());
		if (!_incisions.skinCut(positions, normals, startIncis, endIncis)) {
			_bts.setPhysicsPause(false);
			sendUserMessage((std::string("Incision in history file failed. ") + skinCutUndermineTets::lastRefusal).c_str(), "Program error", false);
		}
		else {
			if (_incisions.physicsRecutRequired()) {
				_bts.setForcesAppliedFlag();
				// Unfortunately this recurring code block doesn't work if put into a lambda. Only Intel knows-
				physicsDone = false;
				_ffg->physicsDrag = true;
				tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
					try {
						_bts.updateOldPhysicsLattice();
						newTopology = true;
						physicsDone = true;
					}
					catch (...) {
						physicsDone = true;
						_ffg->physicsDrag = false;
						taskThreadErrorStr = "Couldn't update physics after incision requiring recut.";
						taskThreadError = true;
					}
					}
				);
			}
			else
				newTopology = true;
		}
		++_historyIt;
	}
	else if (_historyIt->HasKey("undermine"))
	{
		_bts.updateSurfaceDraw();
		json::Array pArr, uArr = (*_historyIt)["undermine"].ToArray();
		float hTx[2], uv[2];
		int tri;
		int material;
		Vec3f hVec;
		json::Object uObj, pObj;
		snapshotForwardMove("undermine", _historyIt - _historyArray.begin());
		for (int n = (int)uArr.size(), i = 0; i < n; ++i) {
			uObj = uArr[i].ToObject();
			pObj = uObj["underminePoint"].ToObject();
			material = pObj["material"].ToInt();
			bool ic = true;  // compatibility with old history files
			if(pObj.HasKey("incisionConnect"))
				ic = pObj["incisionConnect"].ToBool();
			pArr = pObj["historyTexture"].ToArray();
			hTx[0] = pArr[0].ToFloat();
			hTx[1] = pArr[1].ToFloat();
			pArr = pObj["displacement"].ToArray();
			hVec[0] = pArr[0].ToFloat();
			hVec[1] = pArr[1].ToFloat();
			hVec[2] = pArr[2].ToFloat();
			if (!getHistoryAttachPoint(material, hTx, hVec, tri, uv, false)) {
				std::string msg = "History file undermine point location failure.";
				historyAttachFailure(msg);
				return;
			}
			_incisions.addUndermineTriangle(tri, 2, ic);
		}
		_gl3w->drawAll();
		glfwSwapBuffers(_ffg->FFwindow);
		std::this_thread::sleep_for(std::chrono::milliseconds(800));
		_incisions.undermineSkin();
		_undermineTriangles.clear();
		physicsDone = false;
		_ffg->physicsDrag = true;
		tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
			try {
				_bts.updateOldPhysicsLattice();
				newTopology = true;
				physicsDone = true;
			}
			catch (...) {
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadErrorStr = "Topology error following an undermine.";
				taskThreadError = true;
			}
			}
		);
		++_historyIt;
	}
	else if (_historyIt->HasKey("excise"))
	{
		json::Object exciseObj = (*_historyIt)["excise"].ToObject();
		json::Array pArr;
		float hTx[2], uv[2];
		int tri;
		int material;
		Vec3f hVec;
		material = exciseObj["material"].ToInt();
		pArr = exciseObj["historyTexture"].ToArray();
		hTx[0] = pArr[0];
		hTx[1] = pArr[1];
		pArr.Clear();
		pArr = exciseObj["displacement"].ToArray();
		hVec[0] = pArr[0];
		hVec[1] = pArr[1];
		hVec[2] = pArr[2];
		if (!getHistoryAttachPoint(material, hTx, hVec, tri, uv, false)) {
			std::string msg = "History file excise point location failure.";
			historyAttachFailure(msg);
			return;
		}
		if (!exciseRegionIsBounded(_sg.getMaterialTriangles(), tri)) {
			std::string msg = "History file excise region is not enclosed by incisions.";
			historyAttachFailure(msg);
			return;
		}
		snapshotForwardMove("excise", _historyIt - _historyArray.begin());
		_incisions.excise(tri);

		physicsDone = false;
		_ffg->physicsDrag = true;
		tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
			try {
				_bts.updateOldPhysicsLattice();
				newTopology = true;
				physicsDone = true;
			}
			catch (std::exception& e) {  // name the underlying failure in the dialog
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadErrorStr = std::string("Topology error found after excision: ") + e.what();
				taskThreadError = true;
			}
			catch (...) {
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadErrorStr = "Topology error found after excision.";
				taskThreadError = true;
			}
			}
		);
		++_historyIt;
	}
	else if (_historyIt->HasKey("addSuture"))
	{
		json::Object sutureObj = (*_historyIt)["addSuture"].ToObject();
		int edge, sutNum = sutureObj["sutureNum"].ToInt();
		float param, uv[2], xyz[3];
		if (_sutures.getNumberOfSutures() < 1) {	// initialize sutures
			_sutures.setSutureSize(_sg.getSceneNode()->getRadius()*0.003f);
			_sutures.setShapes(_gl3w->getShapes());
			_sutures.setGLmatrices(_gl3w->getGLmatrices());
			_sutures.setPhysicsLattice(_bts.getPdTetPhysics_2());
			_sutures.setVnBccTetrahedra(_bts.getVirtualNodedBccTetrahedra());
			_sutures.setSurgicalActions(this);
		}
		materialTriangles *tr = _sg.getMaterialTriangles();
		int material;
		float hTx[2];
		Vec3f hVec;
		json::Array pArr = sutureObj["historyTexture0"].ToArray();
		hTx[0] = pArr[0].ToFloat();
		hTx[1] = pArr[1].ToFloat();
		pArr.Clear();
		pArr = sutureObj["displacement0"].ToArray();
		hVec[0] = pArr[0].ToFloat();
		hVec[1] = pArr[1].ToFloat();
		hVec[2] = pArr[2].ToFloat();
		material = sutureObj["material0"].ToInt();
		int eTri;
		if (!getHistoryAttachPoint(material, hTx, hVec, eTri, uv, material == 2 ? true : false)) {
			std::string msg = "Attempted attachment of suture number ";
			msg.append(std::to_string(sutNum));
			msg.append(" in history file failed.");
			historyAttachFailure(msg);
			return;
		}
		assert(material == tr->triangleMaterial(eTri));
		if (material == 2) {
			if (uv[1] == 0.0f) {
				edge = 0;
				param = uv[0];
			}
			else if (uv[0] == 0.0f) {
				edge = 2;
				param = 1.0f - uv[1];
			}
			else if (uv[0] + uv[1] > 0.998f) {
				edge = 1;
				param = uv[1];
			}
			else
				assert(false);
		}
		else {
			if (uv[0] + uv[1] > 0.67f) {  // force to an edge
				edge = 1;
				param = uv[1] / (uv[0] + uv[1]);
			}
			else if (uv[0] > uv[1]) {
				edge = 0;
				param = uv[0];
			}
			else {
				edge = 2;
				param = 1.0f - uv[1];
			}
		}
		int sn = _sutures.addUserSuture(tr, eTri, edge, param);
		assert(_sutures.baseToUserSutureNumber(sn) == sutNum);
		pArr.Clear();
		pArr = sutureObj["historyTexture1"].ToArray();
		hTx[0] = pArr[0].ToFloat();
		hTx[1] = pArr[1].ToFloat();
		pArr.Clear();
		pArr = sutureObj["displacement1"].ToArray();
		hVec[0] = pArr[0].ToFloat();
		hVec[1] = pArr[1].ToFloat();
		hVec[2] = pArr[2].ToFloat();
		material = sutureObj["material1"].ToInt();
		if (!getHistoryAttachPoint(material, hTx, hVec, eTri, uv, material == 2 ? true : false)) {
			std::string msg = "Attempted attachment of suture number ";
			msg.append(std::to_string(sutNum));
			msg.append(" in history file failed.");
			historyAttachFailure(msg);
			return;
		}
		tr->getBarycentricPosition(eTri, uv, xyz);
		assert(material == tr->triangleMaterial(eTri));
		if (material == 2) {
			if (uv[1] == 0.0f) {
				edge = 0;
				param = uv[0];
			}
			else if (uv[0] == 0.0f) {
				edge = 2;
				param = 1.0f - uv[1];
			}
			else if (uv[0] + uv[1] > 0.998f) {
				edge = 1;
				param = uv[1];
			}
			else
				assert(false);
		}
		else {
			if (uv[0] + uv[1] > 0.67f) {  // force to an edge
				edge = 1;
				param = uv[1] / (uv[0] + uv[1]);
			}
			else if (uv[0] > uv[1]) {
				edge = 0;
				param = uv[0];
			}
			else {
				edge = 0;
				param = 1.0f - uv[1];
			}
		}
		if (sutureObj["linked"].ToBool())
			_sutures.setLinked(sn, true);
		int sRet = _sutures.setSecondEdge(sn, tr, eTri, edge, param);
		_bts.setForcesAppliedFlag();
		if (!_bts.getPdTetPhysics_2()->solverInitialized()) {  // solver must be initialized to add a suture
			physicsDone = false;
			_ffg->physicsDrag = true;
			tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
				try {
					_bts.initPdPhysics();
					physicsDone = true;
				}
				catch (...) {
					physicsDone = true;
					_ffg->physicsDrag = false;
					taskThreadError = true;
					taskThreadErrorStr = "Couldn't initialize physics after adding hook.";
				}
				}
			);
		}
		if (sRet < 1)
			_sutures.setSecondVertexPosition(sn, xyz);
		else if (sRet < 2) {
			sendUserMessage("Trying to suture to same side of incision is not allowed-", "USER ERROR", false);
			_selectedSurgObject = "";
			setToolState(0);
			++_historyIt;
			return;
		}
		else
			assert(false);
		if(_sutures.isLinked(sn)){
			physicsDone = false;
			_ffg->physicsDrag = true;
			_sutures.laySutureLine(sn);
			physicsDone = true;
		}
		else
			_sutures.setLinked(sn, false);
		_hooks.selectHook(-1);	// deselect hooks
		_sutures.selectSuture(sn);
		char s[20];
#ifdef _WINDOWS
		sprintf_s(s, 19, "S_%d", sn);
#else
		sprintf(s, "S_%d", sn);
#endif
		_selectedSurgObject = s;
		++_historyIt;
		if (_historyIt == _historyArray.end()) {  // automatically promote any fake sutures if this is the last one
			while (!physicsDone)  // physics update thread must be complete before doing next op.
				std::this_thread::sleep_for(std::chrono::milliseconds(20));
			_bts.promoteSutures();
		}
	}
	else if (_historyIt->HasKey("deleteSuture"))
	{
		int sutNum;
		if ((*_historyIt)["deleteSuture"].GetType() == json::ObjectVal) {
			sutNum = (*_historyIt)["deleteSuture"]["autoSuturesFor"].ToInt();
			sutNum = _sutures.userToBaseSutureNumber(sutNum);
			++sutNum;
			if (_sutures.baseToUserSutureNumber(sutNum) < 0)
				_sutures.deleteSuture(sutNum);
		}
		else {
			sutNum = (*_historyIt)["deleteSuture"].ToInt();
			sutNum = _sutures.userToBaseSutureNumber(sutNum);
			_sutures.deleteSuture(sutNum);
		}
		_sutures.selectSuture(-1);
		_hooks.selectHook(-1);
		++_historyIt;
	}
	else if (_historyIt->HasKey("makeDeepCut"))
	{
		// COURT - this is somewhat flawed. If physics state on creation is different than that on execution, the deep side of the normal may have very different outcomes.
		// consider assuring a certain number of physics iterations before execution.  Could also put position of deep post point in history and compute N.  Intermediate
		// incision intersections would still be different.
		json::Array pArr, iArr = (*_historyIt)["makeDeepCut"].ToArray();
		json::Object iObj = iArr[0].ToObject();
		assert(iObj["deepCutObject"].ToInt() == 0);	// for now only one object incisable
		bool startOpen, endOpen;
		startOpen = iObj["openIn"].ToBool();
		endOpen = iObj["openOut"].ToBool();
		int pointNum = iObj["pointNumber"].ToInt();
		materialTriangles* tr = _sg.getMaterialTriangles();
		_incisions.clearDeepCutter();
		if (!_fence.isInitialized()) {	// initialize fence
			_fence.setFenceSize(tr->getDiameter() * 0.01f);
			_fence.setGl3wGraphics(_gl3w);
		}
		_fence.clear();
		float hTx[2], uv[2];
		int tri;
		int material;
		Vec3f hVec, postN, xyz;
		json::Object uObj, pObj;
		for (int i = 0; i < pointNum; ++i) {
			uObj = iArr[i + 1].ToObject();
			if (!uObj.HasKey("deepCutPoint")) {
				sendUserMessage("There is an error in this history file.  Truncating from this point forward-", "", false);
				if (_historyIt != _historyArray.end()) {
					json::Array tarr;
					for (json::Array::ValueVector::iterator it = _historyArray.begin(); it != _historyIt; ++it)
						tarr.push_back(*it);
					_historyArray.Clear();
					_historyArray = tarr;
					_historyIt = _historyArray.end();
				}
				return;
			}
			pObj = uObj["deepCutPoint"].ToObject();
			material = pObj["material"].ToInt();
			pArr = pObj["historyTexture"].ToArray();
			hTx[0] = pArr[0].ToFloat();
			hTx[1] = pArr[1].ToFloat();
			pArr = pObj["displacement"].ToArray();
			hVec[0] = pArr[0].ToFloat();
			hVec[1] = pArr[1].ToFloat();
			hVec[2] = pArr[2].ToFloat();
			if (!getHistoryAttachPoint(material, hTx, hVec, tri, uv, false)) {
				std::string msg = "Can't retrieve deep cut point from history file.";
				historyAttachFailure(msg);
				return;
			}
			tr->getBarycentricPosition(tri, uv, xyz.xyz);
			pArr = pObj["postNormal"].ToArray();
			postN.X = pArr[0].ToFloat();
			postN.Y = pArr[1].ToFloat();
			postN.Z = pArr[2].ToFloat();
			if (i == pointNum - 1)
				startOpen = endOpen;
			else if (i > 0)
				startOpen = false;
			else
				;
			_fence.addPost(tr, tri, xyz.xyz, postN.xyz, false, true, startOpen);
		}
		if (!_incisions.inputCorrectFence(&_fence, _ffg)) {
			sendUserMessage("The deepCut setup in this history file failed. You may try again-", "PROGRAM ERROR");
			_fence.clear();
			_incisions.clearDeepCutter();
			_ffg->setToolState(0);
			setToolState(0);
			_bts.setPhysicsPause(false);
			physicsDone = true;
			_ffg->physicsDrag = false;
			return;
		}
		_bts.updateSurfaceDraw();
		snapshotForwardMove("deep cut", _historyIt - _historyArray.begin());
		if (!_incisions.cutDeep()) {
			taskThreadError = true;
			taskThreadErrorStr = "Attempted deep cut failed.";
		}

		physicsDone = false;
		_ffg->physicsDrag = true;
		tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
			try {
				_bts.updateOldPhysicsLattice();
				newTopology = true;
				physicsDone = true;
			}
			catch (...) {
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadError = true;
				taskThreadErrorStr = "Topology error found after deepCut.";
			}
			}
		);
		_fence.clear();
		++_historyIt;
	}
	else if (_historyIt->HasKey("periostealUndermine")) {
		_bts.updateSurfaceDraw();
		json::Array pArr, uArr = (*_historyIt)["periostealUndermine"].ToArray();
		float hTx[2], uv[2];
		int tri;
		int material;
		Vec3f hVec;
		json::Object uObj, pObj;
		snapshotForwardMove("periosteal undermine", _historyIt - _historyArray.begin());
		for (int n = (int)uArr.size(), i = 0; i < n; ++i) {
			uObj = uArr[i].ToObject();
			pObj = uObj["periostealTriangle"].ToObject();
			material = pObj["material"].ToInt();
			pArr = pObj["historyTexture"].ToArray();
			bool ic = true;
			if (pObj.HasKey("incisionConnect"))
				ic = pObj["incisionConnect"].ToBool();
			hTx[0] = pArr[0].ToFloat();
			hTx[1] = pArr[1].ToFloat();
			pArr = pObj["displacement"].ToArray();
			hVec[0] = pArr[0].ToFloat();
			hVec[1] = pArr[1].ToFloat();
			hVec[2] = pArr[2].ToFloat();
			if (!getHistoryAttachPoint(material, hTx, hVec, tri, uv, false)) {
				std::string msg = "Can't retrieve periosteal undermine point from history file.";
				historyAttachFailure(msg);
				return;
			}
			_incisions.addPeriostealUndermineTriangle(tri, hVec, ic);
		}
		// all periosteal undermine triangles now marked as material 10
		_incisions.clearCurrentUndermine(8);  // set all periosteal undermined triangles to material 8 and reset.
		_bts.fixPeriostealPeriferalVertices();
		physicsDone = false;
		_ffg->physicsDrag = true;
		tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
			try {
				_bts.nonTetPhysicsUpdate();
				newTopology = true;
				physicsDone = true;
			}
			catch (...) {
				physicsDone = true;
				_ffg->physicsDrag = false;
				taskThreadError = true;
				taskThreadErrorStr = "Error occurred after a periosteal undermine";
			}
			}
		);
		++_historyIt;
	}
	else if (_historyIt->HasKey("revert"))
	{
		// The live session reverted a move that had left no history record: the
		// replayed state already equals the state that revert restored.
		++_historyIt;
	}
	else if ((_historyIt->HasKey("undo") || _historyIt->HasKey("redo")) && (_historyIt->HasKey("undo") ? (*_historyIt)["undo"] : (*_historyIt)["redo"]).ToObject().HasKey("noRecord"))
	{
		// An undo/redo of a move that never reached the history (a post, a mark, a
		// grab): the replay holds nothing to take back or bring back.
		++_historyIt;
	}
	else if (_historyIt->HasKey("undo") || _historyIt->HasKey("redo"))
	{
		// Recorded undo/redo (full-trail replay): perform the restore, then keep
		// walking THIS array instead of the snapshot's history.
		const bool isRedo = _historyIt->HasKey("redo");
		json::Array trail = _historyArray;
		size_t pos = (size_t)(_historyIt - _historyArray.begin());
		onKeyDown(isRedo ? SKINFLAPS_REDO_KEY : SKINFLAPS_UNDO_KEY);
		_historyArray = trail;
		_historyIt = _historyArray.begin() + pos + 1;
	}
	else if (_historyIt->HasKey("promoteSutureApproximations"))
	{
		_bts.promoteSutures();
		++_historyIt;
	}
	else if (_historyIt->HasKey("pausePhysics"))
	{
		_bts.setPhysicsPause(true);
		++_historyIt;
		return;  // don't setToolState(0) as will unpause physics
	}
	else
		++_historyIt;
	_ffg->setToolState(0);
	setToolState(0);
	_bts.setPhysicsPause(false);
}

bool surgicalActions::saveCurrentObj(const char* fullFilePath, const char* fileNamePrefix) {
	materialTriangles* tr = _sg.getMaterialTriangles();
	if (tr == nullptr)
		return false;
	return tr->writeObjFile(fullFilePath, fileNamePrefix);
}
