#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS

#include <Winsock2.h>
#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <locale.h>
#include <math.h>

#include <string>
#include <vector>
#include <set>

#pragma comment(lib, "ws2_32.lib")

#pragma pack(push, 1)

struct HWINFO_SENSORS_READING
{
	unsigned int readingType;
	unsigned int sensorIndex;
	unsigned int readingId;
	char labelOriginal[128];
	char labelUser[128];
	char unit[16];
	double value;
	double valueMin;
	double valueMax;
	double valueAvg;
};

struct HWINFO_SENSORS_SENSOR
{
	unsigned int sensorId;
	unsigned int sensorInst;
	char sensorNameOriginal[128];
	char sensorNameUser[128];
};

struct HWINFO_SENSORS_SHARED_MEM2
{
	unsigned int signature;
	unsigned int version;
	unsigned int revision;
	long long int pollTime;
	unsigned int sensorOffset;
	unsigned int sensorSize;
	unsigned int sensorCount;
	unsigned int readingOffset;
	unsigned int readingSize;
	unsigned int readingCount;
};

struct GPUZ_RECORD
{
	wchar_t key[256];
	wchar_t value[256];
};

struct GPUZ_SENSOR_RECORD
{
	wchar_t name[256];
	wchar_t unit[8];
	unsigned int digits;
	double value;
};

struct GPUZ_SH_MEM
{
	unsigned int version;
	volatile int busy;
	unsigned int lastUpdate;
	GPUZ_RECORD data[128];
	GPUZ_SENSOR_RECORD sensors[128];
};

struct MAHM_SHARED_MEMORY_ENTRY
{
	char name[MAX_PATH];
	char units[MAX_PATH];
	char localName[MAX_PATH];
	char localUnits[MAX_PATH];
	char format[MAX_PATH];
	float data;
	float minLimit;
	float maxLimit;
	unsigned int flags;
};

struct MAHM_SHARED_MEMORY_HEADER
{
	unsigned int signature;
	unsigned int version;
	unsigned int headerSize;
	unsigned int entryCount;
	unsigned int entrySize;
	int time;
};

#pragma pack(pop)

#define	MAHM_SHARED_MEMORY_ENTRY_FLAG_SHOW_IN_OSD  0x00000001
#define	MAHM_SHARED_MEMORY_ENTRY_FLAG_SHOW_IN_LCD  0x00000002
#define	MAHM_SHARED_MEMORY_ENTRY_FLAG_SHOW_IN_TRAY 0x00000004

enum class HwinfoReadingType
{
	None,
	Temp,
	Voltage,
	Fan,
	Current,
	Power,
	Clock,
	Usage,
	Other
};

const wchar_t HtmlIndexDataDefault[] =
L"<html>\n\t<head>\n\t\t<title>RemoteHWInfo</title>\n\t</head>\n\t<body>\n\t</body>\n</html>";

const wchar_t HtmlNotFoundDataDefault[] =
L"<html>\n\t<head>\n\t\t<title>RemoteHWInfo</title>\n\t</head>\n\t<body>\n\t\t404 Not Found\n\t</body>\n</html>";

std::string HtmlIndexData;
std::string HtmlNotFoundData;

const size_t MaxFileSize = 16 * 1024 * 1024;
const size_t MaxRequestSize = 16 * 1024;
const unsigned int MaxEntryCount = 1000000;
const long MaxClients = 32;
const DWORD ClientTimeoutMs = 5000;
const long long MaxLogFileSize = 10 * 1024 * 1024;

unsigned int Port = 60000;

bool Hwinfo = true, Gpuz = true, Afterburner = true;
bool LogFileEnable = true;

volatile long ActiveClients = 0;

FILE *LogFile = 0;
CRITICAL_SECTION LogLock;

const char* BaseName(const char *path)
{
	const char *name = path;

	for (const char *p = path; *p; ++p)
	{
		if ((*p == '\\') || (*p == '/'))
			name = p + 1;
	}

	return name;
}

#define LOG(expression) Log(#expression, BaseName(__FILE__), __LINE__, (intptr_t) (expression))

intptr_t Log(const char *expression, const char *fileName, unsigned int line, intptr_t result)
{
	if (LogFile)
	{
		EnterCriticalSection(&LogLock);

		time_t t = time(0);
		tm local = {0};

		if (localtime_s(&local, &t) == 0)
		{
			fprintf(LogFile, "[%02d.%02d.%04d %02d:%02d:%02d][%8s:%04d] %-102s %-20zd (0x%0*zX)\n",
					local.tm_mday, local.tm_mon + 1, local.tm_year + 1900, local.tm_hour, local.tm_min, local.tm_sec, fileName, line, expression, result, (int) (sizeof(result) * 2), result);

			fflush(LogFile);
		}

		LeaveCriticalSection(&LogLock);
	}

	return result;
}

