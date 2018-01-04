/** @file
@brief Implementation
@date 2015
@author
Sensics, Inc.
<http://sensics.com/osvr>
*/

// Copyright 2015 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// Both of these need to be enabled to force-enable logging to files.
#define ENABLE_LOGGING 1
#define ENABLE_LOGFILE 1

// Internal includes
#include "OsvrRenderingPlugin.h"
#include "Unity/IUnityGraphics.h"
#include "UnityRendererType.h"
#include "OsvrUnityRenderer.h"

// Library includes
#include <osvr/ClientKit/Context.h>
#include <osvr/ClientKit/Interface.h>
#include <osvr/Util/Finally.h>
#include <osvr/Util/MatrixConventionsC.h>


#if UNITY_WIN
#define NO_MINMAX
#define WIN32_LEAN_AND_MEAN
// logging on windows
#if defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
#include <fstream>
#include <iostream>
#endif // end define ENABLE_LOGGING
#include <memory>
#endif

// D3D- specific includes
// Include headers for the graphics APIs we support
#if SUPPORT_D3D11
#include <d3d11.h>

#include "Unity/IUnityGraphicsD3D11.h"
#include <osvr/RenderKit/GraphicsLibraryD3D11.h>
#include <osvr/RenderKit/RenderManagerD3D11C.h>

#endif // SUPPORT_D3D11

// OpenGL/OpenGLES-specific includes
#if SUPPORT_OPENGL

#if UNITY_WIN || UNITY_LINUX
// Needed for render buffer calls.  OSVR will have called glewInit() for us
// when we open the display.
#include <GL/glew.h>
#include <GL/gl.h>
#elif UNITY_OSX
// Mac OpenGL include
#include <OpenGL/OpenGL.h>
#include <osvr/RenderKit/GraphicsLibraryOpenGL.h>
#endif // SUPPORT_OPENGL

// VARIABLES
static IUnityInterfaces *s_UnityInterfaces = nullptr;
static IUnityGraphics *s_Graphics = nullptr;
static UnityRendererType s_deviceType = {};

/// @todo is this redundant? (given renderParams)
static double s_nearClipDistance = 0.1;
/// @todo is this redundant? (given renderParams)
static double s_farClipDistance = 1000.0;
/// @todo is this redundant? (given renderParams)
static double s_ipd = 0.063;

// cached viewport values
static std::uint32_t viewportWidth = 0;
static std::uint32_t viewportHeight = 0;

static OsvrUnityRenderer* osvrUnityRenderer = nullptr;

struct FrameInfoD3D11 {
	// Set up the vector of textures to render to and any framebuffer
	// we need to group them.
	std::vector<OSVR_RenderBufferD3D11> renderBuffers;
	ID3D11Texture2D* depthStencilTexture;
	ID3D11DepthStencilView* depthStencilView;
	IDXGIKeyedMutex* keyedMutex;
	FrameInfoD3D11() : renderBuffers(2)
	{
	}

};
static std::vector<FrameInfoD3D11*> frameInfo;
static OSVR_RenderParams s_renderParams;
static OSVR_RenderManager s_render = nullptr;
static OSVR_RenderManagerD3D11 s_renderD3D = nullptr;
static OSVR_ClientContext s_clientContext = nullptr;
static std::vector<OSVR_RenderInfoD3D11> s_renderInfo;
static std::vector<OSVR_RenderInfoD3D11> s_lastRenderInfo;
static OSVR_GraphicsLibraryD3D11 s_libraryD3D;
static OSVR_RenderInfoCount numRenderInfo;
static OSVR_ProjectionMatrix lastGoodProjMatrix;
static OSVR_Pose3 lastGoodPose;
static OSVR_ViewportDescription lastGoodViewportDescription;

// logging
#if defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
static std::ofstream s_debugLogFile;
static std::streambuf *s_oldCout = nullptr;
static std::streambuf *s_oldCerr = nullptr;
#endif // defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)

static int numBuffers = 2;
static int iterations = 0;

static void *s_leftEyeTexturePtr = nullptr;
static void *s_rightEyeTexturePtr = nullptr;
static void *s_leftEyeTexturePtrBuffer2 = nullptr;
static void *s_rightEyeTexturePtrBuffer2 = nullptr;

// D3D11 vars
#if SUPPORT_D3D11
static D3D11_TEXTURE2D_DESC s_textureDesc;
#endif // SUPPORT_D3D11

// OpenGL vars
#if SUPPORT_OPENGL
GLuint s_frameBuffer;
#endif // SUPPORT_OPENGL

// RenderEvents
// Called from Unity with GL.IssuePluginEvent
enum RenderEvents {
	kOsvrEventID_Render = 0,
	kOsvrEventID_Shutdown = 1,
	kOsvrEventID_Update = 2,
	kOsvrEventID_ConstructBuffers = 3,
	kOsvrEventID_ClearRoomToWorldTransform = 4
};

// --------------------------------------------------------------------------
// Helper utilities

// Allow writing to the Unity debug console from inside DLL land.
static DebugFnPtr s_debugLog = nullptr;
void UNITY_INTERFACE_API LinkDebug(DebugFnPtr d) { s_debugLog = d; }

// Only for debugging purposes, as this causes some errors at shutdown
inline void DebugLog(const char *str) {
#if UNITY_ANDROID
	return;/*
		   if (androidDebugLogMethodID != nullptr)
		   {
		   jstring jstr = jniEnvironment->NewStringUTF(str);
		   jniEnvironment->CallStaticVoidMethod(osvrJniWrapperClass,
		   androidDebugLogMethodID, jstr);
		   }
		   return;*/
#else // all platforms besides Android
#if !defined(NDEBUG) || defined(ENABLE_LOGGING)
	if (s_debugLog != nullptr) {
		s_debugLog(str);
	}
#endif // !defined(NDEBUG) || defined(ENABLE_LOGGING)

#if defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
	if (s_debugLogFile) {
		s_debugLogFile << str << std::endl;
	}
#endif // defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
#endif // all platforms besides Android
}

// JNI hook
//@todo look into JNI version. Should this match in JNI plugin?
#if UNITY_ANDROID
// this OnLoad gets called automatically
jint JNI_OnLoad(JavaVM *vm, void *reserved) {
	jniEnvironment = 0;
	vm->AttachCurrentThread(&jniEnvironment, 0);
	return JNI_VERSION_1_6;
}
#endif

// --------------------------------------------------------------------------
// GraphicsDeviceEvents

