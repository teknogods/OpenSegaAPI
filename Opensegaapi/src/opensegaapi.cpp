/*
* This file is part of the OpenParrot project - https://teknoparrot.com / https://github.com/teknogods
*
* See LICENSE and MENTIONS in the root of the source tree for information
* regarding licensing.
*/
extern "C" {
#include "opensegaapi.h"
}

#include <vector>
#define XAUDIO2_HELPER_FUNCTIONS
#include <xaudio2.h>
#include <wrl.h>
#define TSF_IMPLEMENTATION
#include "tsf.h"
#define CHECK_HR(exp) { HRESULT hr = exp; if (FAILED(hr)) { printf("failed %s: %08x\n", #exp, hr); abort(); } }
#pragma comment(lib, "xaudio2.lib")

namespace WRL = Microsoft::WRL;

const GUID OPEN_EAX_NULL_GUID;
const GUID OPEN_EAX_FREQUENCYSHIFTER_EFFECT;
const GUID OPEN_EAX_ECHO_EFFECT;
const GUID OPEN_EAX_REVERB_EFFECT;
const GUID OPEN_EAX_EQUALIZER_EFFECT;
const GUID OPEN_EAX_DISTORTION_EFFECT;
const GUID OPEN_EAX_AGCCOMPRESSOR_EFFECT;
const GUID OPEN_EAX_PITCHSHIFTER_EFFECT;
const GUID OPEN_EAX_FLANGER_EFFECT;
const GUID OPEN_EAX_VOCALMORPHER_EFFECT;
const GUID OPEN_EAX_AUTOWAH_EFFECT;
const GUID OPEN_EAX_RINGMODULATOR_EFFECT;
const GUID OPEN_EAX_CHORUS_EFFECT;

const GUID OPEN_EAXPROPERTYID_EAX40_FXSlot0;
const GUID OPEN_EAXPROPERTYID_EAX40_FXSlot1;
const GUID OPEN_EAXPROPERTYID_EAX40_FXSlot2;
const GUID OPEN_EAXPROPERTYID_EAX40_FXSlot3;

#include <concurrent_queue.h>
#include <functional>

struct OPEN_segaapiBuffer_t;

#ifdef _DEBUG
void info(const char* format, ...)
{
	va_list args;
	char buffer[1024];

	va_start(args, format);
	int len = _vsnprintf_s(buffer, sizeof(buffer), format, args);
	va_end(args);

	buffer[len] = '\n';
	buffer[len + 1] = '\0';

	OutputDebugStringA(buffer);
}
#endif

class XA2Callback : public IXAudio2VoiceCallback
{
public:
	void __stdcall OnVoiceProcessingPassStart(UINT32 BytesRequired) override
	{
	}

	void __stdcall OnVoiceProcessingPassEnd() override
	{
	}

	void __stdcall OnStreamEnd() override
	{
	}

	void __stdcall OnBufferStart(void*) override
	{
	}

	void __stdcall OnLoopEnd(void*) override
	{
	}

	void __stdcall OnVoiceError(void*, HRESULT) override
	{
	}

	void __stdcall OnBufferEnd(void* cxt) override;

public:
	OPEN_segaapiBuffer_t * buffer;
};

struct OPEN_segaapiBuffer_t
{
	void* userData;
	OPEN_HAWOSEGABUFFERCALLBACK callback;
	bool synthesizer;
	bool loop;
	unsigned int channels;
	unsigned int startLoop;
	unsigned int endLoop;
	unsigned int endOffset;
	unsigned int sampleRate;
	unsigned int sampleFormat;
	uint8_t* data;
	size_t size;
	bool playing;
	bool paused;
	bool playWithSetup;

	WAVEFORMATEX xaFormat;

	XAUDIO2_BUFFER xaBuffer;
	IXAudio2SourceVoice* xaVoice;

	float sendVolumes[7];
	int sendChannels[7];
	OPEN_HAROUTING sendRoutes[7];
	float channelVolumes[6];

	tsf* synth;
	tsf_region* region;

	XA2Callback xaCallback;

	concurrency::concurrent_queue<std::function<void()>> defers;
};

void XA2Callback::OnBufferEnd(void* cxt)
{
	std::function<void()> entry;

	while (!buffer->defers.empty())
	{
		XAUDIO2_VOICE_STATE vs;
		buffer->xaVoice->GetState(&vs);

		if (vs.BuffersQueued > 0)
		{
			buffer->xaVoice->FlushSourceBuffers();

			return;
		}

		if (buffer->defers.try_pop(entry))
		{
			entry();
		}
	}
}

template <typename TFn>
void defer_buffer_call(OPEN_segaapiBuffer_t* buffer, const TFn& fn)
{
	if (buffer->xaVoice)
	{
		XAUDIO2_VOICE_STATE vs;
		buffer->xaVoice->GetState(&vs);

		if (vs.BuffersQueued > 0)
		{
			buffer->defers.push(fn);

			buffer->xaVoice->FlushSourceBuffers();

			return;
		}
	}
	fn();
}

