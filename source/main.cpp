#include "precompiled.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

// Alan: For some reason if this gets included after anything else some
// compile time errors get thrown up todo with javascript internal typedefs
#include "scripting/ScriptingHost.h"

#include "sdl.h"
#include "ogl.h"
#include "detect.h"
#include "timer.h"
#include "input.h"
#include "lib.h"
#include "res/res.h"
#include "res/file.h"
#ifdef _M_IX86
#include "sysdep/ia32.h"
#endif

#include "ps/CConsole.h"

#include "Config.h"
#include "MapReader.h"
#include "Terrain.h"
#include "TextureManager.h"
#include "ObjectManager.h"
#include "SkeletonAnimManager.h"
#include "Renderer.h"
#include "Model.h"
#include "UnitManager.h"

#include "BaseEntityCollection.h"
#include "Entity.h"
#include "EntityHandles.h"
#include "EntityManager.h"
#include "PathfindEngine.h"
#include "XML.h"

#include "scripting/JSInterface_Entity.h"
#include "scripting/JSInterface_BaseEntity.h"
#include "scripting/JSInterface_Vector3D.h"

#include "ConfigDB.h"
#include "CLogger.h"


#ifndef NO_GUI
#include "gui/GUI.h"
#endif


CConsole* g_Console = 0;
extern bool conInputHandler(const SDL_Event& ev);



u32 game_ticks;

bool keys[SDLK_LAST];
bool mouseButtons[5];

// Globals
int g_xres, g_yres;
int g_bpp;
int g_freq;


// flag to disable extended GL extensions until fix found - specifically, crashes
// using VBOs on laptop Radeon cards
static bool g_NoGLVBO=false;
// flag to switch on shadows
static bool g_Shadows=false;
// flag to switch off pbuffers
static bool g_NoPBuffer=true;
// flag to switch on fixed frame timing (RC: I'm using this for profiling purposes)
static bool g_FixedFrameTiming=false;
static bool g_VSync = false;

static bool g_EntGraph = false;

static float g_Gamma = 1.0f;

// mapfile to load or null for no map (and to use default terrain)
static const char* g_MapFile=0;


static Handle font;


extern CCamera g_Camera;

extern void terr_init();
extern void terr_update(float time);
extern bool terr_handler(const SDL_Event& ev);

extern int allow_reload();
extern int dir_add_watch(const char* const dir, bool watch_subdirs);




void Testing (void)
{
	g_Console->InsertMessage("Testing Function Registration");
}




static int write_sys_info()
{
	get_gfx_info();

	struct utsname un;
	uname(&un);

	FILE* const f = fopen("../logs/system_info.txt", "w");
	if(!f)
		return -1;

	// .. OS
	fprintf(f, "%s %s (%s)\n", un.sysname, un.release, un.version);
	// .. CPU
	fprintf(f, "%s, %s", un.machine, cpu_type);
	if(cpus > 1)
		fprintf(f, " (x%d)", cpus);
	if(cpu_freq != 0.0f)
	{
		if(cpu_freq < 1e9)
			fprintf(f, ", %.2f MHz\n", cpu_freq*1e-6);
		else
			fprintf(f, ", %.2f GHz\n", cpu_freq*1e-9);
	}
	else
		fprintf(f, "\n");
	// .. memory
	fprintf(f, "%lu MB RAM; %lu MB free\n", tot_mem/MB, avl_mem/MB);
	// .. graphics card
	fprintf(f, "%s\n", gfx_card);
	fprintf(f, "%s\n", gfx_drv_ver);
	fprintf(f, "%dx%d:%d@%d\n", g_xres, g_yres, g_bpp, g_freq);
	// .. network name / ips
	char hostname[100];	// possibly nodename != hostname
	gethostname(hostname, sizeof(hostname));
	fprintf(f, "%s\n", hostname);
	hostent* host = gethostbyname(hostname);
	if(host)
	{
		struct in_addr** ips = (struct in_addr**)host->h_addr_list;
		for(int i = 0; ips && ips[i]; i++)
			fprintf(f, "%s ", inet_ntoa(*ips[i]));
		fprintf(f, "\n");
	}

	fclose(f);
	return 0;
}


// error before GUI is initialized: display message, and quit
// TODO: localization
static void display_startup_error(const wchar_t* msg)
{
	const wchar_t* caption = L"0ad startup problem";

	write_sys_info();
	wdisplay_msg(caption, msg);
	exit(1);
}

// error before GUI is initialized: display message, and quit
// TODO: localization
static void display_startup_error(const char* msg)
{
	const char* caption = "0ad startup problem";

	write_sys_info();
	display_msg(caption, msg);
	exit(1);
}


