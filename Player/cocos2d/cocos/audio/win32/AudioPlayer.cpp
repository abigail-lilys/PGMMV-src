/****************************************************************************
 Copyright (c) 2014-2016 Chukong Technologies Inc.
 Copyright (c) 2017-2018 Xiamen Yaji Software Co., Ltd.

 http://www.cocos2d-x.org

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/

#define LOG_TAG "AudioPlayer"

#include "platform/CCPlatformConfig.h"
#include <stdio.h>

#if (CC_TARGET_PLATFORM == CC_PLATFORM_WIN32) || (CC_TARGET_PLATFORM == CC_PLATFORM_EMSCRIPTEN) // [wasm-port] audio impl guard
#include "audio/win32/AudioPlayer.h"
#include "audio/win32/AudioCache.h"
#include "platform/CCFileUtils.h"
#include "audio/win32/AudioDecoderManager.h"
#include "audio/win32/AudioDecoder.h" // [wasm-port] case

#define VERY_VERY_VERBOSE_LOGGING
#ifdef VERY_VERY_VERBOSE_LOGGING
#define ALOGVV ALOGV
#else
#define ALOGVV(...) do{} while(false)
#endif

using namespace cocos2d;
using namespace cocos2d::experimental;

namespace {
unsigned int __idIndex = 0;
}
#ifdef USE_AGTK//sakihama-h
AudioPlayer::AudioPlayer(std::string filePath)
#else
AudioPlayer::AudioPlayer()
#endif
: _audioCache(nullptr)
, _finishCallbak(nullptr)
, _isDestroyed(false)
, _removeByAudioEngine(false)
, _ready(false)
, _currTime(0.0f)
, _streamingSource(false)
, _rotateBufferThread(nullptr)
, _timeDirty(false)
, _isRotateThreadExited(false)
, _id(++__idIndex)
#ifdef USE_AGTK//hidet-sa
, _pitch(1.0f)
#endif
{
#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
    _wasmDecoder = nullptr;
    _wasmPcmBuffer = nullptr;
    _wasmPcmLeft = nullptr;
    _wasmPcmRight = nullptr;
    _wasmFramesToRead = 0;
    _wasmBufferBytes = 0;
    _wasmCurrentFrame = 0;
    _wasmLoopStart = 0;
    _wasmLoopEnd = 0;
    _wasmTotalFrames = 0;
    _wasmFormat = 0;
    _wasmSampleRate = 0;
    _wasmBytesPerFrame = 0;
    _wasmDuration = 0.0f;
    _wasmStereo = false;
    _wasmOggLooping = false;
    _wasmEof = false;
    _wasmInitialized = false;
#endif
#ifdef USE_AGTK//sakihama-h, 2018.10.03
	_filePath = filePath;
#endif
#ifdef USE_AGTK//hidet-sa
	memset(_pan, 0, sizeof(_pan));
#endif
    memset(_bufferIds, 0, sizeof(_bufferIds));
#ifdef USE_AGTK_STEREO_PAN
	memset(_bufferIds2, 0, sizeof(_bufferIds2));
#endif
}

AudioPlayer::~AudioPlayer()
{
    ALOGVV("~AudioPlayer() (%p), id=%u", this, _id);
    destroy();

    if (_streamingSource)
    {
        alDeleteBuffers(3, _bufferIds);
#ifdef USE_AGTK_STEREO_PAN
		if (_bufferIds2[0]) {
			alDeleteBuffers(3, _bufferIds2);
		}
#endif
    }
}

void AudioPlayer::destroy()
{
    if (_isDestroyed)
        return;

    ALOGVV("AudioPlayer::destroy begin, id=%u", _id);

    _isDestroyed = true;

    do
    {
        if (_audioCache != nullptr)
        {
            if (_audioCache->_state == AudioCache::State::INITIAL)
            {
                ALOGV("AudioPlayer::destroy, id=%u, cache isn't ready!", _id);
                break;
            }

            while (!_audioCache->_isLoadingFinished)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }

        // Wait for play2d to be finished.
        _play2dMutex.lock();
        _play2dMutex.unlock();

#if !defined(__EMSCRIPTEN__) || !defined(USE_AGTK)
        if (_streamingSource)
        {
            if (_rotateBufferThread != nullptr)
            {
                while (!_isRotateThreadExited)
                {
                    _sleepCondition.notify_one();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }

                if (_rotateBufferThread->joinable()) {
                    _rotateBufferThread->join();
                }

                delete _rotateBufferThread;
                _rotateBufferThread = nullptr;
                ALOGVV("rotateBufferThread exited!");
            }
        }
#endif
    } while(false);

    ALOGVV("Before alSourceStop");

#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
    bool wasmStereoForDestroy = _wasmStereo || (_audioCache && _audioCache->_isStereo);
#endif

    alSourceStop(_alSource); CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK_STEREO_PAN
#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
if (wasmStereoForDestroy) {
#else
if (_audioCache && _audioCache->_isStereo) {
#endif
alSourceStop(_alSource2); CHECK_AL_ERROR_DEBUG();
}
#endif

#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
    if (_streamingSource)
    {
        ALint queued = 0;
        alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued); CHECK_AL_ERROR_DEBUG();
        while (queued > 0)
        {
            ALuint bufferId = 0;
            alSourceUnqueueBuffers(_alSource, 1, &bufferId); CHECK_AL_ERROR_DEBUG();
            --queued;
        }
#ifdef USE_AGTK_STEREO_PAN
        if (wasmStereoForDestroy)
        {
            ALint queued2 = 0;
            alGetSourcei(_alSource2, AL_BUFFERS_QUEUED, &queued2); CHECK_AL_ERROR_DEBUG();
            while (queued2 > 0)
            {
                ALuint bufferId = 0;
                alSourceUnqueueBuffers(_alSource2, 1, &bufferId); CHECK_AL_ERROR_DEBUG();
                --queued2;
            }
        }
#endif
    }
    cleanupWasmStream();
#endif

    ALOGVV("Before alSourcei");
    alSourcei(_alSource, AL_BUFFER, NULL); CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK_STEREO_PAN
#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
if (wasmStereoForDestroy) {
#else
if (_audioCache && _audioCache->_isStereo) {
#endif
alSourcei(_alSource2, AL_BUFFER, NULL); CHECK_AL_ERROR_DEBUG();
}
#endif

    _removeByAudioEngine = true;

    _ready = false;
    ALOGVV("AudioPlayer::destroy end, id=%u", _id);
}

void AudioPlayer::setCache(AudioCache* cache)
{
    _audioCache = cache;
}

#ifdef USE_AGTK_STEREO_PAN
void AudioPlayer::calcVolume2(float pan[3], float volume2[2])
{
	if (pan[0] >= 0.0f) {
		volume2[0] = 0.77f * (1 - pan[0]);
		volume2[1] = (0.77f + pan[0] * 0.23f);
	}
	else {
		volume2[0] = (0.77f + (-pan[0]) * 0.23f);
		volume2[1] = 0.77f * (1 + pan[0]);
	}
#if 0
	if (mPan >= 0) {
		mLeftVolume = mVolume * 77 * (50 - mPan) / 50 / 100;
		mRightVolume = mVolume * (77 + mPan * 23 / 50) / 100;
	}
	else {
		mLeftVolume = mVolume * (77 + (-mPan) * 23 / 50) / 100;
		mRightVolume = mVolume * 77 * (50 + mPan) / 50 / 100;
	}
#endif
}
#endif
bool AudioPlayer::play2d()
{

    static unsigned int diagPlayCount = 0;

    _play2dMutex.lock();
    ALOGV("AudioPlayer::play2d, _alSource: %u, player id=%u", _alSource, _id);
#ifdef USE_AGTK_STEREO_PAN
	ALOGV("AudioPlayer::play2d, _alSource2: %u, player id=%u", _alSource2, _id);
#endif

    /*********************************************************************/
    /*       Note that it may be in sub thread or in main thread.       **/
    /*********************************************************************/
    bool ret = false;
    do
    {
        if (_audioCache->_state != AudioCache::State::READY)
        {
            ALOGE("alBuffer isn't ready for play!");
            break;
        }
#ifdef USE_AGTK_STEREO_PAN
#endif

        alSourcei(_alSource, AL_BUFFER, 0);CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK//hidet-sa
		alSourcef(_alSource, AL_PITCH, _pitch); CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK_STEREO_PAN
		float volume2[2];
		static float panList[2] = { -1.0f, 1.0f };
		calcVolume2(_pan, volume2);
		if (_audioCache->_isStereo) {
			alSource3f(_alSource, AL_POSITION, panList[0], _pan[1], _pan[2]); CHECK_AL_ERROR_DEBUG();
			alSourcef(_alSource, AL_GAIN, volume2[0] * _volume); CHECK_AL_ERROR_DEBUG();
		}
		else {
			float positionY;
			if (_pan[0] > 0) {
				positionY = 1 - _pan[0];
			}
			else {
				positionY = -1 - _pan[0];
			}
			alSource3f(_alSource, AL_POSITION, _pan[0], positionY, 0); CHECK_AL_ERROR_DEBUG();
		}
#else
		alSource3f(_alSource, AL_POSITION, _pan[0], _pan[1], _pan[2]); CHECK_AL_ERROR_DEBUG();
#endif
#else
        alSourcef(_alSource, AL_PITCH, 1.0f);CHECK_AL_ERROR_DEBUG();
#endif
#ifdef USE_AGTK_STEREO_PAN
		if (!_audioCache->_isStereo) {
			alSourcef(_alSource, AL_GAIN, _volume); CHECK_AL_ERROR_DEBUG();
		}
#else
        alSourcef(_alSource, AL_GAIN, _volume);CHECK_AL_ERROR_DEBUG();
#endif
        alSourcei(_alSource, AL_LOOPING, AL_FALSE);CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK_STEREO_PAN
		//alSourcei(_alSource2, AL_BUFFER, 0); CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK//hidet-sa
		//alSourcef(_alSource2, AL_PITCH, _pitch); CHECK_AL_ERROR_DEBUG();
		if (_audioCache->_isStereo) {
			alSourcei(_alSource2, AL_BUFFER, 0); CHECK_AL_ERROR_DEBUG();
			alSourcef(_alSource2, AL_PITCH, _pitch); CHECK_AL_ERROR_DEBUG();
			alSource3f(_alSource2, AL_POSITION, panList[1], _pan[1], _pan[2]); CHECK_AL_ERROR_DEBUG();
			alSourcef(_alSource2, AL_GAIN, volume2[1] * _volume); CHECK_AL_ERROR_DEBUG();
#ifdef __EMSCRIPTEN__
                {
                    static unsigned int gainReadbackCount = 0;
                }
#endif

		}
		else {
			//alSource3f(_alSource2, AL_POSITION, _pan[0], _pan[1], _pan[2]); CHECK_AL_ERROR_DEBUG();
			//alSourcef(_alSource2, AL_GAIN, _volume); CHECK_AL_ERROR_DEBUG();
		}
#else
		alSourcef(_alSource2, AL_PITCH, 1.0f); CHECK_AL_ERROR_DEBUG();
		alSourcef(_alSource2, AL_GAIN, _volume); CHECK_AL_ERROR_DEBUG();
#endif
		if (_audioCache->_isStereo) {
			alSourcei(_alSource2, AL_LOOPING, AL_FALSE); CHECK_AL_ERROR_DEBUG();
		}
#endif

        if (_audioCache->_queBufferFrames == 0)
        {
            if (_loop) {
                alSourcei(_alSource, AL_LOOPING, AL_TRUE);
                CHECK_AL_ERROR_DEBUG();
#ifdef USE_AGTK_STEREO_PAN
				if (_audioCache->_isStereo) {
					alSourcei(_alSource2, AL_LOOPING, AL_TRUE);
					CHECK_AL_ERROR_DEBUG();
				}
#endif
            }
#ifdef USE_AGTK
			// シーク再生
			if (_currTime > 0.0f) {
				alSourcef(_alSource, AL_SEC_OFFSET, _currTime);
#ifdef USE_AGTK_STEREO_PAN
				if (_audioCache->_isStereo) {
					alSourcef(_alSource2, AL_SEC_OFFSET, _currTime);
				}
#endif
			}
#endif
        }
        else
        {
            alGenBuffers(3, _bufferIds);
#ifdef USE_AGTK_STEREO_PAN
			if (_audioCache->_isStereo) {
				alGenBuffers(3, _bufferIds2);
			}
#endif

            auto alError = alGetError();
            if (alError == AL_NO_ERROR)
            {
                for (int index = 0; index < QUEUEBUFFER_NUM; ++index)
                {
#ifdef USE_AGTK_STEREO_PAN
					if (_audioCache->_isStereo) {
#ifdef USE_AGTK
						// ストリーミングのシーク再生
						if (_currTime > 0.0f) {
							// 一度キャッシュのデータを初期化
							memset(_audioCache->_queBuffers[index], 0, _audioCache->_queBufferSize[index]);
							memset(_audioCache->_queBuffers2[index], 0, _audioCache->_queBufferSize2[index]);
						}
#endif
						alBufferData(_bufferIds[index], _audioCache->_format, _audioCache->_queBuffers[index], _audioCache->_queBufferSize[index], _audioCache->_sampleRate);
						alBufferData(_bufferIds2[index], _audioCache->_format, _audioCache->_queBuffers2[index], _audioCache->_queBufferSize2[index], _audioCache->_sampleRate);
					}
					else {
						alBufferData(_bufferIds[index], _audioCache->_format, _audioCache->_queBuffers[index], _audioCache->_queBufferSize[index], _audioCache->_sampleRate);
					}
#else
                    alBufferData(_bufferIds[index], _audioCache->_format, _audioCache->_queBuffers[index], _audioCache->_queBufferSize[index], _audioCache->_sampleRate);
#endif
                }
                CHECK_AL_ERROR_DEBUG();
            }
            else
            {
                ALOGE("%s:alGenBuffers error code:%x", __FUNCTION__,alError);
                break;
            }
            _streamingSource = true;
        }

        {
            std::unique_lock<std::mutex> lk(_sleepMutex);
            if (_isDestroyed)
                break;

            if (_streamingSource)
            {
                alSourceQueueBuffers(_alSource, QUEUEBUFFER_NUM, _bufferIds);
#ifdef USE_AGTK_STEREO_PAN
				if (_audioCache->_isStereo) {
					alSourceQueueBuffers(_alSource2, QUEUEBUFFER_NUM, _bufferIds2);
				}
#endif
                CHECK_AL_ERROR_DEBUG();
#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
                if (!initWasmStream(_audioCache->_queBufferFrames * QUEUEBUFFER_NUM + 1))
                {
                    ALOGE("Failed to initialize WASM streaming audio.");
                    break;
                }
#else
                _rotateBufferThread = new std::thread(&AudioPlayer::rotateBufferThread, this, _audioCache->_queBufferFrames * QUEUEBUFFER_NUM + 1);
#endif
            }
            else
            {
                alSourcei(_alSource, AL_BUFFER, _audioCache->_alBufferId);
#ifdef USE_AGTK_STEREO_PAN
				if (_audioCache->_isStereo) {
					alSourcei(_alSource2, AL_BUFFER, _audioCache->_alBufferId2);
				}
#endif
                CHECK_AL_ERROR_DEBUG();
            }

            alSourcePlay(_alSource);
#ifdef USE_AGTK_STEREO_PAN
			if (_audioCache->_isStereo) {
				alSourcePlay(_alSource2);
			}
#endif
        }

        auto alError = alGetError();
        if (alError != AL_NO_ERROR)
        {
            ALOGE("%s:alSourcePlay error code:%x", __FUNCTION__,alError);
            break;
        }

        ALint state;
        alGetSourcei(_alSource, AL_SOURCE_STATE, &state);
        if (state != AL_PLAYING)
        {
            ALOGE("state isn't playing, %d, %s, cache id=%u, player id=%u", state, _audioCache->_fileFullPath.c_str(), _audioCache->_id, _id);
        }
        assert(state == AL_PLAYING);
        _ready = true;
        ret = true;
    } while (false);

    if (!ret)
    {
        _removeByAudioEngine = true;
    }

    _play2dMutex.unlock();
    return ret;
}