static void dumpWaveBuffer(const char* path, unsigned int channels, unsigned int sampleRate, unsigned int sampleBits, void* data, size_t size)
{
#ifdef _DEBUG
	info("dumpWaveBuffer path %s channels %d sampleRate %d sampleBits %d size %d", path, channels, sampleRate, sampleBits, size);
#endif

	struct RIFF_Header
	{
		char chunkID[4];
		long chunkSize;
		char format[4];
	};

	struct WAVE_Format
	{
		char subChunkID[4];
		long subChunkSize;
		short audioFormat;
		short numChannels;
		long sampleRate;
		long byteRate;
		short blockAlign;
		short bitsPerSample;
	};

	struct WAVE_Data
	{
		char subChunkID[4];
		long subChunk2Size;
	};

	FILE* soundFile = NULL;
	struct WAVE_Format wave_format;
	struct RIFF_Header riff_header;
	struct WAVE_Data wave_data;

	soundFile = fopen(path, "wb");

	riff_header.chunkID[0] = 'R';
	riff_header.chunkID[1] = 'I';
	riff_header.chunkID[2] = 'F';
	riff_header.chunkID[3] = 'F';
	riff_header.format[0] = 'W';
	riff_header.format[1] = 'A';
	riff_header.format[2] = 'V';
	riff_header.format[3] = 'E';

	fwrite(&riff_header, sizeof(struct RIFF_Header), 1, soundFile);

	wave_format.subChunkID[0] = 'f';
	wave_format.subChunkID[1] = 'm';
	wave_format.subChunkID[2] = 't';
	wave_format.subChunkID[3] = ' ';

	wave_format.audioFormat = 1;
	wave_format.sampleRate = sampleRate;
	wave_format.numChannels = channels;
	wave_format.bitsPerSample = sampleBits;
	wave_format.byteRate = (sampleRate * sampleBits * channels) / 8;
	wave_format.blockAlign = (sampleBits * channels) / 8;
	wave_format.subChunkSize = 16;

	fwrite(&wave_format, sizeof(struct WAVE_Format), 1, soundFile);

	wave_data.subChunkID[0] = 'd';
	wave_data.subChunkID[1] = 'a';
	wave_data.subChunkID[2] = 't';
	wave_data.subChunkID[3] = 'a';

	wave_data.subChunk2Size = size;

	fwrite(&wave_data, sizeof(struct WAVE_Data), 1, soundFile);
	fwrite(data, wave_data.subChunk2Size, 1, soundFile);

	fclose(soundFile);
}

static unsigned int bufferSampleSize(OPEN_segaapiBuffer_t* buffer)
{
	return buffer->channels * ((buffer->sampleFormat == OPEN_HASF_SIGNED_16PCM) ? 2 : 1);
}

static void updateSynthOnPlay(OPEN_segaapiBuffer_t* buffer, unsigned int offset, size_t length)
{
	// TODO
	//// synth
	//if (buffer->synthesizer)
	//{
	//	if (!buffer->synth->voices)
	//	{
	//		struct tsf_voice *voice = new tsf_voice;
	//		memset(voice, 0, sizeof(tsf_voice));

	//		auto region = buffer->region;

	//		TSF_BOOL doLoop; float filterQDB;
	//		voice->playingPreset = -1;

	//		voice->region = region;
	//		voice->noteGainDB = 0.0f - region->volume;

	//		tsf_voice_calcpitchratio(voice, 0, buffer->synth->outSampleRate);
	//		// The SFZ spec is silent about the pan curve, but a 3dB pan law seems common. This sqrt() curve matches what Dimension LE does; Alchemy Free seems closer to sin(adjustedPan * pi/2).
	//		voice->panFactorLeft = TSF_SQRTF(0.5f - region->pan);
	//		voice->panFactorRight = TSF_SQRTF(0.5f + region->pan);

	//		// Offset/end.
	//		voice->sourceSamplePosition = region->offset;

	//		// Loop.
	//		doLoop = (region->loop_mode != TSF_LOOPMODE_NONE && region->loop_start < region->loop_end);
	//		voice->loopStart = (doLoop ? region->loop_start : 0);
	//		voice->loopEnd = (doLoop ? region->loop_end : 0);

	//		// Setup envelopes.
	//		tsf_voice_envelope_setup(&voice->ampenv, &region->ampenv, 0, 0, TSF_TRUE, buffer->synth->outSampleRate);
	//		tsf_voice_envelope_setup(&voice->modenv, &region->modenv, 0, 0, TSF_FALSE, buffer->synth->outSampleRate);

	//		// Setup lowpass filter.
	//		filterQDB = region->initialFilterQ / 10.0f;
	//		voice->lowpass.QInv = 1.0 / TSF_POW(10.0, (filterQDB / 20.0));
	//		voice->lowpass.z1 = voice->lowpass.z2 = 0;
	//		voice->lowpass.active = (region->initialFilterFc </*=*/ 13500);
	//		if (voice->lowpass.active) tsf_voice_lowpass_setup(&voice->lowpass, tsf_cents2Hertz((float)region->initialFilterFc) / buffer->synth->outSampleRate);

	//		// Setup LFO filters.
	//		tsf_voice_lfo_setup(&voice->modlfo, region->delayModLFO, region->freqModLFO, buffer->synth->outSampleRate);
	//		tsf_voice_lfo_setup(&voice->viblfo, region->delayVibLFO, region->freqVibLFO, buffer->synth->outSampleRate);

	//		voice->pitchInputTimecents = (log(1.0) / log(2.0) * 1200);
	//		voice->pitchOutputFactor = 1.0f;

	//		buffer->synth->voices = voice;
	//	}

	//	buffer->synth->voices->region = buffer->region;

	//	// make input
	//	buffer->synth->outputmode = TSF_MONO;

	//	auto soffset = offset;
	//	auto slength = length;

	//	if (offset == -1)
	//	{
	//		soffset = 0;
	//	}

	//	if (length == -1)
	//	{
	//		slength = buffer->size;
	//	}

	//	std::vector<float> fontSamples(slength / bufferSampleSize(buffer));
	//	buffer->synth->fontSamples = &fontSamples[0];

	//	buffer->region->end = double(fontSamples.size());

	//	for (int i = 0; i < fontSamples.size(); i++)
	//	{
	//		if (buffer->sampleFormat == OPEN_HASF_UNSIGNED_8PCM)
	//		{
	//			fontSamples[i] = (buffer->data[soffset + i] / 128.0f) - 1.0f;
	//		}
	//		else if (buffer->sampleFormat == OPEN_HASF_SIGNED_16PCM)
	//		{
	//			fontSamples[i] = (*(int16_t*)&buffer->data[soffset + (i * 2)]) / 32768.0f;
	//		}
	//	}

	//	std::vector<float> outSamples(slength / bufferSampleSize(buffer));
	//	tsf_voice_render(buffer->synth, buffer->synth->voices, &outSamples[0], outSamples.size());

	//	for (int i = 0; i < outSamples.size(); i++)
	//	{
	//		if (buffer->sampleFormat == OPEN_HASF_UNSIGNED_8PCM)
	//		{
	//			buffer->data[soffset + i] = (uint8_t)((outSamples[i] + 1.0f) * 128.0f);
	//		}
	//		else if (buffer->sampleFormat == OPEN_HASF_SIGNED_16PCM)
	//		{
	//			*(int16_t*)&buffer->data[soffset + (i * 2)] = outSamples[i] * 32768.0f;
	//		}
	//	}
	//}
}