bool LoadFile(const char *fileName, std::string &data)
{
	data.clear();

	FILE *file = 0;

	LOG(file = fopen(fileName, "rb"));

	if (!file)
		return false;

	bool result = false;

	if (LOG(_fseeki64(file, 0, SEEK_END)) == 0)
	{
		long long fileSize = 0;

		LOG(fileSize = _ftelli64(file));

		if ((fileSize > 0) && ((unsigned long long) fileSize <= MaxFileSize))
		{
			if (LOG(_fseeki64(file, 0, SEEK_SET)) == 0)
			{
				data.resize((size_t) fileSize);

				size_t readSize = 0;

				LOG(readSize = fread(&data[0], 1, (size_t) fileSize, file));

				data.resize(readSize);

				result = (readSize > 0);
			}
		}
	}

	LOG(fclose(file));

	return result;
}

std::string UnicodeToUtf8(const wchar_t *unicode, size_t unicodeSize)
{
	std::string utf8;

	if ((unicode == 0) || (unicodeSize == 0) || (unicodeSize > 0x7FFFFFFF))
		return utf8;

	int size = WideCharToMultiByte(CP_UTF8, 0, unicode, (int) unicodeSize, 0, 0, 0, 0);

	if (size > 0)
	{
		utf8.resize((size_t) size);

		size = WideCharToMultiByte(CP_UTF8, 0, unicode, (int) unicodeSize, &utf8[0], size, 0, 0);

		if (size > 0)
			utf8.resize((size_t) size);
		else
			utf8.clear();
	}

	return utf8;
}

std::string AnsiToUtf8(const char *ansi, size_t ansiSize)
{
	if ((ansi == 0) || (ansiSize == 0) || (ansiSize > 0x7FFFFFFF))
		return std::string();

	int size = MultiByteToWideChar(CP_ACP, 0, ansi, (int) ansiSize, 0, 0);

	if (size <= 0)
		return std::string();

	std::wstring unicode((size_t) size, L'\0');

	size = MultiByteToWideChar(CP_ACP, 0, ansi, (int) ansiSize, &unicode[0], size);

	if (size <= 0)
		return std::string();

	return UnicodeToUtf8(unicode.c_str(), (size_t) size);
}

// Escape an UTF-8 string for use inside a JSON string.
std::string JsonEscape(const std::string &utf8)
{
	std::string out;

	out.reserve(utf8.size() + 8);

	for (size_t i = 0; i < utf8.size(); ++i)
	{
		unsigned char c = (unsigned char) utf8[i];

		switch (c)
		{
			case '\\': out += "\\\\"; break;
			case '\"': out += "\\\""; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;

			default:
				if (c < 0x20)
				{
					char buffer[8];

					sprintf(buffer, "\\u%04x", (unsigned int) c);

					out += buffer;
				}
				else
				{
					out += (char) c;
				}
				break;
		}
	}

	return out;
}

// Fixed size ANSI field (not necessarily zero terminated).
std::string EscapeAnsi(const char *str, size_t maxLength)
{
	return JsonEscape(AnsiToUtf8(str, strnlen(str, maxLength)));
}

// Fixed size wide field (not necessarily zero terminated).
std::string EscapeWide(const wchar_t *str, size_t maxLength)
{
	size_t length = 0;

	while ((length < maxLength) && (str[length] != L'\0'))
		++length;

	return JsonEscape(UnicodeToUtf8(str, length));
}

std::string FormatFloat(double value)
{
	char buffer[512];

	switch (_fpclass(value))
	{
		case _FPCLASS_NINF:
			return "\"-Infinity\"";

		case _FPCLASS_PINF:
			return "\"Infinity\"";

		case _FPCLASS_SNAN:
		case _FPCLASS_QNAN:
			return "\"NaN\"";

		default:
			_snprintf(buffer, sizeof(buffer), "%f", value);
			buffer[sizeof(buffer) - 1] = '\0';
			return buffer;
	}
}

// Simple growing output buffer with printf style append.
struct Output
{
	std::string text;

	void Append(const char *text_)
	{
		text += text_;
	}

	void Append(const std::string &text_)
	{
		text += text_;
	}