#if SUPPORT_D3D11
// -------------------------------------------------------------------
///  Direct3D 11 setup/teardown code
inline void DoEventGraphicsDeviceD3D11(UnityGfxDeviceEventType eventType) {
	BOOST_ASSERT_MSG(
		s_deviceType,
		"Should only be able to get in here with a valid device type.");
	BOOST_ASSERT_MSG(
		s_deviceType.getDeviceTypeEnum() == OSVRSupportedRenderers::D3D11,
		"Should only be able to get in here if using D3D11 device type.");

	switch (eventType) {
	case kUnityGfxDeviceEventInitialize: {
		IUnityGraphicsD3D11 *d3d11 =
			s_UnityInterfaces->Get<IUnityGraphicsD3D11>();

		// Put the device and context into a structure to let RenderManager
		// know to use this one rather than creating its own.
		s_libraryD3D.device = d3d11->GetDevice();
		ID3D11DeviceContext *ctx = nullptr;
		s_libraryD3D.device->GetImmediateContext(&ctx);
		s_libraryD3D.context = ctx;
		DebugLog("[OSVR Rendering Plugin] Passed Unity device/context to "
			"RenderManager library.");
		break;
	}
	case kUnityGfxDeviceEventShutdown: {
		// Close the Renderer interface cleanly.
		// This should be handled in ShutdownRenderManager
		/// @todo delete library.D3D11; library.D3D11 = nullptr; 
		break;
	}
	}
}
#endif // SUPPORT_D3D11

#if SUPPORT_OPENGL
// -------------------------------------------------------------------
/// OpenGL setup/teardown code
/// @todo OpenGL path not implemented yet
inline void DoEventGraphicsDeviceOpenGL(UnityGfxDeviceEventType eventType) {
	BOOST_ASSERT_MSG(
		s_deviceType,
		"Should only be able to get in here with a valid device type.");
	BOOST_ASSERT_MSG(
		s_deviceType.getDeviceTypeEnum() == OSVRSupportedRenderers::OpenGL,
		"Should only be able to get in here if using OpenGL device type.");

	switch (eventType) {
	case kUnityGfxDeviceEventInitialize:
		DebugLog("OpenGL Initialize Event");
		break;
	case kUnityGfxDeviceEventShutdown:
		DebugLog("OpenGL Shutdown Event");
		break;
	default:
		break;
	}
}
#endif // SUPPORT_OPENGL

inline void dispatchEventToRenderer(UnityRendererType renderer,
	UnityGfxDeviceEventType eventType) {
	if (!renderer) {
		DebugLog("[OSVR Rendering Plugin] Current device type not supported");
		return;
	}
	switch (renderer.getDeviceTypeEnum()) {
#if SUPPORT_D3D11
	case OSVRSupportedRenderers::D3D11:
		DoEventGraphicsDeviceD3D11(eventType);
		break;
#endif
#if SUPPORT_OPENGL
	case OSVRSupportedRenderers::OpenGL:
		DoEventGraphicsDeviceOpenGL(eventType);
		break;
#endif
	case OSVRSupportedRenderers::EmptyRenderer:
	default:
		break;
	}
}

//@TODO InitSDLGL, ShareContext on OpenGL path

/// Needs the calling convention, even though it's static and not exported,
/// because it's registered as a callback on plugin load.
static void UNITY_INTERFACE_API
OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType) {
	switch (eventType) {
	case kUnityGfxDeviceEventInitialize: {
		if (osvrUnityRenderer == nullptr)
		{
			osvrUnityRenderer = new OsvrUnityRenderer();
		}
		osvrUnityRenderer->OnInitializeGraphicsDeviceEvent();
		//@todo windows, osx context sharing
		/*DebugLog(
		"[OSVR Rendering Plugin] OnGraphicsDeviceEvent(Initialize).\n");
		s_deviceType = s_Graphics->GetRenderer();
		if (!s_deviceType) {
		DebugLog("[OSVR Rendering Plugin] "
		"OnGraphicsDeviceEvent(Initialize): New device type is "
		"not supported!\n");
		}*/
		break;
	}

	case kUnityGfxDeviceEventShutdown: {
		// DebugLog("[OSVR Rendering Plugin] OnGraphicsDeviceEvent(Shutdown).\n");
		/// Here, we want to dispatch before we reset the device type, so the
		/// right device type gets shut down. Thus we return instead of break.
		dispatchEventToRenderer(s_deviceType, eventType);
		// s_deviceType.reset();
		return;
	}

	case kUnityGfxDeviceEventBeforeReset: {
		DebugLog(
			"[OSVR Rendering Plugin] OnGraphicsDeviceEvent(BeforeReset).\n");
		break;
	}

	case kUnityGfxDeviceEventAfterReset: {
		DebugLog(
			"[OSVR Rendering Plugin] OnGraphicsDeviceEvent(AfterReset).\n");
		break;
	}
	}

	dispatchEventToRenderer(s_deviceType, eventType);
}

// --------------------------------------------------------------------------
// UnitySetInterfaces
void UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces *unityInterfaces) {
#if UNITY_WIN
#if defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
	s_debugLogFile.open("RenderPluginLog.txt");

	// Capture std::cout and std::cerr from RenderManager.
	if (s_debugLogFile) {
		s_oldCout = std::cout.rdbuf();
		std::cout.rdbuf(s_debugLogFile.rdbuf());

		s_oldCerr = std::cerr.rdbuf();
		std::cerr.rdbuf(s_debugLogFile.rdbuf());
	}
#endif // defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
#endif // UNITY_WIN
	s_UnityInterfaces = unityInterfaces;
	s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
	s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

	// Run OnGraphicsDeviceEvent(initialize) manually on plugin load
	OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);
}

void UNITY_INTERFACE_API UnityPluginUnload() {
	s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
	OnGraphicsDeviceEvent(kUnityGfxDeviceEventShutdown);
#if UNITY_WIN
#if defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
	if (s_debugLogFile) {
		// Restore the buffers
		std::cout.rdbuf(s_oldCout);
		std::cerr.rdbuf(s_oldCerr);
		s_debugLogFile.close();
	}
#endif // defined(ENABLE_LOGGING) && defined(ENABLE_LOGFILE)
#endif // UNITY_WIN
}

