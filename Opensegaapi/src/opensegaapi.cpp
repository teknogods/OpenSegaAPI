/*
* This file is part of the OpenParrot project - https://teknoparrot.com / https://github.com/teknogods
*
* See LICENSE and MENTIONS in the root of the source tree for information
* regarding licensing.
*/
extern "C" {
#include "opensegaapi.h"
}

#include <xaudio2.h>
#include <vector>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <memory>
#include <cstring>

#pragma comment(lib, "xaudio2.lib")
#pragma comment(lib, "ole32.lib")

// ----------------------------------------------------------------------------
// Internal Constants
// ----------------------------------------------------------------------------
namespace {
    constexpr float VOL_FULL = 1.0f;
    constexpr float VOL_HALF = 0.5f;

    constexpr size_t MAX_SEND_SLOTS = 13;
    constexpr size_t SEGA_OUTPUT_CHANNELS = 6; // 0:FL, 1:FR, 2:C, 3:LFE, 4:RL, 5:RR
    constexpr float INV_UINT32_MAX = 1.0f / 4294967295.0f;

    float RawAttenToGain(int raw) {
        float db = static_cast<float>(raw) * -0.1f;
        if (db <= -100.0f) return 0.0f;
        return std::pow(10.0f, db / 20.0f);
    }

    float RawPitchToRatio(int raw) {
        return std::pow(2.0f, static_cast<float>(raw) / 1200.0f);
    }
}

// ----------------------------------------------------------------------------
// Engine Context
// ----------------------------------------------------------------------------
class AudioSystem {
public:
    static AudioSystem& Get() {
        static AudioSystem instance;
        return instance;
    }

    AudioSystem() {
        constexpr float GLOBAL_HEADROOM = 0.6f;

        for (int i = 0; i < SEGA_OUTPUT_CHANNELS; ++i) {
            _outputScales[i] = GLOBAL_HEADROOM;
            _gameRequestedVolumes[i] = 1.0f;
        }
    }

    bool Initialize() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_xaudio2) return true;

        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) return false;

        hr = XAudio2Create(&_xaudio2, 0, XAUDIO2_DEFAULT_PROCESSOR);
        if (FAILED(hr)) return false;

        hr = _xaudio2->CreateMasteringVoice(&_masteringVoice, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_SAMPLERATE, 0, nullptr, nullptr);
        if (FAILED(hr)) {
            _xaudio2->Release();
            _xaudio2 = nullptr;
            return false;
        }

        XAUDIO2_VOICE_DETAILS details;
        _masteringVoice->GetVoiceDetails(&details);
        _deviceChannels = details.InputChannels;
        _deviceRate = details.InputSampleRate;

        for (int i = 0; i < SEGA_OUTPUT_CHANNELS; ++i) {
            IXAudio2SubmixVoice* submix = nullptr;
            hr = _xaudio2->CreateSubmixVoice(&submix, 1, _deviceRate, 0, 0, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                _submixVoices.push_back(submix);
                ConfigureDownmix(submix, i);

                float initialVol = _gameRequestedVolumes[i] * _outputScales[i];
                submix->SetVolume(initialVol, XAUDIO2_COMMIT_NOW);
            }
        }
        return true;
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(_mutex);

        for (auto* v : _submixVoices) {
            if (v) v->DestroyVoice();
        }
        _submixVoices.clear();

        if (_masteringVoice) {
            _masteringVoice->DestroyVoice();
            _masteringVoice = nullptr;
        }

        if (_xaudio2) {
            _xaudio2->Release();
            _xaudio2 = nullptr;
        }

        CoUninitialize();
    }

    void SetOutputScale(int channelIndex, float scale) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (channelIndex >= 0 && channelIndex < SEGA_OUTPUT_CHANNELS) {
            _outputScales[channelIndex] = scale;
            if (channelIndex < _submixVoices.size()) {
                float finalVol = _gameRequestedVolumes[channelIndex] * _outputScales[channelIndex];
                _submixVoices[channelIndex]->SetVolume(finalVol, XAUDIO2_COMMIT_NOW);
            }
        }
    }

    void SetGameRequestedVolume(int channelIndex, float volume) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (channelIndex >= 0 && channelIndex < SEGA_OUTPUT_CHANNELS) {
            _gameRequestedVolumes[channelIndex] = volume;
            if (channelIndex < _submixVoices.size()) {
                float finalVol = volume * _outputScales[channelIndex];
                _submixVoices[channelIndex]->SetVolume(finalVol, XAUDIO2_COMMIT_NOW);
            }
        }
    }

    float GetOutputVolume(int channelIndex) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (channelIndex >= 0 && channelIndex < SEGA_OUTPUT_CHANNELS) {
            return _gameRequestedVolumes[channelIndex];
        }
        return 0.0f;
    }

    IXAudio2* GetXAudio2() const { return _xaudio2; }
    IXAudio2SubmixVoice* GetSubmix(size_t index) {
        if (index < _submixVoices.size()) return _submixVoices[index];
        return nullptr;
    }
    IXAudio2MasteringVoice* GetMasteringVoice() { return _masteringVoice; }
    std::mutex& GetMutex() { return _mutex; }