static void resetBuffer(OPEN_segaapiBuffer_t* buffer)
{
	buffer->startLoop = 0;
	buffer->endOffset = buffer->size;
	buffer->endLoop = buffer->size;
	buffer->loop = false;
	buffer->paused = false;
	buffer->playWithSetup = false;
	buffer->sendRoutes[0] = OPEN_HA_FRONT_LEFT_PORT;
	buffer->sendRoutes[1] = OPEN_HA_FRONT_RIGHT_PORT;
	buffer->sendRoutes[2] = OPEN_HA_UNUSED_PORT;
	buffer->sendRoutes[3] = OPEN_HA_UNUSED_PORT;
	buffer->sendRoutes[4] = OPEN_HA_UNUSED_PORT;
	buffer->sendRoutes[5] = OPEN_HA_UNUSED_PORT;
	buffer->sendRoutes[6] = OPEN_HA_UNUSED_PORT;
	buffer->sendVolumes[0] = 0.0f;
	buffer->sendVolumes[1] = 0.0f;
	buffer->sendVolumes[2] = 0.0f;
	buffer->sendVolumes[3] = 0.0f;
	buffer->sendVolumes[4] = 0.0f;
	buffer->sendVolumes[5] = 0.0f;
	buffer->sendVolumes[6] = 0.0f;
	buffer->channelVolumes[0] = 1.0f;
	buffer->channelVolumes[1] = 1.0f;
	buffer->channelVolumes[2] = 1.0f;
	buffer->channelVolumes[3] = 1.0f;
	buffer->channelVolumes[4] = 1.0f;
	buffer->channelVolumes[5] = 1.0f;
	buffer->sendChannels[0] = 0;
	buffer->sendChannels[1] = 1;
	buffer->sendChannels[2] = 0;
	buffer->sendChannels[3] = 0;
	buffer->sendChannels[4] = 0;
	buffer->sendChannels[5] = 0;
	buffer->sendChannels[6] = 0;

	auto res = (tsf*)TSF_MALLOC(sizeof(tsf));
	TSF_MEMSET(res, 0, sizeof(tsf));
	res->presetNum = 0;
	res->outSampleRate = buffer->sampleRate;

	buffer->synth = res;

	tsf_region* region = new tsf_region;
	memset(region, 0, sizeof(tsf_region));

	tsf_region_clear(region, 0);

	region->ampenv.delay = 0;
	region->ampenv.hold = 300.0f;
	region->ampenv.attack = 0;
	region->ampenv.decay = 0;
	region->ampenv.release = 0;
	region->ampenv.sustain = 0;

	buffer->region = region;
}

static WRL::ComPtr<IXAudio2> g_xa2;
static IXAudio2MasteringVoice* g_masteringVoice;
static IXAudio2SubmixVoice* g_submixVoices[6];

