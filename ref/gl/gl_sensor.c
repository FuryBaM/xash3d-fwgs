#include "gl_sensor.h"
#include "gl_local.h"
#include "xash3d_mathlib.h"
#include "library.h"
#include "beamdef.h"
#include "particledef.h"
#include "entity_types.h"
#include <stdlib.h>
#include <string.h>

// ---------- Config ----------
static int   g_sensor_w = 320;
static int   g_sensor_h = 240;
static int   g_sensor_ready = 0;

// 0 = try R8/RED, 1 = force LUMINANCE8/LUMINANCE
static int   g_sensor_force_luma = 0;

// ---------- GL objects ----------
static GLuint g_sensor_fbo = 0;
static GLuint g_sensor_mask_tex = 0;
static GLuint g_sensor_depth_rb = 0;

// ---------- CPU buffers ----------
static byte* g_sensor_mask_cpu = NULL;   // w*h bytes (class id per pixel)
static float* g_sensor_depth_cpu = NULL;  // w*h floats (raw depth 0..1)

// ---------- Optional: simple flags you can set from outside ----------
static int g_sensor_enabled = 0;

// ============================================================
// Helpers
// ============================================================

typedef void (APIENTRY* PFNGLGENFRAMEBUFFERSPROC)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLBINDFRAMEBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRY* PFNGLDELETEFRAMEBUFFERSPROC)(GLsizei, const GLuint*);
typedef GLenum(APIENTRY* PFNGLCHECKFRAMEBUFFERSTATUSPROC)(GLenum);
typedef void (APIENTRY* PFNGLFRAMEBUFFERTEXTURE2DPROC)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef void (APIENTRY* PFNGLFRAMEBUFFERRENDERBUFFERPROC)(GLenum, GLenum, GLenum, GLuint);

typedef void (APIENTRY* PFNGLGENRENDERBUFFERSPROC)(GLsizei, GLuint*);
typedef void (APIENTRY* PFNGLBINDRENDERBUFFERPROC)(GLenum, GLuint);
typedef void (APIENTRY* PFNGLDELETERENDERBUFFERSPROC)(GLsizei, const GLuint*);
typedef void (APIENTRY* PFNGLRENDERBUFFERSTORAGEPROC)(GLenum, GLenum, GLsizei, GLsizei);

static PFNGLGENFRAMEBUFFERSPROC            qglGenFramebuffers = NULL;
static PFNGLBINDFRAMEBUFFERPROC            qglBindFramebuffer = NULL;
static PFNGLDELETEFRAMEBUFFERSPROC         qglDeleteFramebuffers = NULL;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC     qglCheckFramebufferStatus = NULL;
static PFNGLFRAMEBUFFERTEXTURE2DPROC       qglFramebufferTexture2D = NULL;
static PFNGLFRAMEBUFFERRENDERBUFFERPROC    qglFramebufferRenderbuffer = NULL;

static PFNGLGENRENDERBUFFERSPROC           qglGenRenderbuffers = NULL;
static PFNGLBINDRENDERBUFFERPROC           qglBindRenderbuffer = NULL;
static PFNGLDELETERENDERBUFFERSPROC        qglDeleteRenderbuffers = NULL;
static PFNGLRENDERBUFFERSTORAGEPROC        qglRenderbufferStorage = NULL;

static void* R_Sensor_GetProc(const char* base)
{
	void* p = gEngfuncs.GL_GetProcAddress(base);
	if (p) return p;

	// fallback: EXT/ARB suffixes (как у них в GL_CheckExtension HACK)
	{
		char name[128];
		Q_snprintf(name, sizeof(name), "%sEXT", base);
		p = gEngfuncs.GL_GetProcAddress(name);
		if (p) return p;

		Q_snprintf(name, sizeof(name), "%sARB", base);
		p = gEngfuncs.GL_GetProcAddress(name);
		if (p) return p;
	}

	return NULL;
}

