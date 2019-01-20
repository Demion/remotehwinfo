#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NON_CONFORMING_SWPRINTFS

#include <Winsock2.h>
#include <Windows.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <locale.h>

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

#pragma pack(pop)

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

const char JsonHeader[] =
"HTTP/1.1 200 OK\r\n"
"Content-Type: application/json; charset=utf-8\r\n"
"Content-Size: %d\r\n"
"Connection: close\r\n"
"Access-Control-Allow-Origin: *\r\n"
"\r\n";

const char HtmlIndexHeader[] =
"HTTP/1.1 200 OK\r\n"
"Content-Type: text/html; charset=utf-8\r\n"
"Content-Size: %d\r\n"
"Connection: close\r\n"
"Access-Control-Allow-Origin: *\r\n"
"\r\n";

const char HtmlNotFoundHeader[] =
"HTTP/1.1 404 Not Found\r\n"
"Content-Type: text/html; charset=utf-8\r\n"
"Content-Size: %d\r\n"
"Connection: close\r\n"
"Access-Control-Allow-Origin: *\r\n"
"\r\n";

const wchar_t HtmlIndexDataDefault[] =
L"<html>\n\t<head>\n\t\t<title>RemoteHWInfo</title>\n\t</head>\n\t<body>\n\t</body>\n</html>";

const wchar_t HtmlNotFoundDataDefault[] =
L"<html>\n\t<head>\n\t\t<title>RemoteHWInfo</title>\n\t</head>\n\t<body>\n\t\t404 Not Found\n\t</body>\n</html>";

char *HtmlIndexData = 0;
size_t HtmlIndexSize = 0;

char *HtmlNotFoundData = 0;
size_t HtmlNotFoundSize = 0;

const unsigned int EntryTotalCount = 1024;
bool EntryEnabled[EntryTotalCount] = {0};

unsigned int Port = 60000;

bool Hwinfo = true;

#define LOG(expression) Log(#expression, strrchr(__FILE__, '\\') + 1, __LINE__, (intptr_t) (expression))

FILE *LogFile = 0;

intptr_t Log(const char *expression, const char *fileName, unsigned int line, intptr_t result)
{
	if (LogFile)
	{
		time_t t = time(0);
		tm *local = localtime(&t);

		fprintf(LogFile, "[%02d.%02d.%04d %02d:%02d:%02d][%8s:%04d] %-102s %-20zd (0x%0*zX)\n",
				local->tm_mday, local->tm_mon + 1, local->tm_year + 1900, local->tm_hour, local->tm_min, local->tm_sec, fileName, line, expression, result, (unsigned int) sizeof(result) * 2, result);

		fflush(LogFile);
	}

	return result;
}

size_t LoadFile(const char *fileName, void **fileData)
{
	size_t readSize = 0;

	FILE *file = 0;

	LOG(file = fopen(fileName, "rb"));

	if (file)
	{
		if (LOG(fseek(file, 0, SEEK_END)) == 0)
		{
			int fileSize = 0;

			LOG(fileSize = ftell(file));

			if (fileSize > 0)
			{
				if (LOG(fseek(file, 0, SEEK_SET)) == 0)
				{
					if (fileData)
					{
						LOG(*fileData = malloc(fileSize));

						if (*fileData)
							LOG(readSize = fread(*fileData, 1, fileSize, file));
					}
				}
			}
		}

		LOG(fclose(file));
	}

	return readSize;
}

size_t UnicodeToUtf8(const wchar_t *unicode, char **utf8)
{
	size_t utf8Size = 0;

	size_t unicodeSize = wcslen(unicode);

	if (unicodeSize > 0)
	{
		LOG(utf8Size = WideCharToMultiByte(CP_UTF8, 0, unicode, (int) unicodeSize, 0, 0, 0, 0));

		if (utf8Size > 0)
		{
			if (utf8)
			{
				LOG(*utf8 = (char*) malloc(utf8Size * sizeof(char)));

				if (*utf8)
				{
					LOG(utf8Size = WideCharToMultiByte(CP_UTF8, 0, unicode, (int) unicodeSize, *utf8, (int) utf8Size, 0, 0));

					if (utf8Size == 0)
						free(*utf8);
				}
			}
		}
	}

	return utf8Size;
}

