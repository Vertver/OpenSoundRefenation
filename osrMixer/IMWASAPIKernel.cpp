/*********************************************************
* Copyright (C) VERTVER, 2019. All rights reserved.
* OpenSoundRefenation - WINAPI open-source DAW
* MIT-License
**********************************************************
* Module Name: OSR WASAPI
**********************************************************
* IMWASAPIKernel.cpp
* WASAPI kernel implementation
*********************************************************/
#include "stdafx.h"
#include "IMWASAPI.h"
#include <Functiondiscoverykeys_devpkey.h>

__forceinline
void
GetObjectsArray(ISoundInterfaceWASAPI* pInterface, HANDLE* pArray)
{
	HANDLE pVoid[6] = { 0 };

	pVoid[0] = pInterface->hInputStart;
	pVoid[1] = pInterface->hInputExit;

	pVoid[2] = pInterface->hOutputStart;
	pVoid[3] = pInterface->hOutputExit;

	pVoid[4] = pInterface->hInputLoadEvent;
	pVoid[5] = pInterface->hOutputLoadEvent;

	memcpy(pArray, pVoid, sizeof(void*) * 6);
}

DWORD
WINAPIV
WASAPIThreadProc(
	LPVOID pParam
)
{
	ISoundInterfaceWASAPI* pInterface = (ISoundInterfaceWASAPI*)pParam;
	WAVEFORMATEX waveFormat = ConvertToWaveFormat(pInterface->OutputHost.FormatType);
	size_t BufferSize = pInterface->OutputHost.BufferSize;
	LPVOID& lpData = pInterface->pOutputBuffer;
	HANDLE hArray[6] = { nullptr };
	DWORD dwTask = 0;
	HRESULT hr = 0;
	HANDLE hMMCSS = NULL;
	BOOL isPlaying = TRUE;

	lpData = (BYTE*)FastAlloc(BufferSize * waveFormat.nChannels * sizeof(f32));
	memset(lpData, 0, BufferSize * waveFormat.nChannels * sizeof(f32));

	hMMCSS = AvSetMmThreadCharacteristicsW(L"Pro Audio", &dwTask);
	ASSERT2(hMMCSS, L"Can't init MMCSS");
	
	AvSetMmThreadPriority(hMMCSS, AVRT_PRIORITY_CRITICAL);

	// wait than our stream was started
	WaitForSingleObject(pInterface->hOutputStart, INFINITE);

	pInterface->thread.EnterSection();
	GetObjectsArray(pInterface, hArray);
	pInterface->thread.LeaveSection();

	while (isPlaying)
	{
		BYTE* pByte = nullptr;

		switch (WaitForMultipleObjects(6, hArray, FALSE, INFINITE))
		{
		case WAIT_OBJECT_0:
			// input start
			break;
		case WAIT_OBJECT_0 + 1:
			// input exit
			break;
		case WAIT_OBJECT_0 + 2:
			// output start
			break;
		case WAIT_OBJECT_0 + 3:
			isPlaying = FALSE;
			break;
		case WAIT_OBJECT_0 + 4:
			// input load event
			break;
		case WAIT_OBJECT_0 + 5:
		{
			hr = pInterface->pRenderClient->GetBuffer(BufferSize, &pByte);
			if (hr == AUDCLNT_E_BUFFER_TOO_LARGE) { continue; }

			if (pByte)
			{
				memcpy(pByte, lpData, BufferSize * waveFormat.nChannels * sizeof(f32));
			}

			hr = pInterface->pRenderClient->ReleaseBuffer(BufferSize, 0);

			if (FAILED(hr))
			{
				isPlaying = FALSE;
			}

			ResetEvent(pInterface->hOutputLoadEvent);
		}
			break;
		default:
			isPlaying = FALSE;
			break;
		}
	}

	FREEKERNELHEAP(lpData);
	AvRevertMmThreadCharacteristics(hMMCSS);
	
	return 0;
}

