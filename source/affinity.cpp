#include "affinity.h"

#include <regex>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <boost/multiprecision/cpp_int.hpp>
#include <color.h>
#include <dbg.h>
#include <convar.h>

// определяем количество доступных потоков на процессоре
size_t g_ProcessorCount = std::thread::hardware_concurrency();
std::vector<size_t> g_MainCoresList;
std::vector<size_t> g_SimCoresList;
std::atomic<size_t> g_TrainRoundRobinIndex{ 0 };

extern ConVar g_CVarTrainSpread;

#if defined(_WIN32)
// инициализация 32 групп процессора с маской 0 для windows
std::array<GROUP_AFFINITY, 32> g_MainThreadGroupAffinity { {
	{0,0}, {0,1}, {0,2}, {0,3}, {0,4}, {0,5}, {0,6}, {0,7},
	{0,8}, {0,9}, {0,10},{0,11},{0,12},{0,13},{0,14},{0,15},
	{0,16},{0,17},{0,18},{0,19},{0,20},{0,21},{0,22},{0,23},
	{0,24},{0,25},{0,26},{0,27},{0,28},{0,29},{0,30},{0,31}
} };

std::array<GROUP_AFFINITY, 32> g_SimThreadGroupAffinity { {
	{0,0}, {0,1}, {0,2}, {0,3}, {0,4}, {0,5}, {0,6}, {0,7},
	{0,8}, {0,9}, {0,10},{0,11},{0,12},{0,13},{0,14},{0,15},
	{0,16},{0,17},{0,18},{0,19},{0,20},{0,21},{0,22},{0,23},
	{0,24},{0,25},{0,26},{0,27},{0,28},{0,29},{0,30},{0,31}
} };

HANDLE CurrentThread()
{
	return ::GetCurrentThread();
}

DWORD CurrentCPU()
{
	return ::GetCurrentProcessorNumber();
}
#elif defined(POSIX)
cpu_set_t g_MainThreadGroupAffinity;
cpu_set_t g_SimThreadGroupAffinity;

pid_t CurrentThread()
{
	return ::gettid();
}

int CurrentCPU()
{
	return ::sched_getcpu();
}
#else
size_t g_MainThreadGroupAffinity = 0;
size_t g_SimThreadGroupAffinity = 0;

size_t CurrentThread()
{
	return 0;
}

size_t CurrentCPU()
{
	return 0;
}
#endif

//------------------------------------------------------------------------------
// вспомогалки для работы со строками
//------------------------------------------------------------------------------
static inline std::string StringToLower(std::string str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return str;
}

static inline std::string TrimString(const std::string& str)
{
	auto start = str.find_first_not_of(" \t\r\n");
	if (start == std::string::npos) return "";
	auto end = str.find_last_not_of(" \t\r\n");
	return str.substr(start, end - start + 1);
}

// превращает список ядер в человекочитаемую строку для консоли (например "2-15" или "0,2,4")
std::string CoresListToString(const std::vector<size_t>& cores)
{
	if (cores.empty())
		return "none";

	if (cores.size() == g_ProcessorCount && g_ProcessorCount > 1)
	{
		return "all (0-" + std::to_string(g_ProcessorCount - 1) + ")";
	}

	std::vector<size_t> sortedCores = cores;
	std::sort(sortedCores.begin(), sortedCores.end());
	sortedCores.erase(std::unique(sortedCores.begin(), sortedCores.end()), sortedCores.end());

	std::ostringstream ss;
	size_t i = 0;
	while (i < sortedCores.size())
	{
		size_t rangeStart = sortedCores[i];
		size_t rangeEnd = rangeStart;
		while (i + 1 < sortedCores.size() && sortedCores[i + 1] == rangeEnd + 1)
		{
			rangeEnd = sortedCores[++i];
		}

		if (ss.tellp() > 0)
			ss << ", ";

		if (rangeStart == rangeEnd)
			ss << rangeStart;
		else
			ss << rangeStart << "-" << rangeEnd;

		++i;
	}

	return ss.str();
}