#if UNITY_WIN
inline void UpdateRenderInfo() {

	if (s_render == nullptr)
	{
		return;
	}
	// Do a call to get the information we need to construct our
	// color and depth render-to-texture buffers.
	OSVR_RenderParams renderParams;
	osvrRenderManagerGetDefaultRenderParams(&renderParams);

	if ((OSVR_RETURN_SUCCESS != osvrRenderManagerGetNumRenderInfo(
		s_render, renderParams, &numRenderInfo))) {
		DebugLog("[OSVR Rendering Plugin] Could not get context number of render infos.");
		ShutdownRenderManager();
		return;
	}

	s_renderInfo.clear();
	for (OSVR_RenderInfoCount i = 0; i < numRenderInfo; i++) {
		OSVR_RenderInfoD3D11 info;
		if ((OSVR_RETURN_SUCCESS != osvrRenderManagerGetRenderInfoD3D11(
			s_renderD3D, i, renderParams, &info))) {
			DebugLog("[OSVR Rendering Plugin] Could not get render info " + i);
			ShutdownRenderManager();
			return;
		}
		s_renderInfo.push_back(info);
	}
	if (numRenderInfo > 0)
	{
		s_lastRenderInfo = s_renderInfo;
	}
}
#endif // UNITY_WIN

// Updates the internal "room to world" transformation (applied to all
// tracker data for this client context instance) based on the user's head
// orientation, so that the direction the user is facing becomes -Z to your
// application. Only rotates about the Y axis (yaw).
//
// Note that this method internally calls osvrClientUpdate() to get a head pose
// so your callbacks may be called during its execution!
/// @todo does this actually get called from anywhere or is it dead code?
void SetRoomRotationUsingHead() { /* s_renderD3D-> SetRoomRotationUsingHead();
								  */
}

// Clears/resets the internal "room to world" transformation back to an
// identity transformation - that is, clears the effect of any other
// manipulation of the room to world transform.
/// @todo does this actually get called from anywhere or is it dead code?
void ClearRoomToWorldTransform() { /*s_render->ClearRoomToWorldTransform(); */
}

void UNITY_INTERFACE_API ShutdownRenderManager() {
	//DebugLog("[OSVR Rendering Plugin] Shutting down RenderManager.");
	if (osvrUnityRenderer != nullptr)
	{
		osvrUnityRenderer->ShutdownRenderManager();
	}
	return;
	///@todo move the code below to d3d file
	if (s_render != nullptr) {
		osvrDestroyRenderManager(s_render);
		s_render = nullptr;
		s_leftEyeTexturePtr = nullptr;
		s_leftEyeTexturePtrBuffer2 = nullptr;
		s_rightEyeTexturePtr = nullptr;
		s_rightEyeTexturePtrBuffer2 = nullptr;
		//s_renderInfo.clear();
		//s_lastRenderInfo.clear();
		/*for (int i = 0; i < frameInfo.size(); i++)
		{
		for (int j = 0; j < frameInfo[i]->renderBuffers.size(); j++)
		{
		frameInfo[i]->renderBuffers[j] = nullptr;
		}

		}*/
		frameInfo.clear();
	}
	s_clientContext = nullptr;
}

// Called from Unity to create a RenderManager, passing in a ClientContext
OSVR_ReturnCode UNITY_INTERFACE_API
CreateRenderManagerFromUnity(OSVR_ClientContext context) {
	if (osvrUnityRenderer != nullptr)
	{
		return osvrUnityRenderer->CreateRenderManager(context);
	}


	if (s_render != nullptr) {
		if (osvrRenderManagerGetDoingOkay(s_render)) {
			DebugLog("[OSVR Rendering Plugin] RenderManager already created "
				"and doing OK - will just return success without trying "
				"to re-initialize.");
			return OSVR_RETURN_SUCCESS;
		}

		DebugLog("[OSVR Rendering Plugin] RenderManager already created, "
			"but not doing OK. Will shut down before creating again.");
		//  ShutdownRenderManager();
		return OSVR_RETURN_SUCCESS;
	}
	if (s_clientContext != nullptr) {
		DebugLog(
			"[OSVR Rendering Plugin] Client context already set! Replacing...");
	}
	s_clientContext = context;

	if (!s_deviceType) {
		// @todo pass the platform from Unity
		// This is a patch to workaround a bug in Unity where the renderer type
		// is not being set on Windows x86 builds. Until the OpenGL path is
		// working, it's safe to assume we're using D3D11, but we'd rather get
		// the platform from Unity than assume it's D3D11.

		s_deviceType = kUnityGfxRendererD3D11;
	}

	bool setLibraryFromOpenDisplayReturn = false;
	/// @todo We should always have a legit value in
	/// s_deviceType.getDeviceTypeEnum() at this point, right?
	switch (s_deviceType.getDeviceTypeEnum()) {

#if SUPPORT_D3D11
	case OSVRSupportedRenderers::D3D11:
		if (OSVR_RETURN_SUCCESS !=
			osvrCreateRenderManagerD3D11(context, "Direct3D11", s_libraryD3D,
			&s_render, &s_renderD3D)) {
			DebugLog("[OSVR Rendering Plugin] Could not create RenderManager");
			return OSVR_RETURN_FAILURE;
		}
#ifdef ATTEMPT_D3D_SHARING
		setLibraryFromOpenDisplayReturn = true;
#endif // ATTEMPT_D3D_SHARING
		break;
#endif // SUPPORT_D3D11

#if SUPPORT_OPENGL
	case OSVRSupportedRenderers::OpenGL:
		// s_render = osvr::renderkit::createRenderManager(context, "OpenGL");
		setLibraryFromOpenDisplayReturn = true;
		break;
#endif // SUPPORT_OPENGL
	}

	if (s_render == nullptr) {
		DebugLog("[OSVR Rendering Plugin] here we aer.");

		DebugLog("[OSVR Rendering Plugin] Could not create RenderManager");

		ShutdownRenderManager();
		return OSVR_RETURN_FAILURE;
	}

	// Open the display and make sure this worked.
	OSVR_OpenResultsD3D11 openResults;
	if ((OSVR_RETURN_SUCCESS !=
		osvrRenderManagerOpenDisplayD3D11(s_renderD3D, &openResults)) ||
		(openResults.status == OSVR_OPEN_STATUS_FAILURE)) {
		DebugLog("[OSVR Rendering Plugin] Could not open display");
		ShutdownRenderManager();
		return OSVR_RETURN_FAILURE;
	}
	if (openResults.library.device == nullptr) {
		DebugLog("[OSVR Rendering Plugin] Could not get device when opening "
			"display");

		ShutdownRenderManager();
		return OSVR_RETURN_FAILURE;
	}
	if (openResults.library.context == nullptr) {
		DebugLog("[OSVR Rendering Plugin] Could not get context when opening "
			"display");

		ShutdownRenderManager();
		return OSVR_RETURN_FAILURE;
	}

	// create a new set of RenderParams for passing to GetRenderInfo()
	osvrRenderManagerGetDefaultRenderParams(&s_renderParams);

	UpdateRenderInfo();

	DebugLog("[OSVR Rendering Plugin] CreateRenderManagerFromUnity Success!");
	return OSVR_RETURN_SUCCESS;
}