static void updateBufferNew(OPEN_segaapiBuffer_t* buffer, unsigned int offset, size_t length)
{
	// don't update with pending defers
	if (!buffer->defers.empty())
	{
#ifdef _DEBUG
		info("updateBufferNew: DEFER!");
#endif
		return;
	}

	CHECK_HR(buffer->xaVoice->FlushSourceBuffers());

	buffer->xaBuffer.Flags = 0;
	buffer->xaBuffer.AudioBytes = buffer->size;
	buffer->xaBuffer.pAudioData = buffer->data;

	if (buffer->loop)
	{
#ifdef _DEBUG
		info("updateBufferNew: loop");
#endif
		// Note: Sega uses byte offsets for begin and end
		//       Xaudio2 uses start sample and length in samples
		buffer->xaBuffer.PlayBegin = buffer->startLoop / bufferSampleSize(buffer);
		buffer->xaBuffer.PlayLength = (min(buffer->endLoop, buffer->endOffset) - buffer->startLoop) / bufferSampleSize(buffer);
		buffer->xaBuffer.LoopBegin = buffer->xaBuffer.PlayBegin;
		buffer->xaBuffer.LoopLength = buffer->xaBuffer.PlayLength;
		buffer->xaBuffer.LoopCount = XAUDIO2_LOOP_INFINITE;
		buffer->xaBuffer.pContext = NULL;
	}
	else
	{
#ifdef _DEBUG
		info("updateBufferNew: no loop");
#endif
		buffer->xaBuffer.PlayBegin = buffer->startLoop / bufferSampleSize(buffer);
		buffer->xaBuffer.PlayLength = (min(buffer->endLoop, buffer->endOffset) - buffer->startLoop) / bufferSampleSize(buffer);
		buffer->xaBuffer.LoopBegin = 0;
		buffer->xaBuffer.LoopLength = 0;
		buffer->xaBuffer.LoopCount = 0;
		buffer->xaBuffer.pContext = NULL;
	}

	buffer->xaVoice->SubmitSourceBuffer(&buffer->xaBuffer);

	// Uncomment to dump audio buffers to wav files (super slow)
	/*auto sampleBits = (buffer->sampleFormat == OPEN_HASF_SIGNED_16PCM) ? 16 : 8;
	char path[255];
	sprintf(path, "C:\\dump\\%08X.wav", &buffer);
	dumpWaveBuffer(path, buffer->channels, buffer->sampleRate, sampleBits, buffer->data, buffer->size);*/
}