#ifdef USE_AGTK_STEREO_PAN
static void copyData(char *pcmData, char *pcmData2, char *buf, int size)
{
	while (size > 0) {
		pcmData[0] = buf[0];
		pcmData[1] = buf[1];
		pcmData2[0] = buf[2];
		pcmData2[1] = buf[3];
		pcmData += 2;
		pcmData2 += 2;
		buf += 4;
		size -= 4;
	}
}
#endif
#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)

bool AudioPlayer::initWasmStream(int offsetFrame)
{


    if (_wasmInitialized)
        return true;

    if (_audioCache == nullptr)
    {
        ALOGE("initWasmStream: AudioCache is null.");
        return false;
    }

#ifdef USE_AGTK_STEREO_PAN
    _wasmStereo = _audioCache->_isStereo;
#else
    _wasmStereo = false;
#endif

    AudioDecoder* decoder = AudioDecoderManager::createDecoder(_audioCache->_fileFullPath.c_str());
    

    if (decoder == nullptr)
    {
        ALOGE("initWasmStream: createDecoder failed.");
        return false;
    }

    if (!decoder->open(_audioCache->_fileFullPath.c_str()))
    {
        ALOGE("initWasmStream: decoder open failed.");
        AudioDecoderManager::destroyDecoder(decoder);
        return false;
    }

    const uint32_t framesToRead = _audioCache->_queBufferFrames;
    const uint32_t bytesPerFrame = decoder->getBytesPerFrame();

    if (framesToRead == 0 || bytesPerFrame == 0 || framesToRead > UINT32_MAX / bytesPerFrame)
    {
        ALOGE("initWasmStream: invalid PCM buffer size.");
        decoder->close();
        AudioDecoderManager::destroyDecoder(decoder);
        return false;
    }

    const uint32_t bufferBytes = framesToRead * bytesPerFrame;
    char* pcmBuffer = (char*)malloc(bufferBytes);
    if (pcmBuffer == nullptr)
    {
        ALOGE("initWasmStream: PCM allocation failed.");
        decoder->close();
        AudioDecoderManager::destroyDecoder(decoder);
        return false;
    }

    char* pcmLeft = nullptr;
    char* pcmRight = nullptr;
#ifdef USE_AGTK_STEREO_PAN
    if (_wasmStereo)
    {
        pcmLeft = (char*)malloc(bufferBytes / 2);
        pcmRight = (char*)malloc(bufferBytes / 2);
        if (pcmLeft == nullptr || pcmRight == nullptr)
        {
            ALOGE("initWasmStream: stereo PCM allocation failed.");
            free(pcmBuffer);
            free(pcmLeft);
            free(pcmRight);
            decoder->close();
            AudioDecoderManager::destroyDecoder(decoder);
            return false;
        }
    }
#endif

    _wasmDecoder = decoder;
    _wasmPcmBuffer = pcmBuffer;
    _wasmPcmLeft = pcmLeft;
    _wasmPcmRight = pcmRight;
    _wasmFramesToRead = framesToRead;
    _wasmBufferBytes = bufferBytes;
    _wasmSampleRate = (ALsizei)decoder->getSampleRate();
    _wasmBytesPerFrame = (ALsizei)bytesPerFrame;
    _wasmFormat = _audioCache->_format;
    _wasmDuration = _audioCache->_duration;
    _wasmTotalFrames = decoder->getTotalFrames();

    _wasmLoopStart = decoder->getLoopStart();
    if (_wasmLoopStart == AudioDecoder::INVALID_FRAME_INDEX)
    {
        _wasmLoopEnd = 0;
    }
    else
    {
        uint64_t loopEnd64 = (uint64_t)_wasmLoopStart + decoder->getLoopLength();
        if (loopEnd64 > _wasmTotalFrames)
            _wasmLoopEnd = _wasmTotalFrames;
        else
            _wasmLoopEnd = (uint32_t)loopEnd64;
    }

    _wasmOggLooping = false;
    _wasmEof = false;
    _wasmCurrentFrame = 0;

    if (offsetFrame > 0)
    {
        if (decoder->seek((uint32_t)offsetFrame))
            _wasmCurrentFrame = (uint32_t)offsetFrame;
    }

    _wasmInitialized = true;
    

    return true;
}

