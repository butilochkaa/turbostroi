#include "wagon.h"

#include "gmsv_turbostroi.h"
#include "affinity.h"
#include "shared_print.h"
#include <const.h>
#include <cstring>
#include <thread>
#include <array>
#include <src/lua.hpp>

extern "C"
{
#include <src/lj_obj.h>
}

#define LOCAL_L lua_State* L = m_Lua.L
#define LOCAL_SELF CWagon* self = static_cast<TLuaData*>(G(L)->allocd)->self

// указатели на активные вагоны по entindex (размер равен max_edicts сервера)
static std::array<CWagon*, MAX_EDICTS> g_Wagons{};

// флаг разрешения отладочной команды metrostroi_turbostroi_run
bool g_RunStringEnabled = false;

// обертка над аллокатором luajit, чтобы каждый вагон жил в своем изолированном куске памяти
static void* l_alloc(void* ud, void* ptr, size_t osize, size_t nsize)
{
	TLuaData* d = static_cast<TLuaData*>(ud);
	return lj_alloc_f(d->msp, ptr, osize, nsize);
}

// создание экземпляра вагона по индексу энтити
CWagon* CWagon::Create(unsigned int idx)
{
	if (idx > 0 && idx < g_Wagons.size())
		return new CWagon(idx);

	return nullptr;
}

// поиск вагона по entindex (0 — это мир, вагоны всегда с 1)
CWagon* CWagon::CWagonByIndex(unsigned int idx)
{
	if (idx > 0 && idx < g_Wagons.size())
		return g_Wagons[idx];

	return nullptr;
}

CWagon::CWagon(unsigned int idx)
{
	m_StartTime = std::chrono::steady_clock::now();

	// сохраняем индекс энтити и заносим себя в таблицу активных вагонов
	m_EntIndex = idx;
	AddToArray();

	// поднимаем чистый изолированный lua стэйт для этого вагона
	m_Lua.L = luaL_newstate();
	lua_getallocf(m_Lua.L, &m_Lua.msp); // забираем дефолтный mspace
	lua_setallocf(m_Lua.L, l_alloc, &m_Lua); // подменяем на наш аллокатор

	LOCAL_L;
	luaL_openlibs(L);

	// регистрируем флаги и базовые функции окружения турбостроя
	lua_pushboolean(L, true);
	lua_setglobal(L, "TURBOSTROI");

	lua_pushstring(L, g_LibraryFileName.c_str());
	lua_setglobal(L, "LIB_TURBOSTROI_FILENAME");

	// потокобезопасный print()
	lua_pushcfunction(L, &SharedPrint::PrintL);
	lua_setglobal(L, "print");

	// функции времени
	lua_pushcfunction(L, &CWagon::CurrentTime);
	lua_setglobal(L, "CurTime");

	lua_pushcfunction(L, &CWagon::PrevTime);
	lua_setglobal(L, "PrevTime");

	lua_pushcfunction(L, &CWagon::DeltaTime);
	lua_setglobal(L, "FrameTime");

	lua_pushcfunction(L, &CWagon::SysTime);
	lua_setglobal(L, "SysTime");

	// индекс вагона
	lua_pushcfunction(L, &CWagon::EntIndex);
	lua_setglobal(L, "EntIndex");

	// прием сообщений из очереди
	lua_pushcfunction(L, &CWagon::ThreadRecvMessages);
	lua_setglobal(L, "RecvMessages");

	// указатель на c++ объект вагона
	lua_pushlightuserdata(L, this);
	lua_setglobal(L, "_CWagon");
}

CWagon::~CWagon()
{
	lua_close(m_Lua.L);
	lj_alloc_destroy(m_Lua.msp);
	RemoveFromArray();
}

// отправка сообщения от сервера (gmod) в поток симуляции вагона
bool CWagon::SimSendMessage(int message, const char* system_name, const char* name, double index, double value)
{
	TThreadMsg tmsg;
	tmsg.message = message;
	tmsg.system_name = system_name;
	tmsg.name = name;
	tmsg.index = index;
	tmsg.value = value;

	TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Sim2ThreadMem);
	return buffer.push(tmsg);
}

int CWagon::SimRecvMessages([[maybe_unused]] std::unique_ptr<TThreadMsg[]>& tmsgs)
{
	// устарело, сейчас сообщения читаются по одному через SimRecvMessage
	return 0;
}

