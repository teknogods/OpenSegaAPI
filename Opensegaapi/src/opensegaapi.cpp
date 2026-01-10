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
#include <dsound.h>
#include <algorithm>
#define CHECK_HR(exp) { HRESULT hr = exp; if (FAILED(hr)) { printf("failed %s: %08x\n", #exp, hr); abort(); } }
#pragma comment(lib, "dsound.lib")
#pragma comment(lib, "dxguid.lib")

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
#else
#define info(x, ...) {}
#endif

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
	bool ownsData;
	bool pendingRouting;

	WAVEFORMATEX dsFormat;

	IDirectSoundBuffer* dsBuffer;
	DWORD lastPlayCursor;

	float sendVolumes[7];
	int sendChannels[7];
	OPEN_HAROUTING sendRoutes[7];
	float channelVolumes[6];

	concurrency::concurrent_queue<std::function<void()>> defers;

	float masterVolume;
	float frequency;
	long pan;
};

static IDirectSound8* g_dsound;
static float g_masterVolumes[12] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f };
static std::vector<OPEN_segaapiBuffer_t*> g_allBuffers;

static void dumpWaveBuffer(const char* path, unsigned int channels, unsigned int sampleRate, unsigned int sampleBits, void* data, size_t size)
{
	info("dumpWaveBuffer path %s channels %d sampleRate %d sampleBits %d size %d", path, channels, sampleRate, sampleBits, size);

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
	if (soundFile == NULL)
	{
		info("dumpWaveBuffer: Failed to open file %s", path);
		return;
	}

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
	buffer->sendVolumes[0] = 1.0f;
	buffer->sendVolumes[1] = 1.0f;
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
	buffer->masterVolume = 1.0f;
	buffer->frequency = 1.0f;
	buffer->pan = 0;
}

static int getChannelToDuplicate(OPEN_segaapiBuffer_t* buffer)
{
	// Only applies to stereo 16-bit PCM buffers
	if (buffer->channels != 2 || buffer->sampleFormat != OPEN_HASF_SIGNED_16PCM)
		return -1;

	// Detect if any channel is routed to BOTH left and right
	for (int ch = 0; ch < (int)buffer->channels; ch++)
	{
		bool toLeft = false;
		bool toRight = false;

		for (int i = 0; i < 7; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT &&
				buffer->sendVolumes[i] > 0.0f &&
				buffer->sendChannels[i] == ch)
			{
				if (buffer->sendRoutes[i] == OPEN_HA_FRONT_LEFT_PORT ||
					buffer->sendRoutes[i] == OPEN_HA_REAR_LEFT_PORT)
					toLeft = true;
				if (buffer->sendRoutes[i] == OPEN_HA_FRONT_RIGHT_PORT ||
					buffer->sendRoutes[i] == OPEN_HA_REAR_RIGHT_PORT)
					toRight = true;
			}
		}

		if (toLeft && toRight)
		{
			info("getChannelToDuplicate: Channel %d routed to BOTH L/R", ch);
			return ch;
		}
	}

	return -1; // No duplication needed
}

// Helper function to duplicate a mono channel to stereo output
static void duplicateChannelToStereo(void* dest, const void* src, size_t bytesToCopy, int sourceChannel)
{
	int16_t* srcSamples = (int16_t*)src;
	int16_t* dstSamples = (int16_t*)dest;
	size_t numFrames = bytesToCopy / 4;  // 4 bytes per stereo 16-bit frame

	info("duplicateChannelToStereo: Duplicating source channel %d to stereo output - %zu frames",
		sourceChannel, numFrames);

	for (size_t i = 0; i < numFrames; i++)
	{
		// Read the channel that needs duplication (0=left, 1=right)
		int16_t sample = srcSamples[i * 2 + sourceChannel];

		// Write to BOTH output channels
		dstSamples[i * 2] = sample;      // Left output
		dstSamples[i * 2 + 1] = sample;  // Right output
	}
}