extern "C" {
	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_CreateBuffer(OPEN_HAWOSEBUFFERCONFIG* pConfig, OPEN_HAWOSEGABUFFERCALLBACK pCallback, unsigned int dwFlags, void* * phHandle)
	{
		if ((phHandle == NULL) || (pConfig == NULL))
		{
			return OPENSEGAERR_BAD_POINTER;
		}

		OPEN_segaapiBuffer_t* buffer = new OPEN_segaapiBuffer_t;

#ifdef _DEBUG
		info("SEGAAPI_CreateBuffer: hHandle: %08X synth: %d, mem caller: %d, mem last: %d, mem alloc: %d, size: %d SampleRate: %d, byNumChans: %d, dwPriority: %d, dwSampleFormat: %d", buffer, (dwFlags & OPEN_HABUF_SYNTH_BUFFER), (dwFlags & OPEN_HABUF_ALLOC_USER_MEM) >> 1, (dwFlags & OPEN_HABUF_USE_MAPPED_MEM) >> 2, dwFlags == 0, pConfig->mapData.dwSize, pConfig->dwSampleRate, pConfig->byNumChans, pConfig->dwPriority, pConfig->dwSampleFormat);
#endif

		buffer->playing = false;
		buffer->callback = pCallback;
		buffer->synthesizer = dwFlags & OPEN_HABUF_SYNTH_BUFFER;
		buffer->sampleFormat = pConfig->dwSampleFormat;
		buffer->sampleRate = pConfig->dwSampleRate;
		buffer->channels = pConfig->byNumChans;
		buffer->userData = pConfig->hUserData;
		buffer->size = pConfig->mapData.dwSize;
		pConfig->mapData.dwOffset = 0;

		// Use buffer supplied by caller
		if (dwFlags & OPEN_HABUF_ALLOC_USER_MEM)
		{
			buffer->data = (uint8_t*)pConfig->mapData.hBufferHdr;
		}
		// Reuse buffer
		else if (dwFlags & OPEN_HABUF_USE_MAPPED_MEM)
		{
			buffer->data = (uint8_t*)pConfig->mapData.hBufferHdr;
		}
		// Allocate new buffer (caller will fill it later)
		else
		{
			buffer->data = (uint8_t*)malloc(buffer->size);
		}

		pConfig->mapData.hBufferHdr = buffer->data;

		auto sampleRate = pConfig->dwSampleRate;
		auto sampleBits = (pConfig->dwSampleFormat == OPEN_HASF_SIGNED_16PCM) ? 16 : 8;
		auto channels = pConfig->byNumChans;

		buffer->xaFormat.cbSize = sizeof(WAVEFORMATEX);
		buffer->xaFormat.nAvgBytesPerSec = (sampleRate * sampleBits * channels) / 8;
		buffer->xaFormat.nSamplesPerSec = sampleRate;
		buffer->xaFormat.wBitsPerSample = sampleBits;
		buffer->xaFormat.nChannels = channels;
		buffer->xaFormat.wFormatTag = 1;
		buffer->xaFormat.nBlockAlign = (sampleBits * channels) / 8;
		buffer->xaCallback.buffer = buffer;

		CHECK_HR(g_xa2->CreateSourceVoice(&buffer->xaVoice, &buffer->xaFormat, 0, 2.0f, &buffer->xaCallback));

		buffer->xaBuffer = { 0 };

		if (buffer->synthesizer)
		{
			// Not supported
		}
		resetBuffer(buffer);

		*phHandle = buffer;
		
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetUserData(void* hHandle, void* hUserData)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetUserData: Handle: %08X UserData: %08X", hHandle, hUserData);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->userData = hUserData;
		return OPEN_SEGA_SUCCESS;
	}


	__declspec(dllexport) void* SEGAAPI_GetUserData(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return nullptr;
		}

#ifdef _DEBUG
		info("SEGAAPI_GetUserData: Handle: %08X", hHandle);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		return buffer->userData;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_UpdateBuffer(void* hHandle, unsigned int dwStartOffset, unsigned int dwLength)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_UpdateBuffer: Handle: %08X dwStartOffset: %08X, dwLength: %08X", hHandle, dwStartOffset, dwLength);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		updateBufferNew(buffer, dwStartOffset, dwLength);
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetEndOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetEndOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->endOffset = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetEndLoopOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetEndLoopOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->endLoop = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetStartLoopOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetStartLoopOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->startLoop = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetSampleRate(void* hHandle, unsigned int dwSampleRate)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetSampleRate: Handle: %08X dwSampleRate: %08X", hHandle, dwSampleRate);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->sampleRate = dwSampleRate;

		defer_buffer_call(buffer, [=]()
		{
			buffer->xaVoice->SetSourceSampleRate(dwSampleRate);
		});

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetLoopState(void* hHandle, int bDoContinuousLooping)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetLoopState: Handle: %08X dwSampleRate: %08X", hHandle, bDoContinuousLooping);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->loop = bDoContinuousLooping;

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetPlaybackPosition(void* hHandle, unsigned int dwPlaybackPos)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetPlaybackPosition: Handle: %08X dwPlaybackPos: %08X", hHandle, dwPlaybackPos);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (dwPlaybackPos != 0)
		{
		}

		// XA2 TODO
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) unsigned int SEGAAPI_GetPlaybackPosition(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return 0;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		XAUDIO2_VOICE_STATE vs;
		buffer->xaVoice->GetState(&vs);

		unsigned int result = (vs.SamplesPlayed * (buffer->xaFormat.wBitsPerSample / 8) * buffer->xaFormat.nChannels) % buffer->size;

#ifdef _DEBUG
		info("SEGAAPI_GetPlaybackPosition: Handle: %08X Samples played: %08d BitsPerSample %08d/%08d nChannels %08d bufferSize %08d Result: %08X", hHandle, vs.SamplesPlayed, buffer->xaFormat.wBitsPerSample, (buffer->xaFormat.wBitsPerSample / 8), buffer->xaFormat.nChannels, buffer->size, result);
#endif

		return result;
	}

	static void updateRouting(OPEN_segaapiBuffer_t* buffer);

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Play(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_Play: Handle: %08X", hHandle);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		updateRouting(buffer);
		updateBufferNew(buffer, -1, -1);

		buffer->playing = true;
		buffer->paused = false;

		// Uncomment to mute music
		//if (buffer->playWithSetup)
		//{
			CHECK_HR(buffer->xaVoice->Start());
		//}
			
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Stop(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_Stop: Handle: %08X", hHandle);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->playing = false;
		buffer->paused = false;
		CHECK_HR(buffer->xaVoice->Stop());
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_HAWOSTATUS SEGAAPI_GetPlaybackStatus(void* hHandle)
	{
		if (hHandle == NULL)
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPENSEGAERR_BAD_HANDLE", hHandle);
#endif
			return OPEN_HAWOSTATUS(OPENSEGAERR_BAD_HANDLE);
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (buffer->paused)
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_PAUSE, buffer is paused", hHandle);
#endif
			return OPEN_HAWOSTATUS_PAUSE;
		}

		// XA2 TODO
		XAUDIO2_VOICE_STATE vs;
		buffer->xaVoice->GetState(&vs);

		if (vs.BuffersQueued == 0)
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_STOP, buffersqueued is 0", hHandle);
#endif
			return OPEN_HAWOSTATUS_STOP;
		}

		if (!buffer->loop && vs.SamplesPlayed >= (min(buffer->size, buffer->endOffset) / bufferSampleSize(buffer)))
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_STOP, Loop false and samples played bigger", hHandle);
#endif
			return OPEN_HAWOSTATUS_STOP;
		}
		if (buffer->playing)
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_ACTIVE, playing true!", hHandle);
#endif
			return OPEN_HAWOSTATUS_ACTIVE;
		}
		else
		{
#ifdef _DEBUG
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_STOP, Playing false!", hHandle);
#endif
			return OPEN_HAWOSTATUS_STOP;
		}
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetReleaseState(void* hHandle, int bSet)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_SetReleaseState: Handle: %08X bSet: %08X", hHandle, bSet);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (bSet)
		{
			buffer->playing = false;
			buffer->xaVoice->FlushSourceBuffers();
			buffer->xaVoice->Stop();
		}
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_DestroyBuffer(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return OPENSEGAERR_BAD_HANDLE;
		}

#ifdef _DEBUG
		info("SEGAAPI_DestroyBuffer: Handle: %08X", hHandle);
#endif

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		buffer->xaVoice->DestroyVoice();
		delete buffer;
		return OPEN_SEGA_SUCCESS;
	}

	int SEGAAPI_SetGlobalEAXProperty(GUID * guid, unsigned long ulProperty, void * pData, unsigned long ulDataSize)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetGlobalEAXProperty:");
