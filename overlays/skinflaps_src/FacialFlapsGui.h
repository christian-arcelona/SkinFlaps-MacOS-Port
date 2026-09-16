// Author: Court Cutting
// Date: November 4, 2020
// Purpose: New gui for cleftSim app using GLFW3, dear imgui and nativeFileDialog
// Copyright 2020 - All rights reserved at this time.

#ifndef _FACIAL_FLAPS_GUI_
#define _FACIAL_FLAPS_GUI_

#ifdef WIN32
#include <direct.h>
#define GetCurrentDir _getcwd
#else
#include <unistd.h>
#include <sys/stat.h>
#define GetCurrentDir getcwd
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

//#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "ImGuiFileDialog.h"
#include "ImGuiFileDialogConfig.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <string>
#include <fstream>
#include <vector>
#ifndef _WIN32
#include <spawn.h>
extern char **environ;
#endif
#include <tbb/task_arena.h>
#include <gl3wGraphics.h>
#include "surgicalActions.h"

// Whole-state undo. The restore itself runs inside surgicalActions::onKeyDown
// under these synthetic key codes (GLFW key codes are never negative);
// surgicalActions reports the outcome through the flag so the GUI can choose
// between "reverted, carry on" and save-and-exit.
constexpr int SKINFLAPS_UNDO_KEY = -7701;
constexpr int SKINFLAPS_UNDO_AFTER_ERROR_KEY = -7702;
constexpr int SKINFLAPS_REDO_KEY = -7703;
extern bool skinflapsUndoRestoreSucceeded;
extern bool skinflapsHoldRevertAvailable();  // a state to revert to exists for an error hold (the redo stack after an undo)
extern bool skinflapsLastMoveWasUndo();
// Error hold: after a surgical action throws, the failed state stays on
// screen for inspection (camera only; surgical input is blocked) until the
// dialog's Close button, which performs the revert.
inline bool skinflapsErrorHold = false;
inline bool skinflapsErrorHistorySaved = false;  // an ERROR_<stamp> pair was written this session (keeps the app log)
inline std::string skinflapsAppLogPath;  // this session's stderr log when the app opened one; the error files go beside it and File > Save session files copies it
// Session stamp: the process start time, fixed at first use, so that
// ERROR_<stamp>_<n>.hst pairs with skinflaps_app_<stamp>.log.
// One instance for the whole program (inline, not static): a static function's
// local static is a copy per source file, and the app log (opened at start-up)
// and the error files (written at the first failure) would get stamps seconds
// apart.
inline const std::string& skinflapsSessionStamp()
{
	static const std::string stamp = []() {
		char buf[32]; std::time_t now = std::time(nullptr);
		std::strftime(buf, sizeof buf, "%Y%m%d_%H%M%S", std::localtime(&now));
		return std::string(buf); }();
	return stamp;
}
inline std::string skinflapsErrorHoldWhat;

#ifdef _WIN32
static constexpr const char* SKINFLAPS_PATH_SEP = "\\";
#else
static constexpr const char* SKINFLAPS_PATH_SEP = "/";
#endif

static ImGuiKey ImGui_ImplGlfw_KeyToImGuiKey(int key)
{
	switch (key)
	{
	case GLFW_KEY_TAB: return ImGuiKey_Tab;
	case GLFW_KEY_LEFT: return ImGuiKey_LeftArrow;
	case GLFW_KEY_RIGHT: return ImGuiKey_RightArrow;
	case GLFW_KEY_UP: return ImGuiKey_UpArrow;
	case GLFW_KEY_DOWN: return ImGuiKey_DownArrow;
	case GLFW_KEY_PAGE_UP: return ImGuiKey_PageUp;
	case GLFW_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
	case GLFW_KEY_HOME: return ImGuiKey_Home;
	case GLFW_KEY_END: return ImGuiKey_End;
	case GLFW_KEY_INSERT: return ImGuiKey_Insert;
	case GLFW_KEY_DELETE: return ImGuiKey_Delete;
	case GLFW_KEY_BACKSPACE: return ImGuiKey_Backspace;
	case GLFW_KEY_SPACE: return ImGuiKey_Space;
	case GLFW_KEY_ENTER: return ImGuiKey_Enter;
	case GLFW_KEY_ESCAPE: return ImGuiKey_Escape;
	case GLFW_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
	case GLFW_KEY_COMMA: return ImGuiKey_Comma;
	case GLFW_KEY_MINUS: return ImGuiKey_Minus;
	case GLFW_KEY_PERIOD: return ImGuiKey_Period;
	case GLFW_KEY_SLASH: return ImGuiKey_Slash;
	case GLFW_KEY_SEMICOLON: return ImGuiKey_Semicolon;
	case GLFW_KEY_EQUAL: return ImGuiKey_Equal;
	case GLFW_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
	case GLFW_KEY_BACKSLASH: return ImGuiKey_Backslash;
	case GLFW_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
	case GLFW_KEY_GRAVE_ACCENT: return ImGuiKey_GraveAccent;
	case GLFW_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
	case GLFW_KEY_SCROLL_LOCK: return ImGuiKey_ScrollLock;
	case GLFW_KEY_NUM_LOCK: return ImGuiKey_NumLock;
	case GLFW_KEY_PRINT_SCREEN: return ImGuiKey_PrintScreen;
	case GLFW_KEY_PAUSE: return ImGuiKey_Pause;
	case GLFW_KEY_KP_0: return ImGuiKey_Keypad0;
	case GLFW_KEY_KP_1: return ImGuiKey_Keypad1;
	case GLFW_KEY_KP_2: return ImGuiKey_Keypad2;
	case GLFW_KEY_KP_3: return ImGuiKey_Keypad3;
	case GLFW_KEY_KP_4: return ImGuiKey_Keypad4;
	case GLFW_KEY_KP_5: return ImGuiKey_Keypad5;
	case GLFW_KEY_KP_6: return ImGuiKey_Keypad6;
	case GLFW_KEY_KP_7: return ImGuiKey_Keypad7;
	case GLFW_KEY_KP_8: return ImGuiKey_Keypad8;
	case GLFW_KEY_KP_9: return ImGuiKey_Keypad9;
	case GLFW_KEY_KP_DECIMAL: return ImGuiKey_KeypadDecimal;
	case GLFW_KEY_KP_DIVIDE: return ImGuiKey_KeypadDivide;
	case GLFW_KEY_KP_MULTIPLY: return ImGuiKey_KeypadMultiply;
	case GLFW_KEY_KP_SUBTRACT: return ImGuiKey_KeypadSubtract;
	case GLFW_KEY_KP_ADD: return ImGuiKey_KeypadAdd;
	case GLFW_KEY_KP_ENTER: return ImGuiKey_KeypadEnter;
	case GLFW_KEY_KP_EQUAL: return ImGuiKey_KeypadEqual;
	case GLFW_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
	case GLFW_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
	case GLFW_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
	case GLFW_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
	case GLFW_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
	case GLFW_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
	case GLFW_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
	case GLFW_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
	case GLFW_KEY_MENU: return ImGuiKey_Menu;
	case GLFW_KEY_0: return ImGuiKey_0;
	case GLFW_KEY_1: return ImGuiKey_1;
	case GLFW_KEY_2: return ImGuiKey_2;
	case GLFW_KEY_3: return ImGuiKey_3;
	case GLFW_KEY_4: return ImGuiKey_4;
	case GLFW_KEY_5: return ImGuiKey_5;
	case GLFW_KEY_6: return ImGuiKey_6;
	case GLFW_KEY_7: return ImGuiKey_7;
	case GLFW_KEY_8: return ImGuiKey_8;
	case GLFW_KEY_9: return ImGuiKey_9;
	case GLFW_KEY_A: return ImGuiKey_A;
	case GLFW_KEY_B: return ImGuiKey_B;
	case GLFW_KEY_C: return ImGuiKey_C;
	case GLFW_KEY_D: return ImGuiKey_D;
	case GLFW_KEY_E: return ImGuiKey_E;
	case GLFW_KEY_F: return ImGuiKey_F;
	case GLFW_KEY_G: return ImGuiKey_G;
	case GLFW_KEY_H: return ImGuiKey_H;
	case GLFW_KEY_I: return ImGuiKey_I;
	case GLFW_KEY_J: return ImGuiKey_J;
	case GLFW_KEY_K: return ImGuiKey_K;
	case GLFW_KEY_L: return ImGuiKey_L;
	case GLFW_KEY_M: return ImGuiKey_M;
	case GLFW_KEY_N: return ImGuiKey_N;
	case GLFW_KEY_O: return ImGuiKey_O;
	case GLFW_KEY_P: return ImGuiKey_P;
	case GLFW_KEY_Q: return ImGuiKey_Q;
	case GLFW_KEY_R: return ImGuiKey_R;
	case GLFW_KEY_S: return ImGuiKey_S;
	case GLFW_KEY_T: return ImGuiKey_T;
	case GLFW_KEY_U: return ImGuiKey_U;
	case GLFW_KEY_V: return ImGuiKey_V;
	case GLFW_KEY_W: return ImGuiKey_W;
	case GLFW_KEY_X: return ImGuiKey_X;
	case GLFW_KEY_Y: return ImGuiKey_Y;
	case GLFW_KEY_Z: return ImGuiKey_Z;
	case GLFW_KEY_F1: return ImGuiKey_F1;
	case GLFW_KEY_F2: return ImGuiKey_F2;
	case GLFW_KEY_F3: return ImGuiKey_F3;
	case GLFW_KEY_F4: return ImGuiKey_F4;
	case GLFW_KEY_F5: return ImGuiKey_F5;
	case GLFW_KEY_F6: return ImGuiKey_F6;
	case GLFW_KEY_F7: return ImGuiKey_F7;
	case GLFW_KEY_F8: return ImGuiKey_F8;
	case GLFW_KEY_F9: return ImGuiKey_F9;
	case GLFW_KEY_F10: return ImGuiKey_F10;
	case GLFW_KEY_F11: return ImGuiKey_F11;
	case GLFW_KEY_F12: return ImGuiKey_F12;
	default: return ImGuiKey_None;
	}
}

