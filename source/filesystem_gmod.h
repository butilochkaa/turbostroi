#pragma once
#include <filesystem.h>
#include <GarrysMod/Lua/LuaBase.h>

// обертки над gmod lua file.Read / file.Write / file.Size для чтения скриптов систем из gma и папок аддонов
const char* GMOD_FileRead(GarrysMod::Lua::ILuaBase* LUA, const char* fileName, const char* pathID);
bool GMOD_FileWrite(GarrysMod::Lua::ILuaBase* LUA, const char* fileName, const char* data);
int64_t GMOD_FileSize(GarrysMod::Lua::ILuaBase* LUA, const char* fileName, const char* pathID);