/// Helper function that handles doing the loop of constructing buffers, and
/// returning failure if any of them in the loop return failure.
template <typename F, typename G>
inline OSVR_ReturnCode applyRenderBufferConstructor(const int numBuffers,
	F &&bufferConstructor,
	G &&bufferCleanup) {

	/// If we bail any time before the end, we'll automatically clean up the
	/// render buffers with this lambda.
	/* auto cleanupBuffers = osvr::util::finally([&] {
	DebugLog("[OSVR Rendering Plugin] Cleaning up render buffers.");
	for (auto &rb : s_renderBuffers) {
	bufferCleanup(rb);
	}
	s_renderBuffers.clear();
	DebugLog("[OSVR Rendering Plugin] Render buffer cleanup complete.");
	});*/

	/// Construct all the buffers as isntructed
	for (int i = 0; i < numBuffers; ++i) {
		auto ret = bufferConstructor(i);
		if (ret != OSVR_RETURN_SUCCESS) {
			DebugLog("[OSVR Rendering Plugin] Failed in a buffer constructor!");
			return OSVR_RETURN_FAILURE;
		}
	}

	/// Register our constructed buffers so that we can use them for
	/// presentation.
	/* if (!s_render->RegisterRenderBuffers(s_renderBuffers)) {
	DebugLog("RegisterRenderBuffers() returned false, cannot continue");
	return OSVR_RETURN_FAILURE;
	}*/
	// Register our constructed buffers so that we can use them for
	// presentation.

	UpdateRenderInfo();

	OSVR_RenderManagerRegisterBufferState registerBufferState;
	if ((OSVR_RETURN_SUCCESS !=
		osvrRenderManagerStartRegisterRenderBuffers(&registerBufferState))) {
		DebugLog("[OSVR Rendering Plugin]Could not start registering render "
			"buffers");
		ShutdownRenderManager();
	}
	for (size_t i = 0; i < numRenderInfo; i++) {
		if ((OSVR_RETURN_SUCCESS !=
			osvrRenderManagerRegisterRenderBufferD3D11(registerBufferState,
			s_renderBuffers[i]))) {
			DebugLog(
				"[OSVR Rendering Plugin]Could not register render buffers");
			ShutdownRenderManager();
		}
	}
	if ((OSVR_RETURN_SUCCESS != osvrRenderManagerFinishRegisterRenderBuffers(
		s_render, registerBufferState, true))) {
		DebugLog("[OSVR Rendering Plugin]Could not finish registering render "
			"buffers");
		ShutdownRenderManager();
	}
	/// Only if we succeed, do we cancel the cleanup and carry on.
	// cleanupBuffers.cancel();
	return OSVR_RETURN_SUCCESS;
}

#if SUPPORT_OPENGL
inline OSVR_ReturnCode ConstructBuffersOpenGL(int eye) {
	// Init glew
	/* glewExperimental = 1u;
	/// @todo doesn't rendermanager do this glewInit for us?
	GLenum err = glewInit();
	if (err != GLEW_OK) {
	DebugLog("glewInit failed, aborting.");
	/// @todo shouldn't we return here then?
	}

	if (eye == 0) {
	// do this once
	glGenFramebuffers(1, &s_frameBuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, s_frameBuffer);
	}

	// The color buffer for this eye.  We need to put this into
	// a generic structure for the Present function, but we only need
	// to fill in the OpenGL portion.
	if (eye == 0) // left eye
	{
	GLuint leftEyeColorBuffer = 0;
	glGenRenderbuffers(1, &leftEyeColorBuffer);
	osvr::renderkit::RenderBuffer rb;
	rb.OpenGL = new osvr::renderkit::RenderBufferOpenGL;
	rb.OpenGL->colorBufferName = leftEyeColorBuffer;
	s_renderBuffers.push_back(rb);
	// "Bind" the newly created texture : all future texture
	// functions will modify this texture glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, leftEyeColorBuffer);

	// Give an empty image to OpenGL ( the last "0" means "empty" )
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
	static_cast<GLsizei>(s_renderInfo[eye].viewport.width),
	static_cast<GLsizei>(s_renderInfo[eye].viewport.height),
	0,
	GL_RGB, GL_UNSIGNED_BYTE, &leftEyeColorBuffer);
	} else // right eye
	{
	GLuint rightEyeColorBuffer = 0;
	glGenRenderbuffers(1, &rightEyeColorBuffer);
	osvr::renderkit::RenderBuffer rb;
	rb.OpenGL = new osvr::renderkit::RenderBufferOpenGL;
	rb.OpenGL->colorBufferName = rightEyeColorBuffer;
	s_renderBuffers.push_back(rb);
	// "Bind" the newly created texture : all future texture
	// functions will modify this texture glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, rightEyeColorBuffer);

	// Give an empty image to OpenGL ( the last "0" means "empty" )
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
	static_cast<GLsizei>(s_renderInfo[eye].viewport.width),
	static_cast<GLsizei>(s_renderInfo[eye].viewport.height),
	0,
	GL_RGB, GL_UNSIGNED_BYTE, &rightEyeColorBuffer);
	}

	// Bilinear filtering
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	*/
	return OSVR_RETURN_SUCCESS;
}
/*
inline void CleanupBufferOpenGL(OSVR_RenderBufferOpenGL &rb) {
	/// @todo incomplete cleanup - but better than leaking in case of failure.
	//  delete rb.OpenGL;
	// rb.OpenGL = nullptr;
}*/
#endif // SUPPORT_OPENGL

#if SUPPORT_D3D11
inline ID3D11Texture2D *GetEyeTextureD3D11(int eye, int buffer) {
	if (buffer == 0)
	{
		return reinterpret_cast<ID3D11Texture2D *>(eye == 0 ? s_leftEyeTexturePtr
			: s_rightEyeTexturePtr);
	}
	else
	{
		return reinterpret_cast<ID3D11Texture2D *>(eye == 0 ? s_leftEyeTexturePtrBuffer2
			: s_rightEyeTexturePtrBuffer2);
	}
}