static void updateRouting(OPEN_segaapiBuffer_t* buffer)
{
	if (!buffer->pendingRouting) return;

	if (!buffer->dsBuffer)
	{
		info("updateRouting: No dsBuffer, skipping");
		buffer->pendingRouting = false;
		return;
	}

	DWORD dsStatus = 0;
	buffer->dsBuffer->GetStatus(&dsStatus);
	bool dsPlaying = (dsStatus & DSBSTATUS_PLAYING) != 0;

	info("updateRouting: ===== ROUTING DEBUG START =====");
	info("updateRouting: Buffer channels=%d, DSoundPlaying=%d, Loop=%d",
		buffer->channels, dsPlaying, buffer->loop);

	for (int i = 0; i < 7; i++)
	{
		info("updateRouting: Send[%d]: route=%d, channel=%d, volume=%f",
			i, buffer->sendRoutes[i], buffer->sendChannels[i], buffer->sendVolumes[i]);
	}

	bool usedPort[6] = { false };
	float levels[6][6] = { 0.0f };
	int numValidRoutes = 0;

	for (int port = 0; port < 6; port++)
	{
		for (int ch = 0; ch < 6; ch++)
		{
			levels[port][ch] = 0.0f;
		}
	}

	for (int i = 0; i < 7; i++)
	{
		if (buffer->sendRoutes[i] == OPEN_HA_UNUSED_PORT ||
			buffer->sendRoutes[i] < 0 ||
			buffer->sendRoutes[i] >= 6 ||
			buffer->sendVolumes[i] <= 0.0f)
		{
			continue;
		}

		int destPort = buffer->sendRoutes[i];
		int srcChannel = buffer->sendChannels[i];

		if (srcChannel < 0 || srcChannel >= (int)buffer->channels || srcChannel >= 6)
		{
			info("updateRouting: WARNING - Send %d has invalid srcChannel %d (buffer has %d channels)",
				i, srcChannel, buffer->channels);
			continue;
		}

		usedPort[destPort] = true;

		float level = buffer->sendVolumes[i] * buffer->channelVolumes[srcChannel];
		levels[destPort][srcChannel] += level;

		numValidRoutes++;

		info("updateRouting: Send %d - SrcChan %d -> DestPort %d, Level %f", i, srcChannel, destPort, level);
	}

	if (numValidRoutes == 0)
	{
		info("updateRouting: No valid routes found, skipping");
		buffer->pendingRouting = false;
		return;
	}

	// Check if center channel is being used
	bool hasCenterChannel = false;
	int centerChannelIndex = -1;
	float centerVolume = 0.0f;

	for (int i = 0; i < 7; i++)
	{
		if (buffer->sendRoutes[i] == OPEN_HA_FRONT_CENTER_PORT && buffer->sendVolumes[i] > 0.0f)
		{
			hasCenterChannel = true;
			centerChannelIndex = buffer->sendChannels[i];
			centerVolume = buffer->sendVolumes[i];
			info("updateRouting: DETECTED center channel routing - send[%d] using channel %d with volume %f",
				i, centerChannelIndex, centerVolume);
			break;
		}
	}

	// Detect LFE/Subwoofer channel usage and calculate its level
	bool hasLFE = false;
	float lfeLevel = 0.0f;

	for (int i = 0; i < 7; i++)
	{
		if (buffer->sendRoutes[i] == OPEN_HA_LFE_PORT && buffer->sendVolumes[i] > 0.0f)
		{
			hasLFE = true;
			int srcChannel = buffer->sendChannels[i];
			if (srcChannel >= 0 && srcChannel < (int)buffer->channels && srcChannel < 6)
			{
				float level = buffer->sendVolumes[i] * buffer->channelVolumes[srcChannel];
				lfeLevel = max(lfeLevel, level);
			}
			info("updateRouting: DETECTED LFE/Subwoofer routing - send[%d] with volume %f, level=%f",
				i, buffer->sendVolumes[i], lfeLevel);
		}
	}

	float overallVolume = 0.0f;

	// Calculate volume from stereo channels
	for (int port = 0; port < 6; port++)
	{
		// Skip LFE port (port 3) when calculating stereo volume
		if (port == OPEN_HA_LFE_PORT)
		{
			continue;
		}

		if (usedPort[port])
		{
			for (int ch = 0; ch < (int)buffer->channels && ch < 6; ch++)
			{
				overallVolume = max(overallVolume, levels[port][ch]);
			}
		}
	}

	// Bass management
	if (hasLFE && overallVolume < 0.0001f)
	{
		// LFE-only audio (like car engine) - downmix to stereo at -20dB
		overallVolume = lfeLevel * 0.1f;
		info("updateRouting: LFE-only audio detected - downmixing to stereo at reduced level: %f", overallVolume);
	}
	else if (hasLFE && overallVolume > 0.0001f)
	{
		// Mix LFE with existing stereo channels at lower level at -22dB
		float lfeContribution = lfeLevel * 0.08f;
		overallVolume = max(overallVolume, lfeContribution);
		info("updateRouting: LFE mixed with stereo - LFE contribution: %f, total: %f", lfeContribution, overallVolume);
	}

	info("updateRouting: Channels=%d, Calculated overallVolume=%f (LFE downmixed if present)", buffer->channels, overallVolume);

	float finalVolume = overallVolume * buffer->masterVolume;

	float globalVolumeFactor = 1.0f;
	for (int i = 0; i < 7; i++)
	{
		if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT && buffer->sendVolumes[i] > 0.0f)
		{
			int physPort = -1;
			switch (buffer->sendRoutes[i])
			{
			case OPEN_HA_FRONT_LEFT_PORT:
				physPort = OPEN_HA_OUT_FRONT_LEFT;
				break;
			case OPEN_HA_FRONT_RIGHT_PORT:
				physPort = OPEN_HA_OUT_FRONT_RIGHT;
				break;
			case OPEN_HA_FRONT_CENTER_PORT:
				physPort = OPEN_HA_OUT_FRONT_CENTER;
				break;
			case OPEN_HA_LFE_PORT:
				// Include LFE master volume for downmixed audio
				physPort = OPEN_HA_OUT_LFE_PORT;
				break;
			case OPEN_HA_REAR_LEFT_PORT:
				physPort = OPEN_HA_OUT_REAR_LEFT;
				break;
			case OPEN_HA_REAR_RIGHT_PORT:
				physPort = OPEN_HA_OUT_REAR_RIGHT;
				break;
			}

			if (physPort >= 0 && physPort < 12)
			{
				globalVolumeFactor = max(globalVolumeFactor, g_masterVolumes[physPort]);
			}
		}
	}

	finalVolume *= globalVolumeFactor;

	long dsVolume = DSBVOLUME_MIN;
	if (finalVolume > 0.00001f)
	{
		dsVolume = (long)(2000.0f * log10(finalVolume));
		dsVolume = max(DSBVOLUME_MIN, min(DSBVOLUME_MAX, dsVolume));
	}

	HRESULT hr = buffer->dsBuffer->SetVolume(dsVolume);
	if (FAILED(hr))
	{
		info("updateRouting: SetVolume FAILED: 0x%08x", hr);
	}

	// Calculate pan based on left/right routing levels
	long dsPan = 0;

	// Check if any channel is sent to BOTH left and right
	bool channelSentToBoth[6] = { false };
	for (int ch = 0; ch < 6; ch++)
	{
		bool toLeft = false;
		bool toRight = false;

		for (int i = 0; i < 7; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT && buffer->sendVolumes[i] > 0.0f)
			{
				// Skip LFE for pan calculation (LFE is non-directional)
				if (buffer->sendRoutes[i] == OPEN_HA_LFE_PORT)
					continue;

				if (buffer->sendChannels[i] == ch)
				{
					if (buffer->sendRoutes[i] == OPEN_HA_FRONT_LEFT_PORT || buffer->sendRoutes[i] == OPEN_HA_REAR_LEFT_PORT)
						toLeft = true;
					if (buffer->sendRoutes[i] == OPEN_HA_FRONT_RIGHT_PORT || buffer->sendRoutes[i] == OPEN_HA_REAR_RIGHT_PORT)
						toRight = true;
				}
			}
		}

		if (toLeft && toRight)
		{
			channelSentToBoth[ch] = true;
			info("updateRouting: Channel %d is sent to BOTH L/R - will CENTER (Pan=0)", ch);
		}
	}

	// If ANY channel is sent to both L/R, force center pan
	bool hasDuplicateRouting = false;
	for (int ch = 0; ch < 6; ch++)
	{
		if (channelSentToBoth[ch])
		{
			hasDuplicateRouting = true;
			break;
		}
	}

	if (hasDuplicateRouting || (hasLFE && overallVolume > 0 && overallVolume <= lfeLevel * 0.6f))
	{
		// Force centered pan for duplicated routing OR LFE-dominant audio
		dsPan = 0;
		info("updateRouting: Duplicate routing or LFE-dominant - FORCING center pan (0)");
	}
	else
	{
		// Normal pan calculation - sum up left and right channel levels
		float leftLevel = 0.0f;
		float rightLevel = 0.0f;

		for (int i = 0; i < 7; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT && buffer->sendVolumes[i] > 0.0f)
			{
				// Skip LFE for pan calculation (LFE is non-directional, so it goes to center)
				if (buffer->sendRoutes[i] == OPEN_HA_LFE_PORT)
				{
					continue;
				}

				int srcChannel = buffer->sendChannels[i];
				if (srcChannel >= 0 && srcChannel < (int)buffer->channels && srcChannel < 6)
				{
					float level = buffer->sendVolumes[i] * buffer->channelVolumes[srcChannel];

					if (buffer->sendRoutes[i] == OPEN_HA_FRONT_LEFT_PORT || buffer->sendRoutes[i] == OPEN_HA_REAR_LEFT_PORT)
						leftLevel += level;
					else if (buffer->sendRoutes[i] == OPEN_HA_FRONT_RIGHT_PORT || buffer->sendRoutes[i] == OPEN_HA_REAR_RIGHT_PORT)
						rightLevel += level;
					else if (buffer->sendRoutes[i] == OPEN_HA_FRONT_CENTER_PORT)
					{
						// Center channel: add to BOTH left and right
						leftLevel += level;
						rightLevel += level;
					}
				}
			}
		}

		// Calculate pan: -10000 (full left) to +10000 (full right)
		if (leftLevel > 0.0f || rightLevel > 0.0f)
		{
			float totalLevel = leftLevel + rightLevel;
			float balance = (rightLevel - leftLevel) / totalLevel; // -1.0 to +1.0
			dsPan = (long)(balance * 10000.0f);
			dsPan = max(-10000L, min(10000L, dsPan));
		}

		info("updateRouting: Pan calculation - Left=%f Right=%f Balance=%f Pan=%d",
			leftLevel, rightLevel, (leftLevel + rightLevel > 0) ? (rightLevel - leftLevel) / (leftLevel + rightLevel) : 0.0f, dsPan);
	}

	hr = buffer->dsBuffer->SetPan(dsPan);
	if (FAILED(hr))
	{
		info("updateRouting: SetPan FAILED: 0x%08x", hr);
	}
	buffer->pan = dsPan;

	long verifyVolume = 0;
	buffer->dsBuffer->GetVolume(&verifyVolume);

	buffer->pendingRouting = false;
	info("updateRouting: Final - Volume=%d dB (verify=%d), overall=%f, master=%f, final=%f, global=%f, Pan=%d, Channels=%d, HasCenter=%d, HasLFE=%d (level=%f), DSPlaying=%d",
		dsVolume, verifyVolume, overallVolume, buffer->masterVolume, finalVolume, globalVolumeFactor, dsPan, buffer->channels, hasCenterChannel, hasLFE, lfeLevel, dsPlaying);
	info("updateRouting: ===== ROUTING DEBUG END =====");
}

