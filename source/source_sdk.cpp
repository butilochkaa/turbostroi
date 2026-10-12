#include "source_sdk.h"

#include "gmsv_turbostroi.h"
#include "filesystem_stl.h"
#include "affinity.h"
#include "shared_print.h"
#include "wagon.h"
#include "lib_turbostroi_lua.h"
#include <dbg.h>
#include <color.h>
#include <utlbuffer.h>
#include <GarrysMod/FactoryLoader.hpp>
#include <edict.h>
#include <globalvars_base.h>
#include <convar.h>
#include <keyvalues.h>
#include <game/server/iplayerinfo.h>

namespace GM = GarrysMod::Lua;

//------------------------------------------------------------------------------
// файловая система stl для чтения и записи turbostroi.cfg
//------------------------------------------------------------------------------
CFileSystem_STL g_FileSystemSTL;

//------------------------------------------------------------------------------
// консольные переменные (квары) и команды
//------------------------------------------------------------------------------
ConVar g_CVarDisableCache("turbostroi_disable_cache", "0", FCVAR_NONE, "Disable scripts cache for development");
ConVar g_CVarMainCores("turbostroi_main_cores", "0", FCVAR_NONE, "Set affinity mask/cores for main thread (e.g. '0-1', '0', 'all')", CVarMainCoresCallback);
ConVar g_CVarTrainCores("turbostroi_train_cores", "0", FCVAR_NONE, "Set affinity mask/cores for train threads (e.g. '2-15', 'all')", CVarTrainCoresCallback);
ConVar g_CVarTrainSpread("turbostroi_train_spread", "1", FCVAR_NONE, "Spread train simulation threads across train cores via round-robin pinning (1=spread, 0=shared mask)", CVarTrainSpreadCallback);
ConVar g_CVarUnpackLib("turbostroi_unpack_lua", "1", FCVAR_NONE, "Enable/disable unpacking lib_turbostroi_v2.lua from DLL");
ConCommand g_CmdClearCache("turbostroi_clear_cache", ClearLoadCache, "Clear cache for reload systems");
ConCommand g_CmdClearPrint("turbostroi_clear_print", ClearPrintQueue, "Clear print queue");
ConCommand g_CmdDisableRunString("metrostroi_turbostroi_run_disable", RunStringDisable, "Disable 'metrostroi_turbostroi_run'");

//------------------------------------------------------------------------------
// колбэк для смены ядер сервера: turbostroi_main_cores
//------------------------------------------------------------------------------
void CVarMainCoresCallback(IConVar* var, const char* pOldValue, float flOldValue)
{
	ConVarRef ref(var);

	if (!SetThreadGroup(g_MainThreadGroupAffinity, g_MainCoresList, ref.GetString()))
		return;

	ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Main thread cores set to: %s\n", CoresListToString(g_MainCoresList).c_str());
	if (!SetAffinityMask(CurrentThread(), g_MainThreadGroupAffinity))
	{
		ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Set main thread affinity mask failed!\n");
	}
	else
	{
		ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Main thread running on CPU%d\n", CurrentCPU());
	}
}

//------------------------------------------------------------------------------
// колбэк для смены ядер поездов: turbostroi_train_cores
//------------------------------------------------------------------------------
void CVarTrainCoresCallback(IConVar* var, const char* pOldValue, float flOldValue)
{
	ConVarRef ref(var);
	if (!SetThreadGroup(g_SimThreadGroupAffinity, g_SimCoresList, ref.GetString()))
		return;

	ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Train cores set to: %s\n", CoresListToString(g_SimCoresList).c_str());
	// на лету пересаживаем все существующие вагоны на новые ядра
	CWagon::ReapplyAffinityAll();
}

//------------------------------------------------------------------------------
// колбэк для переключения режима распределения вагонов: turbostroi_train_spread
//------------------------------------------------------------------------------
void CVarTrainSpreadCallback(IConVar* var, const char* pOldValue, float flOldValue)
{
	ConVarRef ref(var);
	bool spread = ref.GetBool();
	ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Train thread spread mode: %s\n",
		spread ? "Round-Robin Pinning (each wagon gets its own dedicated core)" : "Shared Mask (OS scheduler distributes threads)");
	CWagon::ReapplyAffinityAll();
}

//------------------------------------------------------------------------------
// очистка кэша lua-скриптов
//------------------------------------------------------------------------------
void ClearLoadCache(const CCommand& command)
{
	auto cacheSize = g_LoadedFilesCache.size();
	if (g_LoadedFilesCache.empty())
		ConColorMsg(Color(255, 255, 0, 255), "Turbostroi: No files in cache. Nothing to clear.\n");
	else
	{
		g_LoadedFilesCache.clear();
		ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Cleared %d files in cache!\n", cacheSize);
	}
}

