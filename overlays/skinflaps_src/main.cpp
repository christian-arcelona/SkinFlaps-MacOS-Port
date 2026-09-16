// Dear ImGui: User interface for GLFW + OpenGL 3, using programmable pipeline
// (GLFW is a cross-platform general purpose library for handling windows, inputs, OpenGL/Vulkan/Metal graphics context creation, etc.)
// If you are new to Dear ImGui, read documentation from the docs/ folder + read the top of imgui.cpp.
// Read online: https://github.com/ocornut/imgui/tree/master/docs

#include <stdio.h>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#include <tbb/task_arena.h>
#include <tbb/global_control.h>
#include <atomic>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#include "surgicalActions.h"
#include "Vec3f.h"
#include <gl3wGraphics.h>
#include "FacialFlapsGui.h"

FacialFlapsGui ffg;

// Performance-core count on Apple Silicon, or 0 if it can't be determined.
static int skinflaps_pcore_count()
{
#ifdef __APPLE__
	int n = 0;
	size_t sz = sizeof(n);
	if (sysctlbyname("hw.perflevel0.physicalcpu", &n, &sz, nullptr, 0) == 0 && n > 0)
		return n;
#endif
	return 0;
}

int main(int, char**)
{
	// Session log: stderr goes to <log dir>/skinflaps_app_<stamp>.log so the
	// lines written around a failure can accompany its error files. The
	// installed app keeps it where those files go (its own History folder); a
	// build run from the source tree keeps it in ~/skinflaps_logs. The file is
	// removed at exit unless an error history was written this session.
	std::string appLogPath;
	{
		const char* home = std::getenv("HOME");
		std::string dir;
		if (FacialFlapsGui::installedBundle())
			dir = FacialFlapsGui::userDataDirectory();  // empty when nothing under the home folder is writable
		if (dir.empty() && home)
			dir = std::string(home) + "/skinflaps_logs";
		if (!dir.empty()) {
			std::error_code ec;
			std::filesystem::create_directories(dir, ec);
			appLogPath = dir + "/skinflaps_app_" + skinflapsSessionStamp() + ".log";
			if (!std::freopen(appLogPath.c_str(), "a", stderr))
				appLogPath.clear();
			skinflapsAppLogPath = appLogPath;
		}
	}
	// Cap worker threads to the Performance-core count. Apple Silicon E-cores
	// run far slower than P-cores; in the barrier-synchronized PD solve they
	// become stragglers, so spreading across every core is slower than using
	// the P-cores alone.
	std::unique_ptr<tbb::global_control> threadCap;
	{
		const int n = skinflaps_pcore_count();
		if (n > 0) {
			const std::string ns = std::to_string(n);
			setenv("OMP_NUM_THREADS", ns.c_str(), 1);
			setenv("VECLIB_MAXIMUM_THREADS", ns.c_str(), 1);
			threadCap = std::make_unique<tbb::global_control>(
				tbb::global_control::max_allowed_parallelism, n);
		}
	}
	if (!ffg.initImguiGlfw()) {
		puts("Failed to open Glfw window.\n");
		return 1;
	}
	if (!ffg.initCleftSim()) {
		puts("Failed to initialize cleft simulator.\n");
		return 1;
	}
	surgicalActions* sa = ffg.getSurgicalActions();
	bccTetScene* bts = sa->getBccTetScene();
	sa->physicsDone = true;
	// Headless-friendly replay driver: SKINFLAPS_AUTORUN_HISTORY names a .hst
	// file to load at startup; SKINFLAPS_AUTORUN_STEPS actions are then
	// dispatched, each after the physics has settled, and the program exits.
	bool autoReplayEnabled = false;
	int autoReplayStepsTarget = 0;
	int autoReplayStepsQueued = 0;
	if (const char* autoReplayHistory = std::getenv("SKINFLAPS_AUTORUN_HISTORY");
			autoReplayHistory && *autoReplayHistory) {
		std::filesystem::path hp(autoReplayHistory);
		std::string historyDir = hp.parent_path().string();
		if (historyDir.empty())
			historyDir = ".";
		historyDir.push_back(std::filesystem::path::preferred_separator);
		const std::string historyFile = hp.filename().string();
		if (const char* stepsEnv = std::getenv("SKINFLAPS_AUTORUN_STEPS"); stepsEnv && *stepsEnv)
			autoReplayStepsTarget = std::max(0, std::atoi(stepsEnv));
		// The history's scene file is resolved against the model directory,
		// which the interactive menu path sets before loading.
		FacialFlapsGui::setDefaultDirectories();
		sa->setHistoryDirectory(historyDir.c_str());
		autoReplayEnabled = sa->loadHistory(historyDir.c_str(), historyFile.c_str());
	}
	if (!autoReplayEnabled)
		FacialFlapsGui::startSessionFromEnv();  // a window opened by another one with a history or model to load
	// Between replayed actions the physics must settle to (near-)equilibrium:
	// firing the next action onto an unsettled mesh crashes the topology
	// operations (deep cut, excise). A quivering region never fully stills, so
	// a plateau in per-solve motion also counts as settled.
	std::vector<float> autoPrevCoords;
	int   autoSettleSteps  = 0;
	int   autoPlateauSteps = 0;
	float autoBestDisp     = std::numeric_limits<float>::max();
	bool  autoStepSettled  = true;            // first action dispatches immediately
	const float autoSettleEps  = 1.0e-5f;     // near-equilibrium per-solve max node displacement
	const int   autoPlateauWin = 50;          // consecutive non-decreasing solves => settled
	const int   autoSettleCap  = 1200;        // hard cap on solves per action
	bool updateThrow = false;
	bool physicsErrorLatch = false;  // a task-thread error is terminal; no physics task may be enqueued after one
	while (!glfwWindowShouldClose(ffg.FFwindow))
	{
		try {
				// Poll and handle events (inputs, window resize, etc.)
			// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
			// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
			// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
			// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
			glfwPollEvents();
			// Start the Dear ImGui frame
			ImGui_ImplOpenGL3_NewFrame();
			ImGui_ImplGlfw_NewFrame();
			ImGui::NewFrame();
			if (FacialFlapsGui::physicsDrag)
				ffg.showHourglass();
			ffg.InstanceCleftGui();

			// Rendering
			ImGui::Render();
			ImVec4 clear_color = ImVec4(0.0f, 0.0f, 0.0f, 1.00f);
			glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
			glClear(GL_COLOR_BUFFER_BIT);

			if (sa->taskThreadError && sa->taskThreadErrorHoldable && skinflapsHoldRevertAvailable()) {
				// The failed task followed a forward move whose snapshot can
				// restore the state before it (a deep cut whose physics
				// initialization the solver refused, a hook drag whose solve
				// went non-finite). Nothing irreversible happened to that
				// state, so this becomes an error hold -- inspect, then Close
				// reverts -- instead of the terminal dialog. Physics must stay
				// paused meanwhile: the action un-paused it, and the loop below
				// would otherwise enqueue a solve on the half-initialized
				// solver every frame. A failed restore still falls back to
				// save-and-exit from the hold's Close.
				sa->taskThreadErrorHoldable = false;
				const std::string err = sa->taskThreadErrorStr;
				bts->setPhysicsPause(true);
				sa->newTopology = true;  // draw the cut surface for the inspection
				sa->taskThreadError = false;
				if (!FacialFlapsGui::undoAfterError(err.c_str())) {
					physicsErrorLatch = true;
					ffg.handleThrow(err.c_str());
					throw(std::logic_error(err));
				}
			}
			if (sa->taskThreadError) {
				sa->taskThreadErrorHoldable = false;
				// A task-thread error is terminal: it flows to handleThrow and
				// the save-and-exit dialog. Latch here so no further physics
				// tasks are enqueued afterward -- the error state would only
				// rethrow identically every frame. (The catch must still set
				// physicsDone to keep waiters from deadlocking, so that flag
				// cannot serve as the latch.)
				physicsErrorLatch = true;
				sa->taskThreadError = false;
				std::string err = sa->taskThreadErrorStr;
				ffg.handleThrow(err.c_str());
				throw(std::logic_error(err));
			}

			if (sa->physicsDone) {
				// draw last physics result before starting a new solve
				// Unfortunately all graphics calls must be executed fom the master thread.
				if (sa->newTopology) {
					sa->getSurgGraphics()->setNewTopology();
					sa->getSurgGraphics()->updatePositionsNormalsTangents();
					sa->newTopology = false;
				}
				if (autoReplayEnabled && !autoStepSettled) {
					vnBccTetrahedra* vnt = bts->getVirtualNodedBccTetrahedra();
					int nn = vnt ? vnt->nodeNumber() : 0;
					const Vec3f* nc = nullptr;
					try { if (nn > 0) nc = vnt->getNodeSpatialCoordPointer(); } catch (...) { nc = nullptr; }
					if (nc && nn > 0) {
						if ((int)autoPrevCoords.size() == nn * 3) {
							float maxDisp2 = 0.0f;
							for (int i = 0; i < nn; ++i) {
								float dx = nc[i].xyz[0] - autoPrevCoords[i*3];
								float dy = nc[i].xyz[1] - autoPrevCoords[i*3+1];
								float dz = nc[i].xyz[2] - autoPrevCoords[i*3+2];
								float d2 = dx*dx + dy*dy + dz*dz;
								if (d2 > maxDisp2) maxDisp2 = d2;
							}
							float maxDisp = std::sqrt(maxDisp2);
							++autoSettleSteps;
							if (maxDisp < autoSettleEps) autoStepSettled = true;
							else if (maxDisp < autoBestDisp * 0.98f) { autoBestDisp = maxDisp; autoPlateauSteps = 0; }
							else if (++autoPlateauSteps >= autoPlateauWin) autoStepSettled = true;
							if (!autoStepSettled && autoSettleSteps >= autoSettleCap) autoStepSettled = true;
						}
						autoPrevCoords.resize(nn * 3);
						for (int i = 0; i < nn; ++i) {
							autoPrevCoords[i*3]   = nc[i].xyz[0];
							autoPrevCoords[i*3+1] = nc[i].xyz[1];
							autoPrevCoords[i*3+2] = nc[i].xyz[2];
						}
					}
				}
				if (bts->forcesApplied()) {
					sa->getSutures()->updateSutureGraphics();
					if (sa->getSurgGraphics()->getSceneNode()->visible)
						bts->updateSurfaceDraw();
					else {  // draw only tets without the surface
						if (ffg.getgl3wGraphics()->getLines()->getSceneNode() && ffg.getgl3wGraphics()->getLines()->getSceneNode()->visible)
							bts->drawTetLattice();
					}
				}
				if (ffg.physicsDrag)  //  && ffg.loadFile.empty()
					ffg.physicsDrag = false;
				if (autoReplayEnabled && autoReplayStepsQueued < autoReplayStepsTarget &&
						ffg.nextCounter == 0 && autoStepSettled) {
					++ffg.nextCounter;
					++autoReplayStepsQueued;
					autoStepSettled  = false;
					autoSettleSteps  = 0;
					autoPlateauSteps = 0;
					autoBestDisp     = std::numeric_limits<float>::max();
					autoPrevCoords.clear();
				}
				if (ffg.nextCounter > 0 && !skinflapsErrorHold) {  // a queued step waits while an error hold is up, as live input does
					ffg.getSurgicalActions()->nextHistoryAction();
					--ffg.nextCounter;
				}
				else{
				// below is from: https://www.intel.com/content/www/us/en/develop/documentation/onetbb-documentation/top/onetbb-developer-guide/design-patterns/gui-thread.html
					// physicsDone recheck necessary since nextHistoryAction() may have spawned a task that this one would collide with;
					// no solve during an error hold (the solver may be half-initialized)
					if (bts->forcesApplied() && !bts->isPhysicsPaused() && !physicsErrorLatch && !skinflapsErrorHold) {
						sa->physicsDone = false;
						tbb::task_arena(tbb::task_arena::attach()).enqueue([&]() {
							try {
								bts->updatePhysics();
								sa->physicsDone = true;
							}
							catch (const std::exception& ex) {
								// physicsDone must flip back even on a throw, or
								// nextHistoryAction's busy-wait deadlocks and the
								// Next button stops dispatching.
								sa->physicsDone = true;
								updateThrow = true;
								sa->taskThreadErrorStr = std::string("Couldn't update physics after last action: ") + ex.what();
								sa->taskThreadErrorHoldable = true;  // the forward move's snapshot is the finite state before it (e.g. a non-finite solve after a hook drag)
								sa->taskThreadError = true;
							}
							catch (...) {
								sa->physicsDone = true;
								updateThrow = true;
								sa->taskThreadErrorStr = "Couldn't update physics after last action (unknown exception).";
								sa->taskThreadErrorHoldable = true;
								sa->taskThreadError = true;
							}
							}
						);
					}
				}
			}
			if (autoReplayEnabled && autoReplayStepsQueued >= autoReplayStepsTarget &&
					ffg.nextCounter == 0 && autoStepSettled)
				glfwSetWindowShouldClose(ffg.FFwindow, 1);
			ffg.getgl3wGraphics()->drawAll();

			ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());  // Always do this last so it prints GUI on top of your scene
		}
		catch (const std::runtime_error& re) {
			ffg.nextCounter = 0;
			std::string err = "Program runtime error occurred.\n";
			err += re.what();
			ffg.handleThrow(err.c_str());
		}
		catch (const std::logic_error& le){
			ffg.nextCounter = 0;
			std::string err = "Program logic error occurred.\n";
			err += le.what();
			ffg.handleThrow(err.c_str());
		}
		catch (const std::bad_alloc& ba) {
			ffg.nextCounter = 0;
			std::string err = "Not enough memory in this machine to handle this program.\n";
			err += ba.what();
			ffg.handleThrow(err.c_str());
		}
		catch (...) {
			ffg.nextCounter = 0;
			// catch any other errors
			ffg.handleThrow("Unspecified program error occurred.\n");
		}
		glfwSwapBuffers(ffg.FFwindow);
	}
	while (!updateThrow && !sa->physicsDone)
		;
	ffg.destroyImguiGlfw();
	if (!appLogPath.empty() && !skinflapsErrorHistorySaved) {  // clean session: nothing worth keeping
		std::fflush(stderr);
		std::remove(appLogPath.c_str());
	}
    return 0;
}