static qboolean R_Sensor_LoadFBO(void)
{
	if (qglGenFramebuffers) return true; // уже загружено

	qglGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)R_Sensor_GetProc("glGenFramebuffers");
	qglBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)R_Sensor_GetProc("glBindFramebuffer");
	qglDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)R_Sensor_GetProc("glDeleteFramebuffers");
	qglCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)R_Sensor_GetProc("glCheckFramebufferStatus");
	qglFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)R_Sensor_GetProc("glFramebufferTexture2D");
	qglFramebufferRenderbuffer = (PFNGLFRAMEBUFFERRENDERBUFFERPROC)R_Sensor_GetProc("glFramebufferRenderbuffer");

	qglGenRenderbuffers = (PFNGLGENRENDERBUFFERSPROC)R_Sensor_GetProc("glGenRenderbuffers");
	qglBindRenderbuffer = (PFNGLBINDRENDERBUFFERPROC)R_Sensor_GetProc("glBindRenderbuffer");
	qglDeleteRenderbuffers = (PFNGLDELETERENDERBUFFERSPROC)R_Sensor_GetProc("glDeleteRenderbuffers");
	qglRenderbufferStorage = (PFNGLRENDERBUFFERSTORAGEPROC)R_Sensor_GetProc("glRenderbufferStorage");

	return qglGenFramebuffers && qglBindFramebuffer && qglCheckFramebufferStatus &&
		qglFramebufferTexture2D && qglFramebufferRenderbuffer &&
		qglGenRenderbuffers && qglBindRenderbuffer && qglRenderbufferStorage;
}

static void R_Sensor_FreeCPU(void)
{
	free(g_sensor_mask_cpu);  g_sensor_mask_cpu = NULL;
	free(g_sensor_depth_cpu); g_sensor_depth_cpu = NULL;
}

static void R_Sensor_AllocCPU(void)
{
	size_t maskSize = (size_t)g_sensor_w * (size_t)g_sensor_h;
	size_t depthSize = maskSize * sizeof(float);

	if (!g_sensor_mask_cpu)
		g_sensor_mask_cpu = (unsigned char*)malloc(maskSize);

	if (!g_sensor_depth_cpu)
		g_sensor_depth_cpu = (float*)malloc(depthSize);

	if (!g_sensor_mask_cpu || !g_sensor_depth_cpu)
		gEngfuncs.Host_Error("Sensor: out of memory\n");

	memset(g_sensor_mask_cpu, 0, maskSize);
	memset(g_sensor_depth_cpu, 0, depthSize);
}

static void R_Sensor_DestroyGL(void)
{
	if (g_sensor_fbo)
	{
		qglBindFramebuffer(GL_FRAMEBUFFER, 0);

		if (g_sensor_depth_rb)
		{
			qglDeleteRenderbuffers(1, &g_sensor_depth_rb);
			g_sensor_depth_rb = 0;
		}

		if (g_sensor_mask_tex)
		{
			pglDeleteTextures(1, &g_sensor_mask_tex);
			g_sensor_mask_tex = 0;
		}

		qglDeleteFramebuffers(1, &g_sensor_fbo);
		g_sensor_fbo = 0;
	}

	g_sensor_ready = 0;
}