// вычитывание одного сообщения сервером из очереди вагона
TThreadMsg& CWagon::SimRecvMessage()
{
	TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Thread2SimMem);
	if (buffer.pop(m_Thread2SimMsg))
		return m_Thread2SimMsg;
	else
		return s_EmptyMsg;
}

// сколько сообщений накопилось в очереди на чтение серверу
int CWagon::SimReadAvailable()
{
	const TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Thread2SimMem);
	return buffer.size();
}

// отправка сообщения из потока симуляции в сторону сервера gmod
bool CWagon::ThreadSendMessage(int message, const char* system_name, const char* name, double index, double value)
{
	TThreadMsg tmsg;
	tmsg.message = message;
	tmsg.system_name = system_name;
	tmsg.name = name;
	tmsg.index = index;
	tmsg.value = value;

	TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Thread2SimMem);
	return buffer.push(tmsg);
}

int CWagon::ThreadRecvMessages([[maybe_unused]] std::unique_ptr<TThreadMsg[]>& tmsgs)
{
	return 0;
}

int CWagon::ThreadRecvMessages([[maybe_unused]] lua_State* L)
{
	return 0;
}

// вычитывание сообщения потоком симуляции
TThreadMsg& CWagon::ThreadRecvMessage()
{
	TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Sim2ThreadMem);
	if (buffer.pop(m_Sim2ThreadMsg))
		return m_Sim2ThreadMsg;
	else
		return s_EmptyMsg;
}

// сколько входящих сообщений пришло вагону от сервера
int CWagon::ThreadReadAvailable()
{
	const TThreadMsgBuffer& buffer = reinterpret_cast<TThreadMsgBuffer&>(m_Sim2ThreadMem);
	return buffer.size();
}

// загрузка и исполнение байткода / скрипта в окружении вагона
bool CWagon::LoadBuffer(const char* buf, size_t size, const char* filename)
{
	LOCAL_L;
	if (luaL_loadbuffer(L, buf, size, filename)
		|| lua_pcall(L, 0, LUA_MULTRET, 0))
	{
		std::string err = lua_tostring(L, -1);
		err += '\n';
		g_SharedPrint.Push(err);
		lua_pop(L, 1);
		return false;
	}

	return true;
}

// проверка совпадения версий с lib_turbostroi_v2.lua
bool CWagon::CheckLibLoaded()
{
	LOCAL_L;

	lua_getglobal(L, "LIB_TURBOSTROI_VERSION");
	if (lua_type(L, -1) == LUA_TSTRING)
	{
		const char* ver = lua_tostring(L, -1);
		lua_pop(L, 1);

		if (strcmp(ver, TURBOSTROI_VERSION_STR) == 0)
			return true;
		
		g_SharedPrint.Push("[!] Incompatable lib_turbostroi_v2.lua version (");
		g_SharedPrint.Push(ver);
		g_SharedPrint.Push("), please update to " TURBOSTROI_VERSION_PRINT "\n");
		return false;
	}
	else
	{
		g_SharedPrint.Push("[!] Incompatable lib_turbostroi_v2.lua version, please update to " TURBOSTROI_VERSION_PRINT "\n");
		return false;
	}
}

// добавление системы в список автозагрузки вагона
void CWagon::AddLoadSystem(TTrainSystem& sys)
{
	LOCAL_L;
	lua_getglobal(L, "LoadSystems");
	{
		if (lua_type(L, -1) != LUA_TTABLE)
		{
			lua_pop(L, 1);
			lua_newtable(L);
			lua_setglobal(L, "LoadSystems");
			lua_getglobal(L, "LoadSystems");
		}

		lua_newtable(L);
		{
			lua_pushstring(L, sys.file_name.c_str());
			lua_rawseti(L, -2, 1);

			lua_pushstring(L, sys.base_name.c_str());
			lua_rawseti(L, -2, 2);
		}
		lua_rawseti(L, -2, ++m_SystemCount);
	}
	lua_settop(L, 0);
}