class FacialFlapsGui {
public:
	// Versioned copies of the error histories, next to the app log, named with
	// the session stamp so they pair with skinflaps_app_<stamp>.log:
	// <dir>/ERROR_<stamp>_<n>.hst (the actions still in effect) and
	// <dir>/ERROR_<stamp>_<n>_full.hst (every action committed this session
	// plus an "undo" record wherever a restore happened, replayable through
	// History > Load). <dir> is the app log's folder when the log opened, else
	// the history folder.
	static std::string saveVersionedErrorHistories()
	{
		static int seq = 0;
		std::string dir = reportFolder();
		if (!dir.empty() && dir.back() != '/')
			dir += '/';
		std::string base = dir + "ERROR_" + skinflapsSessionStamp() + "_" + std::to_string(++seq);
		skinflapsErrorHistorySaved = true;
		const bool a = igSurgAct.saveSurgicalHistory((base + ".hst").c_str());
		const bool b = igSurgAct.saveFullHistoryTrail((base + "_full.hst").c_str());
		std::fprintf(stderr, "[error-history] %s.hst%s  %s_full.hst%s\n", base.c_str(), a ? "" : " (FAILED)", base.c_str(), b ? "" : " (FAILED)");
		return a ? base : std::string();
	}

	// After a surgical action throws: when a pre-action snapshot exists, keep
	// the failed state on screen under an error hold and tell the user what
	// failed; returns true when the session can simply continue after the
	// dialog's Close reverts. Otherwise the caller falls through to the
	// save-and-exit flow.
	static bool undoAfterError(const char* what)
	{
		if (!skinflapsHoldRevertAvailable())
			return false;
		skinflapsErrorHold = true;
		skinflapsErrorHoldWhat = what ? what : "";
		std::string msg = skinflapsErrorHoldWhat;
		// The history already holds the attempted action, except a hook drag or
		// a suture that failed before its record was written: write those now,
		// then save so the failure can be replayed and studied after the revert.
		// The paths are in the log ([error-history]); the dialog stays short.
		igSurgAct.recordInFlightHookMove();
		igSurgAct.recordInFlightSuture();
		std::string errHist = historyDirectory + "ERROR.hst";
		const bool saved = igSurgAct.saveSurgicalHistory(errHist.c_str());
		const std::string versioned = saveVersionedErrorHistories();
		msg += skinflapsLastMoveWasUndo() ? "\n\nClose reverts to the state before that undo." : "\n\nClose reverts to before this action.";
		if (!versioned.empty())
			msg += "\nSaved as " + versioned.substr(versioned.find_last_of('/') + 1) + ".hst in " + reportFolderForDisplay() + " (File > Show session folder).";
		else if (saved)
			msg += "\nSaved as ERROR.hst in " + reportFolderForDisplay() + ".";
		sendUserMessage(msg.c_str(), "Action failed - Close reverts");
		return true;
	}

	// Edit > Undo / Redo list entries: step back (or forward) several moves at once.
	static void dispatchUndoSteps(bool redo, int steps)
	{
		if (skinflapsErrorHold || steps < 1)
			return;
		try {
			igSurgAct.undoRedoSteps(redo ? SKINFLAPS_REDO_KEY : SKINFLAPS_UNDO_KEY, steps);
		}
		catch (std::exception& e) {
			if (!undoAfterError(e.what()))
				handleThrow(e.what());
		}
	}

	// Called from the dialog's Close button while an error hold is active.
	static void finishErrorHold()
	{
		skinflapsErrorHold = false;
		skinflapsUndoRestoreSucceeded = false;
		try {
			igSurgAct.onKeyDown(SKINFLAPS_UNDO_AFTER_ERROR_KEY);
		}
		catch (...) {
		}
		if (!skinflapsUndoRestoreSucceeded)
			handleThrow(skinflapsErrorHoldWhat.c_str());  // fall back to save-and-exit
	}

	static bool dispatchRightMouseDown(std::string& name, float(&position)[3], int triangle)
	{
		if (skinflapsErrorHold)
			return false;
		// A throw escaping a GLFW callback must unwind through Cocoa's event
		// dispatch to reach the main-loop handlers, and that unwind is not
		// guaranteed by the platform. Catch at the callback boundary and route
		// to the error hold or handleThrow — the same ERROR.hst + dialog
		// outcome — so failures never depend on it. Same pattern in all four
		// dispatch shims.
		bool accepted = false;
		try {
			accepted = igSurgAct.rightMouseDown(name, position, triangle);
		}
		catch (std::exception& e) {
			if (!undoAfterError(e.what()))
				handleThrow(e.what());
			return false;
		}
		return accepted;
	}

	static void dispatchRightMouseUp(std::string& name, float(&position)[3], int triangle)
	{
		if (skinflapsErrorHold)
			return;
		try {
			igSurgAct.rightMouseUp(name, position, triangle);
		}
		catch (std::exception& e) {
			if (!undoAfterError(e.what()))
				handleThrow(e.what());
		}
	}

	static void dispatchMouseMotion(float dx, float dy)
	{
		if (skinflapsErrorHold)
			return;
		try {
			igSurgAct.mouseMotion(dx, dy);
		}
		catch (std::exception& e) {
			if (!undoAfterError(e.what()))
				handleThrow(e.what());
		}
	}

	static void dispatchOnKeyDown(int key)
	{
		if (skinflapsErrorHold && key != SKINFLAPS_UNDO_AFTER_ERROR_KEY)
			return;
		try {
			igSurgAct.onKeyDown(key);
		}
		catch (std::exception& e) {
			if (!undoAfterError(e.what()))
				handleThrow(e.what());
		}
	}

	static void dispatchSetToolState(int toolState)
	{
		if (skinflapsErrorHold)  // like the surgical shims: a tool change would un-pause physics under the hold
			return;
		igSurgAct.setToolState(toolState);
	}

	static bool dispatchLoadHistory(const char* historyDirPath, const char* historyFileName)
	{
		const bool ok = igSurgAct.loadHistory(historyDirPath, historyFileName);
		if (ok) {
			if (historyDirPath && *historyDirPath)
				historyDirectory = historyDirPath;
			if (historyFileName && *historyFileName)
				historyFile = historyFileName;
		}
		return ok;
	}

	// GLFW callbacks report window coordinates, but picking and viewport events
	// need framebuffer pixels; the two differ on HiDPI (Retina) displays.
	static void toFramebufferCoords(double windowX, double windowY, int& framebufferX, int& framebufferY)
	{
		if (framebufferWidth < 1) framebufferWidth = 1;
		if (framebufferHeight < 1) framebufferHeight = 1;
		if (windowWidth < 1) windowWidth = framebufferWidth;
		if (windowHeight < 1) windowHeight = framebufferHeight;

		const double sx = static_cast<double>(framebufferWidth) / static_cast<double>(windowWidth);
		const double sy = static_cast<double>(framebufferHeight) / static_cast<double>(windowHeight);
		const int fx = static_cast<int>(windowX * sx);
		const int fy = static_cast<int>(windowY * sy);
		framebufferX = std::max(0, std::min(fx, framebufferWidth - 1));
		framebufferY = std::max(0, std::min(fy, framebufferHeight - 1));
	}

	static void sceneMouseButtonEvent(double windowX, double windowY, int button, bool dragging)
	{
		int framebufferX = 0, framebufferY = 0;
		toFramebufferCoords(windowX, windowY, framebufferX, framebufferY);
		igGl3w.mouseButtonEvent(framebufferX, framebufferY, button, dragging);
	}

	static bool pickAtWindowPosition(double windowX, double windowY, std::string& name, float(&position)[3], int& triangle, bool excludeShapes = false, bool excludeStatic = true)
	{
		int framebufferX = 0, framebufferY = 0;
		toFramebufferCoords(windowX, windowY, framebufferX, framebufferY);
		return igGl3w.pick(framebufferX, framebufferY, name, position, triangle, excludeShapes, excludeStatic);
	}