//------------------------------------------------------------------------------
// очистка очереди сообщений в консоль
//------------------------------------------------------------------------------
void ClearPrintQueue(const CCommand& command)
{
	g_SharedPrint.ClearPrintQueue();
}

//------------------------------------------------------------------------------
// принудительное отключение metrostroi_turbostroi_run
//------------------------------------------------------------------------------
void RunStringDisable(const CCommand& command)
{
	g_RunStringEnabled = false;
	ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: 'metrostroi_turbostroi_run' disabled!\n");
}

namespace GarrysMod::Lua
{
	fn_lua_rawseti lua_rawseti = nullptr;
}

// достаем lua_rawseti из lua_shared.dll
static bool GetGLuaPointers()
{
	SourceSDK::ModuleLoader lua_shared_loader("lua_shared");
	GM::lua_rawseti = (GM::fn_lua_rawseti)lua_shared_loader.GetSymbol("lua_rawseti");

	if (GM::lua_rawseti == nullptr)
	{
		ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Fail to get lua_rawseti()!\n");
		return false;
	}

	return true;
}

// цепляем CGlobalVarsBase из серверной библиотеки server.dll
CGlobalVarsBase* g_pServerGlobalVars = nullptr;
static bool GetGlobalVars()
{
	SourceSDK::FactoryLoader server_loader("server");
	IPlayerInfoManager* pIPlayerInfoManager = server_loader.GetInterface<IPlayerInfoManager>(INTERFACEVERSION_PLAYERINFOMANAGER);

	if (pIPlayerInfoManager == nullptr)
	{
		ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Unable to load PlayerInfoManager Interface!\n");
		return false;
	}

	g_pServerGlobalVars = pIPlayerInfoManager->GetGlobalVars();
	return (g_pServerGlobalVars != nullptr);
}

// сохранение конфига турбостроя в garrysmod/cfg/turbostroi.cfg
KeyValues* g_pKVTurbostroi = nullptr;
static void SaveSettings(IFileSystem* filesystem)
{
	if (filesystem == nullptr || g_pKVTurbostroi == nullptr)
		return;

	if (g_CVarMainCores.IsRegistered())
		g_pKVTurbostroi->SetString(g_CVarMainCores.GetName(), g_CVarMainCores.GetString());

	if (g_CVarTrainCores.IsRegistered())
		g_pKVTurbostroi->SetString(g_CVarTrainCores.GetName(), g_CVarTrainCores.GetString());

	if (g_CVarTrainSpread.IsRegistered())
		g_pKVTurbostroi->SetString(g_CVarTrainSpread.GetName(), g_CVarTrainSpread.GetString());

	if (g_CVarUnpackLib.IsRegistered())
		g_pKVTurbostroi->SetString(g_CVarUnpackLib.GetName(), g_CVarUnpackLib.GetString());

	FileHandle_t f = filesystem->Open("garrysmod/cfg/turbostroi.cfg", "wb", "MOD");
	if (f != FILESYSTEM_INVALID_HANDLE)
	{
		CUtlBuffer buf;
		g_pKVTurbostroi->RecursiveSaveToFile(buf, 0, false, false);
		filesystem->Write(buf.Base(), buf.TellPut(), f);
		filesystem->Close(f);
	}
	else
		ConColorMsg(Color(255, 255, 0, 255), "Turbostroi: Fail to save settings.\n");
}

// регистрация кваров и команд в движке
static bool RegisterConCommands()
{
	SourceSDK::FactoryLoader vstdlib_loader("vstdlib");
	ICvar* cvar007 = vstdlib_loader.GetInterface<ICvar>("VEngineCvar007");
	ICvar* cvar004 = vstdlib_loader.GetInterface<ICvar>("VEngineCvar004");
	cvar = g_pCVar = (cvar007 != nullptr) ? cvar007 : cvar004;
	if (g_pCVar == nullptr)
	{
		ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Unable to load Cvar Interface!\n");
		return false;
	}

	g_pCVar->RegisterConCommand(&g_CVarDisableCache);
	g_pCVar->RegisterConCommand(&g_CVarMainCores);
	g_pCVar->RegisterConCommand(&g_CVarTrainCores);
	g_pCVar->RegisterConCommand(&g_CVarTrainSpread);
	g_pCVar->RegisterConCommand(&g_CVarUnpackLib);
	g_pCVar->RegisterConCommand(&g_CmdClearCache);
	g_pCVar->RegisterConCommand(&g_CmdClearPrint);
	g_pCVar->RegisterConCommand(&g_CmdDisableRunString);
	
	return true;
}