	void Format(const char *format, ...)
	{
		va_list args;

		va_start(args, format);
		int length = _vscprintf(format, args);
		va_end(args);

		if (length <= 0)
			return;

		std::vector<char> buffer((size_t) length + 1);

		va_start(args, format);
		_vsnprintf(&buffer[0], buffer.size(), format, args);
		va_end(args);

		buffer[buffer.size() - 1] = '\0';

		text.append(&buffer[0], (size_t) length);
	}
};

// Entry filter from the request (?enable=... / ?disable=...), per request.
struct EntryFilter
{
	enum Mode { All, Enable, Disable };

	Mode mode;
	std::set<unsigned int> indexes;

	EntryFilter() : mode(All) {}

	bool IsEnabled(unsigned int entryIndex) const
	{
		switch (mode)
		{
			case Enable:
				return indexes.count(entryIndex) != 0;

			case Disable:
				return indexes.count(entryIndex) == 0;

			default:
				return true;
		}
	}
};

size_t ViewSize(const void *address)
{
	MEMORY_BASIC_INFORMATION info = {0};

	if (VirtualQuery(address, &info, sizeof(info)) == 0)
		return 0;

	return info.RegionSize;
}

struct HwinfoData
{
	HWINFO_SENSORS_SHARED_MEM2 header;
	std::vector<unsigned char> sensors;
	std::vector<unsigned char> readings;
};

bool GetHwinfo(HwinfoData &data)
{
	bool result = false;

	HANDLE mapFile = 0;

	LOG(mapFile = OpenFileMappingA(FILE_MAP_READ, false, "Global\\HWiNFO_SENS_SM2"));

	if (mapFile)
	{
		void *mapAddress = 0;

		LOG(mapAddress = MapViewOfFile(mapFile, FILE_MAP_READ, 0, 0, 0));

		if (mapAddress)
		{
			unsigned long long viewSize = ViewSize(mapAddress);

			if (viewSize >= sizeof(HWINFO_SENSORS_SHARED_MEM2))
			{
				memcpy(&data.header, mapAddress, sizeof(HWINFO_SENSORS_SHARED_MEM2));

				const HWINFO_SENSORS_SHARED_MEM2 &h = data.header;

				bool valid =
					(h.sensorSize >= sizeof(HWINFO_SENSORS_SENSOR)) && (h.readingSize >= sizeof(HWINFO_SENSORS_READING)) &&
					(h.sensorCount <= MaxEntryCount) && (h.readingCount <= MaxEntryCount) &&
					((unsigned long long) h.sensorOffset + (unsigned long long) h.sensorSize * h.sensorCount <= viewSize) &&
					((unsigned long long) h.readingOffset + (unsigned long long) h.readingSize * h.readingCount <= viewSize);

				if (valid)
				{
					const unsigned char *base = (const unsigned char*) mapAddress;

					data.sensors.assign(base + h.sensorOffset, base + h.sensorOffset + (size_t) h.sensorSize * h.sensorCount);
					data.readings.assign(base + h.readingOffset, base + h.readingOffset + (size_t) h.readingSize * h.readingCount);

					result = true;
				}
			}

			LOG(UnmapViewOfFile(mapAddress));
		}

		LOG(CloseHandle(mapFile));
	}

	return result;
}

bool GetGpuz(GPUZ_SH_MEM &gpuz)
{
	bool result = false;

	memset(&gpuz, 0, sizeof(gpuz));

	HANDLE mapFile = 0;

	LOG(mapFile = OpenFileMappingA(FILE_MAP_READ, false, "GPUZShMem"));

	if (mapFile)
	{
		void *mapAddress = 0;

		LOG(mapAddress = MapViewOfFile(mapFile, FILE_MAP_READ, 0, 0, 0));

		if (mapAddress)
		{
			size_t size = ViewSize(mapAddress);

			if (size > sizeof(GPUZ_SH_MEM))
				size = sizeof(GPUZ_SH_MEM);

			if (size >= offsetof(GPUZ_SH_MEM, data))
			{
				// Wait (shortly) until GPU-Z has finished updating the data.
				for (int attempt = 0; attempt < 10; ++attempt)
				{
					memcpy(&gpuz, mapAddress, size);

					if (gpuz.busy == 0)
						break;

					Sleep(2);
				}

				// Guarantee zero terminated strings.
				for (int i = 0; i < 128; ++i)
				{
					gpuz.data[i].key[255] = L'\0';
					gpuz.data[i].value[255] = L'\0';
					gpuz.sensors[i].name[255] = L'\0';
					gpuz.sensors[i].unit[7] = L'\0';
				}

				result = true;
			}

			LOG(UnmapViewOfFile(mapAddress));
		}

		LOG(CloseHandle(mapFile));
	}

	return result;
}