ISoundInterfaceWASAPI::ISoundInterfaceWASAPI() : pInputClient(nullptr), pOutputClient(nullptr), pInputDevice(nullptr), pOutputDevice(nullptr),
pRenderClient(nullptr), pCaptureClient(nullptr)
{
	memset(&InputHost, 0, sizeof(AUDIO_HOST));
	memset(&OutputHost, 0, sizeof(AUDIO_HOST));

	hInputStart = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputStart = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hInputExit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputExit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hInputLoadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputLoadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

ISoundInterfaceWASAPI::ISoundInterfaceWASAPI(IMMDevice* pInputDevice1, IMMDevice* pOutputDevice1, IAudioClient* pInputClient1,
	IAudioClient* pOutputClient1, IAudioRenderClient* pRenderClient1, IAudioCaptureClient* pCaptureClient1,
	AUDIO_HOST InputHost1, AUDIO_HOST OutputHost1)
{
	pInputClient = pInputClient1;
	pOutputClient = pOutputClient1;

	pInputDevice = pInputDevice1;
	pOutputDevice = pOutputDevice1;

	pRenderClient = pRenderClient1;
	pCaptureClient = pCaptureClient1;

	InputHost = InputHost1;
	OutputHost = OutputHost1;

	hInputStart = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputStart = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hInputExit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputExit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hInputLoadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	hOutputLoadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

OSRCODE
ISoundInterfaceWASAPI::EnumerateAudioInput(
	AUDIO_HOST** pHostsList,
	u32& HostCounts
)
{
	UINT CountOfInputs = 0;
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;

	// create enumerator 
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndPoints);
		pEndPoints->GetCount(&CountOfInputs);
	}
	else
	{
		return MXR_OSR_NO_OUT;
	}

	HostCounts = CountOfInputs;
	if (!HostCounts) { return MXR_OSR_NO_OUT; }

	pHostsList = (AUDIO_HOST**)FastAlloc(sizeof(AUDIO_HOST*) * (CountOfInputs + 1));

	// allocate space for host list
	for (size_t i = 0; i < CountOfInputs + 1; i++)
	{
		pHostsList[i] = (AUDIO_HOST*)FastAlloc(sizeof(AUDIO_HOST));
	}

	for (size_t i = 0; i < CountOfInputs; i++)
	{
		WAVEFORMATEX waveFormat = { 0 };
		PROPVARIANT value = { 0 };
		IMMDevice* pDevice = nullptr;
		IPropertyStore* pProperty = nullptr;
		IAudioClient* pAudioClient = nullptr;

		// get device
		pEndPoints->Item(i, &pDevice);

		if (pDevice)
		{
			pHostsList[i]->DeviceNumber = i;
			pHostsList[i]->DeviceType = InputDevice;
			
			pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);
			
			// get property store for format and device name
			if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProperty)))
			{
				{
					PropVariantInit(&value);

					// get wave format
					if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
					{
						memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
					}

					PropVariantClear(&value);
				}

				{
					PropVariantInit(&value);

					// get device name
					if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
					{
						if (value.vt == VT_LPWSTR)
						{
							// we need to get size of data to allocate
							int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
							LPSTR lpNewString = nullptr;

							if (StringSize && StringSize < sizeof(STRING256))
							{
								// allocate new string at kernel heap
								lpNewString = (LPSTR)FastAlloc(++StringSize);

								// convert to UTF-8
								if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
								{
									strcpy_s(pHostsList[i]->DeviceName, sizeof(STRING256), lpNewString);
								}

								FREEKERNELHEAP(lpNewString);
							}
						}
					}

					PropVariantClear(&value);
				}

				_RELEASE(pProperty);
			}

			// we didn't need to have WAVEFORMATEX struct
			pHostsList[i]->FormatType = ConvertToSingleFormat(waveFormat);
			
			if (pAudioClient)
			{
				UINT32 HostSize = 0;
				pAudioClient->GetBufferSize(&HostSize);

				pHostsList[i]->BufferSize = HostSize;
			}
		}

		_RELEASE(pAudioClient);
		_RELEASE(pDevice);
	}

	_RELEASE(deviceEnumerator);
	_RELEASE(pEndPoints);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::EnumerateAudioOutputs(
	AUDIO_HOST** pHostsList, 
	u32& HostCounts
)
{
	UINT CountOfOutputs = 0;
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;

	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndPoints);
		pEndPoints->GetCount(&CountOfOutputs);
	}
	else
	{
		return MXR_OSR_NO_OUT;
	}

	HostCounts = CountOfOutputs;
	if (!HostCounts) { return MXR_OSR_NO_OUT; }

	pHostsList = (AUDIO_HOST**)FastAlloc(sizeof(AUDIO_HOST*) * (CountOfOutputs + 1));

	// allocate space for host list
	for (size_t i = 0; i < CountOfOutputs + 1; i++)
	{
		pHostsList[i] = (AUDIO_HOST*)FastAlloc(sizeof(AUDIO_HOST));
	}

	for (size_t i = 0; i < CountOfOutputs; i++)
	{
		WAVEFORMATEX waveFormat = { 0 };
		PROPVARIANT value = { 0 };
		IMMDevice* pDevice = nullptr;
		IPropertyStore* pProperty = nullptr;
		IAudioClient* pAudioClient = nullptr;

		// get device
		pEndPoints->Item(i, &pDevice);

		if (pDevice)
		{
			pHostsList[i]->DeviceNumber = i;
			pHostsList[i]->DeviceType = OutputDevice;

			pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);

			// get property store for format and device name
			if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProperty)))
			{
				{
					PropVariantInit(&value);

					// get wave format
					if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
					{
						memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
					}

					PropVariantClear(&value);
				}

				{
					PropVariantInit(&value);

					// get device name
					if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
					{
						if (value.vt == VT_LPWSTR)
						{
							// we need to get size of data to allocate
							int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
							LPSTR lpNewString = nullptr;

							if (StringSize && StringSize < sizeof(STRING256))
							{
								// allocate new string at kernel heap
								lpNewString = (LPSTR)FastAlloc(++StringSize);

								// convert to UTF-8
								if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
								{
									strcpy_s(pHostsList[i]->DeviceName, sizeof(STRING256), lpNewString);
								}

								FREEKERNELHEAP(lpNewString);
							}
						}
					}

					PropVariantClear(&value);
				}

				_RELEASE(pProperty);
			}

			// we didn't need to have WAVEFORMATEX struct
			pHostsList[i]->FormatType = ConvertToSingleFormat(waveFormat);

			if (pAudioClient)
			{
				UINT32 HostSize = 0;
				pAudioClient->GetBufferSize(&HostSize);

				pHostsList[i]->BufferSize = HostSize;
			}
		}

		_RELEASE(pAudioClient);
		_RELEASE(pDevice);
	}

	_RELEASE(deviceEnumerator);
	_RELEASE(pEndPoints);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetDefaultDevice(
	u32& DeviceNumber
)
{
	// not implemented now 
	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetHostDeviceOutInfo(
	u32 DeviceNumber, 
	AUDIO_HOST& HostInfo
)
{
	UINT CountOfOutputs = 0;
	WAVEFORMATEX waveFormat = { 0 };
	PROPVARIANT value = { 0 };
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;
	IPropertyStore* pProperty = nullptr;
	IAudioClient* pAudioClient = nullptr;
	IMMDevice* pDevice = nullptr;

	// create enumerator for single device object
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndPoints);
	}

	// get device
	pEndPoints->Item(DeviceNumber, &pDevice);

	if (pDevice)
	{
		HostInfo.DeviceNumber = DeviceNumber;
		HostInfo.DeviceType = InputDevice;

		pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);

		// get property store for format and device name
		if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProperty)))
		{
			{
				PropVariantInit(&value);

				// get wave format
				if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
				{
					memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
				}

				PropVariantClear(&value);
			}

			{
				PropVariantInit(&value);

				// get device name
				if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
				{
					if (value.vt == VT_LPWSTR)
					{
						// we need to get size of data to allocate
						int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
						LPSTR lpNewString = nullptr;

						if (StringSize && StringSize < sizeof(STRING256))
						{
							// allocate new string at kernel heap
							lpNewString = (LPSTR)FastAlloc(++StringSize);

							// convert to UTF-8
							if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
							{
								strcpy_s(HostInfo.DeviceName, sizeof(STRING256), lpNewString);
							}

							FREEKERNELHEAP(lpNewString);
						}
					}
				}

				PropVariantClear(&value);
			}

			_RELEASE(pProperty);
		}

		// we didn't need to have WAVEFORMATEX struct
		HostInfo.FormatType = ConvertToSingleFormat(waveFormat);

		if (pAudioClient)
		{
			UINT32 HostSize = 0;
			pAudioClient->GetBufferSize(&HostSize);

			HostInfo.BufferSize = HostSize;
		}
	}

	_RELEASE(pAudioClient);
	_RELEASE(pDevice);
	_RELEASE(deviceEnumerator);
	_RELEASE(pEndPoints);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetHostDeviceInInfo(
	u32 DeviceNumber,
	AUDIO_HOST& HostInfo
)
{
	WAVEFORMATEX waveFormat = { 0 };
	PROPVARIANT value = { 0 };
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;
	IPropertyStore* pProperty = nullptr;
	IAudioClient* pAudioClient = nullptr;
	IMMDevice* pDevice = nullptr;

	// create enumerator for single device object
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndPoints);
	}
	
	// get device
	pEndPoints->Item(DeviceNumber, &pDevice);

	if (pDevice)
	{
		HostInfo.DeviceNumber = DeviceNumber;
		HostInfo.DeviceType = InputDevice;

		pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pAudioClient);

		// get property store for format and device name
		if (SUCCEEDED(pDevice->OpenPropertyStore(STGM_READ, &pProperty)))
		{
			{
				PropVariantInit(&value);

				// get wave format
				if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
				{
					memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
				}

				PropVariantClear(&value);
			}

			{
				PropVariantInit(&value);

				// get device name
				if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
				{
					if (value.vt == VT_LPWSTR)
					{
						// we need to get size of data to allocate
						int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
						LPSTR lpNewString = nullptr;

						if (StringSize && StringSize < sizeof(STRING256))
						{
							// allocate new string at kernel heap
							lpNewString = (LPSTR)FastAlloc(++StringSize);

							// convert to UTF-8
							if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
							{
								strcpy_s(HostInfo.DeviceName, sizeof(STRING256), lpNewString);
							}

							FREEKERNELHEAP(lpNewString);
						}
					}
				}

				PropVariantClear(&value);
			}

			_RELEASE(pProperty);
		}

		// we didn't need to have WAVEFORMATEX struct
		HostInfo.FormatType = ConvertToSingleFormat(waveFormat);

		if (pAudioClient)
		{
			UINT32 HostSize = 0;
			pAudioClient->GetBufferSize(&HostSize);

			HostInfo.BufferSize = HostSize;
		}
	}

	_RELEASE(pAudioClient);
	_RELEASE(pDevice);
	_RELEASE(deviceEnumerator);
	_RELEASE(pEndPoints);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetHostDevicesOutCount(
	u32& DeviceCount
)
{
	UINT CountOfOutputs = 0;
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;

	// create enumerator for single device object
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndPoints);
		pEndPoints->GetCount(&CountOfOutputs);
		_RELEASE(pEndPoints);
		_RELEASE(deviceEnumerator);
	}

	DeviceCount = CountOfOutputs;

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetHostDevicesInCount(
	u32& DeviceCount
)
{
	UINT CountOfOutputs = 0;
	IMMDeviceEnumerator* deviceEnumerator = nullptr;
	IMMDeviceCollection* pEndPoints = nullptr;

	// create enumerator for single device object
	if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
	{
		deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndPoints);
		pEndPoints->GetCount(&CountOfOutputs);
		_RELEASE(pEndPoints);
		_RELEASE(deviceEnumerator);
	}

	DeviceCount = CountOfOutputs;

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetVolumeLevel(
	f32& Volume
)
{
	// not implemented now 
	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::CreateCaptureSoundDevice(f64 HostDelay, u32 DeviceNumber)
{
	PROPVARIANT value = { 0 };
	WAVEFORMATEX waveFormat = { 0 };
	IPropertyStore* pProperty = nullptr;
	HRESULT hr = 0;

	{
		IMMDeviceEnumerator* deviceEnumerator = nullptr;
		IMMDeviceCollection* pEndPoints = nullptr;

		// create enumerator for single device object
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
		{
			deviceEnumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &pEndPoints);

			IMMDevice* pDevice = nullptr;
			pEndPoints->Item(DeviceNumber, &pDevice);
			pDevice->QueryInterface(IID_PPV_ARGS(&pInputDevice));

			_RELEASE(pDevice);
			_RELEASE(deviceEnumerator);
			_RELEASE(pEndPoints);
		}
		else
		{
			return MXR_OSR_BAD_LIB;
		}
	}

	pInputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pInputClient);

	if (SUCCEEDED(pInputDevice->OpenPropertyStore(STGM_READ, &pProperty)))
	{
		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
			{
				memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
			}

			PropVariantClear(&value);
		}

		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
			{
				if (value.vt == VT_LPWSTR)
				{
					// we need to get size of data to allocate
					int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
					LPSTR lpNewString = nullptr;

					if (StringSize && StringSize < sizeof(STRING256))
					{
						// allocate new string at kernel heap
						lpNewString = (LPSTR)FastAlloc(++StringSize);

						// convert to UTF-8
						if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
						{
							strcpy_s(InputHost.DeviceName, sizeof(STRING256), lpNewString);
						}

						FREEKERNELHEAP(lpNewString);
					}
				}
			}

			PropVariantClear(&value);
		}

		_RELEASE(pProperty);
	}

	// copy format before mix format
	InputHost.FormatType = ConvertToSingleFormat(waveFormat);

	WAVEFORMATEX* pWave = nullptr;
	if (SUCCEEDED(pInputClient->GetMixFormat(&pWave)))
	{
		memcpy(&waveFormat, pWave, sizeof(WAVEFORMATEX));
		CoTaskMemFree(pWave);
	}

	REFERENCE_TIME refTimeMin = 0;
	REFERENCE_TIME refTimeDefault = 0;

	if (HostDelay)
	{
		refTimeDefault = HostDelay * 10000;		// HostDelay must be in milliseconds
	}
	else
	{
		// it's can be failed, if device is AC97 
		if (FAILED(pInputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
		{
			refTimeDefault = 1000000;			// default device period - 100 msecs
		}
	}

	hr = pInputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

	if (FAILED(hr))
	{
		// can be throwed on Windows 7+
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// it's can be failed, if device is AC97 
			if (FAILED(pInputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
			{
				refTimeDefault = 1000000;
			}

			hr = pInputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

			if (FAILED(hr))
			{
				_RELEASE(pInputClient);
				_RELEASE(pInputDevice);
				return MXR_OSR_UNSUPPORTED_FMT;
			};
		}
		else
		{
			_RELEASE(pInputClient);
			_RELEASE(pInputDevice);
			return MXR_OSR_UNSUPPORTED_FMT;
		}
	}

	u32 Frames = 0;
	pInputClient->GetBufferSize(&Frames);

	InputHost.BufferSize = Frames;
	InputHost.DeviceNumber = DeviceNumber;
	InputHost.DeviceType = InputDevice;

	if (FAILED(pInputClient->GetService(IID_PPV_ARGS(&pCaptureClient))))
	{
		return MXR_OSR_BAD_WFX;
	}

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::CreateRenderSoundDevice(
	f64 HostDelay,
	u32 DeviceNumber
)
{
	PROPVARIANT value = { 0 };
	WAVEFORMATEX waveFormat = { 0 };
	IPropertyStore* pProperty = nullptr;
	HRESULT hr = 0;

	{
		IMMDeviceEnumerator* deviceEnumerator = nullptr;
		IMMDeviceCollection* pEndPoints = nullptr;

		// create enumerator for single device object
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
		{
			deviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndPoints);

			IMMDevice* pDevice = nullptr;
			pEndPoints->Item(DeviceNumber, &pDevice);
			pDevice->QueryInterface(IID_PPV_ARGS(&pOutputDevice));

			_RELEASE(pDevice);
			_RELEASE(deviceEnumerator);
			_RELEASE(pEndPoints);
		}
		else
		{
			return MXR_OSR_BAD_LIB;
		}
	}

	pOutputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pOutputClient);

	if (SUCCEEDED(pOutputDevice->OpenPropertyStore(STGM_READ, &pProperty)))
	{
		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
			{
				memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
			}

			PropVariantClear(&value);
		}

		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
			{
				if (value.vt == VT_LPWSTR)
				{
					// we need to get size of data to allocate
					int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
					LPSTR lpNewString = nullptr;

					if (StringSize && StringSize < sizeof(STRING256))
					{
						// allocate new string at kernel heap
						lpNewString = (LPSTR)FastAlloc(++StringSize);

						// convert to UTF-8
						if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
						{
							strcpy_s(OutputHost.DeviceName, sizeof(STRING256), lpNewString);
						}

						FREEKERNELHEAP(lpNewString);
					}
				}
			}

			PropVariantClear(&value);
		}

		_RELEASE(pProperty);
	}

	// copy format before mix format
	OutputHost.FormatType = ConvertToSingleFormat(waveFormat);

	WAVEFORMATEX* pWave = nullptr;
	if (SUCCEEDED(pInputClient->GetMixFormat(&pWave)))
	{
		memcpy(&waveFormat, pWave, sizeof(WAVEFORMATEX));
		CoTaskMemFree(pWave);
	}

	REFERENCE_TIME refTimeMin = 0;
	REFERENCE_TIME refTimeDefault = 0;

	if (HostDelay)
	{
		refTimeDefault = HostDelay * 10000;		// HostDelay must be in milliseconds
	}
	else
	{
		// it's can be failed, if device is AC97 
		if (FAILED(pOutputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
		{
			refTimeDefault = 1000000;			// default device period - 100 msecs
		}
	}

	hr = pOutputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

	if (FAILED(hr))
	{
		// can be throwed on Windows 7+
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// it's can be failed, if device is AC97 
			if (FAILED(pOutputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
			{
				refTimeDefault = 1000000;
			}

			hr = pOutputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

			if (FAILED(hr))
			{
				_RELEASE(pOutputClient);
				_RELEASE(pOutputDevice);
				return MXR_OSR_UNSUPPORTED_FMT;
			};
		}
		else
		{
			_RELEASE(pOutputClient);
			_RELEASE(pOutputDevice);
			return MXR_OSR_UNSUPPORTED_FMT;
		}
	}

	u32 Frames = 0;
	pOutputClient->GetBufferSize(&Frames);

	OutputHost.BufferSize = Frames;
	OutputHost.DeviceNumber = DeviceNumber;
	OutputHost.DeviceType = OutputDevice;

	if (FAILED(pOutputClient->GetService(IID_PPV_ARGS(&pRenderClient))))
	{
		return MXR_OSR_BAD_WFX;
	}

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::SetDelayLevel(
	f64 HostDelay
)
{
	// not implemented now 
	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::SetVolumeLevel(
	f32 Volume
)
{
	// not implemented now 
	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::CreateCaptureDefaultSoundDevice(
	f64 HostDelay
)
{
	PROPVARIANT value = { 0 };
	WAVEFORMATEX waveFormat = { 0 };
	IPropertyStore* pProperty = nullptr;
	HRESULT hr = 0;

	{
		IMMDeviceEnumerator* deviceEnumerator = nullptr;
		IMMDeviceCollection* pEndPoints = nullptr;

		// create enumerator for single device object
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
		{
			IMMDevice* pDevice = nullptr;
			deviceEnumerator->GetDefaultAudioEndpoint(eCapture, eMultimedia, &pDevice);	
			pDevice->QueryInterface(IID_PPV_ARGS(&pInputDevice));

			_RELEASE(pDevice);
			_RELEASE(deviceEnumerator);
			_RELEASE(pEndPoints);
		}
		else
		{
			return MXR_OSR_BAD_LIB;
		}
	}

	pInputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pInputClient);

	if (SUCCEEDED(pInputDevice->OpenPropertyStore(STGM_READ, &pProperty)))
	{
		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
			{
				memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
			}

			PropVariantClear(&value);
		}

		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
			{
				if (value.vt == VT_LPWSTR)
				{
					// we need to get size of data to allocate
					int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
					LPSTR lpNewString = nullptr;

					if (StringSize && StringSize < sizeof(STRING256))
					{
						// allocate new string at kernel heap
						lpNewString = (LPSTR)FastAlloc(++StringSize);

						// convert to UTF-8
						if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
						{
							strcpy_s(InputHost.DeviceName, sizeof(STRING256), lpNewString);
						}

						FREEKERNELHEAP(lpNewString);
					}
				}
			}

			PropVariantClear(&value);
		}

		_RELEASE(pProperty);
	}

	// copy format before mix format
	InputHost.FormatType = ConvertToSingleFormat(waveFormat);

	WAVEFORMATEX* pWave = nullptr;
	if (SUCCEEDED(pInputClient->GetMixFormat(&pWave)))
	{
		memcpy(&waveFormat, pWave, sizeof(WAVEFORMATEX));
		CoTaskMemFree(pWave);
	};

	REFERENCE_TIME refTimeMin = 0;
	REFERENCE_TIME refTimeDefault = 0;

	if (HostDelay)
	{
		refTimeDefault = HostDelay * 10000;		// HostDelay must be in milliseconds
	}
	else
	{
		// it's can be failed, if device is AC97 
		if (FAILED(pInputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
		{
			refTimeDefault = 1000000;			// default device period - 100 msecs
		}
	}

	hr = pInputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

	if (FAILED(hr))
	{
		// can be throwed on Windows 7+
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// it's can be failed, if device is AC97 
			if (FAILED(pInputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
			{
				refTimeDefault = 1000000;
			}

			hr = pInputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

			if (FAILED(hr))
			{
				_RELEASE(pInputClient);
				_RELEASE(pInputDevice);
				return MXR_OSR_UNSUPPORTED_FMT;
			};
		}
		else
		{
			_RELEASE(pInputClient);
			_RELEASE(pInputDevice);
			return MXR_OSR_UNSUPPORTED_FMT;
		}
	}

	u32 Frames = 0;
	pInputClient->GetBufferSize(&Frames);

	InputHost.BufferSize = Frames;
	InputHost.DeviceNumber = -1;
	InputHost.DeviceType = InputDevice;

	if (FAILED(pInputClient->GetService(IID_PPV_ARGS(&pCaptureClient))))
	{
		return MXR_OSR_BAD_WFX;
	}

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::CreateRenderDefaultSoundDevice(f64 HostDelay)
{

	PROPVARIANT value = { 0 };
	WAVEFORMATEX waveFormat = { 0 };
	IPropertyStore* pProperty = nullptr;
	HRESULT hr = 0;

	{
		IMMDeviceEnumerator* deviceEnumerator = nullptr;
		IMMDeviceCollection* pEndPoints = nullptr;

		// create enumerator for single device object
		if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&deviceEnumerator))))
		{
			IMMDevice* pDevice = nullptr;
			deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
			pDevice->QueryInterface(IID_PPV_ARGS(&pOutputDevice));

			_RELEASE(pDevice);
			_RELEASE(deviceEnumerator);
			_RELEASE(pEndPoints);
		}
		else
		{
			return MXR_OSR_BAD_LIB;
		}
	}

	pOutputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&pOutputClient);

	if (SUCCEEDED(pOutputDevice->OpenPropertyStore(STGM_READ, &pProperty)))
	{
		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_AudioEngine_DeviceFormat, &value)))
			{
				memcpy(&waveFormat, value.blob.pBlobData, min(sizeof(WAVEFORMATEX), value.blob.cbSize));
			}

			PropVariantClear(&value);
		}

		{
			PropVariantInit(&value);

			if (SUCCEEDED(pProperty->GetValue(PKEY_Device_FriendlyName, &value)))
			{
				if (value.vt == VT_LPWSTR)
				{
					// we need to get size of data to allocate
					int StringSize = WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, nullptr, 0, NULL, NULL);
					LPSTR lpNewString = nullptr;

					if (StringSize && StringSize < sizeof(STRING256))
					{
						// allocate new string at kernel heap
						lpNewString = (LPSTR)FastAlloc(++StringSize);

						// convert to UTF-8
						if (WideCharToMultiByte(CP_UTF8, 0, value.pwszVal, -1, lpNewString, StringSize, NULL, NULL))
						{
							strcpy_s(OutputHost.DeviceName, sizeof(STRING256), lpNewString);
						}

						FREEKERNELHEAP(lpNewString);
					}
				}
			}

			PropVariantClear(&value);
		}

		_RELEASE(pProperty);
	}

	// copy format before mix format
	OutputHost.FormatType = ConvertToSingleFormat(waveFormat);

	WAVEFORMATEX* pWave = nullptr;
	if (SUCCEEDED(pInputClient->GetMixFormat(&pWave)))
	{
		memcpy(&waveFormat, pWave, sizeof(WAVEFORMATEX));
		CoTaskMemFree(pWave);
	}

	REFERENCE_TIME refTimeMin = 0;
	REFERENCE_TIME refTimeDefault = 0;

	if (HostDelay)
	{
		refTimeDefault = HostDelay * 10000;		// HostDelay must be in milliseconds
	}
	else
	{
		// it's can be failed, if device is AC97 
		if (FAILED(pOutputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
		{
			refTimeDefault = 1000000;			// default device period - 100 msecs
		}
	}

	hr = pOutputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

	if (FAILED(hr))
	{
		// can be throwed on Windows 7+
		if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
		{
			// it's can be failed, if device is AC97 
			if (FAILED(pOutputClient->GetDevicePeriod(&refTimeDefault, &refTimeMin)))
			{
				refTimeDefault = 1000000;
			}

			hr = pOutputClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, refTimeDefault, 0, &waveFormat, nullptr);

			if (FAILED(hr))
			{
				_RELEASE(pOutputClient);
				_RELEASE(pOutputDevice);
				return MXR_OSR_UNSUPPORTED_FMT;
			};
		}
		else
		{
			_RELEASE(pOutputClient);
			_RELEASE(pOutputDevice);
			return MXR_OSR_UNSUPPORTED_FMT;
		}
	}

	u32 Frames = 0;
	pOutputClient->GetBufferSize(&Frames);

	OutputHost.BufferSize = Frames;
	OutputHost.DeviceNumber = -1;
	OutputHost.DeviceType = OutputDevice;

	if (FAILED(pOutputClient->GetService(IID_PPV_ARGS(&pRenderClient))))
	{
		return MXR_OSR_BAD_WFX;
	}

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::RestartCaptureDefaultSoundDevice(
	f64 HostDelay
)
{
	OSRFAIL2(CloseCaptureSoundDevice(), L"Can't close capture device");

	return CreateCaptureDefaultSoundDevice(HostDelay);
}

OSRCODE
ISoundInterfaceWASAPI::RestartCaptureSoundDevice(
	f64 HostDelay,
	u32 DeviceNumber
)
{
	OSRFAIL2(CloseCaptureSoundDevice(), L"Can't close capture device");

	return CreateCaptureSoundDevice(HostDelay, DeviceNumber);
}

OSRCODE
ISoundInterfaceWASAPI::RestartRenderDefaultSoundDevice(
	f64 HostDelay
)
{
	OSRFAIL2(CloseRenderSoundDevice(), L"Can't close render device");

	return CreateRenderDefaultSoundDevice(HostDelay);
}

OSRCODE
ISoundInterfaceWASAPI::RestartRenderSoundDevice(
	f64 HostDelay,
	u32 DeviceNumber
)
{
	OSRFAIL2(CloseRenderSoundDevice(), L"Can't close render device");

	return CreateRenderSoundDevice(HostDelay, DeviceNumber);
}

OSRCODE
ISoundInterfaceWASAPI::CloseCaptureSoundDevice()
{
	_RELEASE(pCaptureClient);
	_RELEASE(pInputClient);
	_RELEASE(pInputDevice);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::CloseRenderSoundDevice()
{
	StopHost();

	_RELEASE(pCaptureClient);
	_RELEASE(pInputClient);
	_RELEASE(pInputDevice);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::PlayHost()
{
	if (pOutputClient) { pOutputClient->Start(); }

	ResetEvent(hOutputExit);
	SetEvent(hOutputStart);

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::StopHost()
{
	if (pOutputClient) { pOutputClient->Stop(); }

	ResetEvent(hOutputStart);
	SetEvent(hOutputExit);

	return OSR_SUCCESS;
}

int CurrentNumber = 0;
const char* CurrentString = __FILE__;

OSRCODE
ISoundInterfaceWASAPI::RecvPacket(
	void* pData, 
	PacketType Type, 
	size_t DataSize		// in bytes
)
{
	u64 SpecialData = (u64)pData;

	switch (Type)
	{
	case SoundData:
		if (DataSize > 8)
		{
			try
			{
				CurrentNumber = __LINE__; memcpy(pOutputBuffer, pData, DataSize);
			}
			catch (...)
			{
				STRING512 szString = { 0 };
				WSTRING512 szString2 = { 0 };

				_snprintf_s(
					szString,
					512,
					"The application thrown the exception at %i line. File: %s. Are you sure to continue use application?",
					CurrentNumber,
					CurrentString
				);

				for (size_t i = 0; i < 512; i++)
				{
					szString2[i] = (WCHAR)szString[i];
				}

				if (THROW2(szString2))
				{
					WMSG_LOG(L"\n");
					WMSG_LOG(szString2);
					WMSG_LOG(L"\n");
				}
				else
				{
					DestroyApplication(0x2224);
				}
			}

			// set event than we are end to load data
			SetEvent(hOutputLoadEvent);
		}
		break;
	case DeviceInputData:
		switch (InputData(SpecialData))
		{
		case InputData::AcceptData:
			// nothing there
			break;

		case InputData::DiscardData:
			// nothing there
			break;

		case InputData::FlushBuffers:
			// nothing there
			break;

		case InputData::StartRecording:
			SetEvent(hInputStart);
			ResetEvent(hInputExit);
			break;

		case InputData::StopRecording:
			SetEvent(hInputExit);
			ResetEvent(hInputStart);
			break;
		}
		break;
	case DeviceOutputData:
		switch (OutputData(SpecialData))
		{
		case OutputData::AcceptData2:
			// nothing there
			break;

		case OutputData::DiscardData2:
			// nothing there
			break;

		case OutputData::FlushBuffers2:
			memset(pOutputBuffer, 0, DataSize);

			// set event than we are end to load data
			SetEvent(hOutputLoadEvent);
			break;

		case OutputData::StartPlay2:
			ThreadId = thread.CreateUserThread(nullptr, (ThreadFunc*)(WASAPIThreadProc), (LPVOID)this, L"OSR WASAPI worker thread");
			break;

		case OutputData::StopPlay2:
			
			break;
		}

		default:
			break;
		}

	return OSR_SUCCESS;
}

OSRCODE
ISoundInterfaceWASAPI::GetPacket(
	void*& pData, 
	PacketType Type,
	size_t& DataSize
)
{
	return OSR_SUCCESS;
}

OSRCODE 
ISoundInterfaceWASAPI::GetLoadBuffer(
	void*& pData, 
	size_t& BufferSize
) 
{
	pData = pOutputBuffer;
	BufferSize = OutputBufferSize;

	return OSR_SUCCESS;
}

void 
ISoundInterfaceWASAPI::Release()
{
	StopHost();

	CloseCaptureSoundDevice();
	CloseRenderSoundDevice();

	delete this;
}

IObject* 
ISoundInterfaceWASAPI::CloneObject()
{
	return 0;/*new ISoundInterfaceWASAPI(
		pInputDevice, pOutputDevice, pInputClient, pOutputClient, pRenderClient, pCaptureClient, InputHost, OutputHost
	);*/
}

