// Copyright 2008-2018 Yolo Technologies, Inc. All Rights Reserved. https://www.comblockengine.com

#include "loginapp.h"
#include "clientsdk_downloader.h"
#include "helper/sys_info.h"
#include "server/serverconfig.h"

#include "client_lib/client_interface.h"

namespace KBEngine{	


//-------------------------------------------------------------------------------------
ClientSDKDownloader::ClientSDKDownloader(Network::NetworkInterface & networkInterface, const Network::Address& addr, size_t clientWindowSize,
	const std::string& assetsPath, const std::string& binPath, const std::string& options) :
networkInterface_(networkInterface),
addr_(addr),
datas_(NULL),
datasize_(0),
sentSize_(0),
clientWindowSize_(clientWindowSize),
assetsPath_(assetsPath),
binPath_(binPath),
options_(options),
lastTime_(timestamp()),
startTime_(timestamp()),
pid_(0)
{
	Loginapp::getSingleton().networkInterface().dispatcher().addTask(this);

	genSDK();
}

//-------------------------------------------------------------------------------------
ClientSDKDownloader::~ClientSDKDownloader()
{
	DEBUG_MSG(fmt::format("ClientSDKDownloader::~ClientSDKDownloader(): sent {}/{} bytes! addr={}\n", sentSize_, datasize_, addr_.c_str()));

	if (datas_)
		free(datas_);

	if (sentSize_ < datasize_)
		Loginapp::getSingleton().networkInterface().dispatcher().cancelTask(this);
}

//-------------------------------------------------------------------------------------
#if KBE_PLATFORM == PLATFORM_WIN32
DWORD ClientSDKDownloader::startWindowsProcessGenSDK(const std::string& zipfile)
{
	startTime_ = timestamp();

	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	std::string str = binPath_;
	str += "/kbcmd.exe";

	// 用双引号把命令行括起来，以避免路径中存在空格，从而执行错误
	str = "\"" + str + "\"";

	// 増加参数
	str += fmt::format(" --clientsdk={} --zip={}", options_, zipfile);

	wchar_t* szCmdline = KBEngine::strutil::char2wchar(str.c_str());

	// 使用machine当前的工作目录作为新进程的工作目录，
	// 为一些与相对目录的文件操作操作一致的工作目录（如日志）
	wchar_t currdir[1024];
	GetCurrentDirectory(sizeof(currdir), currdir);

	ZeroMemory(&si, sizeof(si));
	si.cb = sizeof(si);
	ZeroMemory(&pi, sizeof(pi));

	if (!CreateProcess(NULL,   // No module name (use command line)
		szCmdline,      // Command line
		NULL,           // Process handle not inheritable
		NULL,           // Thread handle not inheritable
		FALSE,          // Set handle inheritance to FALSE
		CREATE_NEW_CONSOLE,    // No creation flags
		NULL,           // Use parent's environment block
		currdir,        // Use parent's starting directory
		&si,            // Pointer to STARTUPINFO structure
		&pi)           // Pointer to PROCESS_INFORMATION structure
		)
	{
		ERROR_MSG(fmt::format("Machine::startWindowsProcess: CreateProcess failed ({}).\n",
			GetLastError()));

		return 0;
	}

	free(szCmdline);

	return pi.dwProcessId;
}

//-------------------------------------------------------------------------------------
#else
uint16 ClientSDKDownloader::starLinuxProcessGenSDK(const std::string& zipfile)
{
	uint16 childpid;

	if ((childpid = fork()) == 0)
	{
		std::string cmdLine = binPath_ + "kbcmd";

		// 改变当前目录，以让出问题的时候core能在此处生成
		//chdir(bin_path.c_str());

		const char *argv[6];
		const char **pArgv = argv;
		std::string arg1 = fmt::format("--clientsdk={}", options_);
		std::string arg2 = fmt::format("--zip={}", zipfile);

		*pArgv++ = cmdLine.c_str();
		*pArgv++ = arg1.c_str();
		*pArgv++ = arg2.c_str();
		*pArgv = NULL;

		DebugHelper::getSingleton().closeLogger();
		int result = execv(cmdLine.c_str(), (char * const *)argv);

		if (result == -1)
		{
			ERROR_MSG(fmt::format("Machine::starLinuxProcessGenSDK: Failed to exec '{}'\n", cmdLine));
		}

		exit(1);
		return 0;
	}
	else
		return childpid;

	return 0;
}
#endif

//-------------------------------------------------------------------------------------
void ClientSDKDownloader::genSDK()
{
	if (clientWindowSize_ <= 0)
		clientWindowSize_ = 1024;

	std::string zipfile = fmt::format("{}/_tmp/{}.zip", assetsPath_, options_);

	remove(zipfile.c_str());

#if KBE_PLATFORM == PLATFORM_WIN32
	pid_ = startWindowsProcessGenSDK(zipfile);
#else
	pid_ = starLinuxProcessGenSDK(zipfile);
#endif

	if (pid_ <= 0)
		ERROR_MSG(fmt::format("ClientSDKDownloader::genSDK({}): system() error!\n", zipfile));
}

//-------------------------------------------------------------------------------------
bool ClientSDKDownloader::loadSDKDatas()
{
	uint64 now = timestamp();
	if (TimeStamp::toSeconds(now - lastTime_) <= 0.1)
	{
		lastTime_ = now;
		return false;
	}

	// 必须kbcmd进程已经结束
	SystemInfo::PROCESS_INFOS sysinfos = SystemInfo::getSingleton().getProcessInfo(pid_);
	if (!sysinfos.error)
		return false;

	std::string zipfile = fmt::format("{}/_tmp/{}.zip", assetsPath_, options_);

	FILE* f = fopen(zipfile.c_str(), "r+");
	if (f == NULL)
	{
		return false;
	}

	fseek(f, 0, SEEK_END);
	datasize_ = ftell(f);
	rewind(f);

	if (datasize_ > 0)
	{
		datas_ = (uint8*)malloc(datasize_ + 1);

		if (fread(datas_, 1, datasize_, f) != (size_t)datasize_)
		{
		}
	}

	fclose(f);

	if (clientWindowSize_ > datasize_)
		clientWindowSize_ = datasize_;

	return true;
}

//-------------------------------------------------------------------------------------
bool ClientSDKDownloader::process()
{
	uint64 now = timestamp();
	if (TimeStamp::toSeconds(now - startTime_) > 60.0)
	{
		if (!datas_)
		{
			std::string zipfile = fmt::format("{}/_tmp/{}.zip", assetsPath_, options_);
			ERROR_MSG(fmt::format("ClientSDKDownloader::loadSDKDatas(): open {} error!\n", zipfile));
		}

		ERROR_MSG(fmt::format("ClientSDKDownloader::process(): timeout!\n"));
		delete this;
		return false;
	}

	if (pid_ <= 0)
	{
		return true;
	}

	if (!datas_)
	{
		if (!loadSDKDatas())
			return true;
	}

	Network::Channel * pChannel = networkInterface_.findChannel(addr_);

	if (pChannel && sentSize_ < datasize_)
	{
		Network::Bundle* pNewBundle = Network::Bundle::createPoolObject(OBJECTPOOL_POINT);
		pNewBundle->newMessage(ClientInterface::onImportClientSDK);
		(*pNewBundle) << (int)datasize_;

		pNewBundle->appendBlob(datas_, clientWindowSize_);
		pChannel->send(pNewBundle);

		sentSize_ += clientWindowSize_;
		if (clientWindowSize_ > datasize_ - sentSize_)
			clientWindowSize_ = datasize_ - sentSize_;

		return true;
	}

	delete this;
	return false;
}

//-------------------------------------------------------------------------------------

}