// выполнение произвольного кода через metrostroi_turbostroi_run
void CWagon::RunString(const char* buf, unsigned int uid)
{
	if (!g_RunStringEnabled)
		return;

	LOCAL_L;
	static std::string str;
	
	str.clear();
	str += 
R"======(local _retdata=""
local print = function(...)
for k,v in ipairs({...}) do _retdata = _retdata..tostring(v).."\t" end
    _retdata = _retdata.."\n"
end
)======";

	str += buf;
	str += "\nreturn _retdata";

	m_RunStringMutex.lock();
	{
		static std::string _retdata;
		int load = luaL_loadbuffer(L, str.c_str(), str.size(), nullptr);
		if (load == 0) lua_pcall(L, 0, 1, 0);

		_retdata.clear();
		_retdata = lua_tostring(L, -1);
		lua_pop(L, 1);

		ThreadSendMessage(5, _retdata.c_str(), "", uid, EntIndex());
	}
	m_RunStringMutex.unlock();
}

//------------------------------------------------------------------------------
// поток симуляции вагона
//------------------------------------------------------------------------------

// выставляем флаг, что вагон требует обновления ядер при смене кваров
void CWagon::ApplyAffinity()
{
	m_AffinityDirty.store(true, std::memory_order_release);
}

// перепривязываем все активные вагоны на новые ядра
void CWagon::ReapplyAffinityAll()
{
	for (CWagon* wagon : g_Wagons)
	{
		if (wagon != nullptr)
			wagon->ApplyAffinity();
	}
}

// главный рабочий цикл потока симуляции
void CWagon::SimulationThreadFn()
{
	m_ThreadRunning = true;
	size_t assignedCore = 0;

	// привязываем вагон к ядру (с учетом режима round-robin spread)
	if (SetTrainThreadAffinity(CurrentThread(), m_EntIndex, assignedCore))
	{
		std::string str = "[!] Train thread (wagon ent " + std::to_string(m_EntIndex) + ") running on CPU " + std::to_string(assignedCore) + "\n";
		g_SharedPrint.Push(str);
	}
	else
	{
		g_SharedPrint.Push("Turbostroi: Failed to set affinity mask of train thread!\n");
	}

	// сохраняем ссылку на lua-функцию Think в реестре
	LOCAL_L;
	lua_getglobal(L, "Think");
	m_ThinkRef = luaL_ref(L, LUA_REGISTRYINDEX);

	// вызываем Initialize() вагона в lua
	Initialize();

	// небольшая пауза перед первым тиком, чтобы сервер успел передать начальные данные
	std::this_thread::sleep_for(std::chrono::microseconds(g_ThreadTickrate));

	// основной цикл тика симуляции
	while (!g_ForceThreadsFinished && !m_Finish)
	{
		// если администратор сменил turbostroi_train_cores на лету — пересаживаемся на новое ядро
		if (m_AffinityDirty.exchange(false, std::memory_order_acquire))
		{
			size_t newCore = 0;
			SetTrainThreadAffinity(CurrentThread(), m_EntIndex, newCore);
		}

		// синхронизируемся с текущим серверным временем
		double curTime = g_CurrentTime.load(std::memory_order_relaxed);
		if (!UpdateCurTime(curTime))
		{
			// если сервер еще не обновил тик — чуть засыпаем
			std::this_thread::sleep_for(std::chrono::milliseconds(3));
			continue;
		}

		if (g_RunStringEnabled) m_RunStringMutex.lock();
		Think();
		if (g_RunStringEnabled) m_RunStringMutex.unlock();

		// спим положенное время между тиками (по умолчанию 10мс = 100 fps)
		std::this_thread::sleep_for(std::chrono::microseconds(g_ThreadTickrate));
	}

	// освобождаем регистрацию функции Think и завершаем поток
	luaL_unref(L, LUA_REGISTRYINDEX, m_ThinkRef);
	g_SharedPrint.Push("[!] Terminating train thread\n");
	m_ThreadRunning = false;
}

void CWagon::Initialize()
{
	LOCAL_L;
	lua_getglobal(L, "Initialize");
	if (lua_pcall(L, 0, 0, 0))
	{
		std::string err = lua_tostring(L, -1);
		err += '\n';
		g_SharedPrint.Push(err);
		lua_pop(L, 1);
	}
}

void CWagon::Think()
{
	LOCAL_L;
	lua_rawgeti(L, LUA_REGISTRYINDEX, m_ThinkRef);
	lua_pushnumber(L, m_DeltaTime);
	if (lua_pcall(L, 1, 0, 0))
	{
		std::string err = lua_tostring(L, -1);
		err += '\n';
		g_SharedPrint.Push(err);
		lua_pop(L, 1);
	}
}