struct AfterburnerData
{
	MAHM_SHARED_MEMORY_HEADER header;
	std::vector<unsigned char> entries;
};

bool GetAfterburner(AfterburnerData &data)
{
	bool result = false;

	HANDLE mapFile = 0;

	LOG(mapFile = OpenFileMappingA(FILE_MAP_READ, false, "MAHMSharedMemory"));

	if (mapFile)
	{
		void *mapAddress = 0;

		LOG(mapAddress = MapViewOfFile(mapFile, FILE_MAP_READ, 0, 0, 0));

		if (mapAddress)
		{
			unsigned long long viewSize = ViewSize(mapAddress);

			if (viewSize >= sizeof(MAHM_SHARED_MEMORY_HEADER))
			{
				memcpy(&data.header, mapAddress, sizeof(MAHM_SHARED_MEMORY_HEADER));

				const MAHM_SHARED_MEMORY_HEADER &h = data.header;

				bool valid =
					(h.entrySize >= sizeof(MAHM_SHARED_MEMORY_ENTRY)) && (h.entryCount <= MaxEntryCount) &&
					((unsigned long long) h.headerSize + (unsigned long long) h.entrySize * h.entryCount <= viewSize);

				if (valid)
				{
					const unsigned char *base = (const unsigned char*) mapAddress;

					data.entries.assign(base + h.headerSize, base + h.headerSize + (size_t) h.entrySize * h.entryCount);

					result = true;
				}
			}

			LOG(UnmapViewOfFile(mapAddress));
		}

		LOG(CloseHandle(mapFile));
	}

	return result;
}

// Parse "enable=1,2,3" / "disable=1,2,3" from the query string.
void ParseQuery(const std::string &query, EntryFilter &filter)
{
	size_t pos = 0;

	while (pos <= query.size())
	{
		size_t amp = query.find('&', pos);

		if (amp == std::string::npos)
			amp = query.size();

		std::string param = query.substr(pos, amp - pos);

		pos = amp + 1;

		EntryFilter::Mode mode = EntryFilter::All;
		std::string list;

		if (param.compare(0, 7, "enable=") == 0)
		{
			mode = EntryFilter::Enable;
			list = param.substr(7);
		}
		else if (param.compare(0, 8, "disable=") == 0)
		{
			mode = EntryFilter::Disable;
			list = param.substr(8);
		}
		else
		{
			continue;
		}

		// "enable" takes precedence over "disable" like before.
		if ((filter.mode == EntryFilter::Enable) && (mode == EntryFilter::Disable))
			continue;

		if ((filter.mode == EntryFilter::Disable) && (mode == EntryFilter::Enable))
			filter.indexes.clear();

		filter.mode = mode;

		size_t i = 0;

		while (i < list.size())
		{
			size_t comma = list.find(',', i);

			if (comma == std::string::npos)
				comma = list.size();

			bool digits = (comma > i);
			unsigned long long value = 0;

			for (size_t j = i; j < comma; ++j)
			{
				if ((list[j] < '0') || (list[j] > '9'))
				{
					digits = false;
					break;
				}

				value = value * 10 + (unsigned long long) (list[j] - '0');

				if (value > 0xFFFFFFFFull)
				{
					digits = false;
					break;
				}
			}

			if (digits)
				filter.indexes.insert((unsigned int) value);

			i = comma + 1;
		}
	}
}