static void updateBufferNew(OPEN_segaapiBuffer_t* buffer, unsigned int offset, size_t length)
{
	info("updateBufferNew offset=%08X length=%08X, loop=%d, startLoop=%08X, endLoop=%08X, endOffset=%08X",
		offset, length, buffer->loop, buffer->startLoop, buffer->endLoop, buffer->endOffset);

	if (buffer->dsBuffer == NULL || buffer->data == nullptr)
	{
		info("updateBufferNew: Invalid buffer state");
		return;
	}

	// Calculate the actual audio region to play
	unsigned int playStartOffset = buffer->startLoop;
	unsigned int playEndOffset = buffer->loop ? buffer->endLoop : buffer->endOffset;

	// Clamp to buffer size
	if (playStartOffset >= buffer->size) playStartOffset = 0;
	if (playEndOffset > buffer->size) playEndOffset = buffer->size;
	if (playEndOffset <= playStartOffset) playEndOffset = buffer->size;

	unsigned int bytesToPlay = playEndOffset - playStartOffset;

	info("updateBufferNew: Calculated play region: start=%08X end=%08X length=%08X",
		playStartOffset, playEndOffset, bytesToPlay);

	// Get current DirectSound buffer size
	DSBCAPS caps;
	ZeroMemory(&caps, sizeof(DSBCAPS));
	caps.dwSize = sizeof(DSBCAPS);
	HRESULT hr = buffer->dsBuffer->GetCaps(&caps);
	if (FAILED(hr))
	{
		info("updateBufferNew: GetCaps failed: 0x%08x", hr);
		return;
	}

	// Check status and restore if lost
	DWORD status = 0;
	hr = buffer->dsBuffer->GetStatus(&status);
	if (FAILED(hr))
	{
		info("updateBufferNew: GetStatus failed: 0x%08x", hr);
		return;
	}

	if (status & DSBSTATUS_BUFFERLOST)
	{
		hr = buffer->dsBuffer->Restore();
		if (FAILED(hr)) return;
	}

	bool isCurrentlyPlaying = (status & DSBSTATUS_PLAYING) != 0;

	// Lock and update the DirectSound buffer
	void* ptr1 = nullptr;
	void* ptr2 = nullptr;
	DWORD bytes1 = 0, bytes2 = 0;

	hr = buffer->dsBuffer->Lock(0, caps.dwBufferBytes, &ptr1, &bytes1, &ptr2, &bytes2, 0);
	if (FAILED(hr))
	{
		info("updateBufferNew: Failed to lock: 0x%08x", hr);
		return;
	}

	// For looping, tile the loop data to fill the entire DirectSound buffer
	if (buffer->loop && bytesToPlay < caps.dwBufferBytes)
	{
		size_t bytesWritten = 0;
		uint8_t* destPtr = (uint8_t*)ptr1;

		// Keep copying the loop region until we fill the DirectSound buffer
		while (bytesWritten < bytes1)
		{
			size_t chunkSize = min((size_t)(bytes1 - bytesWritten), (size_t)bytesToPlay);

			int channelToDuplicate = getChannelToDuplicate(buffer);
			if (channelToDuplicate >= 0)
			{
				duplicateChannelToStereo(destPtr, buffer->data + playStartOffset, chunkSize, channelToDuplicate);
			}
			else
			{
				memcpy(destPtr, buffer->data + playStartOffset, chunkSize);
			}

			destPtr += chunkSize;
			bytesWritten += chunkSize;
		}

		// Handle wrapped buffer (ptr2)
		if (ptr2 && bytes2 > 0)
		{
			bytesWritten = 0;
			destPtr = (uint8_t*)ptr2;

			while (bytesWritten < bytes2)
			{
				size_t chunkSize = min((size_t)(bytes2 - bytesWritten), (size_t)bytesToPlay);

				int channelToDuplicate = getChannelToDuplicate(buffer);
				if (channelToDuplicate >= 0)
				{
					duplicateChannelToStereo(destPtr, buffer->data + playStartOffset, chunkSize, channelToDuplicate);
				}
				else
				{
					memcpy(destPtr, buffer->data + playStartOffset, chunkSize);
				}

				destPtr += chunkSize;
				bytesWritten += chunkSize;
			}
		}
	}
	else
	{
		// Non-looping or loop fits in buffer - normal copy
		size_t bytesToCopy = min((size_t)bytes1, (size_t)bytesToPlay);
		if (ptr1 && bytes1 > 0)
		{
			int channelToDuplicate = getChannelToDuplicate(buffer);

			if (channelToDuplicate >= 0)
			{
				duplicateChannelToStereo(ptr1, buffer->data + playStartOffset, bytesToCopy, channelToDuplicate);
			}
			else
			{
				memcpy(ptr1, buffer->data + playStartOffset, bytesToCopy);
			}

			// Zero out any remaining space
			if (bytes1 > bytesToCopy)
			{
				memset((uint8_t*)ptr1 + bytesToCopy, 0, bytes1 - bytesToCopy);
			}
		}

		// Handle wrapped buffer
		if (ptr2 && bytes2 > 0)
		{
			size_t remainingData = (bytesToPlay > bytesToCopy) ? (bytesToPlay - bytesToCopy) : 0;
			size_t bytes2ToCopy = min((size_t)bytes2, remainingData);
			if (bytes2ToCopy > 0)
			{
				int channelToDuplicate = getChannelToDuplicate(buffer);

				if (channelToDuplicate >= 0)
				{
					duplicateChannelToStereo(ptr2, buffer->data + playStartOffset + bytesToCopy, bytes2ToCopy, channelToDuplicate);
				}
				else
				{
					memcpy(ptr2, buffer->data + playStartOffset + bytesToCopy, bytes2ToCopy);
				}
			}
			if (bytes2 > bytes2ToCopy)
			{
				memset((uint8_t*)ptr2 + bytes2ToCopy, 0, bytes2 - bytes2ToCopy);
			}
		}
	}

	buffer->dsBuffer->Unlock(ptr1, bytes1, ptr2, bytes2);
	info("updateBufferNew: Updated DirectSound buffer with loop data");

	// Start playback
	if (!isCurrentlyPlaying)
	{
		buffer->dsBuffer->SetCurrentPosition(0);

		DWORD playFlags = buffer->loop ? DSBPLAY_LOOPING : 0;
		hr = buffer->dsBuffer->Play(0, 0, playFlags);

		if (SUCCEEDED(hr))
		{
			info("updateBufferNew: Started playback (loop=%s, DS buffer size=%d, loop region tiled throughout)",
				buffer->loop ? "YES" : "NO", caps.dwBufferBytes);
		}
		else
		{
			info("updateBufferNew: Play failed: 0x%08x", hr);
		}
	}
	else
	{
		info("updateBufferNew: Buffer already playing - updated in place");
	}
}