//------------------------------------------------------------------------------
// гибкий парсер: кушает all, диапазоны 2-15, списки 0,2,4 и старые маски
//------------------------------------------------------------------------------
namespace mp = boost::multiprecision;

bool ParseCoresString(const char* value, size_t maxCPUs, CPU_SET& group, std::vector<size_t>& coresList)
{
	coresList.clear();
	if (value == nullptr)
		return false;

	std::string s = TrimString(value);
	if (s.empty())
		return false;

	std::string lower = StringToLower(s);

	// 1. ключевое слово "all" или "*" — отдаем вообще все ядра камня
	if (lower == "all" || lower == "*")
	{
		for (size_t i = 0; i < maxCPUs; ++i)
			coresList.push_back(i);
	}
	// 2. двоичная битовая маска вида 0b11111110
	else if (lower.rfind("0b", 0) == 0)
	{
		for (size_t i = 2; i < s.size(); ++i)
		{
			if (s[i] == '1')
			{
				size_t bitIdx = s.size() - i - 1;
				if (bitIdx < maxCPUs)
					coresList.push_back(bitIdx);
			}
		}
	}
	// 3. шестнадцатеричная битовая маска вида 0xff
	else if (lower.rfind("0x", 0) == 0)
	{
		try
		{
			mp::uint1024_t num;
			num.assign(s);
			for (size_t i = 0; i < maxCPUs; ++i)
			{
				if (mp::bit_test(num, (unsigned)i))
					coresList.push_back(i);
			}
		}
		catch (const std::exception& e)
		{
			ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Failed to parse hex mask: %s\n", e.what());
			return false;
		}
	}
	// 4. диапазоны и списки ядер (например 0-1, 2-15 или 0,2,4,6)
	else if (s.find('-') != std::string::npos || s.find(',') != std::string::npos ||
		lower.rfind("core", 0) == 0 || lower.rfind("cpu", 0) == 0)
	{
		std::stringstream ss(s);
		std::string token;
		while (std::getline(ss, token, ','))
		{
			token = TrimString(token);
			if (token.empty()) continue;

			// отрезаем возможные префиксы "core" или "cpu"
			std::string tokenLower = StringToLower(token);
			if (tokenLower.rfind("core", 0) == 0) token = token.substr(4);
			else if (tokenLower.rfind("cpu", 0) == 0) token = token.substr(3);
			token = TrimString(token);
			if (!token.empty() && (token[0] == ':' || token[0] == '#'))
				token = TrimString(token.substr(1));

			auto dashPos = token.find('-');
			if (dashPos != std::string::npos)
			{
				// разбираем диапазон от start до end
				std::string startStr = TrimString(token.substr(0, dashPos));
				std::string endStr = TrimString(token.substr(dashPos + 1));
				if (!startStr.empty() && !endStr.empty())
				{
					try
					{
						size_t startCore = (size_t)std::stoull(startStr);
						size_t endCore = (size_t)std::stoull(endStr);
						if (startCore > endCore) std::swap(startCore, endCore);
						for (size_t c = startCore; c <= endCore && c < maxCPUs; ++c)
						{
							coresList.push_back(c);
						}
					}
					catch (...)
					{
						ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Invalid range '%s'\n", token.c_str());
					}
				}
			}
			else
			{
				// одиночное ядро в списке (например 0 в списке "0,2,4")
				try
				{
					size_t c = (size_t)std::stoull(token);
					if (c < maxCPUs)
						coresList.push_back(c);
				}
				catch (...)
				{
					ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Invalid core index '%s'\n", token.c_str());
				}
			}
		}
	}
	// 5. одиночное десятичное число
	else
	{
		try
		{
			unsigned long long val = std::stoull(s);
			// если число больше кол-ва ядер или равно 254 (дефолтная маска из старых конфигов) — парсим как маску
			if (val >= maxCPUs || val == 254)
			{
				mp::uint1024_t num;
				num.assign(s);
				for (size_t i = 0; i < maxCPUs; ++i)
				{
					if (mp::bit_test(num, (unsigned)i))
						coresList.push_back(i);
				}
			}
			else
			{
				// для совместимости: turbostroi_main_cores "1" в старых конфигах значило маску 1 (ядро 0)
				if (val == 1)
				{
					coresList.push_back(0);
				}
				else if (val == 0)
				{
					coresList.push_back(0);
				}
				else
				{
					// пробуем как битовую маску, если не совпало — берем как номер ядра
					mp::uint1024_t num;
					num.assign(s);
					for (size_t i = 0; i < maxCPUs; ++i)
					{
						if (mp::bit_test(num, (unsigned)i))
							coresList.push_back(i);
					}
					if (coresList.empty() && val < maxCPUs)
						coresList.push_back((size_t)val);
				}
			}
		}
		catch (const std::exception& e)
		{
			ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Failed to parse cores '%s': %s\n", value, e.what());
			return false;
		}
	}

	// сортируем ядра по возрастанию и убираем повторы
	std::sort(coresList.begin(), coresList.end());
	coresList.erase(std::unique(coresList.begin(), coresList.end()), coresList.end());

	if (coresList.empty())
	{
		ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: No valid CPU cores parsed from '%s'! (CPU count: %u)\n", value, (unsigned)maxCPUs);
		return false;
	}

	// раскладываем выбранные ядра в нативные структуры cpu_set
#if defined(_WIN32)
	for (size_t i = 0; i < group.size(); ++i)
	{
		group[i].Group = (WORD)i;
		group[i].Mask = 0;
	}

	size_t cpusPerGroup = sizeof(KAFFINITY) * 8;
	size_t maskGroups = 0;
	for (size_t core : coresList)
	{
		size_t groupIdx = core / cpusPerGroup;
		size_t coreInGroup = core % cpusPerGroup;
		if (groupIdx < group.size())
		{
			group[groupIdx].Mask |= (((KAFFINITY)1) << coreInGroup);
		}
	}

	for (size_t i = 0; i < group.size(); ++i)
	{
		if (group[i].Mask != 0)
			maskGroups++;
	}

	// на старых виндах (до win11) один поток не может принадлежать более чем 1 группе процессоров
	if (!IsWindows11() && maskGroups > 1)
	{
		ConColorMsg(Color(255, 255, 0, 255),
			"Turbostroi: Warning: Affinity mask covers more than one processor group (%u groups).\n"
			"  On Windows 10 and older, threads cannot span multiple processor groups.\n",
			(unsigned)maskGroups);
	}
#elif defined(POSIX)
	CPU_ZERO(&group);
	for (size_t core : coresList)
	{
		CPU_SET(core, &group);
	}
#endif

	return true;
}