void AudioPlayer::updateWasmStream()
{

    static unsigned int diagStreamTick = 0;


    if (!_wasmInitialized || _wasmDecoder == nullptr || _isDestroyed)
        return;

    ALint processed = 0;
    alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &processed);
    CHECK_AL_ERROR_DEBUG();

#ifdef USE_AGTK_STEREO_PAN
    ALint processed2 = 0;
    if (_wasmStereo)
    {
        alGetSourcei(_alSource2, AL_BUFFERS_PROCESSED, &processed2);
        CHECK_AL_ERROR_DEBUG();
        if (processed2 < processed)
            processed = processed2;
    }
#endif

    while (processed > 0)
    {
        --processed;

        ALuint bufferId = 0;
        ALuint bufferId2 = 0;

        alSourceUnqueueBuffers(_alSource, 1, &bufferId);
        CHECK_AL_ERROR_DEBUG();

#ifdef USE_AGTK_STEREO_PAN
        if (_wasmStereo)
        {
            alSourceUnqueueBuffers(_alSource2, 1, &bufferId2);
            CHECK_AL_ERROR_DEBUG();
        }
#endif

        if (_wasmEof)
            continue;

        if (_timeDirty)
        {
            _timeDirty = false;
            uint32_t offsetFrame = (uint32_t)(_currTime * _wasmSampleRate);
            if (_wasmDecoder->seek(offsetFrame))
            {
                _wasmCurrentFrame = offsetFrame;
                _wasmOggLooping = false;
            }
            else
            {
                _wasmEof = true;
                continue;
            }
        }
        else
        {
#ifdef USE_AGTK_OGG_LOOP_SUPPORT
#else
            _currTime += QUEUEBUFFER_TIME_STEP;
            if (_currTime > _wasmDuration)
            {
                if (_loop)
                    _currTime = 0.0f;
                else
                    _currTime = _wasmDuration;
            }
#endif
        }

        uint32_t framesRead = 0;

#ifdef USE_AGTK_OGG_LOOP_SUPPORT
        _wasmOggLooping = _wasmOggLooping ||
            (_wasmLoopStart != AudioDecoder::INVALID_FRAME_INDEX &&
             _wasmCurrentFrame >= _wasmLoopStart);

        if (_loop && _wasmOggLooping &&
            _wasmLoopStart != AudioDecoder::INVALID_FRAME_INDEX)
        {
            if (_wasmCurrentFrame >= _wasmLoopEnd)
            {
                if (_wasmDecoder->seek(_wasmLoopStart))
                    _wasmCurrentFrame = _wasmLoopStart;
                else
                {
                    _wasmEof = true;
                    continue;
                }
            }

            const uint64_t nextFrame = (uint64_t)_wasmCurrentFrame + _wasmFramesToRead;
            if (nextFrame > _wasmLoopEnd)
            {
                const uint32_t framesToRead1 = _wasmLoopEnd - _wasmCurrentFrame;
                const uint32_t framesToRead2 = _wasmFramesToRead - framesToRead1;

                const uint32_t framesRead1 = framesToRead1 == 0 ? 0 :
                    _wasmDecoder->readFixedFrames(framesToRead1, _wasmPcmBuffer);

                if (framesToRead1 > 0 && framesRead1 == 0)
                {
                    _wasmEof = true;
                    continue;
                }

                if (!_wasmDecoder->seek(_wasmLoopStart))
                {
                    _wasmEof = true;
                    continue;
                }
                _wasmCurrentFrame = _wasmLoopStart;

                const uint32_t framesRead2 = _wasmDecoder->readFixedFrames(
                    framesToRead2, _wasmPcmBuffer + framesRead1 * _wasmBytesPerFrame);

                if (framesRead2 == 0)
                {
                    _wasmEof = true;
                    continue;
                }

                framesRead = framesRead1 + framesRead2;
                _wasmCurrentFrame += framesRead2;
            }
            else
            {
                framesRead = _wasmDecoder->readFixedFrames(_wasmFramesToRead, _wasmPcmBuffer);
                if (framesRead > 0)
                    _wasmCurrentFrame += framesRead;
            }
        }
        else
        {
            framesRead = _wasmDecoder->readFixedFrames(_wasmFramesToRead, _wasmPcmBuffer);
            if (framesRead > 0)
                _wasmCurrentFrame += framesRead;
        }
#else
        framesRead = _wasmDecoder->readFixedFrames(_wasmFramesToRead, _wasmPcmBuffer);
        if (framesRead > 0)
            _wasmCurrentFrame += framesRead;
#endif

        if (!_wasmEof && framesRead == 0)
        {
            if (_loop)
            {
                if (_wasmDecoder->seek(0))
                {
                    _wasmCurrentFrame = 0;
                    _wasmOggLooping = false;
                    framesRead = _wasmDecoder->readFixedFrames(_wasmFramesToRead, _wasmPcmBuffer);
                    if (framesRead > 0)
                        _wasmCurrentFrame += framesRead;
                    else
                        _wasmEof = true;
                }
                else
                {
                    _wasmEof = true;
                }
            }
            else
            {
                _wasmEof = true;
            }
        }

        if (_wasmEof || framesRead == 0)
        {
            _wasmEof = true;
            continue;
        }

#ifdef USE_AGTK_OGG_LOOP_SUPPORT
        _currTime = (float)_wasmCurrentFrame / (float)_wasmSampleRate;
#endif

#ifdef USE_AGTK_STEREO_PAN
        if (_wasmStereo)
        {
            copyData(_wasmPcmLeft, _wasmPcmRight, _wasmPcmBuffer, framesRead * _wasmBytesPerFrame);
            alBufferData(bufferId, _wasmFormat, _wasmPcmLeft, framesRead * _wasmBytesPerFrame / 2, _wasmSampleRate);
            CHECK_AL_ERROR_DEBUG();
            alBufferData(bufferId2, _wasmFormat, _wasmPcmRight, framesRead * _wasmBytesPerFrame / 2, _wasmSampleRate);
            CHECK_AL_ERROR_DEBUG();
            alSourceQueueBuffers(_alSource, 1, &bufferId);
            CHECK_AL_ERROR_DEBUG();
            alSourceQueueBuffers(_alSource2, 1, &bufferId2);
            CHECK_AL_ERROR_DEBUG();
        }
        else
#endif
        {
            alBufferData(bufferId, _wasmFormat, _wasmPcmBuffer, framesRead * _wasmBytesPerFrame, _wasmSampleRate);
            CHECK_AL_ERROR_DEBUG();
            alSourceQueueBuffers(_alSource, 1, &bufferId);
            CHECK_AL_ERROR_DEBUG();
        }
    }

    ALint queued = 0;
    alGetSourcei(_alSource, AL_BUFFERS_QUEUED, &queued);
    CHECK_AL_ERROR_DEBUG();