// обновление текущего времени и расчет дельты
bool CWagon::UpdateCurTime(double t)
{
	if (m_ServerCurTime == t) return false;
	m_ServerCurTime = t;

	double tCurTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - m_StartTime).count();

	m_PrevTime = m_CurrentTime;
	m_DeltaTime = tCurTime - m_PrevTime;
	m_CurrentTime = tCurTime;
	
	return true;
}

double CWagon::CurrentTime()
{
	return m_CurrentTime;
}

int CWagon::CurrentTime(lua_State* L)
{
	LOCAL_SELF;
	lua_pushnumber(L, self->m_CurrentTime);
	return 1;
}

double CWagon::PrevTime()
{
	return m_PrevTime;
}

int CWagon::PrevTime(lua_State* L)
{
	LOCAL_SELF;
	lua_pushnumber(L, self->m_PrevTime);
	return 1;
}

double CWagon::DeltaTime()
{
	return m_DeltaTime;
}

int CWagon::DeltaTime(lua_State* L)
{
	LOCAL_SELF;
	lua_pushnumber(L, self->m_DeltaTime);
	return 1;
}

int CWagon::SysTime(lua_State* L)
{
	LOCAL_SELF;
	double tSysTime = std::chrono::duration<double>(std::chrono::steady_clock::now() - self->m_StartTime).count();
	lua_pushnumber(L, tSysTime);
	return 1;
}

void CWagon::SetEntIndex(unsigned int idx)
{
	m_EntIndex = idx;
}

unsigned int CWagon::EntIndex()
{
	return m_EntIndex;
}

int CWagon::EntIndex(lua_State* L)
{
	LOCAL_SELF;
	lua_pushnumber(L, self->m_EntIndex);
	return 1;
}

void CWagon::Finish()
{
	m_Finish = true;
}

bool CWagon::ThreadRunning()
{
	return m_ThreadRunning;
}

// добавление вагона в глобальный массив по entindex
void CWagon::AddToArray()
{
	if (m_EntIndex > 0 && m_EntIndex < g_Wagons.size())
		g_Wagons[m_EntIndex] = this;
}

// удаление вагона из глобального массива
void CWagon::RemoveFromArray()
{
	if (m_EntIndex > 0 && m_EntIndex < g_Wagons.size())
		g_Wagons[m_EntIndex] = nullptr;
}

//------------------------------------------------------------------------------
// c ffi экспорт функций турбостроя для быстрых вызовов из luajit
//------------------------------------------------------------------------------
extern "C" TURBOSTROI_EXPORT bool ThreadSendMessage(void* p, int message, const char* system_name, const char* name, double index, double value)
{
	CWagon* userdata = static_cast<CWagon*>(p);

	if (userdata == nullptr)
		return false;

	return userdata->ThreadSendMessage(message, system_name, name, index, value);
}

extern "C" TURBOSTROI_EXPORT bool ThreadSendMessageIPC(void* p, void* receiver, int message, const char* system_name, const char* name, double index, double value)
{
	CWagon* userdata1 = static_cast<CWagon*>(p);
	CWagon* userdata2 = static_cast<CWagon*>(receiver);

	if (userdata1 == nullptr || userdata2 == nullptr)
		return false;

	return userdata2->SimSendMessage(message, system_name, name, index, value);
}

extern "C" TURBOSTROI_EXPORT bool ThreadSendMessageIPCIndex(void* p, int ent_index, int message, const char* system_name, const char* name, double index, double value)
{
	return ThreadSendMessageIPC(p, CWagon::CWagonByIndex(ent_index), message, system_name, name, index, value);
}

extern "C" TURBOSTROI_EXPORT TThreadMsg& ThreadRecvMessage(void* p)
{
	CWagon* userdata = static_cast<CWagon*>(p);

	if (userdata == nullptr)
		return CWagon::s_EmptyMsg;

	return userdata->ThreadRecvMessage();
}

extern "C" TURBOSTROI_EXPORT int ThreadReadAvailable(void* p)
{
	CWagon* userdata = static_cast<CWagon*>(p);

	if (userdata == nullptr)
		return 0;

	return userdata->ThreadReadAvailable();
}