std::string CreateJson(const EntryFilter &filter)
{
	Output out;

	unsigned int entryIndex = 0;

	bool first = false;
	bool first_output = true;

	out.Append("{\n");

	if (Hwinfo)
	{
		HwinfoData hwinfo = {};

		if (GetHwinfo(hwinfo))
		{
			const HWINFO_SENSORS_SHARED_MEM2 &h = hwinfo.header;

			first_output = false;

			out.Format(
				"\t\"hwinfo\":\n"
				"\t{\n"
				"\t\t\"signature\": %u,\n"
				"\t\t\"version\": %u,\n"
				"\t\t\"revision\": %u,\n"
				"\t\t\"pollTime\": %lld,\n"
				"\t\t\"sensorOffset\": %u,\n"
				"\t\t\"sensorSize\": %u,\n"
				"\t\t\"sensorCount\": %u,\n"
				"\t\t\"readingOffset\": %u,\n"
				"\t\t\"readingSize\": %u,\n"
				"\t\t\"readingCount\": %u,\n"
				"\t\t\"sensors\":\n"
				"\t\t[",
				h.signature, h.version, h.revision, h.pollTime, h.sensorOffset, h.sensorSize,
				h.sensorCount, h.readingOffset, h.readingSize, h.readingCount);

			first = true;

			for (unsigned int i = 0; i < h.sensorCount; ++i)
			{
				const HWINFO_SENSORS_SENSOR *sensor = (const HWINFO_SENSORS_SENSOR*) (&hwinfo.sensors[0] + (size_t) h.sensorSize * i);

				if (filter.IsEnabled(entryIndex))
				{
					out.Format(
						"%s\n"
						"\t\t\t{\n"
						"\t\t\t\t\"entryIndex\": %u,\n"
						"\t\t\t\t\"sensorId\": %u,\n"
						"\t\t\t\t\"sensorInst\": %u,\n"
						"\t\t\t\t\"sensorNameOriginal\": \"%s\",\n"
						"\t\t\t\t\"sensorNameUser\": \"%s\"\n"
						"\t\t\t}",
						first ? "" : ",",
						entryIndex,
						sensor->sensorId,
						sensor->sensorInst,
						EscapeAnsi(sensor->sensorNameOriginal, sizeof(sensor->sensorNameOriginal)).c_str(),
						EscapeAnsi(sensor->sensorNameUser, sizeof(sensor->sensorNameUser)).c_str());

					first = false;
				}

				++entryIndex;
			}

			out.Append(
				"\n"
				"\t\t],\n"
				"\t\t\"readings\":\n"
				"\t\t[");

			first = true;

			for (unsigned int i = 0; i < h.readingCount; ++i)
			{
				const HWINFO_SENSORS_READING *reading = (const HWINFO_SENSORS_READING*) (&hwinfo.readings[0] + (size_t) h.readingSize * i);

				if (filter.IsEnabled(entryIndex))
				{
					out.Format(
						"%s\n"
						"\t\t\t{\n"
						"\t\t\t\t\"entryIndex\": %u,\n"
						"\t\t\t\t\"readingType\": %u,\n"
						"\t\t\t\t\"sensorIndex\": %u,\n"
						"\t\t\t\t\"readingId\": %u,\n"
						"\t\t\t\t\"labelOriginal\": \"%s\",\n"
						"\t\t\t\t\"labelUser\": \"%s\",\n"
						"\t\t\t\t\"unit\": \"%s\",\n"
						"\t\t\t\t\"value\": %s,\n"
						"\t\t\t\t\"valueMin\": %s,\n"
						"\t\t\t\t\"valueMax\": %s,\n"
						"\t\t\t\t\"valueAvg\": %s\n"
						"\t\t\t}",
						first ? "" : ",",
						entryIndex,
						reading->readingType,
						reading->sensorIndex,
						reading->readingId,
						EscapeAnsi(reading->labelOriginal, sizeof(reading->labelOriginal)).c_str(),
						EscapeAnsi(reading->labelUser, sizeof(reading->labelUser)).c_str(),
						EscapeAnsi(reading->unit, sizeof(reading->unit)).c_str(),
						FormatFloat(reading->value).c_str(),
						FormatFloat(reading->valueMin).c_str(),
						FormatFloat(reading->valueMax).c_str(),
						FormatFloat(reading->valueAvg).c_str());

					first = false;
				}

				++entryIndex;
			}

			out.Append(
				"\n"
				"\t\t]\n"
				"\t}");
		}
	}

	if (Gpuz)
	{
		GPUZ_SH_MEM *gpuz = new GPUZ_SH_MEM;

		if (GetGpuz(*gpuz))
		{
			if (first_output)
				first_output = false;
			else
				out.Append(",\n");

			out.Format(
				"\t\"gpuz\":\n"
				"\t{\n"
				"\t\t\"version\": %u,\n"
				"\t\t\"busy\": %d,\n"
				"\t\t\"lastUpdate\": %u,\n"
				"\t\t\"data\":\n"
				"\t\t[",
				gpuz->version,
				(int) gpuz->busy,
				gpuz->lastUpdate);

			first = true;

			for (unsigned int i = 0; i < 128; ++i)
			{
				if (gpuz->data[i].key[0] != L'\0')
				{
					if (filter.IsEnabled(entryIndex))
					{
						out.Format(
							"%s\n"
							"\t\t\t{\n"
							"\t\t\t\t\"entryIndex\": %u,\n"
							"\t\t\t\t\"key\": \"%s\",\n"
							"\t\t\t\t\"value\": \"%s\"\n"
							"\t\t\t}",
							first ? "" : ",",
							entryIndex,
							EscapeWide(gpuz->data[i].key, 256).c_str(),
							EscapeWide(gpuz->data[i].value, 256).c_str());

						first = false;
					}

					++entryIndex;
				}
			}

			out.Append(
				"\n"
				"\t\t],\n"
				"\t\t\"sensors\":\n"
				"\t\t[");

			first = true;

			for (unsigned int i = 0; i < 128; ++i)
			{
				if (gpuz->sensors[i].name[0] != L'\0')
				{
					if (filter.IsEnabled(entryIndex))
					{
						out.Format(
							"%s\n"
							"\t\t\t{\n"
							"\t\t\t\t\"entryIndex\": %u,\n"
							"\t\t\t\t\"name\": \"%s\",\n"
							"\t\t\t\t\"unit\": \"%s\",\n"
							"\t\t\t\t\"digits\": %u,\n"
							"\t\t\t\t\"value\": %s\n"
							"\t\t\t}",
							first ? "" : ",",
							entryIndex,
							EscapeWide(gpuz->sensors[i].name, 256).c_str(),
							EscapeWide(gpuz->sensors[i].unit, 8).c_str(),
							gpuz->sensors[i].digits,
							FormatFloat(gpuz->sensors[i].value).c_str());

						first = false;
					}

					++entryIndex;
				}
			}

			out.Append(
				"\n"
				"\t\t]\n"
				"\t}");
		}

		delete gpuz;
	}

	if (Afterburner)
	{
		AfterburnerData afterburner = {};

		if (GetAfterburner(afterburner))
		{
			const MAHM_SHARED_MEMORY_HEADER &h = afterburner.header;

			if (first_output)
				first_output = false;
			else
				out.Append(",\n");

			out.Format(
				"\t\"afterburner\":\n"
				"\t{\n"
				"\t\t\"signature\": %u,\n"
				"\t\t\"version\": %u,\n"
				"\t\t\"headerSize\": %u,\n"
				"\t\t\"entryCount\": %u,\n"
				"\t\t\"entrySize\": %u,\n"
				"\t\t\"time\": %d,\n"
				"\t\t\"entries\":\n"
				"\t\t[",
				h.signature, h.version, h.headerSize, h.entryCount, h.entrySize, h.time);

			first = true;

			for (unsigned int i = 0; i < h.entryCount; ++i)
			{
				const MAHM_SHARED_MEMORY_ENTRY *entry = (const MAHM_SHARED_MEMORY_ENTRY*) (&afterburner.entries[0] + (size_t) h.entrySize * i);

				if (filter.IsEnabled(entryIndex))
				{
					unsigned int digits = 0;

					if (strnlen(entry->format, sizeof(entry->format)) > 2)
						digits = (unsigned int) atoi(entry->format + 2);

					out.Format(
						"%s\n"
						"\t\t\t{\n"
						"\t\t\t\t\"entryIndex\": %u,\n"
						"\t\t\t\t\"name\": \"%s\",\n"
						"\t\t\t\t\"units\": \"%s\",\n"
						"\t\t\t\t\"localName\": \"%s\",\n"
						"\t\t\t\t\"localUnits\": \"%s\",\n"
						"\t\t\t\t\"digits\": %u,\n"
						"\t\t\t\t\"data\": %s,\n"
						"\t\t\t\t\"minLimit\": %s,\n"
						"\t\t\t\t\"maxLimit\": %s,\n"
						"\t\t\t\t\"flags\": %u\n"
						"\t\t\t}",
						first ? "" : ",",
						entryIndex,
						EscapeAnsi(entry->name, sizeof(entry->name)).c_str(),
						EscapeAnsi(entry->units, sizeof(entry->units)).c_str(),
						EscapeAnsi(entry->localName, sizeof(entry->localName)).c_str(),
						EscapeAnsi(entry->localUnits, sizeof(entry->localUnits)).c_str(),
						digits,
						FormatFloat((double) entry->data).c_str(),
						FormatFloat((double) entry->minLimit).c_str(),
						FormatFloat((double) entry->maxLimit).c_str(),
						entry->flags);

					first = false;
				}

				++entryIndex;
			}

			out.Append(
				"\n"
				"\t\t]\n"
				"\t}");
		}
	}

	out.Append("\n}");

	return out.text;
}