#endif

		// Everything is fine
		return TRUE;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Init(void)
	{

#ifdef _DEBUG
		info("SEGAAPI_Init: hello harmfull world");
#endif

		CoInitialize(nullptr);

		CHECK_HR(XAudio2Create(&g_xa2));

		XAUDIO2_DEBUG_CONFIGURATION cfg = { 0 };
		cfg.TraceMask = XAUDIO2_LOG_ERRORS;
		//cfg.BreakMask = XAUDIO2_LOG_ERRORS;
		g_xa2->SetDebugConfiguration(&cfg);

		CHECK_HR(g_xa2->CreateMasteringVoice(&g_masteringVoice));

		XAUDIO2_VOICE_DETAILS vd;
		g_masteringVoice->GetVoiceDetails(&vd);

		for (auto& g_submixVoice : g_submixVoices)
		{
			CHECK_HR(g_xa2->CreateSubmixVoice(&g_submixVoice, 1, vd.InputSampleRate));
		}

		int numChannels = vd.InputChannels;

		auto setSubmixVoice = [=](OPEN_HAROUTING index, float frontLeft, float frontRight, float frontCenter, float lfe,
			float rearLeft, float rearRight)
		{
			float levelMatrix[12] = { 0 };
			levelMatrix[0] = frontLeft + rearLeft;
			levelMatrix[1] = frontRight + rearRight;

			if (numChannels == 2)
			{
				// TODO
			}

			levelMatrix[2] = lfe;

			// TODO: surround data - SetOutputMatrix order is somewhat unclear :/

			g_submixVoices[index]->SetOutputMatrix(g_masteringVoice, 1, numChannels, levelMatrix);
		};

		setSubmixVoice(OPEN_HA_FRONT_LEFT_PORT, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		setSubmixVoice(OPEN_HA_FRONT_RIGHT_PORT, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f);
		setSubmixVoice(OPEN_HA_FRONT_CENTER_PORT, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f);
		setSubmixVoice(OPEN_HA_REAR_LEFT_PORT, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f);
		setSubmixVoice(OPEN_HA_REAR_RIGHT_PORT, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f);
		setSubmixVoice(OPEN_HA_LFE_PORT, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Exit(void)
	{
#ifdef _DEBUG
		info("SEGAAPI_Exit");
#endif
		for (auto& g_submixVoice : g_submixVoices)
		{
			g_submixVoice->DestroyVoice();
		}

		// TODO: deinit XA2
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Reset(void)
	{
#ifdef _DEBUG
		info("SEGAAPI_Reset");
#endif
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetIOVolume(OPEN_HAPHYSICALIO dwPhysIO, unsigned int dwVolume)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetIOVolume: dwPhysIO: %08X dwVolume: %08X", dwPhysIO, dwVolume);
#endif
		g_masteringVoice->SetVolume(dwVolume / (float)0xFFFFFFFF);
		return OPEN_SEGA_SUCCESS;
	}

	static void updateRouting(OPEN_segaapiBuffer_t* buffer)
	{
		float levels[7 * 2];
		IXAudio2SubmixVoice* outVoices[7];

		int numRoutes = 0;

		for (int i = 0; i < /*7*/2; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT && buffer->sendRoutes[i] < 6)
			{
				outVoices[numRoutes] = g_submixVoices[buffer->sendRoutes[i]];

				int levelOff = numRoutes * buffer->channels;

				for (int ch = 0; ch < buffer->channels; ch++)
				{
					levels[levelOff + ch] = 0;
				}

				float level = buffer->sendVolumes[i] * buffer->channelVolumes[buffer->sendChannels[i]];
				levels[levelOff + buffer->sendChannels[i]] = level;

				++numRoutes;
			}
		}

		// can't set no routes
		if (numRoutes == 0)
		{
			return;
		}

		XAUDIO2_SEND_DESCRIPTOR sendDescs[7];
		for (int i = 0; i < numRoutes; i++)
		{
			sendDescs[i].Flags = 0;
			sendDescs[i].pOutputVoice = outVoices[i];
		}

		XAUDIO2_VOICE_SENDS sends;
		sends.SendCount = numRoutes;
		sends.pSends = sendDescs;
		CHECK_HR(buffer->xaVoice->SetOutputVoices(&sends));

		for (int i = 0; i < numRoutes; i++)
		{
			CHECK_HR(buffer->xaVoice->SetOutputMatrix(outVoices[i], buffer->channels, 1, &levels[i * buffer->channels]));
		}
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetSendRouting(void* hHandle, unsigned int dwChannel, unsigned int dwSend, OPEN_HAROUTING dwDest)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetSendRouting: hHandle: %08X dwChannel: %08X dwSend: %08X dwDest: %08X", hHandle, dwChannel, dwSend, dwDest);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->sendRoutes[dwSend] = dwDest;
		buffer->sendChannels[dwSend] = dwChannel;

		updateRouting(buffer);
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetSendLevel(void* hHandle, unsigned int dwChannel, unsigned int dwSend, unsigned int dwLevel)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetSendLevel: hHandle: %08X dwChannel: %08X dwSend: %08X dwLevel: %08X", hHandle, dwChannel, dwSend, dwLevel);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->sendVolumes[dwSend] = dwLevel / (float)0xFFFFFFFF;
		buffer->sendChannels[dwSend] = dwChannel;

		updateRouting(buffer);
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetSynthParam(void* hHandle, OPEN_HASYNTHPARAMSEXT param, int lPARWValue)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetSynthParam: hHandle: %08X OPEN_HASYNTHPARAMSEXT: %08X lPARWValue: %08X", hHandle, param, lPARWValue);
#endif
		enum
		{
			StartAddrsOffset,
			EndAddrsOffset,
			StartloopAddrsOffset,
			EndloopAddrsOffset,
			StartAddrsCoarseOffset,
			ModLfoToPitch,
			VibLfoToPitch,
			ModEnvToPitch,
			InitialFilterFc,
			InitialFilterQ,
			ModLfoToFilterFc,
			ModEnvToFilterFc,
			EndAddrsCoarseOffset,
			ModLfoToVolume,
			Unused1,
			ChorusEffectsSend,
			ReverbEffectsSend,
			Pan,
			Unused2,
			Unused3,
			Unused4,
			DelayModLFO,
			FreqModLFO,
			DelayVibLFO,
			FreqVibLFO,
			DelayModEnv,
			AttackModEnv,
			HoldModEnv,
			DecayModEnv,
			SustainModEnv,
			ReleaseModEnv,
			KeynumToModEnvHold,
			KeynumToModEnvDecay,
			DelayVolEnv,
			AttackVolEnv,
			HoldVolEnv,
			DecayVolEnv,
			SustainVolEnv,
			ReleaseVolEnv,
			KeynumToVolEnvHold,
			KeynumToVolEnvDecay,
			Instrument,
			Reserved1,
			KeyRange,
			VelRange,
			StartloopAddrsCoarseOffset,
			Keynum,
			Velocity,
			InitialAttenuation,
			Reserved2,
			EndloopAddrsCoarseOffset,
			CoarseTune,
			FineTune,
			SampleID,
			SampleModes,
			Reserved3,
			ScaleTuning,
			ExclusiveClass,
			OverridingRootKey,
			Unused5,
			EndOper
		};

		int mapping[26] = {
			InitialAttenuation, ///< 0,         0x00,  initialAttenuation
			FineTune, ///< 1,         0x01,  fineTune + coarseTune * 100
			InitialFilterFc, ///< 2,         0x02,  initialFilterFc
			InitialFilterQ, ///< 3,         0x03,  initialFilterQ
			DelayVolEnv, ///< 4,         0x04,  delayVolEnv
			AttackVolEnv, ///< 5,         0x05,  attackVolEnv
			HoldVolEnv, ///< 6,         0x06,  holdVolEnv
			DecayVolEnv, ///< 7,         0x07,  decayVolEnv
			SustainVolEnv, ///< 8,         0x08,  sustainVolEnv
			ReleaseVolEnv, ///< 9,         0x09,  releaseVolEnv
			DelayModEnv, ///< 10,        0x0A,  delayModEnv
			AttackModEnv, ///< 11,        0x0B,  attackModEnv
			HoldModEnv, ///< 12,        0x0C,  holdModEnv
			DecayModEnv, ///< 13,        0x0D,  decayModEnv
			SustainModEnv, ///< 14,        0x0E,  sustainModEnv
			ReleaseModEnv, ///< 15,        0x0F,  releaseModEnv
			DelayModLFO, ///< 16,        0x10,  delayModLFO
			FreqModLFO, ///< 17,        0x11,  freqModLFO
			DelayVibLFO, ///< 18,        0x12,  delayVibLFO
			FreqVibLFO, ///< 19,        0x13,  freqVibLFO
			ModLfoToPitch, ///< 20,        0x14,  modLfoToPitch
			VibLfoToPitch, ///< 21,        0x15,  vibLfoToPitch
			ModLfoToFilterFc, ///< 22,        0x16,  modLfoToFilterFc
			ModLfoToVolume, ///< 23,        0x17,  modLfoToVolume
			ModEnvToPitch, ///< 24,        0x18,  modEnvToPitch
			ModEnvToFilterFc ///< 25,        0x19,  modEnvToFilterFc
		};

		int realParam = mapping[param];

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		//tsf_hydra_genamount amount;
		//amount.shortAmount = lPARWValue;
		//tsf_region_operator(buffer->region, realParam, &amount);

		if (param == OPEN_HAVP_ATTENUATION)
		{
			float volume = tsf_decibelsToGain(0.0f - lPARWValue / 10.0f);

			buffer->xaVoice->SetVolume(volume);
#ifdef _DEBUG
			info("SEGAAPI_SetSynthParam: OPEN_HAVP_ATTENUATION gain: %f dB: %d", volume, lPARWValue);
#endif
		}
		else if (param == OPEN_HAVP_PITCH)
		{
			float semiTones = lPARWValue / 100.0f;
			float freqRatio = XAudio2SemitonesToFrequencyRatio(semiTones);

			buffer->xaVoice->SetFrequencyRatio(freqRatio);
#ifdef _DEBUG
			info("SEGAAPI_SetSynthParam: OPEN_HAVP_PITCH hHandle: %08X semitones: %f freqRatio: %f", hHandle, semiTones, freqRatio);
#endif
		}

		return OPEN_SEGA_SUCCESS;
	}

	int SEGAAPI_GetSynthParam(void * hHandle, OPEN_HASYNTHPARAMSEXT param)
	{
#ifdef _DEBUG
		info("SEGAAPI_GetSynthParam: hHandle: %08X OPEN_HASYNTHPARAMSEXT: %08X", hHandle, param);
#endif
		return 0; //todo not sure if actually used
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetSynthParamMultiple(void* hHandle, unsigned int dwNumParams, OPEN_SynthParamSet* pSynthParams)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetSynthParamMultiple: hHandle: %08X dwNumParams: %08X pSynthParams: %08X", hHandle, dwNumParams, pSynthParams);
#endif
		for (int i = 0; i < dwNumParams; i++)
		{
			SEGAAPI_SetSynthParam(hHandle, pSynthParams[i].param, pSynthParams[i].lPARWValue);
		}
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_SetChannelVolume(void* hHandle, unsigned int dwChannel, unsigned int dwVolume)
	{
#ifdef _DEBUG
		info("SEGAAPI_SetChannelVolume: hHandle: %08X dwChannel: %08X dwVolume: %08X", hHandle, dwChannel, dwVolume);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->channelVolumes[dwChannel] = dwVolume / (float)0xFFFFFFFF;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) unsigned int SEGAAPI_GetChannelVolume(void* hHandle, unsigned int dwChannel)
	{
#ifdef _DEBUG
		info("SEGAAPI_GetChannelVolume: hHandle: %08X dwChannel: %08X", hHandle, dwChannel);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		return buffer->channelVolumes[dwChannel];
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_Pause(void* hHandle)
	{
#ifdef _DEBUG
		info("SEGAAPI_Pause: hHandle: %08X", hHandle);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		buffer->playing = false;
		buffer->paused = true;
		CHECK_HR(buffer->xaVoice->Stop());
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_PlayWithSetup(
		void* hHandle,
		unsigned int dwNumSendRouteParams, OPEN_SendRouteParamSet* pSendRouteParams,
		unsigned int dwNumSendLevelParams, OPEN_SendLevelParamSet* pSendLevelParams,
		unsigned int dwNumVoiceParams, OPEN_VoiceParamSet* pVoiceParams,
		unsigned int dwNumSynthParams, OPEN_SynthParamSet* pSynthParams
	)
	{
#ifdef _DEBUG
		info("SEGAAPI_PlayWithSetup: hHandle: %08X dwNumSendRouteParams: %d pSendRouteParams: %08X dwNumSendLevelParams: %d pSendLevelParams: %08X dwNumVoiceParams: %d pVoiceParams: %08X dwNumSynthParams: %d pSynthParams: %08X", hHandle, dwNumSendRouteParams, *pSendRouteParams, dwNumSendLevelParams, *pSendLevelParams, dwNumVoiceParams, *pVoiceParams, dwNumSynthParams, *pSynthParams);
		info("dwNumSynthParams: %d", dwNumSynthParams);
#endif
		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->playWithSetup = true;

		for (int i = 0; i < dwNumSendRouteParams; i++)
		{
			SEGAAPI_SetSendRouting(hHandle, pSendRouteParams[i].dwChannel, pSendRouteParams[i].dwSend, pSendRouteParams[i].dwDest);
		}

		for (int i = 0; i < dwNumSendLevelParams; i++)
		{
			SEGAAPI_SetSendLevel(hHandle, pSendLevelParams[i].dwChannel, pSendLevelParams[i].dwSend, pSendLevelParams[i].dwLevel);
		}

		unsigned int loopStart = 0;
		unsigned int loopEnd = 0;
		unsigned int loopState = 0;
		unsigned int endOffset = 0;

		for (int i = 0; i < dwNumVoiceParams; i++)
		{
			switch (pVoiceParams[i].VoiceIoctl)
			{
			case OPEN_VOICEIOCTL_SET_START_LOOP_OFFSET:
				SEGAAPI_SetStartLoopOffset(hHandle, pVoiceParams[i].dwParam1);
				loopStart = pVoiceParams[i].dwParam1;
				break;
			case OPEN_VOICEIOCTL_SET_END_LOOP_OFFSET:
				SEGAAPI_SetEndLoopOffset(hHandle, pVoiceParams[i].dwParam1);
				loopEnd = pVoiceParams[i].dwParam1;
				break;
			case OPEN_VOICEIOCTL_SET_END_OFFSET:
				SEGAAPI_SetEndOffset(hHandle, pVoiceParams[i].dwParam1);
				endOffset = pVoiceParams[i].dwParam1;
				break;
			case OPEN_VOICEIOCTL_SET_LOOP_STATE:
				SEGAAPI_SetLoopState(hHandle, pVoiceParams[i].dwParam1);
				loopState = pVoiceParams[i].dwParam1;
				break;
			case OPEN_VOICEIOCTL_SET_NOTIFICATION_POINT:
#ifdef _DEBUG
				info("Unimplemented! OPEN_VOICEIOCTL_SET_NOTIFICATION_POINT");
#endif
				break;
			case OPEN_VOICEIOCTL_CLEAR_NOTIFICATION_POINT:
#ifdef _DEBUG
				info("Unimplemented! OPEN_VOICEIOCTL_CLEAR_NOTIFICATION_POINT");
#endif
				break;
			case OPEN_VOICEIOCTL_SET_NOTIFICATION_FREQUENCY:
#ifdef _DEBUG
				info("Unimplemented! OPEN_VOICEIOCTL_SET_NOTIFICATION_FREQUENCY");
#endif
				break;
			}
		}

#ifdef _DEBUG
		info("loopdata\t%08X\t%08X\t%08X\t%08X\t%d\t%d", hHandle, loopStart, loopEnd, endOffset, loopState, buffer->size);
#endif

		for (int i = 0; i < dwNumSynthParams; i++)
		{
			SEGAAPI_SetSynthParam(hHandle, pSynthParams[i].param, pSynthParams[i].lPARWValue);
		}

		SEGAAPI_Play(hHandle);

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPENSEGASTATUS SEGAAPI_GetLastStatus(void)
	{
#ifdef _DEBUG
		info("SEGAAPI_GetLastStatus");
#endif
		return OPEN_SEGA_SUCCESS;
	}
}
#pragma optimize("", on)