#ifdef USE_AGTK_STEREO_PAN
    ALint queued2 = 0;
    if (_wasmStereo)
    {
        alGetSourcei(_alSource2, AL_BUFFERS_QUEUED, &queued2);
        CHECK_AL_ERROR_DEBUG();
    }
#endif

    if (_wasmEof)
    {
        // WASM-EOF-DRAIN-FIX: EOF stops decoding, not queued playback.
        bool allQueuesDrained = (queued == 0);
        
        if (queued > 0)
        {
            ALint drainState = AL_STOPPED;
            alGetSourcei(_alSource, AL_SOURCE_STATE, &drainState);
            CHECK_AL_ERROR_DEBUG();
            if (drainState != AL_PLAYING && drainState != AL_PAUSED)
            {
                alSourcePlay(_alSource);
                CHECK_AL_ERROR_DEBUG();
            }
        }
        
        #ifdef USE_AGTK_STEREO_PAN
        if (_wasmStereo)
        {
            if (queued2 > 0)
            {
                allQueuesDrained = false;
                ALint drainState2 = AL_STOPPED;
                alGetSourcei(_alSource2, AL_SOURCE_STATE, &drainState2);
                CHECK_AL_ERROR_DEBUG();
                if (drainState2 != AL_PLAYING && drainState2 != AL_PAUSED)
                {
                    alSourcePlay(_alSource2);
                    CHECK_AL_ERROR_DEBUG();
                }
            }
        }
        #endif
        
        if (allQueuesDrained)
            cleanupWasmStream();
        return;
    }

    ALint sourceState = AL_STOPPED;
    alGetSourcei(_alSource, AL_SOURCE_STATE, &sourceState);
    CHECK_AL_ERROR_DEBUG();

    if (queued > 0 && sourceState != AL_PLAYING)
    {
        alSourcePlay(_alSource);
        CHECK_AL_ERROR_DEBUG();
    }