static void R_Sensor_CreateGL(void)
{
	// Must have required functions
	if (!R_Sensor_LoadFBO())
	{
		gEngfuncs.Con_Printf(S_ERROR "SensorFBO: FBO funcs not available (not loaded)\n");
		g_sensor_ready = 0;
		g_sensor_enabled = 0;
		return;
	}

	// Clean old
	R_Sensor_DestroyGL();

	// Create FBO
	qglGenFramebuffers(1, &g_sensor_fbo);
	qglBindFramebuffer(GL_FRAMEBUFFER, g_sensor_fbo);

	// Create mask texture
	pglGenTextures(1, &g_sensor_mask_tex);
	pglBindTexture(GL_TEXTURE_2D, g_sensor_mask_tex);

	// Choose format
	GLint internalFormat;
	GLenum format;

	if (g_sensor_force_luma)
	{
		internalFormat = GL_LUMINANCE8;
		format = GL_LUMINANCE;
	}
	else
	{
		// Prefer R8/RED when available
		internalFormat = GL_R8;
		format = GL_RED;
	}

	pglTexImage2D(GL_TEXTURE_2D, 0, internalFormat, g_sensor_w, g_sensor_h, 0, format, GL_UNSIGNED_BYTE, NULL);
	pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	pglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	qglFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g_sensor_mask_tex, 0);

	// Create depth renderbuffer
	qglGenRenderbuffers(1, &g_sensor_depth_rb);
	qglBindRenderbuffer(GL_RENDERBUFFER, g_sensor_depth_rb);
	qglRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, g_sensor_w, g_sensor_h);
	qglFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, g_sensor_depth_rb);

	// Validate
	{
		GLenum status = qglCheckFramebufferStatus(GL_FRAMEBUFFER);
		if (status != GL_FRAMEBUFFER_COMPLETE)
		{
			// If R8 failed on old GL, retry as LUMINANCE8
			if (!g_sensor_force_luma)
			{
				g_sensor_force_luma = 1;
				qglBindFramebuffer(GL_FRAMEBUFFER, 0);
				R_Sensor_DestroyGL();
				R_Sensor_CreateGL();
				return;
			}

			gEngfuncs.Host_Error("SensorFBO incomplete: 0x%X\n", (uint)status);
			qglBindFramebuffer(GL_FRAMEBUFFER, 0);
			return;
		}
	}

	// Restore
	qglBindFramebuffer(GL_FRAMEBUFFER, 0);
	g_sensor_ready = 1;
}


// ============================================================
// Public API
// ============================================================

// Call once after GL init / extensions loaded
void R_Sensor_Init(void)
{
	R_Sensor_AllocCPU();
	R_Sensor_CreateGL();
	gEngfuncs.Con_Printf(
		"Sensor: FBO=%u maskTex=%u depthRB=%u\n",
		g_sensor_fbo, g_sensor_mask_tex, g_sensor_depth_rb
	);
}

// Call on shutdown / vid_restart
void R_Sensor_Shutdown(void)
{
	R_Sensor_DestroyGL();
	R_Sensor_FreeCPU();
}

// Optional: enable/disable capture
void R_Sensor_SetEnabled(int enabled)
{
	g_sensor_enabled = enabled ? 1 : 0;
}

// Get pointers to last captured frame (valid after capture)
const byte* R_Sensor_GetMask(int* w, int* h)
{
	if (w) *w = g_sensor_w;
	if (h) *h = g_sensor_h;
	return g_sensor_mask_cpu;
}

const float* R_Sensor_GetDepth(int* w, int* h)
{
	if (w) *w = g_sensor_w;
	if (h) *h = g_sensor_h;
	return g_sensor_depth_cpu;
}


// ============================================================
// Sensor drawing
// ============================================================
//
// Mask policy (you can change):
// 0 = background
// 1 = world
// 2 = brush entity
// 3 = studio (players/NPC)
// 4 = alias
// 5 = sprite
//
// For now we just paint constant color per model type.
// Later you can map per-entity class, material (glass), etc.
//

static void R_Sensor_SetClassColor(byte id)
{
	// Fixed pipeline: set constant vertex color.
	// If you are using shaders elsewhere, ensure no shader is bound here.
	// We intentionally draw with nearest "flat" behavior.
	const float c = (float)id / 255.0f;
	pglColor4f(c, c, c, 1.0f);
}

