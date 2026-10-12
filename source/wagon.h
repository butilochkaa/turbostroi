#pragma once
#include "ring_buffer.h"
#include "mutex.h"
#include <memory>
#include <string>
#include <chrono>
#include <atomic>
extern "C"
{
#include <src/lj_alloc.h>
}

// флаг разрешения отладочной команды metrostroi_turbostroi_run
extern bool g_RunStringEnabled;

// структура межпоточного сообщения (провода trainwire, команды, тумблеры, звуки)
struct TThreadMsg {
	int message = 0;
	const char* system_name = nullptr;
	const char* name = nullptr;
	double index = 0.0;
	double value = 0.0;
};

// описание скрипта системы вагона метро
struct TTrainSystem {
	TTrainSystem(std::string sysName, std::string sysFileName)
	{
		base_name = sysName;
		file_name = sysFileName;
	}

	std::string base_name;
	std::string file_name; // путь к файлу системы в lua
};

class CWagon;
// контекст lua для изолированного вагона
struct TLuaData 
{
	CWagon* self = nullptr;
	void* msp = nullptr;
	lua_State* L = nullptr;
	PRNGState prng{};
};

// класс симуляции отдельного вагона метро
class CWagon {
public:
	// фабрика создания вагона по entindex
	static CWagon* Create(unsigned int idx);
	static CWagon* CWagonByIndex(unsigned int idx);

	// перепривязка ядер для всех активных вагонов на лету
	static void ReapplyAffinityAll();
	~CWagon();

	// пометить вагон как требующий обновления affinity
	void ApplyAffinity();

	inline static TThreadMsg s_EmptyMsg{ 0 };

	// методы со стороны главного потока сервера (gmod)
	bool SimSendMessage(int message, const char* system_name, const char* name, double index, double value);
	int SimRecvMessages(std::unique_ptr<TThreadMsg[]>& tmsgs);
	TThreadMsg& SimRecvMessage();
	int SimReadAvailable();

	// методы со стороны потока симуляции турбостроя
	bool ThreadSendMessage(int message, const char* system_name, const char* name, double index, double value);
	int ThreadRecvMessages(std::unique_ptr<TThreadMsg[]>& tmsgs);
	static int ThreadRecvMessages(lua_State* L);
	TThreadMsg& ThreadRecvMessage();
	int ThreadReadAvailable();

	// загрузка скриптов систем в виртуальную машину вагона
	bool LoadBuffer(const char* buf, size_t size, const char* filename);
	bool CheckLibLoaded();
	void AddLoadSystem(TTrainSystem& sys);
	void RunString(const char* buf, unsigned int uid);

	// точка входа в поток симуляции
	void SimulationThreadFn();
	void Initialize();
	void Think();

	// синхронизация времени с сервером
	bool UpdateCurTime(double t);

	double CurrentTime();
	static int CurrentTime(lua_State* L);

	double PrevTime();
	static int PrevTime(lua_State* L);

	double DeltaTime();
	static int DeltaTime(lua_State* L);

	static int SysTime(lua_State* L);

	void SetEntIndex(unsigned int idx);
	unsigned int EntIndex();
	static int EntIndex(lua_State* L);

	// завершение работы потока
	void Finish();
	bool ThreadRunning();

private:
	CWagon(unsigned int idx);

	TLuaData m_Lua{ this };
	std::chrono::steady_clock::time_point m_StartTime;
	double m_ServerCurTime = -1.0;
	double m_CurrentTime = -1.0;
	double m_PrevTime = 0.0;
	double m_DeltaTime = 0.0;
	bool m_Finish = false;
	bool m_ThreadRunning = false;
	std::atomic<bool> m_AffinityDirty{ false };
	int m_ThinkRef = 0;
	int m_SystemCount = 0;
	unsigned int m_EntIndex = 0;

	// lock-free кольцевые буферы обмена сообщениями (емкость 256 сообщений)
	typedef RingBuffer<TThreadMsg, 256> TThreadMsgBuffer;
	TThreadMsg m_Thread2SimMsg, m_Sim2ThreadMsg;
	char m_Thread2SimMem[sizeof(TThreadMsgBuffer)]{}, m_Sim2ThreadMem[sizeof(TThreadMsgBuffer)]{};
	Mutex m_RunStringMutex;

	// регистрация в глобальном массиве активных вагонов
	void AddToArray();
	void RemoveFromArray();
};