#ifdef USE_AGTK_STEREO_PAN
    if (_wasmStereo && queued2 > 0)
    {
        ALint sourceState2 = AL_STOPPED;
        alGetSourcei(_alSource2, AL_SOURCE_STATE, &sourceState2);
        CHECK_AL_ERROR_DEBUG();
        if (sourceState2 != AL_PLAYING)
        {
            alSourcePlay(_alSource2);
            CHECK_AL_ERROR_DEBUG();
        }
    }
#endif
}

void AudioPlayer::cleanupWasmStream()
{
    if (_wasmDecoder != nullptr)
    {
        _wasmDecoder->close();
        AudioDecoderManager::destroyDecoder(_wasmDecoder);
        _wasmDecoder = nullptr;
    }

    if (_wasmPcmBuffer != nullptr)
    {
        free(_wasmPcmBuffer);
        _wasmPcmBuffer = nullptr;
    }

    if (_wasmPcmLeft != nullptr)
    {
        free(_wasmPcmLeft);
        _wasmPcmLeft = nullptr;
    }

    if (_wasmPcmRight != nullptr)
    {
        free(_wasmPcmRight);
        _wasmPcmRight = nullptr;
    }

    _wasmFramesToRead = 0;
    _wasmBufferBytes = 0;
    _wasmCurrentFrame = 0;
    _wasmLoopStart = 0;
    _wasmLoopEnd = 0;
    _wasmTotalFrames = 0;
    _wasmFormat = 0;
    _wasmSampleRate = 0;
    _wasmBytesPerFrame = 0;
    _wasmDuration = 0.0f;
    _wasmOggLooping = false;
    _wasmInitialized = false;
}