	static void mouse_button_callback(GLFWwindow* window, int button, int action, int mods)
	{
		ImGuiIO& io = ImGui::GetIO();
		// Forward every press/release to ImGui before the WantCaptureMouse branch:
		// a drag straddling the toolbox/scene boundary otherwise leaves ImGui's
		// MouseDown[] state stuck and the UI stops accepting clicks.
		io.AddMouseButtonEvent(button, action);
		if (io.WantCaptureMouse) {
			buttonsDown = 0;
			return;
		}
		double xpos = 0.0;
		double ypos = 0.0;
		glfwGetCursorPos(window, &xpos, &ypos);
		xpos = std::max(0.0, std::min(xpos, static_cast<double>(windowWidth > 0 ? windowWidth - 1 : 0)));
		ypos = std::max(0.0, std::min(ypos, static_cast<double>(windowHeight > 0 ? windowHeight - 1 : 0)));
		if ( button == GLFW_MOUSE_BUTTON_RIGHT) {
			if (action == GLFW_PRESS) {
				buttonsDown |= 4;
				// The surgical modifier latch (ctrlShiftKeyDown) is fed by the key
				// callback, which never sees a Shift/Ctrl press while ImGui holds
				// the keyboard (the frames around a toolbox click) or while another
				// window is key, and a focus change synthesizes a plain release
				// that clears it.  The right press is the very event the tools
				// sample, and the OS reports the modifier state on it: a modifier
				// physically held here is the intended action.  Only ever sets.
				if (mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL))
					ctrlShiftKeyDown = true;
				std::string name; float position[3]; int triangle = 1;
				pickAtWindowPosition(xpos, ypos, name, position, triangle);
				if (!name.empty()) {
					if (dispatchRightMouseDown(name, position, triangle)) {
						lastSurgX = static_cast<float>(xpos);
						lastSurgY = static_cast<float>(ypos);
						surgicalDrag = true;
					}
				}
				else
					surgicalDrag = false;
			}
			else if (action == GLFW_RELEASE) {
				buttonsDown &= 0xfb;
				// Counterpart of the press-time set above: the OS state on the release
				// event is as authoritative as on the press, and every reader of the
				// latch samples it at a right PRESS, so clearing here when no modifier
				// is held cannot change an intended action; it closes the case where
				// the modifier's own release was swallowed.
				if (!(mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL)))
					ctrlShiftKeyDown = false;
				if (surgicalDrag) {
					std::string name; float position[3]; int triangle = 1;
					pickAtWindowPosition(xpos, ypos, name, position, triangle, true);
					dispatchRightMouseUp(name, position, triangle);  // no longer matters how it returns
					surgicalDrag = false;
				}
			}
			else {
				puts("Illegal right mouse button call");
				exit(1);
			}
			sceneMouseButtonEvent(xpos, ypos, buttonsDown < 1 ? -1 : 2, false);
		}
		else if (button == GLFW_MOUSE_BUTTON_LEFT) {
			// The camera pans with a middle-button drag, but Apple trackpads
			// and Magic Mice emit no middle button, so Shift- or Ctrl-left-drag
			// pans as well. The choice latches at PRESS so releasing the
			// modifier mid-drag keeps panning until the button comes up; a
			// plain left-drag still rotates, and the surgical Ctrl/Shift
			// latch (ctrlShiftKeyDown) is unaffected.
			static bool leftDragPans = false;
			bool pans;
			if (action == GLFW_PRESS) {
				pans = (mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL)) != 0;
				leftDragPans = pans;
				buttonsDown |= pans ? 2 : 1;
			}
			else if (action == GLFW_RELEASE) {
				pans = leftDragPans;
				leftDragPans = false;
				buttonsDown &= pans ? 0xfd : 0xfe;
			}
			else
				pans = leftDragPans;
			sceneMouseButtonEvent(xpos, ypos, buttonsDown < 1 ? -1 : (pans ? 1 : 0), false);
		}
		else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
			if (action == GLFW_PRESS)
				buttonsDown = 2;
			else if (action == GLFW_RELEASE)
				buttonsDown &= 0xfd;
			else;
			sceneMouseButtonEvent(xpos, ypos, buttonsDown < 1 ? -1 : 1, false);
		}
		else {
			puts("Illegal right mouse button call");
		}
	}

	static void cursor_position_callback(GLFWwindow* window, double xpos, double ypos)
	{
		// (1) ALWAYS forward mouse data to ImGui! This is automatic with default backends. With your own backend:
		ImGuiIO& io = ImGui::GetIO();
		io.AddMousePosEvent((float)xpos, (float)ypos);
		// (2) ONLY forward mouse data to your underlying app/game.
//		if (!io.WantCaptureMouse)
//			my_game->HandleMouseData(...);
		if (buttonsDown < 1 || io.WantCaptureMouse)
			return;
		if (xpos < 0.0)
			xpos = 0.0;
		if (ypos < 0.0)
			ypos = 0.0;
		if (windowWidth > 0 && xpos > static_cast<double>(windowWidth - 1))
			xpos = static_cast<double>(windowWidth - 1);
		if (windowHeight > 0 && ypos > static_cast<double>(windowHeight - 1))
			ypos = static_cast<double>(windowHeight - 1);
		if (buttonsDown < 1)
			sceneMouseButtonEvent(xpos, ypos, -1, true);
		if (buttonsDown & 1)
			sceneMouseButtonEvent(xpos, ypos, 0, true);
		if (buttonsDown &2)
			sceneMouseButtonEvent(xpos, ypos, 1, true);
		if (buttonsDown & 4) {
			if (surgicalDrag && buttonsDown == 4) {  // buttonsDown == 4 and other buttons off
				const float invW = windowWidth > 0 ? 1.0f / static_cast<float>(windowWidth) : 0.0f;
				const float invH = windowHeight > 0 ? 1.0f / static_cast<float>(windowHeight) : 0.0f;
				dispatchMouseMotion((static_cast<float>(xpos) - lastSurgX) * invW, (lastSurgY - static_cast<float>(ypos)) * invH);
				lastSurgX = (float)xpos;
				lastSurgY = (float)ypos;
			}
			else
				sceneMouseButtonEvent(xpos, ypos, 2, true);
		}
	}

	static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
	{
		ImGuiIO& io = ImGui::GetIO();
		{
			// This callback is ImGui's only source of key state (it replaces the
			// backend's).  Feeding every event, releases included, as a press
			// leaves a key used inside a text field held down there: one
			// Backspace in the save dialog's name box would erase the whole
			// name.  Feed the real state, and let a release reach ImGui even
			// when the field has lost the focus since.
			const int igKey = ImGui_ImplGlfw_KeyToImGuiKey(key);
			if (action == GLFW_RELEASE) {
				if (igKey != ImGuiKey_None)
					io.AddKeyEvent(igKey, false);
				io.AddKeyEvent(ImGuiKey_ModShift, (mods & GLFW_MOD_SHIFT) != 0);
				io.AddKeyEvent(ImGuiKey_ModCtrl, (mods & GLFW_MOD_CONTROL) != 0);
				io.AddKeyEvent(ImGuiKey_ModSuper, (mods & GLFW_MOD_SUPER) != 0);
				io.AddKeyEvent(ImGuiKey_ModAlt, (mods & GLFW_MOD_ALT) != 0);
				if (io.WantCaptureKeyboard)
					return;
			}
			else if (io.WantCaptureKeyboard) {
				io.AddKeyEvent(ImGuiKey_ModShift, (mods & GLFW_MOD_SHIFT) != 0);
				io.AddKeyEvent(ImGuiKey_ModCtrl, (mods & GLFW_MOD_CONTROL) != 0);
				io.AddKeyEvent(ImGuiKey_ModSuper, (mods & GLFW_MOD_SUPER) != 0);
				io.AddKeyEvent(ImGuiKey_ModAlt, (mods & GLFW_MOD_ALT) != 0);
				if (igKey != ImGuiKey_None)
					io.AddKeyEvent(igKey, true);
				return;
			}
		}
		// The primary Delete key on Mac keyboards reports GLFW_KEY_BACKSPACE;
		// only forward delete (fn+Delete) reports GLFW_KEY_DELETE, which is
		// the sole code surgicalActions::onKeyDown acts on. Map Backspace
		// onto Delete here — after the ImGui capture branch, so text fields
		// still receive Backspace — so the labeled Delete key removes the
		// selected hook/suture or last fence post as it does on Windows.
		if (key == GLFW_KEY_BACKSPACE)
			key = GLFW_KEY_DELETE;
		// Cmd+Z: revert to the state before the last surgical action. Cmd
		// carries GLFW_MOD_SUPER, which the surgical modifier latch below
		// ignores, so this cannot collide with it.
		if (action == GLFW_PRESS && key == GLFW_KEY_Z && (mods & GLFW_MOD_SUPER)) {
			dispatchOnKeyDown((mods & GLFW_MOD_SHIFT) ? SKINFLAPS_REDO_KEY : SKINFLAPS_UNDO_KEY);  // Cmd+Z undo, Cmd+Shift+Z redo
			return;
		}
		if (action == GLFW_PRESS) {
			if (key == GLFW_KEY_ESCAPE)
				glfwSetWindowShouldClose(window, 1);
			else if (mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL))
				ctrlShiftKeyDown = true;
			else
				dispatchOnKeyDown(key);
		}
		else if (action == GLFW_RELEASE && scancode != 0) {

			if (key == GLFW_KEY_ESCAPE)
				;
			else if ((mods & (GLFW_MOD_SHIFT | GLFW_MOD_CONTROL)) == 0)
				ctrlShiftKeyDown = false;
			else
					igSurgAct.onKeyUp(key);
		}
		else  // action == GLFW_REPEAT ignore or forced synthetic GLFW key release
			;
	}

	//		The callback function receives two - dimensional scroll offsets.
	static void mouse_wheel_callback(GLFWwindow* window, double xoffset, double yoffset)
	{ // The callback function receives two - dimensional scroll offsets.
		if (wheelZoom)
			igGl3w.mouseWheelEvent((float)yoffset);
	}

	static void window_size_callback(GLFWwindow* window, int width, int height)
	{
		windowWidth = std::max(1, width);
		windowHeight = std::max(1, height);
		minFileDlgSize.x = windowWidth >> 1;
		minFileDlgSize.y = windowHeight >> 1;
	}

	static void framebuffer_size_callback(GLFWwindow* window, int width, int height)
	{
		framebufferWidth = std::max(1, width);
		framebufferHeight = std::max(1, height);
		igGl3w.setViewport(0, 0, framebufferWidth, framebufferHeight);
	}

	static void glfw_error_callback(int error, const char* description)
	{
		fprintf(stderr, "Glfw Error %d: %s\n", error, description);
	}

	static void destroyImguiGlfw() {
		ImGui_ImplOpenGL3_Shutdown();
		ImGui_ImplGlfw_Shutdown();
		ImGui::DestroyContext();
		glfwDestroyWindow(FFwindow);
		glfwTerminate();
	}

	static bool initCleftSim() {
		csgToolstate = 0;
		igGl3w.initializeGraphics();
		igSurgAct.setGl3wGraphics(&igGl3w);
		glfwSetMouseButtonCallback(FFwindow, &mouse_button_callback);
		glfwSetCursorPosCallback(FFwindow, &cursor_position_callback);
		glfwSetScrollCallback(FFwindow, mouse_wheel_callback);
		glfwSetWindowSizeCallback(FFwindow, &window_size_callback);
		glfwSetFramebufferSizeCallback(FFwindow, &framebuffer_size_callback);
		glfwSetKeyCallback(FFwindow, &key_callback);
		glfwGetWindowSize(FFwindow, &windowWidth, &windowHeight);
		windowWidth = std::max(1, windowWidth);
		windowHeight = std::max(1, windowHeight);
		glfwGetFramebufferSize(FFwindow, &framebufferWidth, &framebufferHeight);
		framebufferWidth = std::max(1, framebufferWidth);
		framebufferHeight = std::max(1, framebufferHeight);
		igGl3w.setViewport(0, 0, framebufferWidth, framebufferHeight);
		minFileDlgSize.x = windowWidth >> 1;
		minFileDlgSize.y = windowHeight >> 1;
		return true;
	}

	static bool initImguiGlfw() {
		// Setup window
		glfwSetErrorCallback(&glfw_error_callback);
		if (!glfwInit())
			return false;

		// Decide GL+GLSL versions
#ifdef __APPLE__
	// GL 3.2 + GLSL 150
		const char* glsl_version = "#version 150";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
		glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
		glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // Required on Mac
#else
	// GL 3.0 + GLSL 130
		const char* glsl_version = "#version 130";
		glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
		glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
		glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);
		//glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
		//glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only