inline OSVR_ReturnCode ConstructBuffersD3D11(int eye, int buffer, FrameInfoD3D11* fInfo) {
	DebugLog("[OSVR Rendering Plugin] ConstructBuffersD3D11");
	HRESULT hr;
	// The color buffer for this eye.  We need to put this into
	// a generic structure for the Present function, but we only need
	// to fill in the Direct3D portion.
	//  Note that this texture format must be RGBA and unsigned byte,
	// so that we can present it to Direct3D for DirectMode.
	ID3D11Texture2D *D3DTexture = GetEyeTextureD3D11(eye, buffer);
	unsigned width = static_cast<unsigned>(s_renderInfo[eye].viewport.width);
	unsigned height = static_cast<unsigned>(s_renderInfo[eye].viewport.height);

	D3DTexture->GetDesc(&s_textureDesc);

	// Fill in the resource view for your render texture buffer here
	D3D11_RENDER_TARGET_VIEW_DESC renderTargetViewDesc = {};
	// This must match what was created in the texture to be rendered
	/// @todo Figure this out by introspection on the texture?
	// renderTargetViewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
	/// @todo Interesting - change this line to DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
	/// and not only do you not get direct mode, you get multicolored static on
	/// the display.
	renderTargetViewDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	renderTargetViewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
	renderTargetViewDesc.Texture2D.MipSlice = 0;

	// Create the render target view.
	ID3D11RenderTargetView *renderTargetView =
		nullptr; //< Pointer to our render target view
	hr = s_renderInfo[eye].library.device->CreateRenderTargetView(
		D3DTexture, &renderTargetViewDesc, &renderTargetView);
	if (FAILED(hr)) {
		DebugLog(
			"[OSVR Rendering Plugin] Could not create render target for eye");
		return OSVR_RETURN_FAILURE;
	}

	// Push the filled-in RenderBuffer onto the stack.
	OSVR_RenderBufferD3D11 rbD3D;
	rbD3D.colorBuffer = D3DTexture;
	rbD3D.colorBufferView = renderTargetView;
	fInfo->renderBuffers.push_back(rbD3D);
	//s_renderBuffers.push_back(rbD3D);

	IDXGIKeyedMutex* keyedMutex = nullptr;
	hr = D3DTexture->QueryInterface(
		__uuidof(IDXGIKeyedMutex), (LPVOID*)&keyedMutex);
	if (FAILED(hr) || keyedMutex == nullptr) {
		std::cerr << "Could not get mutex pointer" << std::endl;
		return -2;
	}
	fInfo->keyedMutex = keyedMutex;

	//==================================================================
	// Create a depth buffer

	// Make the depth/stencil texture.
	D3D11_TEXTURE2D_DESC textureDescription = { 0 };
	textureDescription.SampleDesc.Count = 1;
	textureDescription.SampleDesc.Quality = 0;
	textureDescription.Usage = D3D11_USAGE_DEFAULT;
	textureDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
	textureDescription.Width = width;
	textureDescription.Height = height;
	textureDescription.MipLevels = 1;
	textureDescription.ArraySize = 1;
	textureDescription.CPUAccessFlags = 0;
	textureDescription.MiscFlags = 0;
	/// @todo Make this a parameter
	textureDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
	ID3D11Texture2D* depthStencilBuffer;
	hr = s_libraryD3D.device->CreateTexture2D(
		&textureDescription, NULL, &depthStencilBuffer);
	if (FAILED(hr)) {
		std::cerr << "Could not create depth/stencil texture"
			<< std::endl;
		return -4;
	}
	fInfo->depthStencilTexture = depthStencilBuffer;

	// Create the depth/stencil view description
	D3D11_DEPTH_STENCIL_VIEW_DESC depthStencilViewDescription;
	memset(&depthStencilViewDescription, 0, sizeof(depthStencilViewDescription));
	depthStencilViewDescription.Format = textureDescription.Format;
	depthStencilViewDescription.ViewDimension =
		D3D11_DSV_DIMENSION_TEXTURE2D;
	depthStencilViewDescription.Texture2D.MipSlice = 0;

	ID3D11DepthStencilView* depthStencilView;
	hr = s_libraryD3D.device->CreateDepthStencilView(
		depthStencilBuffer,
		&depthStencilViewDescription,
		&depthStencilView);
	if (FAILED(hr)) {
		std::cerr << "Could not create depth/stencil view"
			<< std::endl;
		return -5;
	}
	fInfo->depthStencilView = depthStencilView;

	return OSVR_RETURN_SUCCESS;
}

inline void CleanupBufferD3D11(OSVR_RenderBufferD3D11 &rb) { delete &rb; }
#endif // SUPPORT_D3D11

OSVR_ReturnCode UNITY_INTERFACE_API ConstructRenderBuffers() {
#if UNITY_ANDROID
	if (!setupRenderTextures(gRenderManager)) {
		return OSVR_RETURN_FAILURE;
	}
	else
		return OSVR_RETURN_SUCCESS;
#else
	if (!s_deviceType) {
		DebugLog("[OSVR Rendering Plugin] Device type not supported.");
		return OSVR_RETURN_FAILURE;
	}
	UpdateRenderInfo();

	// construct buffers
	//const int n = static_cast<int>(numRenderInfo);
	switch (s_deviceType.getDeviceTypeEnum()) {
#if SUPPORT_D3D11
	case OSVRSupportedRenderers::D3D11:
		for (int i = 0; i < numBuffers; i++)
		{
			FrameInfoD3D11* f = new FrameInfoD3D11();
			f->renderBuffers.clear();
			for (int j = 0; j < numRenderInfo; j++)
			{
				ConstructBuffersD3D11(i, j, f);
			}
			frameInfo.push_back(f);
		}

		// Register our constructed buffers so that we can use them for
		// presentation.
		OSVR_RenderManagerRegisterBufferState registerBufferState;
		if ((OSVR_RETURN_SUCCESS != osvrRenderManagerStartRegisterRenderBuffers(
			&registerBufferState))) {
			DebugLog("[OSVR Rendering Plugin] Could not start registering render buffers");
			ShutdownRenderManager();
			return OSVR_RETURN_FAILURE;
		}
		for (size_t i = 0; i < frameInfo.size(); i++) {
			for (int j = 0; j < numRenderInfo; j++)
			{
				if ((OSVR_RETURN_SUCCESS != osvrRenderManagerRegisterRenderBufferD3D11(
					registerBufferState, frameInfo[i]->renderBuffers[j]))) {
					DebugLog("[OSVR Rendering Plugin] Could not register render buffer ");
					ShutdownRenderManager();
					return OSVR_RETURN_FAILURE;
				}
			}

		}
		if ((OSVR_RETURN_SUCCESS != osvrRenderManagerFinishRegisterRenderBuffers(
			s_render, registerBufferState, false))) {
			DebugLog("[OSVR Rendering Plugin] Could not finish registering render buffers");
			ShutdownRenderManager();
			return OSVR_RETURN_FAILURE;
		}
		//return applyRenderBufferConstructor(n, ConstructBuffersD3D11,
		// CleanupBufferD3D11);
		break;
#endif
#if SUPPORT_OPENGL
	case OSVRSupportedRenderers::OpenGL:
		// return applyRenderBufferConstructor(n, ConstructBuffersOpenGL,
		//  CleanupBufferOpenGL);
		break;
#endif
	case OSVRSupportedRenderers::EmptyRenderer:
	default:
		DebugLog("Device type not supported.");
		return OSVR_RETURN_FAILURE;
	}
#endif // non-Android platforms
}