std::string MakeResponse(int code, const char *status, const char *contentType, const std::string &body)
{
	char header[512];

	_snprintf(header, sizeof(header),
		"HTTP/1.1 %d %s\r\n"
		"Content-Type: %s\r\n"
		"Content-Length: %Iu\r\n"
		"Cache-Control: no-store\r\n"
		"Connection: close\r\n"
		"Access-Control-Allow-Origin: *\r\n"
		"\r\n",
		code, status, contentType, (size_t) body.size());

	header[sizeof(header) - 1] = '\0';

	return std::string(header) + body;
}

bool SendAll(SOCKET socket, const std::string &data)
{
	size_t total = 0;

	while (total < data.size())
	{
		size_t chunk = data.size() - total;

		if (chunk > 0x100000)
			chunk = 0x100000;

		int sent = send(socket, data.data() + total, (int) chunk, 0);

		if (sent <= 0)
			return false;

		total += (size_t) sent;
	}

	return true;
}

unsigned long int __stdcall ClientThread(void *parameter)
{
	SOCKET clientSocket = (SOCKET) parameter;

	DWORD timeout = ClientTimeoutMs;

	setsockopt(clientSocket, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout));
	setsockopt(clientSocket, SOL_SOCKET, SO_SNDTIMEO, (const char*) &timeout, sizeof(timeout));

	// Read the request header (until the empty line, limited in size).
	std::string request;

	char chunk[2048];

	while (request.size() < MaxRequestSize)
	{
		int received = recv(clientSocket, chunk, sizeof(chunk), 0);

		if (received <= 0)
			break;

		request.append(chunk, (size_t) received);

		if ((request.find("\r\n\r\n") != std::string::npos) || (request.find("\n\n") != std::string::npos))
			break;
	}

	LOG(request.size());

	if (!request.empty())
	{
		// Request line: METHOD SP TARGET SP VERSION
		size_t lineEnd = request.find_first_of("\r\n");

		std::string line = request.substr(0, lineEnd);

		printf("%s\n", JsonEscape(line).c_str());

		std::string method, target;

		size_t space1 = line.find(' ');

		if (space1 != std::string::npos)
		{
			method = line.substr(0, space1);

			size_t space2 = line.find(' ', space1 + 1);

			target = line.substr(space1 + 1, (space2 == std::string::npos) ? std::string::npos : space2 - space1 - 1);
		}

		std::string path = target, query;

		size_t question = target.find('?');

		if (question != std::string::npos)
		{
			path = target.substr(0, question);
			query = target.substr(question + 1);
		}

		std::string response;

		if (method == "GET" && (path == "/json" || path == "/json.json"))
		{
			EntryFilter filter;

			ParseQuery(query, filter);

			response = MakeResponse(200, "OK", "application/json; charset=utf-8", CreateJson(filter));
		}
		else if (method == "GET" && (path == "/" || path == "/index.html"))
		{
			response = MakeResponse(200, "OK", "text/html; charset=utf-8", HtmlIndexData);
		}
		else
		{
			response = MakeResponse(404, "Not Found", "text/html; charset=utf-8", HtmlNotFoundData);
		}

		LOG(SendAll(clientSocket, response));
	}

	LOG(shutdown(clientSocket, SD_BOTH));

	LOG(closesocket(clientSocket));

	InterlockedDecrement(&ActiveClients);

	return 0;
}