char* FormatSpecialChar(char *str)
{
	size_t length = strlen(str);

	for (unsigned int i = 0; i < length; ++i)
	{
		if ((str[i] == '\\') || (str[i] == '\"'))
		{
			for (size_t j = length; j > i; --j)
				str[j] = str[j - 1];

			str[i++] = '\\';

			++length;
		}
	}

	str[length] = '\0';

	return str;
}

bool GetHwinfo(HWINFO_SENSORS_SHARED_MEM2 *hwinfo, void **sensors, void **readings)
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
			if (hwinfo)
				memcpy(hwinfo, mapAddress, sizeof(HWINFO_SENSORS_SHARED_MEM2));

			result = true;

			if (sensors)
			{
				LOG(*sensors = malloc(hwinfo->sensorSize * hwinfo->sensorCount));

				if (*sensors)
				{
					memcpy(*sensors, (unsigned char*) mapAddress + hwinfo->sensorOffset, hwinfo->sensorSize * hwinfo->sensorCount);
				}
				else
				{
					result = false;
				}
			}

			if (result)
			{
				if (readings)
				{
					LOG(*readings = malloc(hwinfo->readingSize * hwinfo->readingCount));

					if (*readings)
					{
						memcpy(*readings, (unsigned char*) mapAddress + hwinfo->readingOffset, hwinfo->readingSize * hwinfo->readingCount);
					}
					else
					{
						if (sensors)
							free(*sensors);

						result = false;
					}
				}
			}

			LOG(UnmapViewOfFile(mapAddress));
		}

		LOG(CloseHandle(mapFile));
	}

	return result;
}

void ParseParams(const char *buffer)
{
	bool enable = false, disable = false;

	char *start = 0;

	if ((start = (char*) strstr(buffer, "?enable=")) != 0)
		enable = true;
	else if ((start = (char*) strstr(buffer, "?disable=")) != 0)
		disable = true;

	if (enable)
	{
		for (unsigned int i = 0; i < EntryTotalCount; ++i)
			EntryEnabled[i] = false;
	}
	else
	{
		for (unsigned int i = 0; i < EntryTotalCount; ++i)
			EntryEnabled[i] = true;
	}

	if ((enable) || (disable))
	{
		start = (char*) strstr(start, "=");
		char *end = (char*) strstr(start, " ");

		if ((start != 0) && (end != 0))
		{
			++start;
			*end = 0;

			while (start < end)
			{
				int sensorIndex = atoi(start);

				if (sensorIndex < EntryTotalCount)
					EntryEnabled[sensorIndex] = enable ? true : false;

				char *next = (char*) strstr(start, ",");

				if (next != 0)
					*next = 0;
				else
					next = end;

				start = next + 1;
			}
		}
	}
}