// загрузка настроек из turbostroi.cfg или server.vdf, либо выставление умных дефолтов
static void LoadConVariables(IFileSystem* filesystem)
{
	if (filesystem == nullptr)
		return;

	g_pKVTurbostroi = new KeyValues("Turbostroi");
	if (g_pKVTurbostroi->LoadFromFile(filesystem, "garrysmod/cfg/turbostroi.cfg", "MOD"))
	{
		const char* main_cores = g_pKVTurbostroi->GetString(g_CVarMainCores.GetName(), g_CVarMainCores.GetDefault());
		const char* train_cores = g_pKVTurbostroi->GetString(g_CVarTrainCores.GetName(), g_CVarTrainCores.GetDefault());
		const char* train_spread = g_pKVTurbostroi->GetString(g_CVarTrainSpread.GetName(), g_CVarTrainSpread.GetDefault());
		const char* unpack_lib = g_pKVTurbostroi->GetString(g_CVarUnpackLib.GetName(), g_CVarUnpackLib.GetDefault());
		g_CVarMainCores.SetValue(main_cores);
		g_CVarTrainCores.SetValue(train_cores);
		g_CVarTrainSpread.SetValue(train_spread);
		g_CVarUnpackLib.SetValue(unpack_lib);
	}
	else
	{
		// умный расчет дефолтов под количество ядер камня
		std::string main_cores = "0";
		std::string train_cores = "1";
		std::string train_spread = "1";

		if (g_ProcessorCount <= 2)
		{
			main_cores = "all";
			train_cores = "all";
		}
		else if (g_ProcessorCount <= 4)
		{
			main_cores = "0";
			train_cores = "1-" + std::to_string(g_ProcessorCount - 1);
		}
		else
		{
			// 6, 8, 12, 16, 24, 32+ ядер: сервер на 0-1, поезда на остальных (со 2 по последнее)
			main_cores = "0-1";
			train_cores = "2-" + std::to_string(g_ProcessorCount - 1);
		}

		// проверяем server.vdf, если там были старые квары
		KeyValues* kvLuaCVars = new KeyValues("CVars");
		if (kvLuaCVars->LoadFromFile(filesystem, "garrysmod/cfg/server.vdf", "MOD"))
		{
			main_cores = kvLuaCVars->GetString(g_CVarMainCores.GetName(), main_cores.c_str());
			train_cores = kvLuaCVars->GetString(g_CVarTrainCores.GetName(), train_cores.c_str());
			train_spread = kvLuaCVars->GetString(g_CVarTrainSpread.GetName(), "1");
		}
		kvLuaCVars->deleteThis();
		kvLuaCVars = nullptr;

		g_CVarMainCores.SetValue(main_cores.c_str());
		g_CVarTrainCores.SetValue(train_cores.c_str());
		g_CVarTrainSpread.SetValue(train_spread.c_str());

		g_pKVTurbostroi->SetString(g_CVarMainCores.GetName(), main_cores.c_str());
		g_pKVTurbostroi->SetString(g_CVarTrainCores.GetName(), train_cores.c_str());
		g_pKVTurbostroi->SetString(g_CVarTrainSpread.GetName(), train_spread.c_str());
	}

	g_CVarMainCores.SetDefault(g_CVarMainCores.GetString());
	g_CVarTrainCores.SetDefault(g_CVarTrainCores.GetString());
	g_CVarTrainSpread.SetDefault("1");
}