static void R_Sensor_DrawEntities_NoViewmodel_NoEFX(void)
{
	int i;

	// solid entities (no sprites here)
	for (i = 0; i < tr.draw_list->num_solid_entities && !RI.onlyClientDraw; i++)
	{
		RI.currententity = tr.draw_list->solid_entities[i];
		RI.currentmodel = RI.currententity->model;

		if (!RI.currententity || !RI.currentmodel) continue;

		switch (RI.currentmodel->type)
		{
		case mod_brush:
			R_Sensor_SetClassColor(2);
			R_DrawBrushModel(RI.currententity);
			break;
		case mod_studio:
			R_Sensor_SetClassColor(3);
			R_DrawStudioModel(RI.currententity);
			break;
		case mod_alias:
			R_Sensor_SetClassColor(4);
			R_DrawAliasModel(RI.currententity);
			break;
		case mod_sprite:
			// optional: ignore sprites for sensor
			// R_Sensor_SetClassColor(5); R_DrawSpriteModel(...)
			break;
		default:
			break;
		}
	}

	// translucent entities: optional.
	// For a first sensor pass, many people skip them.
	for (i = 0; i < tr.draw_list->num_trans_entities && !RI.onlyClientDraw; i++)
	{
		RI.currententity = tr.draw_list->trans_entities[i];
		RI.currentmodel = RI.currententity->model;

		if (!RI.currententity || !RI.currentmodel) continue;

		switch (RI.currentmodel->type)
		{
		case mod_brush:
			R_Sensor_SetClassColor(2);
			R_DrawBrushModel(RI.currententity);
			break;
		case mod_studio:
			R_Sensor_SetClassColor(3);
			R_DrawStudioModel(RI.currententity);
			break;
		case mod_alias:
			R_Sensor_SetClassColor(4);
			R_DrawAliasModel(RI.currententity);
			break;
		case mod_sprite:
			break;
		default:
			break;
		}
	}
}


// Call once per frame AFTER main lists are ready.
// Recommended place: end of R_RenderScene (before R_EndGL) or right after R_RenderScene in R_RenderFrame.
void R_Sensor_CaptureFrame(void)
{
	GLint oldFBO = 0;
	GLint oldViewport[4];

	if (!g_sensor_enabled) return;
	if (!g_sensor_ready) return;

	// Only normal pass and only when world is drawn
	if (!RP_NORMALPASS()) return;
	if (!RI.drawWorld) return;
	if (RI.onlyClientDraw) return;
	if (tr.fCustomRendering) return; // don't capture if rendering is overridden

	// Save state
	pglGetIntegerv(GL_FRAMEBUFFER_BINDING, &oldFBO);
	pglGetIntegerv(GL_VIEWPORT, oldViewport);

	// Bind sensor FBO
	qglBindFramebuffer(GL_FRAMEBUFFER, g_sensor_fbo);
	pglViewport(0, 0, g_sensor_w, g_sensor_h);

	// Minimal state
	pglDisable(GL_BLEND);
	pglDisable(GL_FOG);
	pglDisable(GL_ALPHA_TEST);
	pglEnable(GL_DEPTH_TEST);
	pglDepthMask(GL_TRUE);

	// Force "flat" rendering path: no textures
	pglDisable(GL_TEXTURE_2D);

	// Clear
	pglClearColor(0, 0, 0, 0);
	pglClearDepth(1.0);
	pglClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	// Draw world
	R_Sensor_SetClassColor(1);
	R_DrawWorld();

	// Draw entities (no viewmodel/EFX)
	R_Sensor_DrawEntities_NoViewmodel_NoEFX();

	// Readback
	pglPixelStorei(GL_PACK_ALIGNMENT, 1);

	if (g_sensor_force_luma)
	{
		pglReadPixels(0, 0, g_sensor_w, g_sensor_h, GL_LUMINANCE, GL_UNSIGNED_BYTE, g_sensor_mask_cpu);
	}
	else
	{
		pglReadPixels(0, 0, g_sensor_w, g_sensor_h, GL_RED, GL_UNSIGNED_BYTE, g_sensor_mask_cpu);
	}

	pglReadPixels(0, 0, g_sensor_w, g_sensor_h, GL_DEPTH_COMPONENT, GL_FLOAT, g_sensor_depth_cpu);

	// Restore
	qglBindFramebuffer(GL_FRAMEBUFFER, (GLuint)oldFBO);
	pglViewport(oldViewport[0], oldViewport[1], oldViewport[2], oldViewport[3]);

	// Re-enable texture 2D in case main renderer expects it
	pglEnable(GL_TEXTURE_2D);
}