void UNITY_INTERFACE_API SetNearClipDistance(double distance) {
	s_nearClipDistance = distance;
#if !UNITY_ANDROID
	s_renderParams.nearClipDistanceMeters = s_nearClipDistance;
#endif
}

void UNITY_INTERFACE_API SetFarClipDistance(double distance) {
	s_farClipDistance = distance;
#if !UNITY_ANDROID
	s_renderParams.farClipDistanceMeters = s_farClipDistance;
#endif
}

void UNITY_INTERFACE_API SetIPD(double ipdMeters) {
	s_ipd = ipdMeters;
	// s_renderParams.IPDMeters = s_ipd;
}

OSVR_ViewportDescription UNITY_INTERFACE_API GetViewport(std::uint8_t eye) {
#if UNITY_ANDROID
	OSVR_RenderParams renderParams;
	OSVR_ReturnCode rc = osvrRenderManagerGetDefaultRenderParams(&renderParams);
	checkReturnCode(rc, "osvrRenderManagerGetDefaultRenderParams call failed.");
	RenderInfoCollectionOpenGL renderInfoCollection(gRenderManager,
		renderParams);
	OSVR_RenderInfoOpenGL currentRenderInfo =
		renderInfoCollection.getRenderInfo(eye);
	OSVR_ViewportDescription viewDesc;
	viewDesc.width = currentRenderInfo.viewport.width;
	viewDesc.height = currentRenderInfo.viewport.height;
	viewDesc.left = currentRenderInfo.viewport.left;
	viewDesc.lower = currentRenderInfo.viewport.lower;
	return viewDesc;
#else  // non-Android platoforms
	OSVR_ViewportDescription viewportDescription;
	if (numRenderInfo > 0 && eye <= numRenderInfo - 1) {
		viewportDescription = s_lastRenderInfo[eye].viewport;

		// cache the viewport width and height
		// patches issue where sometimes empty viewport is returned
		//@todo fix the real cause of why this method bugs out occasionally on
		//some machines, more often on others
		if (viewportWidth == 0 && s_lastRenderInfo[eye].viewport.width != 0) {
			viewportWidth = s_lastRenderInfo[eye].viewport.width;
		}
		if (viewportHeight == 0 && s_lastRenderInfo[eye].viewport.height != 0) {
			viewportHeight = s_lastRenderInfo[eye].viewport.height;
		}
		lastGoodViewportDescription = viewportDescription;
	}
	else {
		// we shouldn't be here unless we hit a bug, in which case, we avoid
		// error by returning cached viewport values
		std::string errorLog = "[OSVR Rendering Plugin] Error in GetViewport, "
			"returning cached values. Eye = " +
			std::to_string(int(eye));
		DebugLog(errorLog.c_str());
		/*viewportDescription.left = 0;
		viewportDescription.lower = 0;
		viewportDescription.width = viewportWidth;
		viewportDescription.height = viewportHeight;*/
		lastGoodViewportDescription = viewportDescription;
	}
	return viewportDescription;
#endif // non-Android platforms
}

OSVR_ProjectionMatrix UNITY_INTERFACE_API
GetProjectionMatrix(std::uint8_t eye) {
#if UNITY_ANDROID
	OSVR_RenderParams renderParams;
	OSVR_ReturnCode rc = osvrRenderManagerGetDefaultRenderParams(&renderParams);
	checkReturnCode(rc, "osvrRenderManagerGetDefaultRenderParams call failed.");
	RenderInfoCollectionOpenGL renderInfoCollection(gRenderManager,
		renderParams);
	OSVR_RenderInfoOpenGL currentRenderInfo =
		renderInfoCollection.getRenderInfo(eye);
	OSVR_ProjectionMatrix proj;
	proj.left = currentRenderInfo.projection.left;
	proj.right = currentRenderInfo.projection.right;
	proj.top = currentRenderInfo.projection.top;
	proj.bottom = currentRenderInfo.projection.bottom;
	proj.nearClip = currentRenderInfo.projection.nearClip;
	proj.farClip = currentRenderInfo.projection.farClip;
	return proj;
#else
	OSVR_ProjectionMatrix pm;
	if (numRenderInfo > 0 && eye <= numRenderInfo - 1) {
		pm = s_lastRenderInfo[eye].projection;
		lastGoodProjMatrix = pm;
	}
	else {
		std::string errorLog = "[OSVR Rendering Plugin] Error in "
			"GetProjectionMatrix, returning default values. "
			"Eye = " +
			std::to_string(int(eye));
		DebugLog(errorLog.c_str());
		pm = lastGoodProjMatrix;
	}
	return pm;
#endif // non-Android platforms
}