static int set_vmode(int w, int h, int bpp)
{
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

	if(!SDL_SetVideoMode(w, h, bpp, SDL_OPENGL|SDL_FULLSCREEN))
		return -1;

	glViewport(0, 0, w, h);

#ifndef NO_GUI
	g_GUI.UpdateResolution();
#endif

	oglInit();	// required after each mode change

	return 0;
}


// break out of main loop
static bool quit = false;

static bool handler(const SDL_Event& ev)
{
	int c;

	switch(ev.type)
	{
	case SDL_KEYDOWN:
		c = ev.key.keysym.sym;
		keys[c] = true;

		switch(c)
		{
		case SDLK_ESCAPE:
			quit = true;
			break;
		}
		break;

	case SDL_KEYUP:
		c = ev.key.keysym.sym;
		keys[c] = false;
		break;
	case SDL_MOUSEBUTTONDOWN:
		c = ev.button.button;
		if( c < 5 )
			mouseButtons[c] = true;
		break;
	case SDL_MOUSEBUTTONUP:
		c = ev.button.button;
		if( c < 5 )
			mouseButtons[c] = false;
		break;
	}

	return 0;
}


//////////////////////////////////////////////////////////////////////////////////////////////////
// RenderTerrain: iterate through all terrain patches and submit all patches in viewing frustum to
// the renderer
void RenderTerrain()
{
	CFrustum frustum=g_Camera.GetFustum();
	u32 patchesPerSide=g_Terrain.GetPatchesPerSide();
	for (uint j=0; j<patchesPerSide; j++) {
		for (uint i=0; i<patchesPerSide; i++) {
			CPatch* patch=g_Terrain.GetPatch(i,j);
			if (frustum.IsBoxVisible (CVector3D(0,0,0),patch->GetBounds())) {
				g_Renderer.Submit(patch);
			}
		}
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// SubmitModelRecursive: recurse down given model, submitting it and all its descendents to the
// renderer
void SubmitModelRecursive(CModel* model)
{
	g_Renderer.Submit(model);

	const std::vector<CModel::Prop>& props=model->GetProps();
	for (uint i=0;i<props.size();i++) {
		SubmitModelRecursive(props[i].m_Model);
	}
}

//////////////////////////////////////////////////////////////////////////////////////////////////
// RenderModels: iterate through model list and submit all models in viewing frustum to the
// Renderer
void RenderModels()
{
	CFrustum frustum=g_Camera.GetFustum();

	const std::vector<CUnit*>& units=g_UnitMan.GetUnits();
	for (uint i=0;i<units.size();++i) {
		if (frustum.IsBoxVisible(CVector3D(0,0,0),units[i]->GetModel()->GetBounds())) {
			SubmitModelRecursive(units[i]->GetModel());
		}
	}
}

/////////////////////////////////////////////////////////////////////////////////////////////
// RenderNoCull: render absolutely everything to a blank frame to force renderer
// to load required assets
void RenderNoCull()
{
	g_Renderer.BeginFrame();
	g_Renderer.SetCamera(g_Camera);

	uint i,j;
	const std::vector<CUnit*>& units=g_UnitMan.GetUnits();
	for (i=0;i<units.size();++i) {
		SubmitModelRecursive(units[i]->GetModel());
	}

	u32 patchesPerSide=g_Terrain.GetPatchesPerSide();
	for (j=0; j<patchesPerSide; j++) {
		for (i=0; i<patchesPerSide; i++) {
			CPatch* patch=g_Terrain.GetPatch(i,j);
			g_Renderer.Submit(patch);
		}
	}

	g_Renderer.FlushFrame();
	glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
	g_Renderer.EndFrame();
}


static void Render()
{
	// start new frame
	g_Renderer.BeginFrame();
	g_Renderer.SetCamera(g_Camera);

	// switch on wireframe for terrain if we want it
	g_Renderer.SetTerrainRenderMode( SOLID );

	RenderTerrain();
	RenderModels();
	g_Renderer.FlushFrame();

	if( g_EntGraph )
	{
		glPushAttrib( GL_ENABLE_BIT );
		glDisable( GL_LIGHTING );
		glDisable( GL_TEXTURE_2D );
		glDisable( GL_DEPTH_TEST );
		glColor3f( 1.0f, 0.0f, 1.0f );

		g_EntityManager.renderAll(); // <-- collision outlines, pathing routes

		glPopAttrib();
	}

	// overlay mode
	glPushAttrib(GL_ENABLE_BIT);
	glDisable(GL_CULL_FACE);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_BLEND);
	glAlphaFunc(GL_GREATER, 0.5);
	glEnable(GL_ALPHA_TEST);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();

	glOrtho(0.f, (float)g_xres, 0.f, (float)g_yres, -1.f, 1000.f);

	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();

	// FPS counter
	glLoadIdentity();
	glTranslatef(10, 30, 0);
	font_bind(font);
	glprintf("%d FPS", fps);

	// view params
	glLoadIdentity();
	glTranslatef(10, 90, 0);
	extern float g_CameraZoom;
	glprintf("zoom=%.1f", g_CameraZoom);

#ifndef NO_GUI
	// Temp GUI message GeeTODO
	glLoadIdentity();
	glTranslatef(10, 60, 0);
	glprintf("%s", g_GUI.TEMPmessage.c_str());

	glLoadIdentity();
	g_GUI.Draw();
#endif


	g_Console->Render();

	// restore
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glPopAttrib();

	g_Renderer.EndFrame();
}


static void do_tick()
{
}

//////////////////////////////////////////////////////////////////////////////////////////////
// UpdateWorld: update time dependent data in the world to account for changes over
// the given time (in s)
void UpdateWorld(float time)
{
	const std::vector<CUnit*>& units=g_UnitMan.GetUnits();
	for (uint i=0;i<units.size();++i) {
		units[i]->GetModel()->Update(time);
	}

	g_EntityManager.updateAll( time );

}


void ParseArgs(int argc, char* argv[])
{
	for (int i=1;i<argc;i++) {
		if (argv[i][0]=='-') {
			switch (argv[i][1]) {
				case 'm':
					if (argv[i][2]=='=') {
						g_MapFile=argv[i]+3;
					}
					break;
				case 'g':
					if( argv[i][2] == '=' )
					{
						g_Gamma = (float)atof( argv[i] + 3 );
						if( g_Gamma == 0.0f ) g_Gamma = 1.0f;
					}
					break;
				case 'e':
					g_EntGraph = true; break;
				case 'v':
					g_VSync = true;
					break;
				case 'n':
					if (strncmp(argv[i]+1,"novbo",5)==0) {
						g_NoGLVBO=true;
					}
					else if (strncmp(argv[i]+1,"nopbuffer",9)==0) {
						g_NoPBuffer=true;
					}
					break;

				case 'f':
					if (strncmp(argv[i]+1,"fixedframe",10)==0) {
						g_FixedFrameTiming=true;
					}
					break;

				case 's':
					if (strncmp(argv[i]+1,"shadows",7)==0) {
						g_Shadows=true;
					}
					break;
				case 'x':
					if (strncmp(argv[i], "-xres=", 6)==0) {
						g_ConfigDB.CreateValue(CFG_SYSTEM, "xres")
									->m_String=argv[i]+6;
					}
					break;
				case 'y':
					if (strncmp(argv[i], "-yres=", 6)==0) {
						g_ConfigDB.CreateValue(CFG_SYSTEM, "yres")
									->m_String=argv[i]+6;
					}
					break;
				case 'c':
					if (strcmp(argv[i], "-conf") == 0) {
						if (argc-i >= 1) // At least one arg left
						{
							i++;
							char *arg=argv[i];
							char *equ=strchr(arg, '=');
							if (equ)
							{
								*equ=0;
								g_ConfigDB.CreateValue(CFG_SYSTEM, arg)
									->m_String=(equ+1);
							}
						}
					}
					break;
			}
		}
	}
	
	CConfigValue *val;
	if (val=g_ConfigDB.GetValue(CFG_SYSTEM, "xres"))
		val->GetInt(g_xres);
	if (val=g_ConfigDB.GetValue(CFG_SYSTEM, "yres"))
		val->GetInt(g_yres);
		
	LOG(NORMAL, "g_x/yres is %dx%d\n", g_xres, g_yres);
}


extern u64 PREVTSC;

int main(int argc, char* argv[])
{
#ifdef _MSC_VER
u64 TSC=rdtsc();
debug_out(
"----------------------------------------\n"\
"MAIN (elapsed = %f ms)\n"\
"----------------------------------------\n", (TSC-PREVTSC)/2e9*1e3);
PREVTSC=TSC;
#endif

	const int ERR_MSG_SIZE = 1000;
	wchar_t err_msg[ERR_MSG_SIZE];

	lib_init();

	// set 24 bit (float) FPU precision for faster divides / sqrts
#ifdef _M_IX86
	_control87(_PC_24, _MCW_PC);
#endif


	detect();

	// init SDL
	if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER|SDL_INIT_NOPARACHUTE) < 0)
	{
		swprintf(err_msg, ERR_MSG_SIZE, L"SDL library initialization failed: %s\n", SDL_GetError());
		display_startup_error(err_msg);
	}
	atexit(SDL_Quit);
	SDL_EnableUNICODE(1);

	// preferred video mode = current desktop settings
	// (command line params may override these)
	get_cur_vmode(&g_xres, &g_yres, &g_bpp, &g_freq);

	// set current directory to "$game_dir/data".
	// this is necessary because it is otherwise unknown,
	// especially if run from a shortcut / symlink.
	//
	// "../data" is relative to the executable (in "$game_dir/system").
	//
	// rationale for data/ being root: untrusted scripts must not be
	// allowed to overwrite critical game (or worse, OS) files.
	// the VFS prevents any accesses to files above this directory.
	int err = file_rel_chdir(argv[0], "../data");
	if(err < 0)
	{
		swprintf(err_msg, ERR_MSG_SIZE, L"error setting current directory.\n"\
			L"argv[0] is probably incorrect. please start the game via command-line.");
		display_startup_error(err_msg);
	}
	
	new CConfigDB;
	g_ConfigDB.SetConfigFile(CFG_SYSTEM, false, "config/system.cfg");
	g_ConfigDB.Reload(CFG_SYSTEM);
	
	g_ConfigDB.SetConfigFile(CFG_MOD, true, "config/mod.cfg");
	// No point in reloading mod.cfg here - we haven't mounted mods yet
	
	g_ConfigDB.SetConfigFile(CFG_USER, true, "config/user.cfg");
	// Same thing here; we haven't even started up yet - this will wait until
	// the profile dir is VFS mounted (or we will do a new SetConfigFile with
	// a generated profile path)
	
	ParseArgs(argc, argv);

	// Create the scripting host.  This needs to be done before the GUI is created.
	new ScriptingHost;

	// GUI is notified in set_vmode, so this must come before that.