// автоматическая распаковка встроенного lib_turbostroi_v2.lua с проверкой по crc32
bool UnpackLibTurbostroi()
{
#ifndef LIB_TURBOSTROI_LUA_H
	static_assert(false, "Please generate 'lib_turbostroi_lua.h' via 'premake5 lualib2header'");
#endif

	if (!g_CVarUnpackLib.GetBool())
	{
		ConColorMsg(Color(0, 127, 255, 255), "Turbostroi: Unpacking bundled 'lib_turbostroi_v2.lua' is disabled. To enable set \"turbostroi_unpack_lua 1\".\n");
		ConColorMsg(Color(0, 127, 255, 255), "            If you know what are you doing, ignore this.\n");
		return true;
	}

	// вычисляем crc32 встроенного в dll lua скрипта
	CRC32_t crcLua;
	CRC32_Init(&crcLua);
	CRC32_ProcessBuffer(&crcLua, g_LibTurbostroiLua, g_LibTurbostroiLuaSize);
	CRC32_Final(&crcLua);

	CFileSystem_STL& fs = g_FileSystemSTL;
	std::string lib_path = "garrysmod/lua/metrostroi/";
	std::string lib_name = "lib_turbostroi_v2.lua";
	std::string lib_fullpath = lib_path + lib_name;

	// проверяем, не совпадает ли уже файл на диске
	FileHandle_t f = fs.Open(lib_fullpath.c_str(), "rb");
	if (f)
	{
		auto fSize = fs.Size(f);

		std::unique_ptr<char> buf(new char[fSize]());
		fs.Read(buf.get(), fSize, f);
		fs.Close(f);

		CRC32_t crcCurr;
		CRC32_Init(&crcCurr);
		CRC32_ProcessBuffer(&crcCurr, buf.get(), fSize);
		CRC32_Final(&crcCurr);

		if (crcCurr == crcLua)
		{
			ConColorMsg(Color(255, 0, 255, 255), "Turbostroi: 'lib_turbostroi_v2.lua' is up to date.\n");
			return true;
		}
	}

	ConColorMsg(Color(255, 0, 255, 255), "Turbostroi: Unpacking '%s'...", lib_fullpath.c_str());
	fs.CreateDirHierarchy(lib_path.c_str(), "GAME");
	f = fs.Open(lib_fullpath.c_str(), "wb");
	if (!f)
	{
		ConColorMsg(Color(255, 0, 0, 255), "\n"
			                               "            Failed! Please check folder permissions\n"
										   "            or set \"turbostroi_unpack_lua 0\" and install it manually.\n");
		return false;
	}

	auto written = fs.Write(g_LibTurbostroiLua, g_LibTurbostroiLuaSize, f);
	fs.Close(f);

	if (written != g_LibTurbostroiLuaSize)
	{
		ConColorMsg(Color(255, 0, 0, 255), "\n"
										   "            Failed! Please check folder permissions\n"
									       "            or set \"turbostroi_unpack_lua 0\" and install it manually.\n");
		fs.RemoveFile(lib_fullpath.c_str(), "GAME");
		return false;
	}

	ConColorMsg(Color(0, 255, 0, 255), "Unpacked!\n");
	return true;
}

// проверка наличия turbostroi.txt с фразой "I'm developer" для команды runstring
void CheckRunStringEnabled()
{
	FileHandle_t f = g_FileSystemSTL.Open("turbostroi.txt", "rb");
	if (f == nullptr) return;

	int fSize = g_FileSystemSTL.Size(f);

	std::unique_ptr<char> buf(new char[fSize]());
	g_FileSystemSTL.Read(buf.get(), fSize, f);
	g_FileSystemSTL.Close(f);

	if (strncmp(buf.get(), "I'm developer", 13) == 0)
	{
		ConColorMsg(Color(255, 0, 0, 255),
			"!!! ========================================================= !!!\n"
			"!!!    Be careful! 'metrostroi_turbostroi_run' is enabled,    !!!\n"
			"!!!              disable it if you don't need it!             !!!\n"
			"!!! ========================================================= !!!\n");
		
		g_RunStringEnabled = true;
	}
}

// входная точка инициализации source sdk
bool InitSourceSDK()
{
	if (!GetGLuaPointers())
		return false;

	if (!GetGlobalVars())
		return false;

	if (!RegisterConCommands())
		return false;

	LoadConVariables(&g_FileSystemSTL);
	
	if (!UnpackLibTurbostroi())
		return false;

	CheckRunStringEnabled();

	return true;
}

// очистка при выгрузке модуля
void ShutdownSourceSDK()
{
	SaveSettings(&g_FileSystemSTL);

	if (g_pKVTurbostroi != nullptr)
	{
		g_pKVTurbostroi->deleteThis();
		g_pKVTurbostroi = nullptr;
	}

	if (g_pCVar != nullptr)
	{
		g_pCVar->UnregisterConCommand(&g_CVarDisableCache);
		g_pCVar->UnregisterConCommand(&g_CVarMainCores);
		g_pCVar->UnregisterConCommand(&g_CVarTrainCores);
		g_pCVar->UnregisterConCommand(&g_CVarTrainSpread);
		g_pCVar->UnregisterConCommand(&g_CVarUnpackLib);
		g_pCVar->UnregisterConCommand(&g_CmdClearCache);
		g_pCVar->UnregisterConCommand(&g_CmdClearPrint);
		g_pCVar->UnregisterConCommand(&g_CmdDisableRunString);
		g_pCVar = nullptr;
	}
}