extern "C" {
	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_CreateBuffer(OPEN_HAWOSEBUFFERCONFIG* pConfig, OPEN_HAWOSEGABUFFERCALLBACK pCallback, unsigned int dwFlags, void** phHandle)
	{
		// Validate input parameters
		if (phHandle == NULL || pConfig == NULL)
		{
			info("SEGAAPI_CreateBuffer: Invalid parameters (phHandle: %08X, pConfig: %08X)", phHandle, pConfig);
			return OPEN_SEGAERR_BAD_POINTER;
		}

		// Validate configuration parameters
		if (pConfig->byNumChans == 0 || pConfig->byNumChans > 6)
		{
			info("SEGAAPI_CreateBuffer: Invalid channel count: %d", pConfig->byNumChans);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		if (pConfig->dwSampleRate == 0)
		{
			info("SEGAAPI_CreateBuffer: Invalid sample rate: %d", pConfig->dwSampleRate);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		if (pConfig->mapData.dwSize == 0)
		{
			info("SEGAAPI_CreateBuffer: Invalid buffer size: %d", pConfig->mapData.dwSize);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		// Log buffer creation request
		info("SEGAAPI_CreateBuffer: synth=%d, userMem=%d, mappedMem=%d, size=%d, sampleRate=%d, channels=%d, priority=%d, format=%d, callback=%08X",
			(dwFlags & OPEN_HABUF_SYNTH_BUFFER) ? 1 : 0,
			(dwFlags & OPEN_HABUF_ALLOC_USER_MEM) ? 1 : 0,
			(dwFlags & OPEN_HABUF_USE_MAPPED_MEM) ? 1 : 0,
			pConfig->mapData.dwSize,
			pConfig->dwSampleRate,
			pConfig->byNumChans,
			pConfig->dwPriority,
			pConfig->dwSampleFormat,
			pCallback);

		// Allocate and initialize buffer structure
		OPEN_segaapiBuffer_t* buffer = new OPEN_segaapiBuffer_t();
		if (!buffer)
		{
			info("SEGAAPI_CreateBuffer: Failed to allocate buffer structure");
			return OPEN_SEGAERR_FAIL;
		}

		// Calculate format parameters
		auto sampleBits = (pConfig->dwSampleFormat == OPEN_HASF_SIGNED_16PCM) ? 16 : 8;
		auto blockAlign = (sampleBits * pConfig->byNumChans) / 8;

		// Initialize all members explicitly
		buffer->userData = pConfig->hUserData;
		buffer->callback = pCallback;
		buffer->synthesizer = (dwFlags & OPEN_HABUF_SYNTH_BUFFER) != 0;
		buffer->loop = false;
		buffer->channels = pConfig->byNumChans;
		buffer->startLoop = 0;
		buffer->endLoop = pConfig->mapData.dwSize;
		buffer->endOffset = pConfig->mapData.dwSize;
		buffer->sampleRate = pConfig->dwSampleRate;
		buffer->sampleFormat = pConfig->dwSampleFormat;
		buffer->data = nullptr;
		buffer->size = pConfig->mapData.dwSize;  // Keep the FULL requested size
		buffer->playing = false;
		buffer->paused = false;
		buffer->playWithSetup = false;
		buffer->ownsData = false;
		buffer->pendingRouting = false;
		buffer->dsBuffer = nullptr;
		buffer->lastPlayCursor = 0;
		buffer->masterVolume = 1.0f;
		buffer->frequency = 1.0f;
		buffer->pan = 0;

		// Validate minimum buffer size
		const unsigned int MIN_BUFFER_SIZE = blockAlign * 4;
		if (buffer->size < MIN_BUFFER_SIZE)
		{
			info("SEGAAPI_CreateBuffer: Buffer size %d too small (min %d), adjusting", buffer->size, MIN_BUFFER_SIZE);
			buffer->size = MIN_BUFFER_SIZE;
		}

		// Ensure buffer size is aligned to block size
		if (buffer->size % blockAlign != 0)
		{
			unsigned int alignedSize = ((buffer->size + blockAlign - 1) / blockAlign) * blockAlign;
			info("SEGAAPI_CreateBuffer: Aligning buffer size from %d to %d", buffer->size, alignedSize);
			buffer->size = alignedSize;
		}

		// Update config to reflect actual size
		pConfig->mapData.dwSize = buffer->size;
		buffer->endLoop = buffer->size;
		buffer->endOffset = buffer->size;

		// Handle memory allocation for the FULL buffer (this is cheap - just virtual memory)
		if (dwFlags & OPEN_HABUF_ALLOC_USER_MEM)
		{
			if (pConfig->mapData.hBufferHdr == nullptr)
			{
				info("SEGAAPI_CreateBuffer: OPEN_HABUF_ALLOC_USER_MEM flag set but hBufferHdr is NULL");
				delete buffer;
				return OPEN_SEGAERR_BAD_POINTER;
			}
			buffer->data = (uint8_t*)pConfig->mapData.hBufferHdr;
			buffer->ownsData = false;
		}
		else if (dwFlags & OPEN_HABUF_USE_MAPPED_MEM)
		{
			if (pConfig->mapData.hBufferHdr == nullptr)
			{
				info("SEGAAPI_CreateBuffer: OPEN_HABUF_USE_MAPPED_MEM flag set but hBufferHdr is NULL");
				delete buffer;
				return OPEN_SEGAERR_BAD_POINTER;
			}
			buffer->data = (uint8_t*)pConfig->mapData.hBufferHdr;
			buffer->ownsData = false;
		}
		else
		{
			// Allocate the FULL buffer in system memory
			buffer->data = (uint8_t*)malloc(buffer->size);
			if (!buffer->data)
			{
				info("SEGAAPI_CreateBuffer: Failed to allocate %d bytes for audio data", buffer->size);
				delete buffer;
				return OPEN_SEGAERR_FAIL;
			}
			memset(buffer->data, 0, buffer->size); // Initialize to silence
			buffer->ownsData = true;
		}

		// Set output pointer for mapped memory
		pConfig->mapData.hBufferHdr = buffer->data;
		pConfig->mapData.dwOffset = 0;

		// Determine DirectSound buffer size (cap for hardware, but track full size in software)
		const unsigned int MAX_DSOUND_BUFFER = 8 * 1024 * 1024; // 8 MB safe limit
		unsigned int dsBufferSize = buffer->size;

		if (dsBufferSize > MAX_DSOUND_BUFFER)
		{
			info("SEGAAPI_CreateBuffer: Large buffer detected (%d bytes). Using streaming approach with %d byte DirectSound buffer.",
				buffer->size, MAX_DSOUND_BUFFER);
			dsBufferSize = MAX_DSOUND_BUFFER;

			// Ensure DirectSound buffer is aligned
			if (dsBufferSize % blockAlign != 0)
			{
				dsBufferSize = (dsBufferSize / blockAlign) * blockAlign;
			}
		}

		// Setup WAVEFORMATEX structure
		buffer->dsFormat.wFormatTag = WAVE_FORMAT_PCM;
		buffer->dsFormat.nChannels = (WORD)pConfig->byNumChans;
		buffer->dsFormat.nSamplesPerSec = pConfig->dwSampleRate;
		buffer->dsFormat.wBitsPerSample = (WORD)sampleBits;
		buffer->dsFormat.nBlockAlign = (WORD)blockAlign;
		buffer->dsFormat.nAvgBytesPerSec = pConfig->dwSampleRate * blockAlign;
		buffer->dsFormat.cbSize = 0;

		// Create DirectSound buffer with the safe size
		DSBUFFERDESC dsbd;
		ZeroMemory(&dsbd, sizeof(DSBUFFERDESC));
		dsbd.dwSize = sizeof(DSBUFFERDESC);
		dsbd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_CTRLPAN |
			DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
		dsbd.dwBufferBytes = dsBufferSize;  // Use the capped size for DirectSound
		dsbd.lpwfxFormat = &buffer->dsFormat;

		info("SEGAAPI_CreateBuffer: Creating DirectSound buffer - Size=%d (logical=%d), Rate=%d, Bits=%d, Channels=%d, BlockAlign=%d",
			dsBufferSize, buffer->size, pConfig->dwSampleRate, sampleBits, pConfig->byNumChans, blockAlign);

		IDirectSoundBuffer* tempBuffer = nullptr;
		HRESULT hr = g_dsound->CreateSoundBuffer(&dsbd, &tempBuffer, NULL);
		if (FAILED(hr))
		{
			info("SEGAAPI_CreateBuffer: Failed to create DirectSound buffer: 0x%08x (size=%d, rate=%d, channels=%d, bits=%d)",
				hr, dsBufferSize, pConfig->dwSampleRate, pConfig->byNumChans, sampleBits);

			if (buffer->ownsData && buffer->data)
			{
				free(buffer->data);
			}
			delete buffer;
			return OPEN_SEGAERR_FAIL;
		}

		buffer->dsBuffer = tempBuffer;
		info("SEGAAPI_CreateBuffer: DirectSound buffer created successfully");

		// Initialize buffer state
		resetBuffer(buffer);

		// Add to global buffer list
		g_allBuffers.push_back(buffer);

		// Return handle
		*phHandle = buffer;
		info("SEGAAPI_CreateBuffer: Buffer created successfully, hHandle: %08X", buffer);

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetUserData(void* hHandle, void* hUserData)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetUserData: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetUserData: Handle: %08X UserData: %08X", hHandle, hUserData);

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

		info("SEGAAPI_GetUserData: Handle: %08X", hHandle);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		return buffer->userData;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_UpdateBuffer(void* hHandle, unsigned int dwStartOffset, unsigned int dwLength)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_UpdateBuffer: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_UpdateBuffer: Handle: %08X dwStartOffset: %08X, dwLength: %08X", hHandle, dwStartOffset, dwLength);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		// Apply any pending routing changes before updating buffers
		if (buffer->pendingRouting)
		{
			info("SEGAAPI_UpdateBuffer: Applying pending routing changes");
			updateRouting(buffer);
		}

		// Check if we have any valid routes set up before updating
		bool hasValidRoutes = false;
		for (int i = 0; i < 7; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT &&
				buffer->sendRoutes[i] >= 0 &&
				buffer->sendRoutes[i] < 6)
			{
				hasValidRoutes = true;
				break;
			}
		}

		// If no valid routes, set up default stereo routing before buffer update
		if (!hasValidRoutes)
		{
			info("SEGAAPI_UpdateBuffer: No valid routes detected, setting up default stereo");
			buffer->sendRoutes[0] = OPEN_HA_FRONT_LEFT_PORT;
			buffer->sendRoutes[1] = OPEN_HA_FRONT_RIGHT_PORT;
			buffer->sendVolumes[0] = 1.0f;
			buffer->sendVolumes[1] = 1.0f;
			buffer->sendChannels[0] = 0;
			buffer->sendChannels[1] = 1;
			buffer->pendingRouting = true;

			// Actually apply the routing configuration to DirectSound
			updateRouting(buffer);
			info("SEGAAPI_UpdateBuffer: Default routing applied");
		}

		updateBufferNew(buffer, dwStartOffset, dwLength);
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetEndOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetEndOffset: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetEndOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->endOffset = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetEndLoopOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetEndLoopOffset: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetEndLoopOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->endLoop = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetStartLoopOffset(void* hHandle, unsigned int dwOffset)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetStartLoopOffset: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetStartLoopOffset: Handle: %08X dwOffset: %08X", hHandle, dwOffset);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->startLoop = dwOffset;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSampleRate(void* hHandle, unsigned int dwSampleRate)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetSampleRate: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetSampleRate: Handle: %08X dwSampleRate: %08X", hHandle, dwSampleRate);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->sampleRate = dwSampleRate;

		if (buffer->dsBuffer)
		{
			buffer->dsBuffer->SetFrequency(dwSampleRate);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetLoopState(void* hHandle, int bDoContinuousLooping)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetLoopState: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetLoopState: Handle: %08X bDoContinuousLooping: %d", hHandle, bDoContinuousLooping);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->loop = bDoContinuousLooping;

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetPlaybackPosition(void* hHandle, unsigned int dwPlaybackPos)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetPlaybackPosition: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetPlaybackPosition: Handle: %08X dwPlaybackPos: %08X", hHandle, dwPlaybackPos);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (buffer->dsBuffer && dwPlaybackPos < buffer->size)
		{
			buffer->dsBuffer->SetCurrentPosition(dwPlaybackPos);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) unsigned int SEGAAPI_GetPlaybackPosition(void* hHandle)
	{
		if (hHandle == NULL)
		{
			return 0;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (buffer->dsBuffer == NULL)
		{
			return 0;
		}

		DWORD playCursor = 0;
		DWORD writeCursor = 0;
		buffer->dsBuffer->GetCurrentPosition(&playCursor, &writeCursor);

		info("SEGAAPI_GetPlaybackPosition: Handle: %08X PlayCursor: %08X", hHandle, playCursor);

		return playCursor;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Play(void* hHandle)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_Play: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_Play: Handle: %08X", hHandle);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		// Check if we have any valid routes set up
		bool hasValidRoutes = false;
		for (int i = 0; i < 7; i++)
		{
			if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT &&
				buffer->sendRoutes[i] >= 0 &&
				buffer->sendRoutes[i] < 6)
			{
				hasValidRoutes = true;
				break;
			}
		}

		// If no valid routes, set up default stereo routing
		if (!hasValidRoutes)
		{
			info("SEGAAPI_Play: No valid routes, setting up default stereo routing");
			buffer->sendRoutes[0] = OPEN_HA_FRONT_LEFT_PORT;
			buffer->sendRoutes[1] = OPEN_HA_FRONT_RIGHT_PORT;
			buffer->sendVolumes[0] = 1.0f;
			buffer->sendVolumes[1] = 1.0f;
			buffer->sendChannels[0] = 0;
			buffer->sendChannels[1] = 1;
			buffer->pendingRouting = true;
		}

		if (buffer->dsBuffer && !buffer->loop)
		{
			DWORD status = 0;
			buffer->dsBuffer->GetStatus(&status);
			bool isCurrentlyPlaying = (status & DSBSTATUS_PLAYING) != 0;

			if (isCurrentlyPlaying)
			{
				// Check playback position to see if sound has finished
				DWORD playCursor = 0;
				DWORD writeCursor = 0;
				buffer->dsBuffer->GetCurrentPosition(&playCursor, &writeCursor);

				unsigned int bytesToPlay = buffer->endOffset - buffer->startLoop;

				// Only stop and restart if the sound has finished
				if (playCursor < buffer->lastPlayCursor || playCursor >= bytesToPlay)
				{
					info("SEGAAPI_Play: Non-looping sound finished (cursor=%d, length=%d) - allowing restart",
						playCursor, bytesToPlay);
					buffer->dsBuffer->Stop();
					buffer->dsBuffer->SetCurrentPosition(0);
				}
			}
		}

		updateRouting(buffer);
		updateBufferNew(buffer, 0, 0);

		buffer->playing = true;
		buffer->paused = false;
		buffer->lastPlayCursor = 0;

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Stop(void* hHandle)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_Stop: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_Stop: Handle: %08X", hHandle);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->playing = false;
		buffer->paused = false;

		if (buffer->dsBuffer)
		{
			CHECK_HR(buffer->dsBuffer->Stop());
			buffer->dsBuffer->SetCurrentPosition(0);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_HAWOSTATUS SEGAAPI_GetPlaybackStatus(void* hHandle)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_INVALID", hHandle);
			return OPEN_HAWOSTATUS_INVALID;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (buffer->paused)
		{
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_PAUSE", hHandle);
			return OPEN_HAWOSTATUS_PAUSE;
		}

		if (buffer->dsBuffer)
		{
			DWORD status = 0;
			buffer->dsBuffer->GetStatus(&status);
			bool isHardwarePlaying = (status & DSBSTATUS_PLAYING) != 0;

			// For non-looping sounds, check if playback position indicates completion
			if (!buffer->loop && buffer->playing && isHardwarePlaying)
			{
				DWORD playCursor = 0;
				DWORD writeCursor = 0;
				buffer->dsBuffer->GetCurrentPosition(&playCursor, &writeCursor);

				// Calculate expected end position based on endOffset
				unsigned int bytesToPlay = buffer->endOffset - buffer->startLoop;

				// If we've played past the end of the audio data, the sound is done
				// Check if cursor has wrapped around or reached the end
				if (playCursor < buffer->lastPlayCursor || playCursor >= bytesToPlay)
				{
					info("SEGAAPI_GetPlaybackStatus: Non-looping sound reached end (cursor=%d, length=%d)",
						playCursor, bytesToPlay);

					// Stop the buffer explicitly
					buffer->dsBuffer->Stop();
					buffer->dsBuffer->SetCurrentPosition(0);
					isHardwarePlaying = false;
				}

				buffer->lastPlayCursor = playCursor;
			}

			if (!isHardwarePlaying && buffer->playing)
			{
				info("SEGAAPI_GetPlaybackStatus: Sound finished");

				// Clear playing flag
				buffer->playing = false;

				// Process deferred calls
				std::function<void()> fn;
				while (buffer->defers.try_pop(fn))
				{
					fn();
				}

				// Call the application callback if registered
				if (buffer->callback)
				{
					info("SEGAAPI_GetPlaybackStatus: Calling application callback");
					buffer->callback(hHandle, OPEN_HAWOS_NOTIFY);
				}
			}

			if (!isHardwarePlaying)
			{
				info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_STOP", hHandle);
				return OPEN_HAWOSTATUS_STOP;
			}
		}

		if (buffer->playing)
		{
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_ACTIVE", hHandle);
			return OPEN_HAWOSTATUS_ACTIVE;
		}
		else
		{
			info("SEGAAPI_GetPlaybackStatus: Handle: %08X, Status: OPEN_HAWOSTATUS_STOP", hHandle);
			return OPEN_HAWOSTATUS_STOP;
		}
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetReleaseState(void* hHandle, int bSet)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetReleaseState: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetReleaseState: Handle: %08X bSet: %08X", hHandle, bSet);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (bSet && buffer->dsBuffer)
		{
			buffer->playing = false;
			buffer->dsBuffer->Stop();
			buffer->dsBuffer->SetCurrentPosition(0);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_DestroyBuffer(void* hHandle)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_DestroyBuffer: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_DestroyBuffer: Handle: %08X", hHandle);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		g_allBuffers.erase(std::remove(g_allBuffers.begin(), g_allBuffers.end(), buffer), g_allBuffers.end());

		if (buffer->dsBuffer)
		{
			buffer->dsBuffer->Stop();
			buffer->dsBuffer->Release();
		}

		if (buffer->ownsData && buffer->data)
		{
			free(buffer->data);
		}

		delete buffer;
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) int SEGAAPI_SetGlobalEAXProperty(GUID* guid, unsigned long ulProperty, void* pData, unsigned long ulDataSize)
	{
		info("SEGAAPI_SetGlobalEAXProperty:");

		// Everything is fine
		return TRUE;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Init(void)
	{
		info("SEGAAPI_Init");

		CoInitialize(nullptr);

		CHECK_HR(DirectSoundCreate8(NULL, &g_dsound, NULL));
		CHECK_HR(g_dsound->SetCooperativeLevel(GetDesktopWindow(), DSSCL_PRIORITY));

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Exit(void)
	{
		info("SEGAAPI_Exit");

		if (g_dsound)
		{
			g_dsound->Release();
			g_dsound = nullptr;
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Reset(void)
	{
		info("SEGAAPI_Reset");
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetIOVolume(OPEN_HAPHYSICALIO dwPhysIO, unsigned int dwVolume)
	{
		info("SEGAAPI_SetIOVolume: dwPhysIO: %08X dwVolume: %08X", dwPhysIO, dwVolume);

		if (dwPhysIO < 0 || dwPhysIO >= 12)
		{
			info("SEGAAPI_SetIOVolume: Invalid physical IO port %d", dwPhysIO);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		float volume = dwVolume / (float)0xFFFFFFFF;
		g_masterVolumes[dwPhysIO] = volume;

		info("SEGAAPI_SetIOVolume: Set master volume for port %d to %f", dwPhysIO, volume);

		for (auto buffer : g_allBuffers)
		{
			bool affectsThisBuffer = false;
			for (int i = 0; i < 7; i++)
			{
				if (buffer->sendRoutes[i] != OPEN_HA_UNUSED_PORT)
				{
					int physPort = -1;
					switch (buffer->sendRoutes[i])
					{
					case OPEN_HA_FRONT_LEFT_PORT:
						physPort = OPEN_HA_OUT_FRONT_LEFT;
						break;
					case OPEN_HA_FRONT_RIGHT_PORT:
						physPort = OPEN_HA_OUT_FRONT_RIGHT;
						break;
					case OPEN_HA_FRONT_CENTER_PORT:
						physPort = OPEN_HA_OUT_FRONT_CENTER;
						break;
					case OPEN_HA_LFE_PORT:
						physPort = OPEN_HA_OUT_LFE_PORT;
						break;
					case OPEN_HA_REAR_LEFT_PORT:
						physPort = OPEN_HA_OUT_REAR_LEFT;
						break;
					case OPEN_HA_REAR_RIGHT_PORT:
						physPort = OPEN_HA_OUT_REAR_RIGHT;
						break;
					}

					if (physPort == dwPhysIO)
					{
						affectsThisBuffer = true;
						break;
					}
				}
			}

			if (affectsThisBuffer)
			{
				buffer->pendingRouting = true;
				updateRouting(buffer);
			}
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSendRouting(void* hHandle, unsigned int dwChannel, unsigned int dwSend, OPEN_HAROUTING dwDest)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetSendRouting: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetSendRouting: hHandle: %08X dwChannel: %08X dwSend: %08X dwDest: %08X", hHandle, dwChannel, dwSend, dwDest);

		if (dwSend >= 7)
		{
			info("SEGAAPI_SetSendRouting: Invalid send %d", dwSend);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		if (dwChannel >= 6)
		{
			info("SEGAAPI_SetSendRouting: Invalid channel %d", dwChannel);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->sendRoutes[dwSend] = dwDest;
		buffer->sendChannels[dwSend] = dwChannel;
		buffer->pendingRouting = true;

		if (buffer->dsBuffer)
		{
			info("SEGAAPI_SetSendLevel: Applying routing immediately");
			updateRouting(buffer);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSendLevel(void* hHandle, unsigned int dwChannel, unsigned int dwSend, unsigned int dwLevel)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetSendLevel: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetSendLevel: hHandle: %08X dwChannel: %08X dwSend: %08X dwLevel: %08X", hHandle, dwChannel, dwSend, dwLevel);

		if (dwSend >= 7)
		{
			info("SEGAAPI_SetSendLevel: Invalid send %d", dwSend);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		if (dwChannel >= 6)
		{
			info("SEGAAPI_SetSendLevel: Invalid channel %d", dwChannel);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		buffer->sendVolumes[dwSend] = dwLevel / (float)0xFFFFFFFF;
		buffer->sendChannels[dwSend] = dwChannel;
		buffer->pendingRouting = true;

		if (buffer->dsBuffer)
		{
			updateRouting(buffer);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSynthParam(void* hHandle, OPEN_HASYNTHPARAMSEXT param, int lPARWValue)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetSynthParam: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetSynthParam: hHandle: %08X OPEN_HASYNTHPARAMSEXT: %08X lPARWValue: %08X", hHandle, param, lPARWValue);

		if (param >= 26)
		{
			info("SEGAAPI_SetSynthParam: Invalid param %d", param);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (param == OPEN_HAVP_ATTENUATION)
		{
			long attenuationDB = -(long)(lPARWValue * 10);

			// Store as linear gain for routing calculations
			buffer->masterVolume = powf(10.0f, attenuationDB / 2000.0f);
			buffer->pendingRouting = true;
			updateRouting(buffer);

			info("SEGAAPI_SetSynthParam: OPEN_HAVP_ATTENUATION dB: %d (-%f dB), gain: %f",
				lPARWValue, lPARWValue / 10.0f, buffer->masterVolume);
		}
		else if (param == OPEN_HAVP_PITCH)
		{
			float semiTones = lPARWValue / 100.0f;
			float freqRatio = powf(2.0f, semiTones / 12.0f);

			buffer->frequency = freqRatio;

			if (buffer->dsBuffer)
			{
				DWORD newFreq = (DWORD)(buffer->sampleRate * freqRatio);
				newFreq = max(DSBFREQUENCY_MIN, min(DSBFREQUENCY_MAX, newFreq));
				buffer->dsBuffer->SetFrequency(newFreq);
			}

			info("SEGAAPI_SetSynthParam: OPEN_HAVP_PITCH hHandle: %08X semitones: %f freqRatio: %f", hHandle, semiTones, freqRatio);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) int SEGAAPI_GetSynthParam(void* hHandle, OPEN_HASYNTHPARAMSEXT param)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_GetSynthParam: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_GetSynthParam: hHandle: %08X OPEN_HASYNTHPARAMSEXT: %08X", hHandle, param);

		return 0; //todo not sure if actually used
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSynthParamMultiple(void* hHandle, unsigned int dwNumParams, OPEN_SynthParamSet* pSynthParams)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetSynthParamMultiple: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		if (dwNumParams > 0 && pSynthParams == NULL)
		{
			info("SEGAAPI_SetSynthParamMultiple: Null params pointer with count %d", dwNumParams);
			return OPEN_SEGAERR_BAD_POINTER;
		}

		info("SEGAAPI_SetSynthParamMultiple: hHandle: %08X dwNumParams: %08X pSynthParams: %08X", hHandle, dwNumParams, pSynthParams);

		for (unsigned int i = 0; i < dwNumParams; i++)
		{
			SEGAAPI_SetSynthParam(hHandle, pSynthParams[i].param, pSynthParams[i].lPARWValue);
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetChannelVolume(void* hHandle, unsigned int dwChannel, unsigned int dwVolume)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_SetChannelVolume: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_SetChannelVolume: hHandle: %08X dwChannel: %08X dwVolume: %08X", hHandle, dwChannel, dwVolume);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (dwChannel >= 6)
		{
			info("SEGAAPI_SetChannelVolume: Invalid channel %d", dwChannel);
			return OPEN_SEGAERR_BAD_PARAM;
		}

		buffer->channelVolumes[dwChannel] = dwVolume / (float)0xFFFFFFFF;
		buffer->pendingRouting = true;

		updateRouting(buffer);
		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) unsigned int SEGAAPI_GetChannelVolume(void* hHandle, unsigned int dwChannel)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_GetChannelVolume: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return 0;
		}

		info("SEGAAPI_GetChannelVolume: hHandle: %08X dwChannel: %08X", hHandle, dwChannel);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		if (dwChannel >= 6)
		{
			info("SEGAAPI_GetChannelVolume: Invalid channel %d", dwChannel);
			return 0;
		}

		return (unsigned int)(buffer->channelVolumes[dwChannel] * 0xFFFFFFFF);
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Pause(void* hHandle)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_Pause: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_Pause: hHandle: %08X", hHandle);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;

		buffer->playing = false;
		buffer->paused = true;

		if (buffer->dsBuffer)
		{
			CHECK_HR(buffer->dsBuffer->Stop());
		}

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_PlayWithSetup(
		void* hHandle,
		unsigned int dwNumSendRouteParams, OPEN_SendRouteParamSet* pSendRouteParams,
		unsigned int dwNumSendLevelParams, OPEN_SendLevelParamSet* pSendLevelParams,
		unsigned int dwNumVoiceParams, OPEN_VoiceParamSet* pVoiceParams,
		unsigned int dwNumSynthParams, OPEN_SynthParamSet* pSynthParams
	)
	{
		if (hHandle == NULL)
		{
			info("SEGAAPI_PlayWithSetup: Handle: %08X, Status: OPEN_SEGAERR_BAD_HANDLE", hHandle);
			return OPEN_SEGAERR_BAD_HANDLE;
		}

		info("SEGAAPI_PlayWithSetup: hHandle: %08X dwNumSendRouteParams: %d pSendRouteParams: %08X dwNumSendLevelParams: %d pSendLevelParams: %08X dwNumVoiceParams: %d pVoiceParams: %08X dwNumSynthParams: %d pSynthParams: %08X", hHandle, dwNumSendRouteParams, pSendRouteParams, dwNumSendLevelParams, pSendLevelParams, dwNumVoiceParams, pVoiceParams, dwNumSynthParams, pSynthParams);
		info("dwNumSynthParams: %d", dwNumSynthParams);

		OPEN_segaapiBuffer_t* buffer = (OPEN_segaapiBuffer_t*)hHandle;
		buffer->playWithSetup = true;

		for (unsigned int i = 0; i < dwNumSendRouteParams; i++)
		{
			SEGAAPI_SetSendRouting(hHandle, pSendRouteParams[i].dwChannel, pSendRouteParams[i].dwSend, pSendRouteParams[i].dwDest);
		}

		for (unsigned int i = 0; i < dwNumSendLevelParams; i++)
		{
			SEGAAPI_SetSendLevel(hHandle, pSendLevelParams[i].dwChannel, pSendLevelParams[i].dwSend, pSendLevelParams[i].dwLevel);
		}

		unsigned int loopStart = 0;
		unsigned int loopEnd = 0;
		unsigned int loopState = 0;
		unsigned int endOffset = 0;

		for (unsigned int i = 0; i < dwNumVoiceParams; i++)
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
				info("Unimplemented! OPEN_VOICEIOCTL_SET_NOTIFICATION_POINT");
				break;
			case OPEN_VOICEIOCTL_CLEAR_NOTIFICATION_POINT:
				info("Unimplemented! OPEN_VOICEIOCTL_CLEAR_NOTIFICATION_POINT");
				break;
			case OPEN_VOICEIOCTL_SET_NOTIFICATION_FREQUENCY:
				info("Unimplemented! OPEN_VOICEIOCTL_SET_NOTIFICATION_FREQUENCY");
				break;
			}
		}

		info("Loopdata: hHandle: %08X, loopStart: %08X, loopEnd: %08X, endOffset: %08X, loopState: %d, size: %d", hHandle, loopStart, loopEnd, endOffset, loopState, buffer->size);

		for (unsigned int i = 0; i < dwNumSynthParams; i++)
		{
			SEGAAPI_SetSynthParam(hHandle, pSynthParams[i].param, pSynthParams[i].lPARWValue);
		}

		SEGAAPI_Play(hHandle);

		return OPEN_SEGA_SUCCESS;
	}

	__declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_GetLastStatus(void)
	{
		info("SEGAAPI_GetLastStatus");
		return OPEN_SEGA_SUCCESS;
	}
}
#pragma optimize("", on)