#endif // __EMSCRIPTEN__ && USE_AGTK

#if !defined(__EMSCRIPTEN__) || !defined(USE_AGTK)
void AudioPlayer::rotateBufferThread(int offsetFrame)
{
    char* tmpBuffer = nullptr;
#ifdef USE_AGTK_STEREO_PAN
	char* tmpBuffer1 = nullptr;
	char* tmpBuffer2 = nullptr;
#endif
	AudioDecoder* decoder = AudioDecoderManager::createDecoder(_audioCache->_fileFullPath.c_str());
    do
    {
        BREAK_IF(decoder == nullptr || !decoder->open(_audioCache->_fileFullPath.c_str()));

#ifdef USE_AGTK_OGG_LOOP_SUPPORT
		int currentFrame = decoder->getSampleRate() * QUEUEBUFFER_TIME_STEP * MARGIN_FOR_PITCH * QUEUEBUFFER_NUM;
		auto loopStart = decoder->getLoopStart();
		auto loopEnd = decoder->getLoopStart() + decoder->getLoopLength();
		auto totalFreame = decoder->getTotalFrames();
		if (loopEnd > totalFreame) {
			// ループ終了位置が範囲外に設定されているので訂正する
			loopEnd = totalFreame;
		}
		bool looping = false;
#endif
        uint32_t framesRead = 0;
        const uint32_t framesToRead = _audioCache->_queBufferFrames;
        const uint32_t bufferSize = framesToRead * decoder->getBytesPerFrame();
        tmpBuffer = (char*)malloc(bufferSize);
        memset(tmpBuffer, 0, bufferSize);
#ifdef USE_AGTK_STEREO_PAN
		tmpBuffer1 = (char*)malloc(bufferSize / 2);
		memset(tmpBuffer1, 0, bufferSize / 2);
		tmpBuffer2 = (char*)malloc(bufferSize / 2);
		memset(tmpBuffer2, 0, bufferSize / 2);
#endif

        if (offsetFrame != 0) {
            decoder->seek(offsetFrame);
#ifdef USE_AGTK_OGG_LOOP_SUPPORT
			currentFrame = offsetFrame;
#endif
        }

        ALint sourceState;
        ALint bufferProcessed = 0;
        bool needToExitThread = false;

        while (!_isDestroyed) {
            alGetSourcei(_alSource, AL_SOURCE_STATE, &sourceState);
            if (sourceState == AL_PLAYING) {
                alGetSourcei(_alSource, AL_BUFFERS_PROCESSED, &bufferProcessed);
                while (bufferProcessed > 0) {
                    bufferProcessed--;
                    if (_timeDirty) {
                        _timeDirty = false;
                        offsetFrame = _currTime * decoder->getSampleRate();
                        decoder->seek(offsetFrame);
#ifdef USE_AGTK_OGG_LOOP_SUPPORT
						currentFrame = offsetFrame;
#endif
                    }
                    else {
#ifdef USE_AGTK_OGG_LOOP_SUPPORT
#else
                        _currTime += QUEUEBUFFER_TIME_STEP;
                        if (_currTime > _audioCache->_duration) {
                            if (_loop) {
                                _currTime = 0.0f;
                            } else {
                                _currTime = _audioCache->_duration;
                            }
                        }
#endif
                    }

#ifdef USE_AGTK_OGG_LOOP_SUPPORT
					looping = looping || (loopStart != 0xfffffff && (uint32_t)currentFrame >= loopStart);
					if (_loop && looping && currentFrame + framesToRead > loopEnd) {
						auto framesToRead1 = loopEnd - currentFrame;
						auto framesToRead2 = framesToRead - framesToRead1;
						// ループ終了位置までの読み込み
						auto framesRead1 = framesToRead1 == 0 ? 0 : decoder->readFixedFrames(framesToRead1, tmpBuffer);
						if (framesToRead1 > 0 && framesRead1 == 0) {
							needToExitThread = true;
							break;
						}
						// ループ終了位置まで読み込んだので、残りのバッファをループ開始位置から読み込み
						decoder->seek(loopStart);
						currentFrame = loopStart;
						auto framesRead2 = decoder->readFixedFrames(framesToRead2, tmpBuffer + framesRead1 * decoder->getBytesPerFrame());
						if (framesRead2 == 0) {
							needToExitThread = true;
							break;
						}
						framesRead = framesRead1 + framesRead2;
						currentFrame += framesRead2;
					}
					else {
						framesRead = decoder->readFixedFrames(framesToRead, tmpBuffer);
						if (framesRead == 0) {
							if (_loop) {
								decoder->seek(0);
								currentFrame = 0;
								framesRead = decoder->readFixedFrames(framesToRead, tmpBuffer);
							}
							else {
								needToExitThread = true;
								break;
							}
						}
						currentFrame += framesRead;
					}
					_currTime = (float)currentFrame / decoder->getSampleRate();
#else
                    framesRead = decoder->readFixedFrames(framesToRead, tmpBuffer);

                    if (framesRead == 0) {
                        if (_loop) {
                            decoder->seek(0);
                            framesRead = decoder->readFixedFrames(framesToRead, tmpBuffer);
                        } else {
                            needToExitThread = true;
                            break;
                        }
                    }
#endif

                    ALuint bid;
#ifdef USE_AGTK_STEREO_PAN
					if (_audioCache->_isStereo) {
						copyData(tmpBuffer1, tmpBuffer2, tmpBuffer, framesRead * decoder->getBytesPerFrame());
						alSourceUnqueueBuffers(_alSource, 1, &bid);
						alBufferData(bid, _audioCache->_format, tmpBuffer1, framesRead * decoder->getBytesPerFrame() / 2, decoder->getSampleRate());
						alSourceQueueBuffers(_alSource, 1, &bid);
						alSourceUnqueueBuffers(_alSource2, 1, &bid);
						alBufferData(bid, _audioCache->_format, tmpBuffer2, framesRead * decoder->getBytesPerFrame() / 2, decoder->getSampleRate());
						alSourceQueueBuffers(_alSource2, 1, &bid);
					}
					else {
						alSourceUnqueueBuffers(_alSource, 1, &bid);
						alBufferData(bid, _audioCache->_format, tmpBuffer, framesRead * decoder->getBytesPerFrame(), decoder->getSampleRate());
						alSourceQueueBuffers(_alSource, 1, &bid);
					}
#else
                    alSourceUnqueueBuffers(_alSource, 1, &bid);
                    alBufferData(bid, _audioCache->_format, tmpBuffer, framesRead * decoder->getBytesPerFrame(), decoder->getSampleRate());
                    alSourceQueueBuffers(_alSource, 1, &bid);
#endif
                }
            }

            std::unique_lock<std::mutex> lk(_sleepMutex);
            if (_isDestroyed || needToExitThread) {
                break;
            }

            _sleepCondition.wait_for(lk,std::chrono::milliseconds(75));
        }

    } while(false);

    ALOGV("Exit rotate buffer thread ...");
    if (decoder != nullptr)
    {
        decoder->close();
    }
    AudioDecoderManager::destroyDecoder(decoder);
    free(tmpBuffer);
#ifdef USE_AGTK_STEREO_PAN
	free(tmpBuffer1);
	free(tmpBuffer2);
#endif
    _isRotateThreadExited = true;
    ALOGV("%s exited.\n", __FUNCTION__);
}
#endif // !__EMSCRIPTEN__ || !USE_AGTK

bool AudioPlayer::setLoop(bool loop)
{
    if (!_isDestroyed ) {
        _loop = loop;
        return true;
    }

    return false;
}

bool AudioPlayer::setTime(float time)
{
    if (_isDestroyed || time < 0.0f)
        return false;

    float duration = 0.0f;

#if defined(__EMSCRIPTEN__) && defined(USE_AGTK)
    if (_wasmInitialized)
        duration = _wasmDuration;
    else if (_audioCache != nullptr)
        duration = _audioCache->_duration;
    else
        return false;
#else
    if (_audioCache == nullptr)
        return false;
    duration = _audioCache->_duration;
#endif

    if (time >= duration)
        return false;

    _currTime = time;
    _timeDirty = true;
    return true;
}

#endif