#endif

	// Create window with graphics context
		FFwindow = glfwCreateWindow(1280, 720, "Skin Flaps Simulator", NULL, NULL);  // setting 4th argument to glfwGetPrimaryMonitor() creates full screen monitor
		if (FFwindow == NULL)
			return false;
		glfwMakeContextCurrent(FFwindow);
		glfwSwapInterval(1); // Enable vsync

		// Initialize OpenGL loader
#if defined(IMGUI_IMPL_OPENGL_LOADER_GL3W)
		bool err = gl3wInit() != 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLEW)
		bool err = glewInit() != GLEW_OK;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD)
		bool err = gladLoadGL() == 0;
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLAD2)
		bool err = gladLoadGL(glfwGetProcAddress) == 0; // glad2 recommend using the windowing library loader instead of the (optionally) bundled one.
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING2)
		bool err = false;
		glbinding::Binding::initialize();
#elif defined(IMGUI_IMPL_OPENGL_LOADER_GLBINDING3)
		bool err = false;
		glbinding::initialize([](const char* name) { return (glbinding::ProcAddress)glfwGetProcAddress(name); });
#else
		bool err = false; // If you use IMGUI_IMPL_OPENGL_LOADER_CUSTOM, your loader is likely to requires some form of initialization.
#endif
		if (err)
		{
			fprintf(stderr, "Failed to initialize OpenGL loader!\n");
			return false;
		}

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		// Disable imgui.ini persistence: the default writes a relative
		// "imgui.ini" that, for an installed .app, lands in an unpredictable
		// or read-only location (and modifying a signed bundle is invalid).
		io.IniFilename = nullptr;
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

		// Setup Dear ImGui style
		ImGui::StyleColorsDark();
		//ImGui::StyleColorsClassic();

		// Setup Platform/Renderer backends
		ImGui_ImplGlfw_InitForOpenGL(FFwindow, true);
		ImGui_ImplOpenGL3_Init(glsl_version);

		// Load Fonts
		// - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
		// - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
		// - If the file cannot be loaded, the function will return NULL. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
		// - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
		// - Read 'docs/FONTS.md' for more instructions and details.
		// - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
		//io.Fonts->AddFontDefault();
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
		//io.Fonts->AddFontFromFileTTF("../../misc/fonts/ProggyTiny.ttf", 10.0f);
		//ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
		//IM_ASSERT(font != NULL);
		return true;
	}

	static inline surgicalActions* getSurgicalActions() { return &igSurgAct; }
	static inline gl3wGraphics* getgl3wGraphics() { return &igGl3w; }

	static inline bool CtrlOrShiftKeyIsDown() { return ctrlShiftKeyDown;  }

	static void setToolState(int toolState) { csgToolstate = toolState; }

	static void getFileName(const char *startPath, const char *fileFilterSuffix, std::string &startDirectory, bool mustExist, bool chooseDirectory=false, const char* defaultName=""){
		// "smd" is a module file and "hst" is a history file
		std::string suffix(fileFilterSuffix), dialogTitle;
		int flags = 0;
		if (chooseDirectory) {
			FileDlgMode = 2;
			dialogTitle = "Please select a directory for your blend shapes -";
			suffix.clear();
			ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByTypeDir, "", ImVec4(1.0f, 0.4f, 0.0f, 1.0f));
			const char* dlgStart = (startPath && *startPath) ? startPath :
#ifdef _WIN32
				"C:\\";
#else
				"/";
#endif
			ImGuiFileDialog::Instance()->OpenDialog("FileDialogKey", dialogTitle.c_str(), nullptr, dlgStart);
			return;
		}
		else if (mustExist) {  // load dialog
			FileDlgMode = 0;
			if (suffix.find("hst") < suffix.size())
				dialogTitle = "Load Surgical History file -";
			else
				dialogTitle = "Load Model file -";
			flags = ImGuiFileDialogFlags_DisableCreateDirectoryButton | ImGuiFileDialogFlags_ReadOnlyFileNameField;
		}
		else {  // save dialog
			FileDlgMode = 1;
			if (suffix.find(".hst") < suffix.size())
				dialogTitle = "Save current Surgical History file -";
			else { // .obj
				assert(suffix.find(".obj") < suffix.size());
				dialogTitle = "Save blend shape .obj file -";
			}
			flags = ImGuiFileDialogFlags_DisableCreateDirectoryButton | ImGuiFileDialogFlags_ConfirmOverwrite;
		}
		ImGuiFileDialog::Instance()->SetFileStyle(IGFD_FileStyleByTypeDir, "", ImVec4(1.0f, 0.4f, 0.0f, 1.0f));
		ImGuiFileDialog::Instance()->OpenDialog("FileDialogKey", dialogTitle.c_str(), suffix.c_str(), startDirectory.c_str(), defaultName ? defaultName : "", 1, nullptr, flags);
	}

	static void sendUserMessage(const char *message, const char *windowTitle) {
		user_message = message;
		user_message_title = windowTitle;
		user_message_flag = true;
		user_message_time = glfwGetTime();
		user_message_kind = skinflapsErrorHold ? 1 : 0;  // a notice fades on its own; a hold waits for Close (it reverts)
	}

	static void handleThrow(const char* message) {
		user_message = message;
		std::string errHist = historyDirectory + "ERROR.hst";
		igSurgAct.recordInFlightHookMove();  // a drag or a suture that failed before its record was written
		igSurgAct.recordInFlightSuture();
		igSurgAct.saveSurgicalHistory(errHist.c_str());
		user_message.append("\n\nHistory to this point has been saved in ERROR.hst\n");
		if (const std::string versioned = saveVersionedErrorHistories(); !versioned.empty())
			user_message.append("(also " + versioned.substr(versioned.find_last_of('/') + 1) + ".hst, in " + reportFolderForDisplay() + ")\n");
		user_message_title = "Program exception thrown";
		user_message_flag = true;
		user_message_time = glfwGetTime();
		user_message_kind = 2;
		except_thrown_flag = true;
	}

	static void showHourglass() {
		// from: https ://github.com/ocornut/imgui/wiki/Image-Loading-and-Displaying-Examples#Example-for-OpenGL-users
		physicsDrag = true;
		if (hourglassTexture > 0xfffffffe) {
			std::string str(modelDirectory);
			str.append("Hourglass_2.jpg");
			int image_width = 0;
			int image_height = 0;
			unsigned char* image_data = stbi_load(str.c_str(), &image_width, &image_height, NULL, 4);
			if (image_data != NULL) {
				GLuint image_texture;
				glGenTextures(1, &image_texture);
				glBindTexture(GL_TEXTURE_2D, image_texture);
				// Setup filtering parameters for display
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // This is required on WebGL for non power-of-two textures
				glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // Same
				// Upload pixels into texture
#if defined(GL_UNPACK_ROW_LENGTH) && !defined(__EMSCRIPTEN__)
				glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#endif
				glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image_width, image_height, 0, GL_RGBA, GL_UNSIGNED_BYTE, image_data);
				stbi_image_free(image_data);

				hourglassTexture = image_texture;
				hourglassWidth = image_width;
				hourglassHeight = image_height;
			}
			if (hourglassTexture > 0xfffffffe) {
				str = "Unable to load Hourglass.jpg input file";
				sendUserMessage(str.c_str(), "Program data error");
			}
		}
		if (hourglassTexture < 0xffffffff) {
			ImGui::SetNextWindowPos(ImVec2(104., 24.), 0, ImVec2(0.0, 0.0));
			ImGui::Begin("Processing", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
			ImGui::Image((void*)(intptr_t)hourglassTexture, ImVec2((float)hourglassWidth, (float)hourglassHeight));
			ImGui::End();
		}
	}

	static void setModelFile(const std::string &modelFileName ) {
		modelFile = modelFileName;
	}