#ifndef NO_GUI
	new CGUI;
#endif

	if(set_vmode(g_xres, g_yres, 32) < 0)
	{
		swprintf(err_msg, ERR_MSG_SIZE, L"could not set %dx%d graphics mode: %s\n", g_xres, g_yres, SDL_GetError());
		display_startup_error(err_msg);
	}

	write_sys_info();

	if(!oglExtAvail("GL_ARB_multitexture") || !oglExtAvail("GL_ARB_texture_env_combine"))
		display_startup_error(L"required ARB_multitexture or ARB_texture_env_combine extension not available");

	// enable/disable VSync
	// note: "GL_EXT_SWAP_CONTROL" is "historical" according to dox.
	if(oglExtAvail("WGL_EXT_swap_control"))
		wglSwapIntervalEXT(g_VSync? 1 : 0);


	if(SDL_SetGamma(g_Gamma, g_Gamma, g_Gamma) < 0)
	{
		debug_warn("SDL_SetGamma failed");
	}

	// start up Xerces - only needs to be done once (unless locale changes mid-game, for
	// some reason), not on every XML file load; multiple initialization calls are ok, though,
	// provided there are a matching number of XMLPlatformUtils::Terminate calls
	XMLPlatformUtils::Initialize();


	vfs_mount("", "mods/official/", 0);