void CreateServer()
{
	printf("\n");

	LOG(setlocale(LC_CTYPE, ""));

	if (!LoadFile("index.html", HtmlIndexData))
		HtmlIndexData = UnicodeToUtf8(HtmlIndexDataDefault, wcslen(HtmlIndexDataDefault));

	if (!LoadFile("404.html", HtmlNotFoundData))
		HtmlNotFoundData = UnicodeToUtf8(HtmlNotFoundDataDefault, wcslen(HtmlNotFoundDataDefault));

	WSADATA wsaData = {0};

	if (LOG(WSAStartup(MAKEWORD(2, 2), &wsaData)) == 0)
	{
		SOCKET serverSocket = INVALID_SOCKET;

		LOG(serverSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));

		if (serverSocket != INVALID_SOCKET)
		{
			struct sockaddr_in serverAddress = {0};

			serverAddress.sin_family = AF_INET;
			serverAddress.sin_addr.S_un.S_addr = INADDR_ANY;
			serverAddress.sin_port = htons((unsigned short) Port);

			if (LOG(bind(serverSocket, (sockaddr*) &serverAddress, sizeof(serverAddress))) != 0)
			{
				printf("Error: bind to port %u failed (WSA error %d). Port already in use?\n", Port, WSAGetLastError());
			}
			else if (LOG(listen(serverSocket, SOMAXCONN)) != 0)
			{
				printf("Error: listen failed (WSA error %d).\n", WSAGetLastError());
			}
			else
			{
				printf("Listening on port %u\n", Port);

				while (true)
				{
					SOCKET clientSocket = INVALID_SOCKET;

					LOG(clientSocket = accept(serverSocket, 0, 0));

					if (clientSocket == INVALID_SOCKET)
					{
						Sleep(10);

						continue;
					}

					// Limit the number of simultaneous clients.
					if (InterlockedIncrement(&ActiveClients) > MaxClients)
					{
						InterlockedDecrement(&ActiveClients);

						SendAll(clientSocket, MakeResponse(503, "Service Unavailable", "text/plain; charset=utf-8", "503 Service Unavailable"));

						shutdown(clientSocket, SD_BOTH);
						closesocket(clientSocket);

						continue;
					}

					HANDLE clientThread = 0;

					LOG(clientThread = CreateThread(0, 0, ClientThread, (void*) clientSocket, 0, 0));

					if (clientThread != 0)
					{
						LOG(CloseHandle(clientThread));
					}
					else
					{
						InterlockedDecrement(&ActiveClients);

						closesocket(clientSocket);
					}
				}
			}

			LOG(closesocket(serverSocket));
		}

		LOG(WSACleanup());
	}
}