private:
    void ConfigureDownmix(IXAudio2SubmixVoice* submix, int channelIndex) {
        std::vector<float> matrix(_deviceChannels, 0.0f);

        if (_deviceChannels >= 6) {
            if (channelIndex < _deviceChannels) matrix[channelIndex] = 1.0f;
        }
        else if (_deviceChannels == 2) {
            switch (channelIndex) {
            case 0: matrix[0] = VOL_FULL; break; // FL
            case 1: matrix[1] = VOL_FULL; break; // FR
            case 2: matrix[0] = VOL_FULL; matrix[1] = VOL_FULL; break; // Center
            case 3: matrix[0] = VOL_HALF; matrix[1] = VOL_HALF; break; // LFE
            case 4: matrix[0] = VOL_HALF; break; // RL
            case 5: matrix[1] = VOL_HALF; break; // RR
            }
        }
        else {
            matrix[0] = 1.0f;
        }
        submix->SetOutputMatrix(_masteringVoice, 1, _deviceChannels, matrix.data(), 0);
    }

    IXAudio2* _xaudio2 = nullptr;
    IXAudio2MasteringVoice* _masteringVoice = nullptr;
    std::vector<IXAudio2SubmixVoice*> _submixVoices;
    uint32_t _deviceChannels = 2;
    uint32_t _deviceRate = 48000;
    std::mutex _mutex;

    float _outputScales[SEGA_OUTPUT_CHANNELS];
    float _gameRequestedVolumes[SEGA_OUTPUT_CHANNELS];
};

// ----------------------------------------------------------------------------
// Voice Class
// ----------------------------------------------------------------------------
class SegaVoice {
public:
    SegaVoice(const OPEN_HAWOSEBUFFERCONFIG* config, OPEN_HAWOSEGABUFFERCALLBACK cb, bool isMap)
        : _callback(cb), _userData(config->hUserData), _format(*config), _isMappedMemory(isMap)
    {
        _dataSize = config->mapData.dwSize;

        if (_isMappedMemory) {
            _audioDataPtr = static_cast<uint8_t*>(config->mapData.hBufferHdr);
        }
        else {
            _internalBuffer.resize(_dataSize, 0);
            _audioDataPtr = _internalBuffer.data();
            const_cast<OPEN_HAWOSEBUFFERCONFIG*>(config)->mapData.hBufferHdr = _audioDataPtr;
        }

        _blockAlign = config->byNumChans * ((config->dwSampleFormat == OPEN_HASF_SIGNED_16PCM) ? 2 : 1);
        _loopEnd = _dataSize;
        _playEnd = _dataSize;

        std::fill(_channelVol, _channelVol + SEGA_OUTPUT_CHANNELS, 0xFFFFFFFF);
        std::memset(_sendLevel, 0, sizeof(_sendLevel));

        UpdateRoutingByFormat(); // Initialize Routing

        CreateXAudio2Voice();
    }

    ~SegaVoice() {
        if (_voice) {
            _voice->DestroyVoice();
            _voice = nullptr;
        }
    }

    OPEN_SEGASTATUS Play() {
        if (!_voice) return OPEN_SEGAERR_FAIL;
        ApplyMix();
        _voice->Stop(0, XAUDIO2_COMMIT_NOW);
        _voice->FlushSourceBuffers();
        SubmitBuffer(0);

        _status = OPEN_HAWOSTATUS_ACTIVE;
        _voice->Start(0, XAUDIO2_COMMIT_NOW);

        return OPEN_SEGA_SUCCESS;
    }

