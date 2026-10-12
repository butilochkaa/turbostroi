#pragma once
#include <iconvar.h>

struct lua_State;
namespace GarrysMod::Lua
{
    // указатель на сырую lua_rawseti из lua_shared.dll
    typedef void (*fn_lua_rawseti)(lua_State* L, int idx, int n);
    extern fn_lua_rawseti lua_rawseti;
}

// инициализация и освобождение интерфейсов source engine
bool InitSourceSDK();
void ShutdownSourceSDK();

// колбэки консольных команд и переменных
void CVarMainCoresCallback(IConVar* var, const char* pOldValue, float flOldValue);
void CVarTrainCoresCallback(IConVar* var, const char* pOldValue, float flOldValue);
void CVarTrainSpreadCallback(IConVar* var, const char* pOldValue, float flOldValue);
void ClearLoadCache(const CCommand& command);
void ClearPrintQueue(const CCommand& command);
void RunStringDisable(const CCommand& command);

class ConVar;
extern ConVar g_CVarTrainSpread;