#ifdef _MSC_VER
u64 CURTSC=rdtsc();
debug_out(
"----------------------------------------\n"\
"VFS ready (elapsed = %f ms)\n"\
"----------------------------------------\n", (CURTSC-PREVTSC)/2e9*1e3);
PREVTSC=CURTSC;
#endif


#ifndef NO_GUI
	// GUI uses VFS, so this must come after VFS init.
	g_GUI.Initialize();
	g_GUI.LoadXMLFile("gui/styles.xml");
	g_GUI.LoadXMLFile("gui/hello.xml");
	g_GUI.LoadXMLFile("gui/sprite1.xml");
#endif


	font = font_load("fonts/verdana.fnt");

	g_Console = new CConsole(0, g_yres-600.f, (float)g_xres, 600.f);

	// create renderer
	new CRenderer;

	// set renderer options from command line options - NOVBO must be set before opening the renderer
	g_Renderer.SetOptionBool(CRenderer::OPT_NOVBO,g_NoGLVBO);
	g_Renderer.SetOptionBool(CRenderer::OPT_SHADOWS,g_Shadows);
	g_Renderer.SetOptionBool(CRenderer::OPT_NOPBUFFER,g_NoPBuffer);

	// create terrain related stuff
	new CTextureManager;

	// create actor related stuff
	new CSkeletonAnimManager;
	new CObjectManager;
	new CUnitManager;

	g_Renderer.Open(g_xres,g_yres,g_bpp);

	// terr_init loads a bunch of resources as well as setting up the terrain
	terr_init();


	// This needs to be done after the renderer has loaded all its actors...
	new CBaseEntityCollection;
	new CEntityManager;
	new CPathfindEngine;

	g_EntityTemplateCollection.loadTemplates();

	// Register the JavaScript interfaces with the runtime
	JSI_Entity::init();
	JSI_BaseEntity::init();
	JSI_Vector3D::init();