#ifdef _WIN32
	static std::wstring RegGetString(HKEY hKey, const std::wstring& subKey, const std::wstring& value)
	{
		DWORD dataSize{};
		LONG retCode = ::RegGetValue(
			hKey,
			subKey.c_str(),
			value.c_str(),
			RRF_RT_REG_SZ,
			nullptr,
			nullptr,
			&dataSize);
		if (retCode < ERROR_SUCCESS)
			return std::wstring();

		std::wstring data;
		data.resize(dataSize / sizeof(wchar_t));

		retCode = ::RegGetValue(
			hKey,
			subKey.c_str(),
			value.c_str(),
			RRF_RT_REG_SZ,
			nullptr,
			&data[0],
			&dataSize);
		if (retCode != ERROR_SUCCESS)
			return std::wstring();

		DWORD stringLengthInWchars = dataSize / sizeof(wchar_t);
		stringLengthInWchars--; // Exclude the NUL written by the Win32 API
		data.resize(stringLengthInWchars);
		return data;
	}

	static void setDefaultDirectories() {
		if (historyDirectory.empty() || modelDirectory.empty()) {
			char buff[200];
			HKEY hKey = HKEY_LOCAL_MACHINE;
			std::wstring ret, subKey = L"SOFTWARE\\SkinFlaps", value = L"ModelDir";
			ret = RegGetString(hKey, subKey, value);
			if (!ret.empty()) {
				size_t i;
				wcstombs_s(&i, buff, (size_t)200, ret.c_str(), (size_t)199);
				modelDirectory = buff;
			} else {
				modelDirectory.clear();
			}
			subKey = L"SOFTWARE\\SkinFlaps"; value = L"HistoryDir";
			ret = RegGetString(hKey, subKey, value);
			if (!ret.empty()) {
				size_t i;
				wcstombs_s(&i, buff, (size_t)200, ret.c_str(), (size_t)199);
				historyDirectory = buff;
			} else {
				historyDirectory.clear();
			}
			if (modelDirectory.empty() || historyDirectory.empty()) {
				GetCurrentDir(buff, 200);
				modelDirectory.assign(buff);
				size_t pos = modelDirectory.rfind("Build");
				if (pos == std::string::npos) {  // not part of program build. Use install dir.
					historyDirectory = "C:\\Users\\SkinFlaps";
					modelDirectory = "C:\\ProgramData\\SkinFlaps";
				} else {  // doing program building and testing
					std::string projectFolder = "SkinFlaps";
					pos = modelDirectory.rfind(projectFolder);
					modelDirectory.erase(modelDirectory.begin() + pos + projectFolder.size(), modelDirectory.end());
					historyDirectory = modelDirectory;
				}
				modelDirectory.append("\\Model\\");
				historyDirectory.append("\\History\\");
			}
			igSurgAct.setModelDirectory(modelDirectory.c_str());
			igSurgAct.setHistoryDirectory(historyDirectory.c_str());
		}
	}
#else
	static void setDefaultDirectories() {
		if (historyDirectory.empty() || modelDirectory.empty()) {
			auto hasAssets = [](const std::filesystem::path& root) {
				std::error_code ecModel, ecHistory;
				return std::filesystem::is_directory(root / "Model", ecModel) &&
					std::filesystem::is_directory(root / "History", ecHistory);
			};

			std::filesystem::path resolvedBase;

			// A double-clicked .app launches with cwd "/", so the asset
			// search must anchor on the executable, not the cwd. Order:
			//   1. <App>.app/Contents/Resources  (assets bundled in-app)
			//   2. probe upward from the executable directory  (assets
			//      laid out beside the .app)
			//   3. probe upward from the cwd  (dev builds run from source)
#ifdef __APPLE__
			{
				uint32_t size = 0;
				_NSGetExecutablePath(nullptr, &size);
				std::string execBuf(size ? size : 1, '\0');
				if (size && _NSGetExecutablePath(&execBuf[0], &size) == 0) {
					std::error_code ec;
					std::filesystem::path execPath =
						std::filesystem::canonical(execBuf.c_str(), ec);
					if (!ec) {
						std::filesystem::path resources =
							execPath.parent_path().parent_path() / "Resources";
						if (hasAssets(resources)) {
							resolvedBase = resources;
						} else {
							std::filesystem::path probe = execPath.parent_path();
							for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
								if (hasAssets(probe)) { resolvedBase = probe; break; }
								probe = probe.parent_path();
							}
						}
					}
				}
			}
#endif

			if (resolvedBase.empty()) {
				std::filesystem::path base;
				char buff[512];
				if (GetCurrentDir(buff, sizeof(buff)))
					base = std::filesystem::path(buff);
				if (base.empty())
					base = std::filesystem::current_path();
				resolvedBase = base;
				if (!hasAssets(resolvedBase)) {
					std::filesystem::path probe = base;
					for (int depth = 0; depth < 8 && !probe.empty(); ++depth) {
						if (hasAssets(probe)) { resolvedBase = probe; break; }
						probe = probe.parent_path();
					}
				}
			}

			modelDirectory = (resolvedBase / "Model").string() + SKINFLAPS_PATH_SEP;
			historyDirectory = (resolvedBase / "History").string() + SKINFLAPS_PATH_SEP;
			// Installed app (assets inside the bundle's Resources, which the installer
			// leaves root-owned): the model stays in the bundle, histories live in the
			// user's own ~/SkinFlaps/History, seeded with the demos on first use, so
			// History > Save, ERROR.hst and the session files have somewhere to go.
			if (resolvedBase.filename() == "Resources") {
				const std::string userHistory = userHistoryDirectory(resolvedBase / "History");
				if (!userHistory.empty())
					historyDirectory = userHistory;
			}
			igSurgAct.setModelDirectory(modelDirectory.c_str());
			igSurgAct.setHistoryDirectory(historyDirectory.c_str());
		}
	}
#endif

	// ------------------------------------------------------------------
	// Session model.  The scene loader is written for one model per process:
	// it appends static objects, collision sets and tet subsets to what is
	// already loaded, and the cutter, the deep-bed map and the fence size are
	// process-wide.  A fresh slate is therefore a fresh process: "New session",
	// "New window", and a model or history load asked for while a session is
	// active spawn this same executable, and all but "New window" then close
	// this window.  The child starts through the same code as a Finder launch
	// and reads SKINFLAPS_START_HISTORY / SKINFLAPS_START_MODEL to open what
	// the user chose.  Undo, redo, the error hold and the physics thread all
	// begin clean in the child.
	enum { SESSION_NONE = 0, SESSION_NEW = 1, SESSION_LOAD_HISTORY = 2, SESSION_LOAD_MODEL = 3, SESSION_NEW_WINDOW = 4 };
	static inline int pendingSessionKind = SESSION_NONE;   // a confirmation is up for this change
	static inline bool sessionModalOpened = false;
	static inline int spawnOnAccept = SESSION_NONE;         // the next file-dialog accept starts a new window with the chosen file
	static inline bool sessionSaveMode = false;             // the next .hst save writes the whole session (history, full trail, log, error files)

	static std::string executablePath() {
		static const std::string p = []() {
#ifdef __APPLE__
			uint32_t size = 0; _NSGetExecutablePath(nullptr, &size);
			std::string buf(size ? size : 1, '\0');
			if (size && _NSGetExecutablePath(&buf[0], &size) == 0) {
				std::error_code ec;
				std::filesystem::path exe = std::filesystem::canonical(buf.c_str(), ec);
				if (!ec) return exe.string();
			}
#endif
			return std::string(); }();
		return p;
	}
	// The app installed from the package: Model and History inside the bundle's Resources.
	static bool installedBundle() {
		static const bool v = []() {
			const std::string exe = executablePath();
			if (exe.empty()) return false;
			std::error_code ec;
			const std::filesystem::path res = std::filesystem::path(exe).parent_path().parent_path() / "Resources";
			return std::filesystem::is_directory(res / "Model", ec) && std::filesystem::is_directory(res / "History", ec); }();
		return v;
	}
	// The installed app's writable folder for histories, error files and logs:
	// ~/SkinFlaps/History, created on first use.  A dangling symlink left at that
	// path (or at ~/SkinFlaps) is removed first, since it points nowhere; when the
	// folder still cannot be created or written, ~/Library/Application Support/
	// SkinFlaps/History is used instead.  Empty only when neither works (the
	// caller then keeps the bundle's read-only folder).  Resolved once.
	static std::string userDataDirectory() {
		static const std::string resolved = []() -> std::string {
			const char* home = std::getenv("HOME");
			if (!home || !*home)
				return std::string();
			auto clearDangling = [](const std::filesystem::path& p) {
				std::error_code e1, e2;
				if (std::filesystem::is_symlink(p, e1) && !std::filesystem::exists(p, e2)) {
					std::error_code e3;
					std::filesystem::remove(p, e3);
					std::fprintf(stderr, "[session] removed the dangling symlink %s so the folder can be created\n", p.string().c_str());
				}
			};
			auto usable = [](const std::filesystem::path& dir) -> bool {
				std::error_code ec;
				std::filesystem::create_directories(dir, ec);
				if (!std::filesystem::is_directory(dir, ec))
					return false;
				const std::filesystem::path probe = dir / ".skinflaps_write_test";
				{ std::ofstream f(probe); if (!f.is_open()) return false; }
				std::filesystem::remove(probe, ec);
				return true;
			};
			const std::filesystem::path base = std::filesystem::path(home) / "SkinFlaps";
			clearDangling(base);
			clearDangling(base / "History");
			if (usable(base / "History"))
				return (base / "History").string() + SKINFLAPS_PATH_SEP;
			const std::filesystem::path alt = std::filesystem::path(home) / "Library" / "Application Support" / "SkinFlaps" / "History";
			if (usable(alt)) {
				std::fprintf(stderr, "[session] %s is not usable; using %s\n", (base / "History").string().c_str(), alt.string().c_str());
				return alt.string() + SKINFLAPS_PATH_SEP;
			}
			std::fprintf(stderr, "[session] no writable folder for histories under %s; histories stay in the bundle, which is read only\n", home);
			return std::string(); }();
		return resolved;
	}
	// userDataDirectory(), seeded with the bundled demo histories the first time.
	static std::string userHistoryDirectory(const std::filesystem::path& bundledHistory) {
		const std::string dirStr = userDataDirectory();
		if (dirStr.empty())
			return dirStr;
		const std::filesystem::path dir(dirStr);
		std::error_code iterEc;
		for (const auto& entry : std::filesystem::directory_iterator(bundledHistory, iterEc)) {
			if (iterEc) break;
			if (!entry.is_regular_file()) continue;
			const std::string name = entry.path().filename().string();
			if (entry.path().extension() != ".hst" || name.rfind("ERROR", 0) == 0) continue;  // the demos only
			const std::filesystem::path dst = dir / name;
			std::error_code copyEc;
			if (!std::filesystem::exists(dst, copyEc))
				std::filesystem::copy_file(entry.path(), dst, copyEc);
		}
		return dirStr;
	}
	static bool sessionActive() { return !igSurgAct.historyEmpty() || !historyFile.empty() || !modelFile.empty(); }
	// Where this session's error files and log go: the app log's folder when
	// the app opened one, else the history folder.  For dialogs, the home
	// folder shows as ~.
	static std::string reportFolder() {
		if (!skinflapsAppLogPath.empty())
			return std::filesystem::path(skinflapsAppLogPath).parent_path().string();
		return historyDirectory;
	}
	static std::string reportFolderForDisplay() {
		std::string f = reportFolder();
		while (f.size() > 1 && f.back() == '/') f.pop_back();
		if (const char* home = std::getenv("HOME"); home && *home && f.rfind(home, 0) == 0)
			f = "~" + f.substr(std::strlen(home));
		return f;
	}
	// File > Show session folder: opens it in Finder.
	static void showSessionFolder() {
#ifndef _WIN32
		const std::string f = reportFolder();
		std::error_code ec;
		std::filesystem::create_directories(f, ec);
		char* argv[] = { const_cast<char*>("/usr/bin/open"), const_cast<char*>(f.c_str()), nullptr };
		pid_t pid = 0;
		if (posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr, argv, environ) != 0)
			sendUserMessage(("Could not open " + reportFolderForDisplay() + "-").c_str(), "Session");
		else
			std::fprintf(stderr, "[session] opened %s in Finder\n", f.c_str());