    OPEN_SEGASTATUS Stop() {
        if (!_voice) return OPEN_SEGAERR_FAIL;
        _status = OPEN_HAWOSTATUS_STOP;
        _voice->Stop(0, XAUDIO2_COMMIT_NOW);
        _voice->FlushSourceBuffers();
        return OPEN_SEGA_SUCCESS;
    }

    OPEN_SEGASTATUS Pause() {
        if (!_voice) return OPEN_SEGAERR_FAIL;
        _status = OPEN_HAWOSTATUS_PAUSE;
        _voice->Stop(0, XAUDIO2_COMMIT_NOW);
        return OPEN_SEGA_SUCCESS;
    }

    OPEN_SEGASTATUS SetPosition(unsigned int pos) {
        bool wasPlaying = (_status == OPEN_HAWOSTATUS_ACTIVE);
        RecreateVoice();
        SubmitBuffer(pos);
        if (wasPlaying) _voice->Start(0, XAUDIO2_COMMIT_NOW);
        return OPEN_SEGA_SUCCESS;
    }

    OPEN_SEGASTATUS UpdateBufferParams() {
        if (!_voice) return OPEN_SEGAERR_FAIL;
        _voice->FlushSourceBuffers();
        SubmitBuffer(0);
        if (_status == OPEN_HAWOSTATUS_ACTIVE) _voice->Start(0, XAUDIO2_COMMIT_NOW);
        return OPEN_SEGA_SUCCESS;
    }

    void SetLoopState(int looping) {
        if (_isLooping == looping) return; 
        _isLooping = looping;
        UpdateRoutingByFormat();
        _mixDirty = true;
        ApplyMix();
    }

    void SetRouting(uint32_t ch, uint32_t send, OPEN_HAROUTING dest) {
        if (ch < SEGA_OUTPUT_CHANNELS && send < MAX_SEND_SLOTS) {
            _routing[ch][send] = dest;
            _mixDirty = true;
            ApplyMix();
        }
    }

    void SetVolume(uint32_t ch, uint32_t vol) {
        if (ch < SEGA_OUTPUT_CHANNELS) {
            _channelVol[ch] = vol;
            _mixDirty = true;
            ApplyMix();
        }
    }

    void SetSendLevel(uint32_t ch, uint32_t send, uint32_t level) {
        if (ch < SEGA_OUTPUT_CHANNELS && send < MAX_SEND_SLOTS) {
            _sendLevel[ch][send] = level;
            _mixDirty = true;
            ApplyMix();
        }
    }

    void SetSynthParam(OPEN_HASYNTHPARAMSEXT param, int val) {
        if (param == OPEN_HAVP_ATTENUATION) {
            _currentGain = RawAttenToGain(val);
            if (_voice) _voice->SetVolume(_currentGain, XAUDIO2_COMMIT_NOW);
        }
        else if (param == OPEN_HAVP_PITCH) {
            _currentRatio = RawPitchToRatio(val);
            if (_voice) _voice->SetFrequencyRatio(_currentRatio, XAUDIO2_COMMIT_NOW);
        }
        if (param < 30) _synthParams[param] = val;
    }

    void SetSampleRate(unsigned int rate) {
        if (_format.dwSampleRate == rate) return;
        _format.dwSampleRate = rate;
        RecreateVoice();
    }

    OPEN_HAWOSTATUS GetStatus() {
        if (_status == OPEN_HAWOSTATUS_PAUSE) return OPEN_HAWOSTATUS_PAUSE;
        XAUDIO2_VOICE_STATE state;
        _voice->GetState(&state, 0);
        return (state.BuffersQueued > 0) ? _status : OPEN_HAWOSTATUS_STOP;
    }

    unsigned int GetPosition() {
        XAUDIO2_VOICE_STATE state;
        _voice->GetState(&state, 0);
        return static_cast<unsigned int>(state.SamplesPlayed) * _blockAlign % _dataSize;
    }