void PrintUsage()
{
	printf(
		"\n"
		"Usage:\n"
		"-port (60000 = default)\n"
		"-hwinfo (0 = disable; 1 = enable = default)\n"
		"-gpuz (0 = disable; 1 = enable = default)\n"
		"-afterburner (0 = disable; 1 = enable = default)\n"
		"-log (0 = disable; 1 = enable = default)\n"
		"-help\n"
		"\n"
		"http://ip:port/json.json (UTF-8)\n"
		"http://ip:port/json.json?enable=0,1,2,3 (0,1,2,3 = entryIndex)\n"
		"http://ip:port/json.json?disable=0,1,2,3 (0,1,2,3 = entryIndex)\n"
		"http://ip:port/index.html (UTF-8)\n"
		"http://ip:port/404.html (UTF-8)\n");
}

// Returns false if the program should exit right away (-help or invalid arguments).
bool ParseArgs(int argc, char *argv[])
{
	int arg = 1;

	while (arg < argc)
	{
		const char *name = argv[arg];

		if (_stricmp(name, "-port") == 0)
		{
			if (arg + 1 < argc)
			{
				long port = atol(argv[++arg]);

				if ((port < 1) || (port > 65535))
				{
					printf("Error: invalid port \"%s\" (1-65535).\n", argv[arg]);

					return false;
				}

				Port = (unsigned int) port;
			}
		}
		else if (_stricmp(name, "-hwinfo") == 0)
		{
			if (arg + 1 < argc)
				Hwinfo = (atoi(argv[++arg]) != 0);
		}
		else if (_stricmp(name, "-gpuz") == 0)
		{
			if (arg + 1 < argc)
				Gpuz = (atoi(argv[++arg]) != 0);
		}
		else if (_stricmp(name, "-afterburner") == 0)
		{
			if (arg + 1 < argc)
				Afterburner = (atoi(argv[++arg]) != 0);
		}
		else if (_stricmp(name, "-log") == 0)
		{
			if (arg + 1 < argc)
				LogFileEnable = (atoi(argv[++arg]) != 0);
		}
		else if (_stricmp(name, "-help") == 0)
		{
			PrintUsage();

			return false;
		}

		++arg;
	}

	return true;
}

int main(int argc, char *argv[])
{
	if ((argc > 1) && (!ParseArgs(argc, argv)))
		return EXIT_SUCCESS;

	if ((Hwinfo) || (Gpuz) || (Afterburner))
	{
		InitializeCriticalSection(&LogLock);

		if (LogFileEnable)
		{
			// Simple log rotation: start over if the old log became too big.
			FILE *old = fopen("log.txt", "rb");

			bool truncate = false;

			if (old)
			{
				if ((_fseeki64(old, 0, SEEK_END) == 0) && (_ftelli64(old) > MaxLogFileSize))
					truncate = true;

				fclose(old);
			}

			LogFile = fopen("log.txt", truncate ? "w" : "a");
		}

		CreateServer();

		if (LogFile)
		{
			fclose(LogFile);

			LogFile = 0;
		}

		DeleteCriticalSection(&LogLock);
	}

	return EXIT_SUCCESS;
}