#else
		sendUserMessage((std::string("Session files are in ") + reportFolder() + "-").c_str(), "Session");
#endif
	}
	static void closeThisWindow(const char* why) {
		std::fprintf(stderr, "[session] closing this window (%s)\n", why);
		std::fflush(stderr);
		glfwSetWindowShouldClose(FFwindow, 1);
	}
	// Start another copy of this program.  Its environment is this one's minus
	// the start-up requests, so a window opened from a child does not reopen
	// what the child itself was told to open; it takes its own session stamp
	// from its own start time.
	static bool spawnSkinFlaps(const char* startHistory, const char* startModel) {
#ifdef _WIN32
		sendUserMessage("Opening another window is not available on this platform-", "Session");
		return false;
#else
		const std::string exe = executablePath();
		if (exe.empty()) {
			sendUserMessage("Could not find this program's executable to open another window-", "Session");
			return false;
		}
		std::vector<std::string> env;
		for (char** e = environ; e && *e; ++e) {
			const std::string kv(*e);
			if (kv.rfind("SKINFLAPS_START_", 0) == 0)
				continue;
			env.push_back(kv);
		}
		if (startHistory && *startHistory) env.push_back(std::string("SKINFLAPS_START_HISTORY=") + startHistory);
		if (startModel && *startModel) env.push_back(std::string("SKINFLAPS_START_MODEL=") + startModel);
		std::vector<char*> envp;
		for (auto& s : env) envp.push_back(&s[0]);
		envp.push_back(nullptr);
		char* argv[] = { const_cast<char*>(exe.c_str()), nullptr };
		pid_t pid = 0;
		const int rc = posix_spawn(&pid, exe.c_str(), nullptr, nullptr, argv, envp.data());
		if (rc != 0) {
			sendUserMessage((std::string("Could not open another window: ") + std::strerror(rc) + "-").c_str(), "Session");
			return false;
		}
		std::fprintf(stderr, "[session] spawned pid %d%s%s%s%s\n", (int)pid,
			(startHistory && *startHistory) ? " history=" : "", (startHistory && *startHistory) ? startHistory : "",
			(startModel && *startModel) ? " model=" : "", (startModel && *startModel) ? startModel : "");
		std::fflush(stderr);
		return true;
#endif
	}
	// Menu entry point: nothing loaded yet -> the plain load paths; a session
	// under way -> the confirmation modal, then performSessionChange().
	static void requestSessionChange(int kind) {
		if (skinflapsErrorHold) {
			sendUserMessage("Close the error first-", "Session");
			return;
		}
		if (kind == SESSION_NEW_WINDOW) {
			spawnSkinFlaps(nullptr, nullptr);
			return;
		}
		if (!sessionActive()) {
			if (kind == SESSION_LOAD_HISTORY) {
				setDefaultDirectories();
				getFileName(historyDirectory.c_str(), ".hst", historyDirectory, true, false);
			}
			else if (kind == SESSION_LOAD_MODEL) {
				setDefaultDirectories();
				getFileName(modelDirectory.c_str(), ".smd", modelDirectory, true, false);
			}
			else
				sendUserMessage("Nothing to clear: no model or history is loaded-", "Session");
			return;
		}
		pendingSessionKind = kind;
		sessionModalOpened = false;
	}
	// After the confirmation: path is the chosen file for a load; null opens
	// the file dialog first and the accept handler finishes.
	static void performSessionChange(int kind, const char* path) {
		if (kind == SESSION_NEW) {
			if (spawnSkinFlaps(nullptr, nullptr))
				closeThisWindow("new session");
		}
		else if (kind == SESSION_LOAD_HISTORY || kind == SESSION_LOAD_MODEL) {
			if (path && *path) {
				if (spawnSkinFlaps(kind == SESSION_LOAD_HISTORY ? path : nullptr, kind == SESSION_LOAD_MODEL ? path : nullptr))
					closeThisWindow(kind == SESSION_LOAD_HISTORY ? "history load in a new window" : "model load in a new window");
			}
			else {
				spawnOnAccept = kind;
				setDefaultDirectories();
				if (kind == SESSION_LOAD_HISTORY)
					getFileName(historyDirectory.c_str(), ".hst", historyDirectory, true, false);
				else
					getFileName(modelDirectory.c_str(), ".smd", modelDirectory, true, false);
			}
		}
		else if (kind == SESSION_NEW_WINDOW)
			spawnSkinFlaps(nullptr, nullptr);
	}
	// A child window opening what its parent chose (see spawnSkinFlaps).
	static void startSessionFromEnv() {
		const char* h = std::getenv("SKINFLAPS_START_HISTORY");
		const char* m = std::getenv("SKINFLAPS_START_MODEL");
		if ((!h || !*h) && (!m || !*m))
			return;
		setDefaultDirectories();
		if (h && *h) {
			std::filesystem::path p(h);
			historyDirectory = p.parent_path().string() + SKINFLAPS_PATH_SEP;
			historyFile = p.filename().string();
			if (dispatchLoadHistory(historyDirectory.c_str(), historyFile.c_str())) {
				std::string title("Skin Flaps Simulator playing - ");
				title.append(historyFile);
				glfwSetWindowTitle(FFwindow, title.c_str());
			}
			else {
				historyFile.clear();
				sendUserMessage("The history file could not be loaded-", "History file Error");
			}
		}
		else {
			std::filesystem::path p(m);
			modelDirectory = p.parent_path().string() + SKINFLAPS_PATH_SEP;
			modelFile = p.filename().string();
			std::string title("Skin Flaps Simulator Model is - ");
			title.append(modelFile);
			glfwSetWindowTitle(FFwindow, title.c_str());
			if (!igSurgAct.loadScene(modelDirectory.c_str(), modelFile.c_str()))
				sendUserMessage("The model file did not load successfully.", "Model file Error");
		}
	}
	// File > Save session files: <name>.hst (the actions in effect), <name>_full.hst
	// (every move including undos, replayable), their clock sidecars, <name>.log (this
	// session's log so far) and copies of this session's ERROR_<stamp>_* files when
	// they live elsewhere -- everything a report needs, in the folder the user chose.
	static void saveSessionFiles(const std::string& dir, const std::string& hstName) {
		std::string base = hstName;
		if (base.size() > 4 && base.compare(base.size() - 4, 4, ".hst") == 0)
			base.resize(base.size() - 4);
		const std::string stem = dir + base;
		std::string list;
		if (!igSurgAct.saveSurgicalHistory((stem + ".hst").c_str()))
			return;  // the save routine's own dialog said why
		list = base + ".hst";
		if (igSurgAct.saveFullHistoryTrail((stem + "_full.hst").c_str()))
			list += ", " + base + "_full.hst";
		std::fflush(stderr);
		std::error_code ec;
		if (!skinflapsAppLogPath.empty() && std::filesystem::exists(skinflapsAppLogPath, ec)) {
			std::filesystem::copy_file(skinflapsAppLogPath, stem + ".log", std::filesystem::copy_options::overwrite_existing, ec);
			if (!ec)
				list += ", " + base + ".log";
		}
		int copied = 0;
		if (!skinflapsAppLogPath.empty()) {
			std::error_code e2;
			const std::filesystem::path src(reportFolder()), dst(dir);
			if (std::filesystem::is_directory(src, e2) && !std::filesystem::equivalent(src, dst, e2)) {
				const std::string prefix = "ERROR_" + skinflapsSessionStamp() + "_";
				for (const auto& entry : std::filesystem::directory_iterator(src, e2)) {
					if (e2) break;
					const std::string name = entry.path().filename().string();
					if (!entry.is_regular_file() || name.rfind(prefix, 0) != 0) continue;
					std::error_code e3;
					std::filesystem::copy_file(entry.path(), dst / name, std::filesystem::copy_options::overwrite_existing, e3);
					if (!e3) ++copied;
				}
			}
		}
		std::string msg = "Saved " + list;
		if (copied)
			msg += " and " + std::to_string(copied) + " error file" + (copied > 1 ? "s" : "");
		msg += " in " + dir + "-";
		std::fprintf(stderr, "[session] %s\n", msg.c_str());
		std::fflush(stderr);
		sendUserMessage(msg.c_str(), "Session files saved");
	}

	static void InstanceCleftGui()
	{
		if (getTextInput) {

			ImGui::SetNextWindowPos(ImVec2(150., 54.), 0, ImVec2(0.0, 0.0));
			ImGui::Begin("Enter your subdirectory name-", NULL, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);   // &user_message_flag  Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
			ImGui::Text("Don't use spaces or special characters.");
			char buf[255]{};
			if (ImGui::InputText("Your directory name", buf, sizeof(buf), 32 | 8)) {  // flag are imgui.INPUT_TEXT_ENTER_RETURNS_TRUE = 32, imgui.INPUT_TEXT_CHARS_NO_BLANK = 8
				if (historyDirectory.empty())
					setDefaultDirectories();
				std::string r = historyDirectory, s;
				s.assign(buf);
				r += s;
#ifdef _WIN32
				if (_mkdir(r.c_str()) != 0) {
#else
				if (mkdir(r.c_str(), 0755) != 0) {
#endif
					sendUserMessage("Sorry that directory either already exists,\nor could not be created.\n\nTry again-", "User subdirectory create failed-");
				}
				else {
					sendUserMessage("Your new History subdirectory was successfully created.", "User subdirectory creation succeeded");
				}
				getTextInput = false;
				for (int i = 0; i < 255; ++i)
					buf[i] = '\0';
			}
			if (ImGui::Button("  Cancel  ")) {
				getTextInput = false;
				for (int i = 0; i < 255; ++i)
					buf[i] = '\0';
			}
			ImGui::End();
		}
		if (ImGui::BeginMainMenuBar())
		{
			if (ImGui::BeginMenu("File"))
			{
				if (ImGui::MenuItem("New window")) requestSessionChange(SESSION_NEW_WINDOW);
				if (ImGui::MenuItem("New session...")) requestSessionChange(SESSION_NEW);
				ImGui::Separator();
				if (ImGui::MenuItem("Load model")) requestSessionChange(SESSION_LOAD_MODEL);
				if (ImGui::MenuItem("Show session folder")) showSessionFolder();
				if (ImGui::MenuItem("Save session files...")) {
					if (skinflapsErrorHold)
						sendUserMessage("Close the error first-", "Session");
					else if (!sessionActive())
						sendUserMessage("Nothing to save yet: load a model or a history first-", "Session");
					else {
						sessionSaveMode = true;
						setDefaultDirectories();
						const std::string suggested = "SESSION_" + skinflapsSessionStamp() + ".hst";
						getFileName(historyDirectory.c_str(), ".hst", historyDirectory, false, false, suggested.c_str());
					}
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Exit")) { glfwSetWindowShouldClose(FFwindow, 1); }
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Edit"))
			{
				// Hover-able lists of the moves that can be undone / redone, newest
				// first; choosing the k-th entry steps back (or forward) k moves.
				// Any new forward move clears the redo list (snapshotForwardMove).
				auto stepMenu = [&](const char* label, bool redo, const char* shortcut) {
					if (ImGui::BeginMenu(label)) {
						const std::vector<std::string> names = redo ? igSurgAct.redoActionNames(12) : igSurgAct.undoActionNames(12);
						if (names.empty())
							ImGui::MenuItem(redo ? "Nothing to redo" : "Nothing to undo", NULL, false, false);
						for (size_t i = 0; i < names.size(); ++i) {
							const std::string item = std::to_string(i + 1) + ".  " + names[i];
							if (ImGui::MenuItem(item.c_str(), i == 0 ? shortcut : NULL))
								dispatchUndoSteps(redo, (int)i + 1);
						}
						ImGui::EndMenu();
					}
				};
				stepMenu("Undo", false, "Cmd+Z");
				stepMenu("Redo", true, "Cmd+Shift+Z");
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("History"))
			{
				if (ImGui::MenuItem("Load")) requestSessionChange(SESSION_LOAD_HISTORY);
				if (ImGui::MenuItem("Save")) {
					if (modelFile.empty())
						sendUserMessage("A model file must be loaded before a surgical history file can be created.", "User error");
					else {
						setDefaultDirectories();
						getFileName(historyDirectory.c_str(), ".hst", historyDirectory, false, false);
					}
				}
				if (ImGui::MenuItem("Next")) {
					if (modelDirectory.empty()) {
						setDefaultDirectories();
						getFileName(historyDirectory.c_str(), ".hst", historyDirectory, true, false);
					}
					else
						++nextCounter;
				}
				ImGui::Separator();
				if (ImGui::MenuItem("Create user subdirectory")) {
					getTextInput = true;
				}
				if (ImGui::MenuItem("Output blend shape file")) {
					if (modelFile.empty())
						sendUserMessage("A model file must be loaded before a blend shape file can be created.", "User error");
					else {
						if (objDirectory.empty()) {
							setDefaultDirectories();
							std::string startDir = modelDirectory.empty() ? historyDirectory : modelDirectory;
							getFileName(startDir.c_str(), ".obj", objDirectory, false, true);
						}
						else
							getFileName(objDirectory.c_str(), ".obj", objDirectory, false, false);
					}
				}
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Tools"))
			{
				if (ImGui::MenuItem("View", NULL, csgToolstate == 0, true)) { csgToolstate = 0; dispatchSetToolState(0); }
				if (ImGui::MenuItem("Hook", NULL, csgToolstate == 1, true)) { csgToolstate = 1; dispatchSetToolState(1); }
				if (ImGui::MenuItem("Knife", NULL, csgToolstate == 2, true)) { csgToolstate = 2; dispatchSetToolState(2); }
				if (ImGui::MenuItem("Undermine", NULL, csgToolstate == 3, true)) { csgToolstate = 3; dispatchSetToolState(3); }
				if (ImGui::MenuItem("Suture", NULL, csgToolstate == 4, true)) { csgToolstate = 4; dispatchSetToolState(4); }
				if (ImGui::MenuItem("Excise", NULL, csgToolstate == 5, true)) { csgToolstate = 5; dispatchSetToolState(5); }
				if (ImGui::MenuItem("Deep cut", NULL, csgToolstate == 6, true)) { csgToolstate = 6; dispatchSetToolState(6); }
				if (ImGui::MenuItem("Periosteal", NULL, csgToolstate == 7, true)) { csgToolstate = 7; dispatchSetToolState(7); }
				if (ImGui::MenuItem("Promote sutures")) { igSurgAct.promoteFakeSutures();  csgToolstate = 0; dispatchSetToolState(0); }
				if (ImGui::MenuItem("Pause physics")) { igSurgAct.pausePhysics();  csgToolstate = 0; dispatchSetToolState(0); }
				ImGui::Separator();
				if (ImGui::Checkbox("Use Power Hooks", &powerHooks))
					igSurgAct._strongHooks = powerHooks;
				ImGui::Checkbox("Show Toolbox", &showToolbox);
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("View"))
			{
				if (ImGui::MenuItem("View Physics", NULL, &viewPhysics, true)) {
					if (viewPhysics) {
						igSurgAct.getBccTetScene()->createTetLatticeDrawing();
						igSurgAct.getBccTetScene()->setVisability(2, 1);
					}
					else
						igSurgAct.getBccTetScene()->setVisability(2, 0);
				}
				if (ImGui::MenuItem("View Surface", NULL, &viewSurface, true)) {
					if(viewSurface)
						igSurgAct.getBccTetScene()->setVisability(1, 2);
					else
						igSurgAct.getBccTetScene()->setVisability(0, 2);
				}
				ImGui::Separator();
				if (ImGui::BeginMenu("Zoom control"))
				{
					if (ImGui::MenuItem("Mouse Wheel", "", wheelZoom)) {
						igGl3w.setMouseWheelZoom(true);
						wheelZoom = true;
					}
					if (ImGui::MenuItem("Right Mouse", "", !wheelZoom)) {
						igGl3w.setMouseWheelZoom(false);
						wheelZoom = false;
					}
					ImGui::EndMenu();
				}
				ImGui::EndMenu();
			}
			ImGui::EndMainMenuBar();
		}
		if (showToolbox) {
			// toolbar
			ImGui::SetNextWindowPos(ImVec2(4., 24.), 0, ImVec2(0.0, 0.0));
			ImGui::Begin("     TOOLS", 0, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);  // | ImGuiWindowFlags_NoScrollbar
			if (ImGui::RadioButton("View", csgToolstate == 0)) {
				dispatchSetToolState(0);
				csgToolstate = 0;
			}
			if (ImGui::RadioButton("Hook", csgToolstate == 1)){
				dispatchSetToolState(1);
				csgToolstate = 1;
			}
			if (ImGui::RadioButton("Knife", csgToolstate == 2)){
				dispatchSetToolState(2);
				csgToolstate = 2;
			}
			if(ImGui::RadioButton("Undermine", csgToolstate == 3)){
				dispatchSetToolState(3);
				csgToolstate = 3;
			}
			if(ImGui::RadioButton("Suture", csgToolstate == 4)){
				dispatchSetToolState(4);
				csgToolstate = 4;
			}
			if(ImGui::RadioButton("Excise", csgToolstate == 5)){
				dispatchSetToolState(5);
				csgToolstate = 5;
			}
			if(ImGui::RadioButton("Deep Cut", csgToolstate == 6)){
				dispatchSetToolState(6);
				csgToolstate = 6;
			}
			if(ImGui::RadioButton("Periosteal", csgToolstate == 7)){
				dispatchSetToolState(7);
				csgToolstate = 7;
			}

			ImGui::Separator();
			if (ImGui::Button("   NEXT   ")) {  // Buttons return true when clicked (most widgets return true when edited/activated)
				if (modelDirectory.empty()) {
					setDefaultDirectories();
					getFileName(historyDirectory.c_str(), ".hst", historyDirectory, true, false);
				}
				else
					++nextCounter;
			}
			ImGui::End();
		}
		if (pendingSessionKind != SESSION_NONE) {
			// Warning before a session is cleared: the change happens in a new
			// window and this one closes.
			static const char* modalTitle = "Start a new session?";
			if (!sessionModalOpened) {
				ImGui::OpenPopup(modalTitle);
				sessionModalOpened = true;
			}
			const ImGuiIO& io = ImGui::GetIO();
			ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
			if (ImGui::BeginPopupModal(modalTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse)) {
				const char* what = pendingSessionKind == SESSION_NEW ? "start a new session" : pendingSessionKind == SESSION_LOAD_HISTORY ? "load a history file" : "load a model";
				ImGui::Text("This will %s in a new window and close this one.", what);
				ImGui::Text("Anything not saved in this session is lost.");
				ImGui::Text("File > Save session files keeps the history, the full trail and the log.");
				ImGui::Separator();
				if (ImGui::Button("  Continue  ")) {
					const int k = pendingSessionKind;
					pendingSessionKind = SESSION_NONE;
					sessionModalOpened = false;
					ImGui::CloseCurrentPopup();
					performSessionChange(k, nullptr);
				}
				ImGui::SameLine();
				if (ImGui::Button("  Cancel  ")) {
					pendingSessionKind = SESSION_NONE;
					sessionModalOpened = false;
					ImGui::CloseCurrentPopup();
				}
				ImGui::EndPopup();
			}
		}
		if (user_message_flag)
		{
			// A plain notice (a refusal, an undo/redo confirmation) is transient: it
			// sits at the bottom left, does not take focus, and fades out on its own
			// after a few seconds.  An error hold and the terminal dialog wait for
			// Close (it reverts / exits).
			const bool transient = (user_message_kind == 0);
			const double noticeSeconds = 6.0, fadeSeconds = 1.5;
			double alpha = 1.0;
			if (transient) {
				const double elapsed = glfwGetTime() - user_message_time;
				if (elapsed >= noticeSeconds)
					user_message_flag = false;
				else if (elapsed > noticeSeconds - fadeSeconds)
					alpha = std::max(0.05, (noticeSeconds - elapsed) / fadeSeconds);
			}
			if (user_message_flag) {
			ImGuiWindowFlags msgFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse;
			if (transient) {
				ImGui::SetNextWindowPos(ImVec2(12.0f, ImGui::GetIO().DisplaySize.y - 12.0f), ImGuiCond_Always, ImVec2(0.0f, 1.0f));  // bottom left: discreet
				ImGui::SetNextWindowBgAlpha(0.88f);
				msgFlags |= ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
				ImGui::PushStyleVar(ImGuiStyleVar_Alpha, (float)alpha);
			}
			ImGui::Begin(user_message_title.c_str(), NULL, msgFlags);   // &user_message_flag  Pass a pointer to our bool variable (the window will have a closing button that will clear the bool when clicked)
			{
				// Wrap the text to the window: ImGui::Text() never wraps, so a long
				// reason would run off the auto-sized dialog (and a '%' in a message
				// would be read as a format directive).  The wrap width follows the
				// current window size.
				const float wrapW = std::min(720.0f, std::max(320.0f, ImGui::GetIO().DisplaySize.x * 0.6f));
				ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapW);
				ImGui::TextUnformatted(user_message.c_str());
				ImGui::PopTextWrapPos();
			}
			if (ImGui::Button("  Close  ")) {
				user_message_flag = false;
				if (skinflapsErrorHold)
					finishErrorHold();
				if (except_thrown_flag)
					glfwSetWindowShouldClose(FFwindow, 1);
			}
			ImGui::End();
			if (transient)
				ImGui::PopStyleVar();
			}
		}
		if (ImGuiFileDialog::Instance()->Display("FileDialogKey", ImGuiWindowFlags_NoCollapse, minFileDlgSize))  // , maxSize))
		{
			if (ImGuiFileDialog::Instance()->IsOk())
			{
				if (FileDlgMode < 1) {  // read op
					std::string inFile = ImGuiFileDialog::Instance()->GetCurrentFileName();
					if (inFile.rfind("hst") < inFile.size() && spawnOnAccept == SESSION_LOAD_HISTORY) {
						// A session is under way: the chosen history opens in a new window.
						spawnOnAccept = SESSION_NONE;
						const std::string chosen = ImGuiFileDialog::Instance()->GetCurrentPath() + SKINFLAPS_PATH_SEP + inFile;
						ImGuiFileDialog::Instance()->Close();
						performSessionChange(SESSION_LOAD_HISTORY, chosen.c_str());
						return;
					}
					else if (inFile.rfind("hst") < inFile.size()) {
						// loadHistory hard-refuses once the history array is
						// non-empty — after loading a history OR after recording
						// any surgical action — and ignoring the refusal would
						// leave a misleading "playing <file>" title over a silent
						// no-op. historyEmpty() is the authoritative check; the
						// title changes only when a load succeeds, and a failed
						// first load (unreadable or unparseable file) says so.
						if (!historyFile.empty() || !igSurgAct.historyEmpty()) {
							sendUserMessage("A history file is already loaded, or surgical actions have already been recorded. Please restart the program if you would like to load a history-", "User Error");
						}
						else {
							historyDirectory = ImGuiFileDialog::Instance()->GetCurrentPath();
							historyDirectory.append(SKINFLAPS_PATH_SEP);
							historyFile = inFile;
							if (dispatchLoadHistory(historyDirectory.c_str(), historyFile.c_str())) {
								std::string title("Skin Flaps Simulator playing - ");
								title.append(historyFile);
								glfwSetWindowTitle(FFwindow, title.c_str());
							}
							else {
								historyFile.clear();
								sendUserMessage("The history file could not be loaded-", "History file Error");
							}
						}
					}
					else if (spawnOnAccept == SESSION_LOAD_MODEL) {
						spawnOnAccept = SESSION_NONE;
						const std::string chosen = ImGuiFileDialog::Instance()->GetCurrentPath() + SKINFLAPS_PATH_SEP + inFile;
						ImGuiFileDialog::Instance()->Close();
						performSessionChange(SESSION_LOAD_MODEL, chosen.c_str());
						return;
					}
					else {
						assert(inFile.rfind("smd") < inFile.size());
						modelDirectory = ImGuiFileDialog::Instance()->GetCurrentPath();
						modelDirectory.append(SKINFLAPS_PATH_SEP);
						modelFile = inFile;
						std::string title("Skin Flaps Simulator Model is - ");
						title.append(modelFile);
						glfwSetWindowTitle(FFwindow, title.c_str());

//						loadDir = modelDirectory;
//						loadFile = modelFile;

						if(!igSurgAct.loadScene(modelDirectory.c_str(), modelFile.c_str()))
							sendUserMessage("The model file did not load successfully.", "Model file Error");
					}
				}
				else if (FileDlgMode < 2) {  // write op
					std::string outFile = ImGuiFileDialog::Instance()->GetCurrentFileName();
					if (outFile.rfind(".hst") < outFile.size() && sessionSaveMode) {
						sessionSaveMode = false;
						std::string dir = ImGuiFileDialog::Instance()->GetCurrentPath();
						dir.append(SKINFLAPS_PATH_SEP);
						saveSessionFiles(dir, outFile);
					}
					else if (outFile.rfind(".hst") < outFile.size()) {
						historyDirectory = ImGuiFileDialog::Instance()->GetCurrentPath();
						historyDirectory.append(SKINFLAPS_PATH_SEP);
						historyFile = outFile;
						std::string fullPath = historyDirectory;
						fullPath.append(historyFile);
						igSurgAct.saveSurgicalHistory(fullPath.c_str());
					}
					else{  // blend shape .obj output
						assert(outFile.rfind(".obj") < outFile.size());
						if (modelFile.empty())
							sendUserMessage("Can not save a blend shape without an active model file loaded.", "User error");
						else {
							std::string path = objDirectory, prefix = outFile;
							path.append(outFile);
							prefix.resize(prefix.size() - 4);  //  .erase(prefix.size() - 4, 4);
							igSurgAct.saveCurrentObj(path.c_str(), prefix.c_str());
						}
					}
				}
				else{  // find/create blend shape directory before saving blend shape file
					objDirectory = ImGuiFileDialog::Instance()->GetCurrentPath();
					objDirectory.append(SKINFLAPS_PATH_SEP);
					ImGuiFileDialog::Instance()->Close();
					getFileName(objDirectory.c_str(), ".obj", objDirectory, false, false);
					return;
				}

			}
			spawnOnAccept = SESSION_NONE;
			sessionSaveMode = false;

			// close
			ImGuiFileDialog::Instance()->Close();
		}
	}

	FacialFlapsGui(){
		igSurgAct.setFacialFlapsGui(this);
		user_message_flag = false;
		getTextInput = false;
	}

	~FacialFlapsGui(){}

	static GLFWwindow* FFwindow;
	static int nextCounter;
	static bool user_message_flag, physicsDrag, getTextInput;
	static inline double user_message_time = 0.0;  // when the current message was set (glfwGetTime)
	static inline int user_message_kind = 0;       // 0 notice (transient), 1 error hold (Close reverts), 2 terminal (Close exits)

private:
	static bool powerHooks, showToolbox, viewPhysics, viewSurface, wheelZoom, except_thrown_flag;
	static int csgToolstate;
	static std::string historyDirectory, modelDirectory, objDirectory, modelFile, historyFile, user_message, user_message_title;
	static unsigned char buttonsDown;
	static bool surgicalDrag, ctrlShiftKeyDown;
	static int windowWidth, windowHeight, framebufferWidth, framebufferHeight;
	static ImVec2 minFileDlgSize;
	static int FileDlgMode;
	static GLuint hourglassTexture;
	static int hourglassWidth, hourglassHeight;
	static float lastSurgX, lastSurgY;
	static surgicalActions igSurgAct;
	static gl3wGraphics igGl3w;

};  // class FacialFlapsGui

#endif  // #ifndef _FACIAL_FLAPS_GUI_