OSVR_Pose3 UNITY_INTERFACE_API GetEyePose(std::uint8_t eye) {
#if UNITY_ANDROID
	OSVR_RenderParams renderParams;
	OSVR_ReturnCode rc = osvrRenderManagerGetDefaultRenderParams(&renderParams);
	checkReturnCode(rc, "osvrRenderManagerGetDefaultRenderParams call failed.");
	RenderInfoCollectionOpenGL renderInfoCollection(gRenderManager,
		renderParams);
	OSVR_RenderInfoOpenGL currentRenderInfo =
		renderInfoCollection.getRenderInfo(eye);
	return currentRenderInfo.pose;
#else
	OSVR_Pose3 pose;
	osvrPose3SetIdentity(&pose);
	if (numRenderInfo > 0 && eye <= numRenderInfo - 1 && s_render != nullptr) {
		pose = s_lastRenderInfo[eye].pose;
		lastGoodPose = pose;
	}
	else {
		std::string errorLog = "[OSVR Rendering Plugin] Error in GetEyePose, "
			"returning default values. Eye = " +
			std::to_string(int(eye));
		DebugLog(errorLog.c_str());
		pose = lastGoodPose;
	}
	return pose;
#endif // non-Android platforms
}

// --------------------------------------------------------------------------
// Should pass in eyeRenderTexture.GetNativeTexturePtr(), which gets updated in
// Unity when the camera renders.
// On Direct3D-like devices, GetNativeTexturePtr() returns a pointer to the base
// texture type (IDirect3DBaseTexture9 on D3D9, ID3D11Resource on D3D11). On
// OpenGL-like devices the texture "name" is returned; cast the pointer to
// integer type to get it. On platforms that do not support native code plugins,
// this function always returns NULL.
// Note that calling this function when using multi-threaded rendering will
// synchronize with the rendering thread (a slow operation), so best practice is
// to set up needed texture pointers only at initialization time.
// For more reference, see:
// http://docs.unity3d.com/ScriptReference/Texture.GetNativeTexturePtr.html
int UNITY_INTERFACE_API SetColorBufferFromUnity(void *texturePtr,
	std::uint8_t eye, std::uint8_t buffer) {
	if (!s_deviceType) {
		return OSVR_RETURN_FAILURE;
	}

	DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity");

#if UNITY_ANDROID
	if (eye == 0) {
		if (buffer == 0)
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 0 buffer 0");

			gLeftEyeTextureID = (GLuint)texturePtr;
		}
		else
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 1 buffer 0");

			gLeftEyeTextureIDBuffer2 = (GLuint)texturePtr;
		}
	}
	else {
		if (buffer == 0)
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 0 buffer 1");

			gRightEyeTextureID = (GLuint)texturePtr;
		}
		else
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 1 buffer 1");

			gRightEyeTextureIDBuffer2 = (GLuint)texturePtr;
		}
	}

	return OSVR_RETURN_SUCCESS;
#else
	if (eye == 0) {
		if (buffer == 0)
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 0 buffer 0");

			s_leftEyeTexturePtr = texturePtr;
		}
		else
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 1 buffer 0");

			s_leftEyeTexturePtrBuffer2 = texturePtr;
		}
	}
	else {
		if (buffer == 0)
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 0 buffer 1");

			s_rightEyeTexturePtr = texturePtr;
		}
		else
		{
			DebugLog("[OSVR Rendering Plugin] SetColorBufferFromUnity eye 1 buffer 1");

			s_rightEyeTexturePtrBuffer2 = texturePtr;
		}
	}

	return OSVR_RETURN_SUCCESS;
#endif
}
#if SUPPORT_D3D11
// Renders the view from our Unity cameras by copying data at
// Unity.RenderTexture.GetNativeTexturePtr() to RenderManager colorBuffers
void RenderViewD3D11(const OSVR_RenderInfoD3D11 &ri,
	ID3D11RenderTargetView *renderTargetView, int eyeIndex, int frame) {
	auto context = ri.library.context;
	// Set up to render to the textures for this eye
	context->OMSetRenderTargets(1, &renderTargetView, NULL);

	// copy the updated RenderTexture from Unity to RenderManager colorBuffer
	frameInfo[frame]->renderBuffers[eyeIndex].colorBuffer = GetEyeTextureD3D11(eyeIndex, frame);
}
#endif // SUPPORT_D3D11

#if SUPPORT_OPENGL
// Render the world from the specified point of view.
//@todo This is not functional yet.
inline void RenderViewOpenGL(
	const OSVR_RenderInfoOpenGL &ri, //< Info needed to render
	GLuint frameBufferObj, //< Frame buffer object to bind our buffers to
	GLuint colorBuffer,    //< Color buffer to render into
	int eyeIndex) {
	// Render to our framebuffer
	/*  glBindFramebuffer(GL_FRAMEBUFFER, frameBufferObj);

	// Set color and depth buffers for the frame buffer
	glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, colorBuffer,
	0);
	// glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
	// GL_RENDERBUFFER, depthBuffer);

	// Set the list of draw buffers.
	GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
	glDrawBuffers(1, DrawBuffers); // "1" is the size of DrawBuffers

	// Always check that our framebuffer is ok
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
	DebugLog("RenderView: Incomplete Framebuffer");
	return;
	}

	// Set the viewport to cover our entire render texture.
	glViewport(0, 0, static_cast<GLsizei>(ri.viewport.width),
	static_cast<GLsizei>(ri.viewport.height));

	// Set the OpenGL projection matrix
	GLdouble projection[16];
	osvr::renderkit::OSVR_Projection_to_OpenGL(projection, ri.projection);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glMultMatrixd(projection);

	/// Put the transform into the OpenGL ModelView matrix
	GLdouble modelView[16];
	osvr::renderkit::OSVR_PoseState_to_OpenGL(modelView, ri.pose);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glMultMatrixd(modelView);

	// Clear the screen to red and clear depth
	glClearColor(1, 0, 0, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// =================================================================
	// This is where we draw our world and hands and any other objects.
	// We're in World Space.  To find out about where to render objects
	// in OSVR spaces (like left/right hand space) we need to query the
	// interface and handle the coordinate tranforms ourselves.

	// update native texture from code
	glBindTexture(GL_TEXTURE_2D,
	s_renderBuffers[eyeIndex].OpenGL->colorBufferName);
	int texWidth, texHeight;
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &texWidth);
	glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &texHeight);

	GLuint glTex = eyeIndex == 0 ? (GLuint)s_leftEyeTexturePtr
	: (GLuint)s_rightEyeTexturePtr;

	// unsigned char* data = new unsigned char[texWidth*texHeight * 4];
	// FillTextureFromCode(texWidth, texHeight, texHeight * 4, data);
	// glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texWidth, texHeight, GL_RGBA,
	// GL_UNSIGNED_BYTE, (GLuint));
	// delete[] data;
	// Draw a cube with a 5-meter radius as the room we are floating in.
	// draw_cube(5.0);
	*/
}
#endif // SUPPORT_OPENGL