size_t CreateJson(char **jsonData)
{
	wchar_t json[100000];

	int entryIndex = 0;

	bool first = false;

	swprintf(json, L"{\n");

	if (Hwinfo)
	{
		HWINFO_SENSORS_SHARED_MEM2 hwinfo = {0};

		void *sensors = 0;
		void *readings = 0;

		if (GetHwinfo(&hwinfo, &sensors, &readings))
		{
			swprintf(json + wcslen(json),
					 L"\t\"hwinfo\":\n"
					 L"\t{\n"
					 L"\t\t\"signature\": %d,\n"
					 L"\t\t\"version\": %d,\n"
					 L"\t\t\"revision\": %d,\n"
					 L"\t\t\"pollTime\": %lld,\n"
					 L"\t\t\"sensorOffset\": %d,\n"
					 L"\t\t\"sensorSize\": %d,\n"
					 L"\t\t\"sensorCount\": %d,\n"
					 L"\t\t\"readingOffset\": %d,\n"
					 L"\t\t\"readingSize\": %d,\n"
					 L"\t\t\"readingCount\": %d,\n"
					 L"\t\t\"sensors\":\n"
					 L"\t\t[",
					 hwinfo.signature,
					 hwinfo.version,
					 hwinfo.revision,
					 hwinfo.pollTime,
					 hwinfo.sensorOffset,
					 hwinfo.sensorSize,
					 hwinfo.sensorCount,
					 hwinfo.readingOffset,
					 hwinfo.readingSize,
					 hwinfo.readingCount);

			first = true;

			for (unsigned int i = 0; i < hwinfo.sensorCount; ++i)
			{
				HWINFO_SENSORS_SENSOR *sensor = (HWINFO_SENSORS_SENSOR*) ((unsigned char*) sensors + hwinfo.sensorSize * i);

				if (EntryEnabled[entryIndex])
				{
					swprintf(json + wcslen(json),
							 L"%hs\n"
							 L"\t\t\t{\n"
							 L"\t\t\t\t\"entryIndex\": %d,\n"
							 L"\t\t\t\t\"sensorId\": %u,\n"
							 L"\t\t\t\t\"sensorInst\": %d,\n"
							 L"\t\t\t\t\"sensorNameOriginal\": \"%hs\",\n"
							 L"\t\t\t\t\"sensorNameUser\": \"%hs\"\n"
							 L"\t\t\t}",
							 first ? "" : ",",
							 entryIndex,
							 sensor->sensorId,
							 sensor->sensorInst,
							 FormatSpecialChar(sensor->sensorNameOriginal),
							 FormatSpecialChar(sensor->sensorNameUser));

					first = false;
				}

				++entryIndex;
			}

			swprintf(json + wcslen(json),
					 L"\n"
					 L"\t\t],\n"
					 L"\t\t\"readings\":\n"
					 L"\t\t[");

			first = true;

			for (unsigned int i = 0; i < hwinfo.readingCount; ++i)
			{
				HWINFO_SENSORS_READING *reading = (HWINFO_SENSORS_READING*) ((unsigned char*) readings + hwinfo.readingSize * i);

				if (EntryEnabled[entryIndex])
				{
					swprintf(json + wcslen(json),
							 L"%hs\n"
							 L"\t\t\t{\n"
							 L"\t\t\t\t\"entryIndex\": %d,\n"
							 L"\t\t\t\t\"readingType\": %d,\n"
							 L"\t\t\t\t\"sensorIndex\": %d,\n"
							 L"\t\t\t\t\"readingId\": %u,\n"
							 L"\t\t\t\t\"labelOriginal\": \"%hs\",\n"
							 L"\t\t\t\t\"labelUser\": \"%hs\",\n"
							 L"\t\t\t\t\"unit\": \"%hs\",\n"
							 L"\t\t\t\t\"value\": %lf,\n"
							 L"\t\t\t\t\"valueMin\": %lf,\n"
							 L"\t\t\t\t\"valueMax\": %lf,\n"
							 L"\t\t\t\t\"valueAvg\": %lf\n"
							 L"\t\t\t}",
							 first ? "" : ",",
							 entryIndex,
							 reading->readingType,
							 reading->sensorIndex,
							 reading->readingId,
							 FormatSpecialChar(reading->labelOriginal),
							 FormatSpecialChar(reading->labelUser),
							 FormatSpecialChar(reading->unit),
							 reading->value,
							 reading->valueMin,
							 reading->valueMax,
							 reading->valueAvg);

					first = false;
				}

				++entryIndex;
			}

			swprintf(json + wcslen(json),
					 L"\n"
					 L"\t\t]\n"
					 L"\t}");

			free(sensors);
			free(readings);
		}
	}

	swprintf(json + wcslen(json), L"\n}");

	return UnicodeToUtf8(json, jsonData);
}