// применить маску группы с сохранением списка выбранных ядер
bool SetThreadGroup(CPU_SET& group, std::vector<size_t>& coresList, const char* value)
{
	return ParseCoresString(value, g_ProcessorCount, group, coresList);
}

// перегрузка для совместимости без выходного вектора
bool SetThreadGroup(CPU_SET& group, const char* value)
{
	std::vector<size_t> dummy;
	return ParseCoresString(value, g_ProcessorCount, group, dummy);
}

// жестко сажаем поток на одно конкретное ядро (нужно для round-robin пина вагонов)
bool SetThreadToCore(std::thread::native_handle_type handle, size_t coreId)
{
#if defined(_WIN32)
	size_t cpusPerGroup = sizeof(KAFFINITY) * 8;
	WORD groupIdx = (WORD)(coreId / cpusPerGroup);
	size_t coreInGroup = coreId % cpusPerGroup;
	KAFFINITY mask = ((KAFFINITY)1) << coreInGroup;

	GROUP_AFFINITY ga{};
	ga.Group = groupIdx;
	ga.Mask = mask;

	return (::SetThreadGroupAffinity(handle, &ga, nullptr) != 0);
#elif defined(POSIX)
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(coreId, &cpuset);
	return (sched_setaffinity(handle, sizeof(cpuset), &cpuset) == 0);
#else
	return false;
#endif
}

