/*********************************************************
* Copyright (C) VERTVER, 2018. All rights reserved.
* OpenSoundRefenation - WINAPI open-source DAW
* MIT-License
**********************************************************
* Module Name: OSR entry-point
**********************************************************
* osrKernel.cpp
* Kernel part
*********************************************************/
#include "stdafx.h"
#ifdef WIN32
#include "OSRWin32kernel.h"
#include "OSRSystem.h"

DLL_API HMODULE hMixer;
DLL_API HMODULE hDecoder;
DLL_API HMODULE hLibSndFile;
DLL_API HMODULE hAVCodec;
DLL_API HMODULE hAVUtil;
DLL_API HMODULE hNetwork;
DLL_API bool bNetwork;
#endif

#include "OSRConfig.h"

DWORD MainThreadId = 0;
typedef DWORD(NTAPI *DISCARD_MEMORY_POINTER) (PVOID, SIZE_T);

DWORD 
GetMainThreadId()
{
	return MainThreadId;
}

BOOL 
UnloadToPage(
	LPVOID pData,
	SIZE_T DataSize
)
{
	static DISCARD_MEMORY_POINTER pFunction = nullptr;
	static HMODULE hStaticModule = GetModuleHandleW(L"kernel32.dll");

	// don't let use local libraries
	ASSERT1(hStaticModule, L"Can't load kernel32 module");

	if (!pFunction) { pFunction = (DISCARD_MEMORY_POINTER)GetProcAddress(hStaticModule, "DiscardVirtualMemory"); }

	if (pFunction)
	{
		return pFunction(pData, DataSize);
	}

	return TRUE;
}

VOID
InitApplication()
{
#ifdef WIN32
	CreateTempDirectory();
	CreateKernelHeap();
	WSTRING_PATH szName = { 0 };	
	threadSystem.SetUserThreadName(GetCurrentThreadId(), const_cast<LPWSTR>(L"OSR Main Thread"));
	previous_filter = SetUnhandledExceptionFilter(UnhandledFilter);

	MainThreadId = GetCurrentThreadId();

	//#TODO: create here events for dynamic splash screen
	hDecoder = GetModuleHandleW(OSR_DECODER_NAME);
	if (!hDecoder) 
	{ 
		hDecoder = LoadLibraryW(OSR_DECODER_NAME);
		ASSERT1(hDecoder, L"OSR Error: Can't load 'osrDecoder.dll' library");
	}
	MSG_LOG("OSR: Decoder loaded");

	hMixer = GetModuleHandleW(OSR_MIXER_NAME);
	if (!hMixer) 
	{ 
		hMixer = LoadLibraryW(OSR_MIXER_NAME);
		ASSERT1(hMixer, L"OSR Error: Can't load 'osrMixer.dll' library");
	}
	MSG_LOG("OSR: Mixer loaded");

	hLibSndFile = GetModuleHandleW(L"libsndfile-1.dll");
	if (!hLibSndFile)
	{
		hLibSndFile = LoadLibraryW(L"libsndfile-1.dll");
		ASSERT1(hLibSndFile, L"OSR Error: Can't load 'libsndfile-1.dll' library");
	}
	MSG_LOG("OSR: libsndfile loaded");

	hNetwork = GetModuleHandleW(L"osrNetwork.dll");
	if (!hNetwork)
	{
		hNetwork = LoadLibraryW(L"osrNetwork.dll");
	}
	bNetwork = !!hNetwork;

	CoInitialize(nullptr);
	OleInitialize(nullptr);
#endif

}

VOID 
SuspendMainThread()
{
	if (GetCurrentThreadId() != GetMainThreadId())
	{
		HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, GetMainThreadId());

		if (hThread)
		{
			SuspendThread(hThread);
			CloseHandle(hThread);
		}
	}
}

VOID
ResumeMainThread()
{
	if (GetCurrentThreadId() != GetMainThreadId())
	{
		HANDLE hThread = OpenThread(THREAD_ALL_ACCESS, FALSE, GetMainThreadId());
		
		if (hThread)
		{ 
			ResumeThread(hThread); 
			CloseHandle(hThread);
		}
	}
}