// if no map name specified, load test01.pmp (for convenience during
// development. that means loading no map at all is currently impossible.
// is that a problem?
if(!g_MapFile)
	g_MapFile = "test01.pmp";


	// load a map if we were given one
	if (g_MapFile) {
		CStr mapfilename("mods/official/maps/scenarios/");
		mapfilename+=g_MapFile;
		try {
			CMapReader reader;
			reader.LoadMap(mapfilename);
		} catch (...) {
			char errmsg[256];
			sprintf(errmsg, "Failed to load map %s\n", mapfilename.c_str());
			display_startup_error(errmsg);
		}
	}

	// Initialize entities

	CMessage init_msg (CMessage::EMSG_INIT);
	g_EntityManager.dispatchAll(&init_msg);

#ifndef NO_GUI
	in_add_handler(gui_handler);
#endif
	in_add_handler(handler);
	in_add_handler(terr_handler);

	in_add_handler(conInputHandler);

	// render everything to a blank frame to force renderer to load everything
	RenderNoCull();

	size_t frameCount=0;
	if (g_FixedFrameTiming) {
#if 0		// TOPDOWN
		g_Camera.SetProjection(1.0f,10000.0f,DEGTORAD(90));
		g_Camera.m_Orientation.SetIdentity();
		g_Camera.m_Orientation.RotateX(DEGTORAD(90));
		g_Camera.m_Orientation.Translate(CELL_SIZE*250*0.5, 250, CELL_SIZE*250*0.5);
#else		// std view
		g_Camera.SetProjection(1.0f,10000.0f,DEGTORAD(20));
		g_Camera.m_Orientation.SetXRotation(DEGTORAD(30));
		g_Camera.m_Orientation.RotateY(DEGTORAD(-45));
		g_Camera.m_Orientation.Translate(350, 350, -275);
#endif
		g_Camera.UpdateFrustum();
	}

g_Console->RegisterFunc(Testing, "Testing");

#ifdef _MSC_VER
{
u64 CURTSC=rdtsc();
debug_out(
"----------------------------------------\n"\
"READY (elapsed = %f ms)\n"\
"----------------------------------------\n", (CURTSC-PREVTSC)/2e9*1e3);
PREVTSC=CURTSC;
}
#endif


// fixed timestep main loop
	const double TICK_TIME = 30e-3;	// [s]
	double time0 = get_time();

	while(!quit)
	{
		res_reload_changed_files();


// TODO: limiter in case simulation can't keep up?
#if 0
		double time1 = get_time();
		while((time1-time0) > TICK_TIME)
		{
			game_ticks++;
			in_get_events();
			do_tick();
			time0 += TICK_TIME;
		}
		UpdateWorld(float(time1-time0));
		terr_update(float(time1-time0));
		Render();
		SDL_GL_SwapBuffers();

		calc_fps();

#else

		double time1 = get_time();

		mouseButtons[SDL_BUTTON_WHEELUP] = false;
		mouseButtons[SDL_BUTTON_WHEELDOWN] = false;

		float TimeSinceLastFrame = (float)(time1-time0);
		in_get_events();
		UpdateWorld(TimeSinceLastFrame);
		if (!g_FixedFrameTiming) terr_update(float(TimeSinceLastFrame));
		g_Console->Update(TimeSinceLastFrame);
		Render();
		SDL_GL_SwapBuffers();

		calc_fps();
		time0=time1;
		frameCount++;
		if (g_FixedFrameTiming && frameCount==100) quit=true;
#endif
	}

#ifndef NO_GUI
	g_GUI.Destroy();
	delete &g_GUI;
#endif

	delete g_Console;
//	delete &g_ScriptingHost;
	delete &g_Pathfinder;
	delete &g_EntityManager;
	delete &g_EntityTemplateCollection;

	// destroy actor related stuff
	delete &g_UnitMan;
	delete &g_ObjMan;
	delete &g_SkelAnimMan;

	// destroy terrain related stuff
	delete &g_TexMan;

	// destroy renderer
	delete &g_Renderer;

	delete &g_ConfigDB;

	// close down Xerces
	XMLPlatformUtils::Terminate();

	exit(0);
	return 0;
}