    unsigned int _loopStart = 0;
    unsigned int _loopEnd = 0;
    unsigned int _playEnd = 0;
    int _isLooping = 0;
    void* _userData = nullptr;
    OPEN_HAWOSEBUFFERCONFIG _format;
    int _synthParams[30] = { 0 };

private:
    void UpdateRoutingByFormat() {
        for (int i = 0; i < SEGA_OUTPUT_CHANNELS; i++) {
            for (int j = 0; j < MAX_SEND_SLOTS; j++) {
                _routing[i][j] = OPEN_HA_UNUSED_PORT;
            }
        }

        if (_format.byNumChans == 1) {
            _routing[0][0] = OPEN_HA_FRONT_LEFT_PORT;
            _routing[0][1] = OPEN_HA_FRONT_RIGHT_PORT;
        }
        else if (_format.byNumChans == 2) {
            _routing[0][0] = OPEN_HA_FRONT_LEFT_PORT;
            _routing[1][1] = OPEN_HA_FRONT_RIGHT_PORT;
        }
        else {
            for (unsigned int i = 0; i < _format.byNumChans; ++i) {
                if (i < SEGA_OUTPUT_CHANNELS) _routing[i][i] = (OPEN_HAROUTING)i;
            }
        }
    }

    void CreateXAudio2Voice() {
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = static_cast<WORD>(_format.byNumChans);
        fmt.nSamplesPerSec = _format.dwSampleRate;
        fmt.nBlockAlign = static_cast<WORD>(_blockAlign);
        fmt.nAvgBytesPerSec = fmt.nSamplesPerSec * fmt.nBlockAlign;
        fmt.wBitsPerSample = (_format.dwSampleFormat == OPEN_HASF_SIGNED_16PCM) ? 16 : 8;
        fmt.cbSize = 0;

        AudioSystem::Get().GetXAudio2()->CreateSourceVoice(&_voice, &fmt, 0, XAUDIO2_MAX_FREQ_RATIO, nullptr, nullptr, nullptr);
        _voice->SetVolume(_currentGain, XAUDIO2_COMMIT_NOW);
    }

    void RecreateVoice() {
        if (_voice) {
            _voice->DestroyVoice();
            _voice = nullptr;
        }
        CreateXAudio2Voice();
        _voice->SetVolume(_currentGain, XAUDIO2_COMMIT_NOW);
        _voice->SetFrequencyRatio(_currentRatio, XAUDIO2_COMMIT_NOW);
        _mixDirty = true;
        ApplyMix();
    }

    void SubmitBuffer(unsigned int byteOffset) {
        XAUDIO2_BUFFER buf = {};
        buf.Flags = XAUDIO2_END_OF_STREAM;
        buf.AudioBytes = static_cast<uint32_t>(_dataSize);
        buf.pAudioData = _audioDataPtr;
        unsigned int startBytes = (byteOffset > 0) ? byteOffset : _loopStart;
        buf.PlayBegin = (_blockAlign > 0) ? (startBytes / _blockAlign) : 0;
        buf.PlayLength = (_blockAlign > 0) ? ((_playEnd - startBytes) / _blockAlign) : 0;

        if (_isLooping) {
            buf.LoopBegin = (_blockAlign > 0) ? (_loopStart / _blockAlign) : 0;
            buf.LoopLength = (_blockAlign > 0) ? ((_loopEnd - _loopStart) / _blockAlign) : 0;
            buf.LoopCount = XAUDIO2_LOOP_INFINITE;
        }
        _voice->SubmitSourceBuffer(&buf, nullptr);
    }