inline void DoRender() {
	if (!s_deviceType) {
		return;
	}
	//DebugLog("[OSVR Rendering Plugin] DoRender.");

	switch (s_deviceType.getDeviceTypeEnum()) {
#if SUPPORT_D3D11
	case OSVRSupportedRenderers::D3D11: {


		int frame = iterations % numBuffers;

		// Grab and lock the mutex, so that we will be able to render
		// to it whether or not RenderManager locks it on our behalf.
		// it will not be auto-locked when we're in the non-ATW case.
		//std::cout << "RenderThread: locking buffer for frame " << frame << " using key " << 0 << std::endl;
		/*auto hr = frameInfo[frame]->keyedMutex->AcquireSync(0, 500);
		if (FAILED(hr) || hr == E_FAIL || hr == WAIT_ABANDONED || hr == WAIT_TIMEOUT) {
		std::cerr << "RenderThread: could not lock buffer for frame " << frame << std::endl;
		switch (hr) {
		case E_FAIL:
		std::cerr << "RenderThread: error == E_FAIL" << std::endl;
		break;
		case WAIT_ABANDONED:
		std::cerr << "RenderThread: error == WAIT_ABANDONED" << std::endl;
		break;
		case WAIT_TIMEOUT:
		std::cerr << "RenderThread: error == WAIT_TIMEOUT" << std::endl;
		break;
		default:
		std::cerr << "RenderThread: error == (unknown error type: " << hr << ")" << std::endl;
		break;
		}
		osvrDestroyRenderManager(s_render);
		return;
		}*/


		const auto n = static_cast<int>(numRenderInfo);
		// Render into each buffer using the specified information.
		for (int i = 0; i < n; ++i) {
			RenderViewD3D11(s_renderInfo[i], frameInfo[frame]->renderBuffers[i].colorBufferView,
				i, frame);
		}

		// Grab and lock the mutex, so that we will be able to render
		// to it whether or not RenderManager locks it on our behalf.
		// it will not be auto-locked when we're in the non-ATW case.
		//std::cout << "RenderThread: Unlocking buffer for frame " << frame << " using key " << 1 << std::endl;
		/*hr = frameInfo[frame]->keyedMutex->ReleaseSync(0);
		if (FAILED(hr)) {
		std::cerr << "RenderThread: could not unlock buffer for frame " << frame << std::endl;
		osvrDestroyRenderManager(s_render);
		return;
		}*/

		// Send the rendered results to the screen
		OSVR_RenderManagerPresentState presentState;
		if ((OSVR_RETURN_SUCCESS !=
			osvrRenderManagerStartPresentRenderBuffers(&presentState))) {
			DebugLog("[OSVR Rendering Plugin] Could not start presenting "
				"render buffers");
			ShutdownRenderManager();
		}
		// create normalized cropping viewports for side-by-side rendering to a single render target
		std::vector<OSVR_ViewportDescription> NVCPs;
		double fraction = 1.0 / s_renderInfo.size();
		for (size_t i = 0; i < s_renderInfo.size(); i++) {
			OSVR_ViewportDescription v;
			v.left = fraction * i;
			v.lower = 0.0;
			v.width = fraction;
			v.height = 1;
			NVCPs.push_back(v);
		}
		OSVR_ViewportDescription fullView;
		fullView.left = fullView.lower = 0;
		fullView.width = fullView.height = 1;
		for (size_t i = 0; i < numRenderInfo; i++) {
			if ((OSVR_RETURN_SUCCESS !=
				osvrRenderManagerPresentRenderBufferD3D11(
				presentState, frameInfo[frame]->renderBuffers[i], s_renderInfo[i],
				fullView))) {
				DebugLog(
					"[OSVR Rendering Plugin] Could not present render buffer ");
				ShutdownRenderManager();
			}
		}

		if ((OSVR_RETURN_SUCCESS !=
			osvrRenderManagerFinishPresentRenderBuffers(
			s_render, presentState, s_renderParams, true))) {
			DebugLog("[OSVR Rendering Plugin] Could not finish presenting "
				"render buffers");
			ShutdownRenderManager();
		}

		iterations++;

		break;
	}
#endif // SUPPORT_D3D11

#if SUPPORT_OPENGL
	case OSVRSupportedRenderers::OpenGL: {
#if UNITY_ANDROID
		//DebugLog("[OSVR Rendering Plugin] RenderFrame.");
		renderFrame();
#endif
		// OpenGL
		//@todo OpenGL path is not working yet
		// Render into each buffer using the specified information.

		/* for (int i = 0; i < n; ++i) {
		RenderViewOpenGL(s_renderInfo[i], s_frameBuffer,
		s_renderBuffers[i].OpenGL->colorBufferName, i);
		}

		// Send the rendered results to the screen
		if (!s_render->PresentRenderBuffers(s_renderBuffers, s_renderInfo)) {
		DebugLog("PresentRenderBuffers() returned false, maybe because "
		"it was asked to quit");
		}
		break;*/
	}
#endif // SUPPORT_OPENGL

	case OSVRSupportedRenderers::EmptyRenderer:
	default:
		break;
	}
}

// --------------------------------------------------------------------------
// UnityRenderEvent
// This will be called for GL.IssuePluginEvent script calls; eventID will
// be the integer passed to IssuePluginEvent.
/// @todo does this actually need to be exported? It seems like
/// GetRenderEventFunc returning it would be sufficient...
void UNITY_INTERFACE_API OnRenderEvent(int eventID) {
	// Unknown graphics device type? Do nothing.
	if (!s_deviceType) {
		return;
	}

	switch (eventID) {
		// Call the Render loop
	case kOsvrEventID_Render:
		DoRender();
		break;
	case kOsvrEventID_Shutdown:
		break;
	case kOsvrEventID_Update:
#if UNITY_WIN
		UpdateRenderInfo();
#endif
		break;
	case kOsvrEventID_ConstructBuffers:
		if (osvrUnityRenderer != nullptr)
		{
			osvrUnityRenderer->ConstructRenderBuffers();
		}
		// SetRoomRotationUsingHead();
		break;
	case kOsvrEventID_ClearRoomToWorldTransform:
		// ClearRoomToWorldTransform();
		break;
	default:
		break;
	}
}

// --------------------------------------------------------------------------
// GetRenderEventFunc, a function we export which is used to get a
// rendering event callback function.
UnityRenderingEvent UNITY_INTERFACE_API GetRenderEventFunc() {
	return &OnRenderEvent;
}