unsigned long int __stdcall ClientThread(void *parameter)
{
	SOCKET clientSocket = (SOCKET) parameter;

	char buffer[100000];

	int received = 0, sent = 0;

	LOG(received = recv(clientSocket, buffer, sizeof(buffer), 0));

	if (received > 0)
	{
		buffer[received] = 0;

		printf(buffer);

		size_t size = 0;

		if ((strstr(buffer, "GET /json ") != 0) || (strstr(buffer, "GET /json?") != 0) ||
			(strstr(buffer, "GET /json.json ") != 0) || (strstr(buffer, "GET /json.json?") != 0))
		{
			ParseParams(buffer);

			char *jsonData = 0;

			size_t jsonSize = CreateJson(&jsonData);

			sprintf(buffer, JsonHeader, jsonSize);

			size = strlen(buffer);

			if (jsonSize > 0)
			{
				memcpy(buffer + size, jsonData, jsonSize);

				free(jsonData);

				size += jsonSize;
			}

			buffer[size] = 0;

			printf(JsonHeader, jsonSize);
		}
		else if ((strstr(buffer, "GET / ") != 0) || (strstr(buffer, "GET /?") != 0) ||
			(strstr(buffer, "GET /index.html ") != 0) || (strstr(buffer, "GET /index.html?") != 0))
		{
			sprintf(buffer, HtmlIndexHeader, HtmlIndexSize);

			size = strlen(buffer);

			if (HtmlIndexSize > 0)
			{
				memcpy(buffer + size, HtmlIndexData, HtmlIndexSize);

				size += HtmlIndexSize;
			}

			buffer[size] = 0;

			printf(HtmlIndexHeader, HtmlIndexSize);
		}
		else
		{
			sprintf(buffer, HtmlNotFoundHeader, HtmlNotFoundSize);

			size = strlen(buffer);

			if (HtmlNotFoundSize > 0)
			{
				memcpy(buffer + size, HtmlNotFoundData, HtmlNotFoundSize);

				size += HtmlNotFoundSize;
			}

			buffer[size] = 0;

			printf(HtmlNotFoundHeader, HtmlNotFoundSize);
		}

		LOG(sent = send(clientSocket, buffer, (int) size, 0));
	}

	LOG(shutdown(clientSocket, SD_BOTH));

	LOG(closesocket(clientSocket));

	return sent;
}

void CreateServer()
{
	printf("\n");

	LOG(setlocale(LC_CTYPE, ""));

	HtmlIndexSize = LoadFile("index.html", (void**) &HtmlIndexData);

	if (HtmlIndexSize == 0)
		HtmlIndexSize = UnicodeToUtf8(HtmlIndexDataDefault, &HtmlIndexData);

	HtmlNotFoundSize = LoadFile("404.html", (void**) &HtmlNotFoundData);

	if (HtmlNotFoundSize == 0)
		HtmlNotFoundSize = UnicodeToUtf8(HtmlNotFoundDataDefault, &HtmlNotFoundData);

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
			serverAddress.sin_port = htons(Port);

			if (LOG(bind(serverSocket, (sockaddr*) &serverAddress, sizeof(serverAddress))) == 0)
			{
				if (LOG(listen(serverSocket, SOMAXCONN)) == 0)
				{
					while (true)
					{
						SOCKET clientSocket = INVALID_SOCKET;

						LOG(clientSocket = accept(serverSocket, 0, 0));

						if (clientSocket != INVALID_SOCKET)
						{
							HANDLE clientThread = 0;

							if (LOG(clientThread = CreateThread(0, 0, ClientThread, (void*) clientSocket, 0, 0)) != 0)
								LOG(CloseHandle(clientThread));
						}
					}
				}
			}

			LOG(closesocket(serverSocket));
		}

		LOG(WSACleanup());
	}

	if (HtmlIndexData)
		free(HtmlIndexData);

	if (HtmlNotFoundData)
		free(HtmlNotFoundData);
}

void PrintUsage()
{
	printf(
		"\n"
		"Usage:\n"
		"-port (60000 = default)\n"
		"-hwinfo (0 = disable 1 = enable = default)\n"
		"-help\n"
		"\n"
		"http://ip:port/json.json (UTF-8)\n"
		"http://ip:port/json.json?enable=0,1,2,3 (0,1,2,3 = entryIndex)\n"
		"http://ip:port/json.json?disable=0,1,2,3 (0,1,2,3 = entryIndex)\n"
		"http://ip:port/index.html (UTF-8)\n"
		"http://ip:port/404.html (UTF-8)\n");
}

void ParseArgs(int argc, char *argv[])
{
	int arg = 1;

	while (arg < argc)
	{
		if (strcmp(strupr(argv[arg]), "-PORT") == 0)
		{
			if (arg + 1 < argc)
				Port = atoi(argv[++arg]);
		}
		else if (strcmp(strupr(argv[arg]), "-HWINFO") == 0)
		{
			if (arg + 1 < argc)
				Hwinfo = (atoi(argv[++arg]) != 0);
		}
		else if (strcmp(strupr(argv[arg]), "-HELP") == 0)
		{
			Hwinfo = false;

			PrintUsage();
		}

		++arg;
	}
}

int main(int argc, char *argv[])
{
	LogFile = fopen("log.txt", "a");

	if (argc > 1)
		ParseArgs(argc, argv);

	if (Hwinfo)
		CreateServer();

	if (LogFile)
		fclose(LogFile);

	return EXIT_SUCCESS;
}