    void ApplyMix() {
        if (!_mixDirty || !_voice) return;

        XAUDIO2_SEND_DESCRIPTOR sendDesc[MAX_SEND_SLOTS];
        int activeSlotMap[MAX_SEND_SLOTS];
        std::memset(activeSlotMap, -1, sizeof(activeSlotMap));

        uint32_t sendCount = 0;

        for (int slot = 0; slot < MAX_SEND_SLOTS; ++slot) {
            bool isActive = false;
            OPEN_HAROUTING targetPort = OPEN_HA_UNUSED_PORT;
            for (int ch = 0; ch < _format.byNumChans; ++ch) {
                if (_routing[ch][slot] != OPEN_HA_UNUSED_PORT) {
                    targetPort = _routing[ch][slot];
                    isActive = true;
                    break;
                }
            }
            if (isActive && targetPort < SEGA_OUTPUT_CHANNELS) {
                IXAudio2SubmixVoice* target = AudioSystem::Get().GetSubmix(targetPort);
                if (target) {
                    sendDesc[sendCount].Flags = 0;
                    sendDesc[sendCount].pOutputVoice = target;
                    activeSlotMap[slot] = sendCount;
                    sendCount++;
                }
            }
        }

        if (sendCount > 0) {
            XAUDIO2_VOICE_SENDS sendList = { sendCount, sendDesc };
            _voice->SetOutputVoices(&sendList);
        }
        else {
            _voice->SetOutputVoices(nullptr);
        }

        float matrix[16];
        uint32_t actualMatrixSize = (_format.byNumChans > 16) ? 16 : _format.byNumChans;

        for (int slot = 0; slot < MAX_SEND_SLOTS; ++slot) {
            int sendIndex = activeSlotMap[slot];
            if (sendIndex == -1) continue;

            for (int ch = 0; ch < actualMatrixSize; ++ch) {
                matrix[ch] = 0.0f;
                if (_routing[ch][slot] != OPEN_HA_UNUSED_PORT) {
                    float chGain = static_cast<float>(_channelVol[ch]) * INV_UINT32_MAX;
                    float sendGain = static_cast<float>(_sendLevel[ch][slot]) * INV_UINT32_MAX;
                    matrix[ch] = chGain * sendGain;
                }
            }
            IXAudio2SubmixVoice* target = static_cast<IXAudio2SubmixVoice*>(sendDesc[sendIndex].pOutputVoice);
            _voice->SetOutputMatrix(target, _format.byNumChans, 1, matrix, XAUDIO2_COMMIT_NOW);
        }
        _mixDirty = false;
    }

    IXAudio2SourceVoice* _voice = nullptr;
    std::vector<uint8_t> _internalBuffer;
    uint8_t* _audioDataPtr = nullptr;
    size_t _dataSize = 0;
    OPEN_HAWOSEGABUFFERCALLBACK _callback = nullptr;
    OPEN_HAWOSTATUS _status = OPEN_HAWOSTATUS_STOP;
    bool _isMappedMemory = false;
    unsigned int _blockAlign = 0;
    float _currentGain = 1.0f;
    float _currentRatio = 1.0f;
    OPEN_HAROUTING _routing[SEGA_OUTPUT_CHANNELS][MAX_SEND_SLOTS];
    uint32_t _sendLevel[SEGA_OUTPUT_CHANNELS][MAX_SEND_SLOTS];
    uint32_t _channelVol[SEGA_OUTPUT_CHANNELS];
    bool _mixDirty = true;
};

// ----------------------------------------------------------------------------
// C-API Implementation
// ----------------------------------------------------------------------------

#define GET_VOICE(h) static_cast<SegaVoice*>(h)
#define API_LOCK std::lock_guard<std::mutex> lock(AudioSystem::Get().GetMutex())