VOID
DestroyApplication(DWORD dwError)
{
#ifdef WIN32
	FreeLibrary(hDecoder);
	FreeLibrary(hMixer);
	FreeLibrary(hNetwork);
	FreeLibrary(hLibSndFile);
	FreeLibrary(hAVUtil);
	FreeLibrary(hAVCodec);
	
	DestroyKernelHeap();
	ExitProcess(dwError);
#else
	exit(dwError);
#endif
}

#ifdef WIN32
VOID
ThrowCriticalError(
	LPCWSTR lpText
)
{
	MSG_LOG("Critical error. That's all, my friend.");
	MSG_LOG("--------------------------------------------");
	WMSG_LOG(lpText);
	MSG_LOG("--------------------------------------------");

	MessageBoxW(NULL, lpText, L"Kernel error", MB_OK | MB_ICONHAND);
	DestroyApplication(GetLastError());
}


BOOL
ThrowApplicationError(
	LPCWSTR lpText
)
{
	DWORD dwError = GetLastError();
	std::wstring szMsg = {};

	if (dwError)
	{
		szMsg = L"Application throw error. Are you wan't to get next?" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			FormatError(dwError);
	}
	else
	{
		szMsg = L"Application throw error. Are you wan't to get next?" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			L"No system error";

	}

	WMSG_LOG(szMsg.c_str());
	int MsgBox = MessageBoxW(NULL, szMsg.c_str(), L"Application error", MB_YESNO | MB_ICONHAND);

	if (MsgBox == IDYES) { return TRUE; }
	return FALSE;
}

BOOL
ThrowDebugError(
	LPCWSTR lpText
)
{
	DWORD dwError = GetLastError();
	std::wstring szMsg = {};
	if (dwError)
	{
		szMsg = L"Application throw debug error. Hey, that's not so bad. Continue?" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			FormatError(dwError);
	}
	else
	{
		szMsg = L"Application throw debug error. Hey, that's not so bad. Continue?" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			L"No system error";
	}

	WMSG_LOG(szMsg.c_str());
	int MsgBox = MessageBoxW(NULL, szMsg.c_str(), L"Application debug error", MB_YESNO | MB_ICONHAND);

	if (MsgBox == IDYES) { return TRUE; }
	return FALSE;
}

BOOL
ThrowWarning(
	LPCWSTR lpText
)
{
	DWORD dwError = GetLastError();

	std::wstring szMsg = {};
	if (dwError)
	{
		szMsg = L"Warning!" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			FormatError(dwError);
	}
	else
	{
		szMsg = L"Warning!" +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + lpText +
			std::wstring(L"\n") + L" " + std::wstring(L"\n") + L"Windows error: " +
			L"No system error";
	}

	WMSG_LOG(szMsg.c_str());
	int MsgBox = MessageBoxW(NULL, szMsg.c_str(), L"Warning", MB_YESNO | MB_ICONWARNING);

	if (MsgBox == IDYES) { return TRUE; }
	return FALSE;
}

BOOL
IsNetworkInstalled()
{
	return bNetwork;
}

#else
void
ThrowCriticalError(
	const char* lpText
)
{
	MSG_LOG("Critical error. That's all, my friend.");
	MSG_LOG("--------------------------------------------");
	MSG_LOG(lpText);
	MSG_LOG("--------------------------------------------");

	//#TODO: make custom message box
	exit(-1);
}

bool
ThrowApplicationError(
	const char* lpText
)
{
	std::string szMsg = {};

	szMsg = "Application throw error. Are you wan't to get next?" +
		std::string("\n") + " " + std::string("\n") + lpText;

	MSG_LOG(szMsg.c_str());
	//#TODO: make custom message box
	return false;
}

bool
ThrowDebugError(
	const char* lpText
)
{
	std::string szMsg = {};

	szMsg = "Application throw debug error. Hey, that's not so bad. Continue?" +
		std::string("\n") + " " + std::string("\n") + lpText;

	MSG_LOG(szMsg.c_str());
	//#TODO: make custom message box
	return false;
}

bool
ThrowWarning(
	const char* lpText
)
{
	std::string szMsg = {};
	szMsg = "Warning!" +
		std::string("\n") + " " + std::string("\n") + lpText;

	MSG_LOG(szMsg.c_str());
	//#TODO: make custom message box
	return false;
}
#endif