// умное распределение потока вагона по ядрам
bool SetTrainThreadAffinity(std::thread::native_handle_type handle, unsigned int wagonIndex, size_t& assignedCore)
{
	if (g_SimCoresList.empty())
	{
		assignedCore = CurrentCPU();
		return SetAffinityMask(handle, g_SimThreadGroupAffinity);
	}

	bool spread = g_CVarTrainSpread.GetBool();
	if (spread)
	{
		// режим round-robin: каждый новый вагон садится на следующее ядро из пула
		size_t idx = g_TrainRoundRobinIndex.fetch_add(1, std::memory_order_relaxed);
		size_t core = g_SimCoresList[idx % g_SimCoresList.size()];
		assignedCore = core;
		return SetThreadToCore(handle, core);
	}
	else
	{
		// режим общей маски: шедулер ос сам тасует потоки по разрешенным ядрам
		bool ok = SetAffinityMask(handle, g_SimThreadGroupAffinity);
		assignedCore = CurrentCPU();
		return ok;
	}
}

// установка полной маски группы для потока
bool SetAffinityMask(std::thread::native_handle_type handle, const CPU_SET& group)
{
#if defined(_WIN32)
	bool status = true;

	// на windows 11 используем современный SetThreadSelectedCpuSetMasks
	typedef BOOL(WINAPI* pFnSetThreadSelectedCpuSetMasks)(HANDLE Thread, PGROUP_AFFINITY CpuSetMasks, USHORT CpuSetMaskCount);
	static pFnSetThreadSelectedCpuSetMasks fnSetThreadSelectedCpuSetMasks = nullptr;
	static char win11API = -1;
	if (win11API == -1)
	{
		win11API = 0;
		HMODULE kernel32 = IsWindows11() ? LoadLibrary("kernel32.dll") : nullptr;
		if (kernel32)
		{
			fnSetThreadSelectedCpuSetMasks = (pFnSetThreadSelectedCpuSetMasks)GetProcAddress(kernel32, "SetThreadSelectedCpuSetMasks");
			if (fnSetThreadSelectedCpuSetMasks)
				win11API = 1;
		}
		
		ConColorMsg(Color(0, 255, 0, 255), "Turbostroi: Using WinAPI SetThreadSelectedCpuSetMasks: %s\n", win11API ? "Yes" : "No");
	}

	if (win11API)
	{
		size_t groupCount = ((g_ProcessorCount - 1) / (sizeof(KAFFINITY) * 8)) + 1;
		if (fnSetThreadSelectedCpuSetMasks(handle, (PGROUP_AFFINITY)group.data(), (USHORT)groupCount) == 0)
		{
			ConColorMsg(Color(255, 0, 255, 255), "Turbostroi: Set affinity group failed! (0x%04X)\n", GetLastError());
			status = false;
		}
	}
	else
	{
		// для windows 10 и старее применяем группу
		for (size_t i = 0; i < group.size(); i++)
		{
			// обязательно пропускаем пустые группы, иначе windows 10 падает с ошибкой 87
			if (group[i].Mask == 0)
				continue;

			if (::SetThreadGroupAffinity(handle, &group[i], nullptr) == 0)
			{
				ConColorMsg(Color(255, 0, 0, 255), "Turbostroi: Set affinity group #%u failed! (%d)\n", (unsigned)i, GetLastError());
				status = false;
			}

			// не идем дальше реального количества ядер
			if ((i + 1) * sizeof(KAFFINITY) * 8 >= g_ProcessorCount)
				break;
		}
	}

	return status;
#elif defined(POSIX)
	if (sched_setaffinity(handle, sizeof(group), &group) != 0)
	{
		ConColorMsg(Color(255, 0, 255, 255), "Turbostroi: Set affinity failed on thread! (%d)\n", errno);
		return false;
	}

	return true;
#else
	ConColorMsg(Color(255, 255, 0, 255), "Turbostroi: Set affinity not supported on your system!");
	return true;
#endif
}