extern "C" {

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Init(void) {
        return AudioSystem::Get().Initialize() ? OPEN_SEGA_SUCCESS : OPEN_SEGAERR_FAIL;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Exit(void) {
        AudioSystem::Get().Shutdown();
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_CreateBuffer(OPEN_HAWOSEBUFFERCONFIG* pConfig, OPEN_HAWOSEGABUFFERCALLBACK pCallback, unsigned int dwFlags, void** phHandle) {
        API_LOCK;
        if (!pConfig || !phHandle) return OPEN_SEGAERR_BAD_POINTER;
        bool isMap = (dwFlags & OPEN_HABUF_ALLOC_USER_MEM) || (dwFlags & OPEN_HABUF_USE_MAPPED_MEM);
        *phHandle = new SegaVoice(pConfig, pCallback, isMap);
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_DestroyBuffer(void* hHandle) {
        API_LOCK;
        if (!hHandle) return OPEN_SEGAERR_BAD_HANDLE;
        delete GET_VOICE(hHandle);
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Play(void* h) { API_LOCK; return GET_VOICE(h)->Play(); }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Pause(void* h) { API_LOCK; return GET_VOICE(h)->Pause(); }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Stop(void* h) { API_LOCK; return GET_VOICE(h)->Stop(); }
    __declspec(dllexport) OPEN_HAWOSTATUS SEGAAPI_GetPlaybackStatus(void* h) { API_LOCK; return GET_VOICE(h)->GetStatus(); }
    __declspec(dllexport) unsigned int SEGAAPI_GetPlaybackPosition(void* h) { API_LOCK; return GET_VOICE(h)->GetPosition(); }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetPlaybackPosition(void* h, unsigned int p) { API_LOCK; return GET_VOICE(h)->SetPosition(p); }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_UpdateBuffer(void* h, unsigned int, unsigned int) { API_LOCK; return GET_VOICE(h)->UpdateBufferParams(); }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSendRouting(void* h, unsigned int c, unsigned int s, OPEN_HAROUTING d) {
        API_LOCK; GET_VOICE(h)->SetRouting(c, s, d); return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSendLevel(void* h, unsigned int c, unsigned int s, unsigned int l) {
        API_LOCK; GET_VOICE(h)->SetSendLevel(c, s, l); return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetChannelVolume(void* h, unsigned int c, unsigned int v) {
        API_LOCK; GET_VOICE(h)->SetVolume(c, v); return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSynthParam(void* h, OPEN_HASYNTHPARAMSEXT p, int v) {
        API_LOCK; GET_VOICE(h)->SetSynthParam(p, v); return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) int SEGAAPI_GetSynthParam(void* h, OPEN_HASYNTHPARAMSEXT p) {
        API_LOCK; return GET_VOICE(h)->_synthParams[p];
    }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSampleRate(void* h, unsigned int r) {
        API_LOCK; GET_VOICE(h)->SetSampleRate(r); return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetStartLoopOffset(void* h, unsigned int v) {
        API_LOCK;
        if (GET_VOICE(h)->_loopStart == v) return OPEN_SEGA_SUCCESS;
        GET_VOICE(h)->_loopStart = v;
        GET_VOICE(h)->UpdateBufferParams();
        return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) unsigned int SEGAAPI_GetStartLoopOffset(void* h) { API_LOCK; return GET_VOICE(h)->_loopStart; }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetEndLoopOffset(void* h, unsigned int v) {
        API_LOCK;
        if (GET_VOICE(h)->_loopEnd == v) return OPEN_SEGA_SUCCESS;
        GET_VOICE(h)->_loopEnd = v;
        GET_VOICE(h)->UpdateBufferParams();
        return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) unsigned int SEGAAPI_GetEndLoopOffset(void* h) { API_LOCK; return GET_VOICE(h)->_loopEnd; }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetEndOffset(void* h, unsigned int v) {
        API_LOCK;
        GET_VOICE(h)->_playEnd = v;
        GET_VOICE(h)->UpdateBufferParams();
        return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) unsigned int SEGAAPI_GetEndOffset(void* h) { API_LOCK; return GET_VOICE(h)->_playEnd; }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetLoopState(void* h, int b) { API_LOCK; GET_VOICE(h)->SetLoopState(b); return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) int SEGAAPI_GetLoopState(void* h) { API_LOCK; return GET_VOICE(h)->_isLooping; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetUserData(void* h, void* u) { API_LOCK; GET_VOICE(h)->_userData = u; return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) void* SEGAAPI_GetUserData(void* h) { API_LOCK; return GET_VOICE(h)->_userData; }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_PlayWithSetup(void* h, unsigned int nr, OPEN_SendRouteParamSet* pr, unsigned int nl, OPEN_SendLevelParamSet* pl, unsigned int nv, OPEN_VoiceParamSet* pv, unsigned int ns, OPEN_SynthParamSet* ps) {
        API_LOCK;
        auto* v = GET_VOICE(h);
        for (unsigned int i = 0; i < nr; ++i) v->SetRouting(pr[i].dwChannel, pr[i].dwSend, pr[i].dwDest);
        for (unsigned int i = 0; i < nl; ++i) v->SetSendLevel(pl[i].dwChannel, pl[i].dwSend, pl[i].dwLevel);
        for (unsigned int i = 0; i < nv; ++i) {
            switch (pv[i].VoiceIoctl) {
            case OPEN_VOICEIOCTL_SET_START_LOOP_OFFSET: v->_loopStart = pv[i].dwParam1; break;
            case OPEN_VOICEIOCTL_SET_END_LOOP_OFFSET: v->_loopEnd = pv[i].dwParam1; break;
            case OPEN_VOICEIOCTL_SET_END_OFFSET: v->_playEnd = pv[i].dwParam1; break;
            case OPEN_VOICEIOCTL_SET_LOOP_STATE: v->SetLoopState(pv[i].dwParam1); break;
            case OPEN_VOICEIOCTL_SET_PLAY_POSITION: v->SetPosition(pv[i].dwParam1); break;
            }
        }
        v->UpdateBufferParams();
        for (unsigned int i = 0; i < ns; ++i) v->SetSynthParam(ps[i].param, ps[i].lPARWValue);

        return v->Play();
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetIOVolume(OPEN_HAPHYSICALIO p, unsigned int v) {
        AudioSystem::Get().SetGameRequestedVolume(static_cast<int>(p), static_cast<float>(v) * INV_UINT32_MAX);
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) unsigned int SEGAAPI_GetIOVolume(OPEN_HAPHYSICALIO p) {
        return static_cast<unsigned int>(AudioSystem::Get().GetOutputVolume(static_cast<int>(p)) * 4294967295.0f);
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_GetFormat(void* h, OPEN_HAWOSEFORMAT* f) {
        API_LOCK;
        auto* v = GET_VOICE(h);
        f->dwSampleRate = v->_format.dwSampleRate;
        f->dwSampleFormat = v->_format.dwSampleFormat;
        f->byNumChans = v->_format.byNumChans;
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetFormat(void* h, OPEN_HAWOSEFORMAT* f) {
        API_LOCK;
        auto* v = GET_VOICE(h);
        v->_format.dwSampleRate = f->dwSampleRate;
        v->_format.dwSampleFormat = f->dwSampleFormat;
        v->_format.byNumChans = f->byNumChans;
        return OPEN_SEGA_SUCCESS;
    }

    __declspec(dllexport) unsigned int SEGAAPI_GetSampleRate(void* h) { API_LOCK; return GET_VOICE(h)->_format.dwSampleRate; }

    // ------------------------------------------------------------------------
    // Stub Functions
    // ------------------------------------------------------------------------
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetPriority(void*, unsigned int) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) unsigned int SEGAAPI_GetPriority(void*) { return 0xFFFFFFFF; }
    __declspec(dllexport) int SEGAAPI_SetGlobalEAXProperty(GUID*, unsigned long, void*, unsigned long) { return 1; }
    __declspec(dllexport) int SEGAAPI_GetGlobalEAXProperty(GUID*, unsigned long, void*, unsigned long) { return 1; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSPDIFOutChannelStatus(unsigned int, unsigned int) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_GetSPDIFOutChannelStatus(unsigned int*, unsigned int*) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSPDIFOutSampleRate(OPEN_HASPDIFOUTRATE) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_HASPDIFOUTRATE SEGAAPI_GetSPDIFOutSampleRate(void) { return OPEN_HASPDIFOUT_44_1KHZ; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSPDIFOutChannelRouting(unsigned int, OPEN_HAROUTING) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_HAROUTING SEGAAPI_GetSPDIFOutChannelRouting(unsigned int) { return OPEN_HA_UNUSED_PORT; }
    __declspec(dllexport) void SEGAAPI_SetLastStatus(OPEN_SEGASTATUS) {}
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_GetLastStatus(void) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_Reset(void) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetSynthParamMultiple(void* h, unsigned int n, OPEN_SynthParamSet* p) {
        API_LOCK; for (unsigned int i = 0; i < n; ++i) GET_VOICE(h)->SetSynthParam(p[i].param, p[i].lPARWValue); return OPEN_SEGA_SUCCESS;
    }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_GetSynthParamMultiple(void* h, unsigned int n, OPEN_SynthParamSet* p) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetReleaseState(void* h, int s) { if (s) return SEGAAPI_Stop(h); return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetNotificationFrequency(void*, unsigned int) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_SetNotificationPoint(void*, unsigned int) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_SEGASTATUS SEGAAPI_ClearNotificationPoint(void*, unsigned int) { return OPEN_SEGA_SUCCESS; }
    __declspec(dllexport) OPEN_HAROUTING SEGAAPI_GetSendRouting(void*, unsigned int, unsigned int) { return OPEN_HA_UNUSED_PORT; }
    __declspec(dllexport) unsigned int SEGAAPI_GetSendLevel(void*, unsigned int, unsigned int) { return 0; }
    __declspec(dllexport) unsigned int SEGAAPI_GetChannelVolume(void*, unsigned int) { return 0xFFFFFFFF; }

}