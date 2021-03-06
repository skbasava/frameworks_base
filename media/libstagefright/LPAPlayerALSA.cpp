/*
 * Copyright (C) 2009 The Android Open Source Project
 * Copyright (c) 2009-2012, Code Aurora Forum. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_NDDEBUG 0
#define LOG_NDEBUG 0
#define LOG_TAG "LPAPlayerALSA"

#include <utils/Log.h>
#include <utils/threads.h>

#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/poll.h>
#include <sys/eventfd.h>
#include <binder/IPCThreadState.h>
#include <media/AudioTrack.h>

extern "C" {
   #include <asound.h>
   #include "alsa_audio.h"
}

#include <media/stagefright/LPAPlayer.h>
#include <media/stagefright/MediaDebug.h>
#include <media/stagefright/MediaDefs.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaSource.h>
#include <media/stagefright/MetaData.h>
#include <media/stagefright/MediaErrors.h>

#include <hardware_legacy/power.h>

#include <linux/unistd.h>

#include "include/AwesomePlayer.h"
#include <powermanager/PowerManager.h>

static const char   mName[] = "LPAPlayer";

#define MEM_BUFFER_SIZE 262144
//#define PMEM_BUFFER_SIZE (4800 * 4)
#define MEM_BUFFER_COUNT 4

//Values to exit poll via eventfd
#define KILL_EVENT_THREAD 1
#define SIGNAL_EVENT_THREAD 2
#define PCM_FORMAT 2
#define NUM_FDS 2
#define LPA_SESSION_ID 1
namespace android {
int LPAPlayer::objectsAlive = 0;

LPAPlayer::LPAPlayer(
                    const sp<MediaPlayerBase::AudioSink> &audioSink, bool &initCheck,
                    AwesomePlayer *observer)
:AudioPlayer(audioSink,observer),
mInputBuffer(NULL),
mSampleRate(0),
mLatencyUs(0),
mFrameSize(0),
mNumFramesPlayed(0),
mPositionTimeMediaUs(-1),
mPositionTimeRealUs(-1),
mSeeking(false),
mInternalSeeking(false),
mReachedEOS(false),
mFinalStatus(OK),
mSeekTimeUs(0),
mPauseTime(0),
mNumA2DPBytesPlayed(0),
mStarted(false),
mIsFirstBuffer(false),
mFirstBufferResult(OK),
mFirstBuffer(NULL),
mAudioSink(audioSink),
mObserver(observer) {
    LOGV("LPAPlayer::LPAPlayer() ctor");
    a2dpDisconnectPause = false;
    mSeeked = false;
    objectsAlive++;
    timeStarted = 0;
    numChannels =0;
    afd = -1;
    timePlayed = 0;
    isPaused = false;
    bIsA2DPEnabled = false;
    mAudioFlinger = NULL;
    AudioFlingerClient = NULL;
    efd = -1;
    /* Initialize Suspend/Resume related variables */
    mQueue.start();
    mQueueStarted      = true;
    mPauseEvent        = new TimedEvent(this, &LPAPlayer::onPauseTimeOut);
    mPauseEventPending = false;
    mPlaybackSuspended = false;
    getAudioFlinger();
    LOGV("Registering client with AudioFlinger");
    mAudioFlinger->registerClient(AudioFlingerClient);
    mAudioSinkOpen = false;
    mIsAudioRouted = false;
    a2dpThreadStarted = true;
    asyncReset = false;

    sessionId = LPA_SESSION_ID;
    bEffectConfigChanged = false;
    initCheck = true;

    mDeathRecipient = new PMDeathRecipient(this);
}

void LPAPlayer::acquireWakeLock()
{
    Mutex::Autolock _l(pmLock);

    if (mPowerManager == 0) {
        // use checkService() to avoid blocking if power service is not up yet
        sp<IBinder> binder =
            defaultServiceManager()->checkService(String16("power"));
        if (binder == 0) {
            LOGW("Thread %s cannot connect to the power manager service", mName);
        } else {
            mPowerManager = interface_cast<IPowerManager>(binder);
            binder->linkToDeath(mDeathRecipient);
        }
    }
    if (mPowerManager != 0 && mWakeLockToken == 0) {
        sp<IBinder> binder = new BBinder();
        status_t status = mPowerManager->acquireWakeLock(POWERMANAGER_PARTIAL_WAKE_LOCK,
                                                         binder,
                                                         String16(mName));
        if (status == NO_ERROR) {
            mWakeLockToken = binder;
        }
        LOGV("acquireWakeLock() %s status %d", mName, status);
    }
}

void LPAPlayer::releaseWakeLock()
{
    Mutex::Autolock _l(pmLock);

    if (mWakeLockToken != 0) {
        LOGV("releaseWakeLock() %s", mName);
        if (mPowerManager != 0) {
            mPowerManager->releaseWakeLock(mWakeLockToken, 0);
        }
        mWakeLockToken.clear();
    }
}

void LPAPlayer::clearPowerManager()
{
    Mutex::Autolock _l(pmLock);
    releaseWakeLock();
    mPowerManager.clear();
}

void LPAPlayer::PMDeathRecipient::binderDied(const wp<IBinder>& who)
{
    parentClass->clearPowerManager();
    LOGW("power manager service died !!!");
}

LPAPlayer::~LPAPlayer() {
    LOGV("LPAPlayer::~LPAPlayer()");
    if (mQueueStarted) {
        mQueue.stop();
    }

    reset();

    mAudioFlinger->deregisterClient(AudioFlingerClient);
    objectsAlive--;

    releaseWakeLock();
    if (mPowerManager != 0) {
        sp<IBinder> binder = mPowerManager->asBinder();
        binder->unlinkToDeath(mDeathRecipient);
    }
}

void LPAPlayer::getAudioFlinger() {
    Mutex::Autolock _l(AudioFlingerLock);

    if ( mAudioFlinger.get() == 0 ) {
        sp<IServiceManager> sm = defaultServiceManager();
        sp<IBinder> binder;
        do {
            binder = sm->getService(String16("media.audio_flinger"));
            if ( binder != 0 )
                break;
            LOGW("AudioFlinger not published, waiting...");
            usleep(500000); // 0.5 s
        } while ( true );
        if ( AudioFlingerClient == NULL ) {
            AudioFlingerClient = new AudioFlingerLPAdecodeClient(this);
        }

        binder->linkToDeath(AudioFlingerClient);
        mAudioFlinger = interface_cast<IAudioFlinger>(binder);
    }
    LOGE_IF(mAudioFlinger==0, "no AudioFlinger!?");
}

LPAPlayer::AudioFlingerLPAdecodeClient::AudioFlingerLPAdecodeClient(void *obj)
{
    LOGV("LPAPlayer::AudioFlingerLPAdecodeClient::AudioFlingerLPAdecodeClient");
    pBaseClass = (LPAPlayer*)obj;
}

void LPAPlayer::AudioFlingerLPAdecodeClient::binderDied(const wp<IBinder>& who) {
    Mutex::Autolock _l(pBaseClass->AudioFlingerLock);

    pBaseClass->mAudioFlinger.clear();
    LOGW("AudioFlinger server died!");
}

void LPAPlayer::AudioFlingerLPAdecodeClient::ioConfigChanged(int event, int ioHandle, void *param2) {
    LOGV("ioConfigChanged() event %d", event);

    if ( event != AudioSystem::A2DP_OUTPUT_STATE &&
         event != AudioSystem::EFFECT_CONFIG_CHANGED) {
        return;
    }

    switch ( event ) {
    case AudioSystem::A2DP_OUTPUT_STATE:
        {
            LOGV("ioConfigChanged() A2DP_OUTPUT_STATE iohandle is %d with A2DPEnabled in %d", ioHandle, pBaseClass->bIsA2DPEnabled);
            if ( -1 == ioHandle ) {
                if ( pBaseClass->bIsA2DPEnabled ) {
                    pBaseClass->bIsA2DPEnabled = false;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }
                    LOGV("ioConfigChanged:: A2DP Disabled");
                }
            } else {
                if ( !pBaseClass->bIsA2DPEnabled ) {

                    pBaseClass->bIsA2DPEnabled = true;
                    if (pBaseClass->mStarted) {
                        pBaseClass->handleA2DPSwitch();
                    }

                    LOGV("ioConfigChanged:: A2DP Enabled");
                }
            }
        }
        break;
    case AudioSystem::EFFECT_CONFIG_CHANGED:
        {
            LOGV("Received notification for change in effect module");
            // Seek to current media time - flush the decoded buffers with the driver
            if(!pBaseClass->bIsA2DPEnabled) {
                pthread_mutex_lock(&pBaseClass->effect_mutex);
                pBaseClass->bEffectConfigChanged = true;
                pthread_mutex_unlock(&pBaseClass->effect_mutex);
                // Signal effects thread to re-apply effects
                LOGV("Signalling Effects Thread");
                pthread_cond_signal(&pBaseClass->effect_cv);
            }
        }
    }

    LOGV("ioConfigChanged Out");
}

void LPAPlayer::handleA2DPSwitch() {
    Mutex::Autolock autoLock(mLock);

    LOGV("handleA2dpSwitch()");
    if (bIsA2DPEnabled) {
        struct pcm * local_handle = (struct pcm *)handle;
        if (!isPaused) {
            if(mIsAudioRouted) {
                if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
                    LOGE("AUDIO PAUSE failed");
                }
            }

            LOGV("paused for bt switch");
            mSeekTimeUs += getTimeStamp(A2DP_CONNECT);
        }
        else {
            mSeekTimeUs = mPauseTime;
        }

        mInternalSeeking = true;
        mNumA2DPBytesPlayed = 0;
        mReachedEOS = false;
        pthread_cond_signal(&a2dp_notification_cv);
    } else {
        if (isPaused)
            pthread_cond_signal(&a2dp_notification_cv);
        else
            a2dpDisconnectPause = true;
    }
}

void LPAPlayer::setSource(const sp<MediaSource> &source) {
    CHECK_EQ(mSource, NULL);
    LOGV("Setting source from LPA Player");
    mSource = source;
}

status_t LPAPlayer::start(bool sourceAlreadyStarted) {
    CHECK(!mStarted);
    CHECK(mSource != NULL);

    LOGV("start: sourceAlreadyStarted %d", sourceAlreadyStarted);
    //Check if the source is started, start it
    status_t err;
    if (!sourceAlreadyStarted) {
        err = mSource->start();

        if (err != OK) {
            return err;
        }
    }

    //Create event, decoder and a2dp thread and initialize all the
    //mutexes and coditional variables
    createThreads();
    LOGV("All Threads Created.");

    // We allow an optional INFO_FORMAT_CHANGED at the very beginning
    // of playback, if there is one, getFormat below will retrieve the
    // updated format, if there isn't, we'll stash away the valid buffer
    // of data to be used on the first audio callback.

    CHECK(mFirstBuffer == NULL);

    MediaSource::ReadOptions options;
    if (mSeeking) {
        options.setSeekTo(mSeekTimeUs);
        mSeeking = false;
    }

    mFirstBufferResult = mSource->read(&mFirstBuffer, &options);
    if (mFirstBufferResult == INFO_FORMAT_CHANGED) {
        LOGV("INFO_FORMAT_CHANGED!!!");
        CHECK(mFirstBuffer == NULL);
        mFirstBufferResult = OK;
        mIsFirstBuffer = false;
    } else {
        mIsFirstBuffer = true;
    }

    /*TODO: Check for bA2dpEnabled */

    sp<MetaData> format = mSource->getFormat();
    const char *mime;
    bool success = format->findCString(kKeyMIMEType, &mime);
    CHECK(success);
    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));

    success = format->findInt32(kKeySampleRate, &mSampleRate);
    CHECK(success);

    success = format->findInt32(kKeyChannelCount, &numChannels);
    CHECK(success);


    if (!bIsA2DPEnabled) {
        LOGV("Opening a routing session for audio playback: sessionId = %d mSampleRate %d numChannels %d",
             sessionId, mSampleRate, numChannels);
        status_t err = mAudioSink->openSession(AUDIO_FORMAT_PCM_16_BIT, sessionId, mSampleRate, numChannels);
        if (err != OK) {
            if (mFirstBuffer != NULL) {
                mFirstBuffer->release();
                mFirstBuffer = NULL;
            }

            if (!sourceAlreadyStarted) {
                mSource->stop();
            }

            LOGE("Opening a routing session failed");
            return err;
        }
        acquireWakeLock();
        mIsAudioRouted = true;
    }
    else {
        LOGV("Before Audio Sink Open");
        status_t ret = mAudioSink->open(mSampleRate, numChannels,AUDIO_FORMAT_PCM_16_BIT, DEFAULT_AUDIOSINK_BUFFERCOUNT);
        mAudioSink->start();
        LOGV("After Audio Sink Open");
        mAudioSinkOpen = true;
    }

    LOGV("pcm_open hardware 0,4 for LPA ");
    //Open PCM driver
    if (numChannels == 1)
        handle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_MONO) , (char *) "hw:0,4");
    else
        handle = (void *)pcm_open((PCM_MMAP | DEBUG_ON | PCM_STEREO) , (char *) "hw:0,4");

    struct pcm * local_handle = (struct pcm *)handle;
    if (!local_handle) {
        LOGE("Failed to initialize ALSA hardware hw:0,4");
        return BAD_VALUE;
    }

    struct snd_pcm_hw_params *params;
    struct snd_pcm_sw_params *sparams;
    params = (struct snd_pcm_hw_params*) calloc(1, sizeof(struct snd_pcm_hw_params));
    if (!params) {
      LOGV( "Aplay:Failed to allocate ALSA hardware parameters!");
      return -1;
    }
    param_init(params);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_MMAP_INTERLEAVED);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    param_set_mask(params, SNDRV_PCM_HW_PARAM_SUBFORMAT,SNDRV_PCM_SUBFORMAT_STD);
    param_set_min(params, SNDRV_PCM_HW_PARAM_PERIOD_BYTES, MEM_BUFFER_SIZE);
    param_set_int(params, SNDRV_PCM_HW_PARAM_SAMPLE_BITS, 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_FRAME_BITS,
                numChannels - 1 ? 32 : 16);
    param_set_int(params, SNDRV_PCM_HW_PARAM_CHANNELS, numChannels);
    param_set_int(params, SNDRV_PCM_HW_PARAM_RATE, mSampleRate);
    param_set_hw_refine(local_handle, params);
    if (param_set_hw_params(local_handle, params)) {
     LOGV( "Aplay:cannot set hw params");
     return -22;
    }
    param_dump(params);
    local_handle->buffer_size = pcm_buffer_size(params);
    local_handle->period_size = pcm_period_size(params);
    local_handle->period_cnt = local_handle->buffer_size/local_handle->period_size;
    LOGV("period_cnt = %d\n", local_handle->period_cnt);
    LOGV("period_size = %d\n", local_handle->period_size);
    LOGV("buffer_size = %d\n", local_handle->buffer_size);

    sparams = (struct snd_pcm_sw_params*) calloc(1, sizeof(struct snd_pcm_sw_params));
    if (!sparams) {
     LOGV( "Aplay:Failed to allocate ALSA software parameters!\n");
     return -1;
    }
    // Get the current software parameters
    sparams->tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
    sparams->period_step = 1;
    sparams->avail_min = local_handle->period_size/2;
    sparams->start_threshold = local_handle->period_size/2;
    sparams->stop_threshold =  local_handle->buffer_size;
    sparams->xfer_align = (local_handle->flags & PCM_MONO) ? local_handle->period_size/2 : local_handle->period_size/4; /* needed for old kernels */
    sparams->silence_size = 0;
    sparams->silence_threshold = 0;
    if (param_set_sw_params(local_handle, sparams)) {
     LOGV( "Aplay:cannot set sw params");
     return -22;
    }
    mmap_buffer(local_handle);
    if (!bIsA2DPEnabled)
       pcm_prepare(local_handle);
    handle = (void *)local_handle;
    //Map PMEM buffer
    LOGV("LPA Driver Started");
    mStarted = true;

    LOGV("Waking up decoder thread");
    pthread_cond_signal(&decoder_cv);
    return OK;
}

status_t LPAPlayer::seekTo(int64_t time_us) {
    Mutex::Autolock autoLock1(mSeekLock);
    Mutex::Autolock autoLock(mLock);
    LOGV("seekTo: time_us %lld", time_us);
    if ( mReachedEOS ) {
        mReachedEOS = false;
    }
    mSeeking = true;

    mSeekTimeUs = time_us;
    struct pcm * local_handle = (struct pcm *)handle;
    LOGV("In seekTo(), mSeekTimeUs %lld",mSeekTimeUs);
    if (!bIsA2DPEnabled) {
        if (mStarted) {
            LOGV("Paused case, %d",isPaused);

            pthread_mutex_lock(&mem_response_mutex);
            pthread_mutex_lock(&mem_request_mutex);
            memBuffersResponseQueue.clear();
            memBuffersRequestQueue.clear();

            List<BuffersAllocated>::iterator it = bufPool.begin();
            for(;it!=bufPool.end();++it) {
                 memBuffersRequestQueue.push_back(*it);
            }

            pthread_mutex_unlock(&mem_request_mutex);
            pthread_mutex_unlock(&mem_response_mutex);
            LOGV("Transferred all the buffers from response queue to rquest queue to handle seek");
            if (!isPaused) {
                if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
                    LOGE("Audio Pause failed");
                }
                local_handle->start = 0;
                pcm_prepare(local_handle);
                LOGV("Reset, drain and prepare completed");
                local_handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
                sync_ptr(local_handle);
                LOGV("appl_ptr= %ld", local_handle->sync_ptr->c.control.appl_ptr);
                pthread_cond_signal(&decoder_cv);
            }
        }
    } else {
        if (!memBuffersResponseQueue.empty())
            mSeeked = true;

        if (!isPaused) {
            mAudioSink->pause();
            mAudioSink->flush();
            mAudioSink->start();
        }
        mNumA2DPBytesPlayed = 0;
    }

    return OK;
}

void LPAPlayer::pause(bool playPendingSamples) {
    CHECK(mStarted);

    LOGV("pause: playPendingSamples %d", playPendingSamples);
    isPaused = true;
    A2DPState state;
    if (playPendingSamples) {
        isPaused = true;
        if (!bIsA2DPEnabled) {
            struct pcm * local_handle = (struct pcm *)handle;
            if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
                LOGE("Audio Pause failed");
            }
            if (!mPauseEventPending) {
                LOGV("Posting an event for Pause timeout");
                mQueue.postEventWithDelay(mPauseEvent, LPA_PAUSE_TIMEOUT_USEC);
                mPauseEventPending = true;
            }
            if (mAudioSink.get() != NULL)
                mAudioSink->pauseSession();
            state = A2DP_DISABLED;
        }
        else {
            if (mAudioSink.get() != NULL)
                mAudioSink->stop();
            state = A2DP_ENABLED;
        }
        mPauseTime = mSeekTimeUs + getTimeStamp(state);
    } else {
        if (a2dpDisconnectPause) {
            a2dpDisconnectPause = false;
            mAudioSink->pause();
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_DISCONNECT);
            pthread_cond_signal(&a2dp_notification_cv);
        } else {
            if (!bIsA2DPEnabled) {
                LOGV("LPAPlayer::Pause - Pause driver");
                struct pcm * local_handle = (struct pcm *)handle;
                pthread_mutex_lock(&pause_mutex);
                if (local_handle->start != 1) {
                    pthread_cond_wait(&pause_cv, &pause_mutex);
                }
                pthread_mutex_unlock(&pause_mutex);
                if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
                    LOGE("Audio Pause failed");
                }

                if(!mPauseEventPending) {
                    LOGV("Posting an event for Pause timeout");
                    mQueue.postEventWithDelay(mPauseEvent, LPA_PAUSE_TIMEOUT_USEC);
                    mPauseEventPending = true;
                }

                if (mAudioSink.get() != NULL) {
                    mAudioSink->pauseSession();
                }
                state = A2DP_DISABLED;
            } else {
                mAudioSink->pause();
                mAudioSink->flush();
                state = A2DP_ENABLED;
            }
            mPauseTime = mSeekTimeUs + getTimeStamp(state);
        }
    }
}

void LPAPlayer::resume() {
    LOGV("resume: isPaused %d",isPaused);
    Mutex::Autolock autoLock(resumeLock);
    if ( isPaused) {
        CHECK(mStarted);
        if (!bIsA2DPEnabled) {
            LOGE("LPAPlayer::resume - Resuming Driver");
            if(mPauseEventPending) {
                LOGV("Resume(): Cancelling the puaseTimeout event");
                mPauseEventPending = false;
                mQueue.cancelEvent(mPauseEvent->eventID());
            }
            if (mAudioSinkOpen) {
                mAudioSink->close();
                mAudioSinkOpen = false;
                LOGV("Singal to A2DP thread for clean up after closing Audio sink");
                pthread_cond_signal(&a2dp_cv);
            }

            if (!mIsAudioRouted) {
                LOGV("Opening a session for LPA playback");
                status_t err = mAudioSink->openSession(AUDIO_FORMAT_PCM_16_BIT, sessionId);
                acquireWakeLock();
                mIsAudioRouted = true;
            }

            LOGV("Attempting Sync resume\n");
            struct pcm * local_handle = (struct pcm *)handle;
            if (!(mSeeking || mInternalSeeking)) {
                if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,0) < 0)
                    LOGE("AUDIO Resume failed");
                LOGV("Sync resume done\n");
            }
            else {
                local_handle->start = 0;
                pcm_prepare(local_handle);
                LOGV("Reset, drain and prepare completed");
                local_handle->sync_ptr->flags = SNDRV_PCM_SYNC_PTR_APPL | SNDRV_PCM_SYNC_PTR_AVAIL_MIN;
                sync_ptr(local_handle);
                LOGV("appl_ptr= %ld", local_handle->sync_ptr->c.control.appl_ptr);
            }
            if (mAudioSink.get() != NULL) {
                mAudioSink->resumeSession();
            }
        } else {
            isPaused = false;

            if (!mAudioSinkOpen) {
                if (mAudioSink.get() != NULL) {
                    LOGV("%s mAudioSink close session", __func__);
                    mAudioSink->closeSession();
                    releaseWakeLock();
                    mIsAudioRouted = false;
                } else {
                    LOGE("close session NULL");
                }

                LOGV("Resume: Before Audio Sink Open");
                status_t ret = mAudioSink->open(mSampleRate, numChannels,AUDIO_FORMAT_PCM_16_BIT,
                                                DEFAULT_AUDIOSINK_BUFFERCOUNT);
                mAudioSink->start();
                LOGV("Resume: After Audio Sink Open");
                mAudioSinkOpen = true;

                LOGV("Resume: Waking up the decoder thread");
                pthread_cond_signal(&decoder_cv);
            } else {
                /* If AudioSink is already open just start it */
                mAudioSink->start();
            }
            LOGV("Waking up A2dp thread");
            pthread_cond_signal(&a2dp_cv);
        }
        isPaused = false;
        pthread_cond_signal(&decoder_cv);

        /*
        Signal to effects thread so that it can apply the new effects
        enabled during pause state
        */
        pthread_cond_signal(&effect_cv);
    }
}

void LPAPlayer::reset() {
    LOGV("Reset called!!!!!");
    asyncReset = true;

    struct pcm * local_handle = (struct pcm *)handle;

    LOGV("reset() requestQueue.size() = %d, responseQueue.size() = %d effectsQueue.size() = %d",
         memBuffersRequestQueue.size(), memBuffersResponseQueue.size(), effectsQueue.size());

    // make sure the Effects thread has exited
    requestAndWaitForEffectsThreadExit();

    // make sure Decoder thread has exited
    requestAndWaitForDecoderThreadExit();

    // make sure the event thread also has exited
    requestAndWaitForEventThreadExit();

    requestAndWaitForA2DPThreadExit();

    requestAndWaitForA2DPNotificationThreadExit();



    // Make sure to release any buffer we hold onto so that the
    // source is able to stop().
    if (mFirstBuffer != NULL) {
        mFirstBuffer->release();
        mFirstBuffer = NULL;
    }

    if (mInputBuffer != NULL) {
        LOGV("AudioPlayer releasing input buffer.");
        mInputBuffer->release();
        mInputBuffer = NULL;
    }

    mSource->stop();

    // The following hack is necessary to ensure that the OMX
    // component is completely released by the time we may try
    // to instantiate it again.
    wp<MediaSource> tmp = mSource;
    mSource.clear();
    while (tmp.promote() != NULL) {
        usleep(1000);
    }

    memBufferDeAlloc();
    LOGE("Buffer Deallocation complete! Closing pcm handle");

    if (local_handle->start) {
        if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_PAUSE,1) < 0) {
            LOGE("Audio Pause failed");
        }
    }
    local_handle->start = 0;
    if (mIsAudioRouted)
       pcm_prepare(local_handle);
    pcm_close(local_handle);
    handle = (void*)local_handle;

        // Close the audiosink after all the threads exited to make sure
    // there is no thread writing data to audio sink or applying effect
    if (bIsA2DPEnabled) {
        mAudioSink->close();
    } else {
        mAudioSink->closeSession();
        releaseWakeLock();
    }
    mAudioSink.clear();

    LOGV("reset() after memBuffersRequestQueue.size() = %d, memBuffersResponseQueue.size() = %d ",memBuffersRequestQueue.size(),memBuffersResponseQueue.size());

    mNumFramesPlayed = 0;
    mPositionTimeMediaUs = -1;
    mPositionTimeRealUs = -1;
    mSeeking = false;
    mInternalSeeking = false;
    mReachedEOS = false;
    mFinalStatus = OK;
    mStarted = false;
}


bool LPAPlayer::isSeeking() {
    Mutex::Autolock autoLock(mLock);
    return mSeeking;
}

bool LPAPlayer::reachedEOS(status_t *finalStatus) {
    *finalStatus = OK;

    Mutex::Autolock autoLock(mLock);
    *finalStatus = mFinalStatus;
    return mReachedEOS;
}


void *LPAPlayer::decoderThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->decoderThreadEntry();
    return NULL;
}


void LPAPlayer::decoderThreadEntry() {

    pthread_mutex_lock(&decoder_mutex);

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"LPA DecodeThread", 0, 0, 0);

    LOGV("decoderThreadEntry wait for signal \n");
    if (!mStarted) {
        pthread_cond_wait(&decoder_cv, &decoder_mutex);
    }
    LOGV("decoderThreadEntry ready to work \n");
    pthread_mutex_unlock(&decoder_mutex);
    if (killDecoderThread) {
        pthread_mutex_unlock(&mem_request_mutex);
        return;
    }
    pthread_cond_signal(&event_cv);

    int32_t mem_fd;

    //TODO check PMEM_BUFFER_SIZE from handle.
    memBufferAlloc(MEM_BUFFER_SIZE, &mem_fd);
    while (1) {
        pthread_mutex_lock(&mem_request_mutex);

        if (killDecoderThread) {
            pthread_mutex_unlock(&mem_request_mutex);
            break;
        }

        LOGV("decoder memBuffersRequestQueue.size() = %d, memBuffersResponseQueue.size() = %d ",
             memBuffersRequestQueue.size(),memBuffersResponseQueue.size());

        if (memBuffersRequestQueue.empty() || mReachedEOS || isPaused ||
            (bIsA2DPEnabled && !mAudioSinkOpen) || asyncReset ) {
            LOGV("decoderThreadEntry: a2dpDisconnectPause %d  mReachedEOS %d bIsA2DPEnabled %d "
                 "mAudioSinkOpen %d asyncReset %d ", a2dpDisconnectPause,
                 mReachedEOS, bIsA2DPEnabled, mAudioSinkOpen, asyncReset);
            LOGV("decoderThreadEntry: waiting on decoder_cv");
            pthread_cond_wait(&decoder_cv, &mem_request_mutex);
            pthread_mutex_unlock(&mem_request_mutex);
            LOGV("decoderThreadEntry: received a signal to wake up");
            continue;
        }

        pthread_mutex_unlock(&mem_request_mutex);

        //Queue the buffers back to Request queue
        if (mReachedEOS || (bIsA2DPEnabled && !mAudioSinkOpen) || asyncReset || a2dpDisconnectPause) {
            LOGV("%s: mReachedEOS %d bIsA2DPEnabled %d ", __func__, mReachedEOS, bIsA2DPEnabled);
        }
        //Queue up the buffers for writing either for A2DP or LPA Driver
        else {
            struct msm_audio_aio_buf aio_buf_local;
            Mutex::Autolock autoLock(mSeekLock);

            pthread_mutex_lock(&mem_request_mutex);
            List<BuffersAllocated>::iterator it = memBuffersRequestQueue.begin();
            BuffersAllocated buf = *it;
            memBuffersRequestQueue.erase(it);
            pthread_mutex_unlock(&mem_request_mutex);
            memset(buf.localBuf, 0x0, MEM_BUFFER_SIZE);
            memset(buf.memBuf, 0x0, MEM_BUFFER_SIZE);

            LOGV("Calling fillBuffer for size %d",MEM_BUFFER_SIZE);
            buf.bytesToWrite = fillBuffer(buf.localBuf, MEM_BUFFER_SIZE);
            LOGV("fillBuffer returned size %d",buf.bytesToWrite);

            if ( buf.bytesToWrite ==  0) {
                /* Put the buffer back into requestQ */
                /* This is zero byte buffer - no need to put in response Q*/
                pthread_mutex_lock(&mem_request_mutex);
                memBuffersRequestQueue.push_front(buf);
                pthread_mutex_unlock(&mem_request_mutex);
                /*Post EOS to Awesome player when i/p EOS is reached,
                  all input buffers have been decoded and response queue is empty*/
                if(mObserver && mReachedEOS && memBuffersResponseQueue.empty()) {
                    LOGV("Posting EOS event..zero byte buffer and response queue is empty");
                    mObserver->postAudioEOS();
                }
                continue;
            }
            pthread_mutex_lock(&mem_response_mutex);
            memBuffersResponseQueue.push_back(buf);
            pthread_mutex_unlock(&mem_response_mutex);

            if (!bIsA2DPEnabled){
                LOGV("Start Event thread\n");
                pthread_cond_signal(&event_cv);
                // Make sure the buffer is added to response Q before applying effects
                // If there is a change in effects while applying on current buffer
                // it will be re applied as the buffer already present in responseQ
                if (!asyncReset) {
                    pthread_mutex_lock(&apply_effect_mutex);
                    LOGV("decoderThread: applying effects on mem buf at buf.memBuf %p", buf.memBuf);
                    mAudioFlinger->applyEffectsOn((int16_t*)buf.localBuf,
                                                  (int16_t*)buf.memBuf,
                                                  (int)buf.bytesToWrite);
                    pthread_mutex_unlock(&apply_effect_mutex);
                    LOGV("decoderThread: Writing buffer to driver with mem fd %d", buf.memFd);

                    {
                        if (mSeeking) {
                            continue;
                        }
                        LOGV("PCM write start");
                        struct pcm * local_handle = (struct pcm *)handle;
                        pcm_write(local_handle, buf.memBuf, local_handle->period_size);
                        if (mReachedEOS) {
                            if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_START) < 0)
                                LOGE("AUDIO Start failed");
                            else
                                local_handle->start = 1;
                        }
                        if (buf.bytesToWrite < MEM_BUFFER_SIZE && memBuffersResponseQueue.size() == 1) {
                            LOGV("Last buffer case");
                            uint64_t writeValue = SIGNAL_EVENT_THREAD;
                            write(efd, &writeValue, sizeof(uint64_t));
                        }
                        LOGV("PCM write complete");
                        pthread_mutex_lock(&pause_mutex);
                        pthread_cond_signal(&pause_cv);
                        pthread_mutex_unlock(&pause_mutex);
                    }
                }
            }
            else
                pthread_cond_signal(&a2dp_cv);
        }
    }
    decoderThreadAlive = false;
    LOGV("decoder Thread is dying");
}

void *LPAPlayer::eventThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->eventThreadEntry();
    return NULL;
}

void LPAPlayer::eventThreadEntry() {
    struct msm_audio_event cur_pcmdec_event;

    pthread_mutex_lock(&event_mutex);
    int rc = 0;
    int err_poll = 0;
    int avail = 0;
    int i = 0;

    pid_t tid  = gettid();
    androidSetThreadPriority(tid, ANDROID_PRIORITY_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"LPA EventThread", 0, 0, 0);


    LOGV("eventThreadEntry wait for signal \n");
    pthread_cond_wait(&event_cv, &event_mutex);
    LOGV("eventThreadEntry ready to work \n");
    pthread_mutex_unlock(&event_mutex);

    if (killEventThread) {
        eventThreadAlive = false;
        LOGV("Event Thread is dying.");
        return;
    }

    LOGV("Allocating poll fd");
    struct pollfd pfd[NUM_FDS];

    struct pcm * local_handle = (struct pcm *)handle;
    pfd[0].fd = local_handle->timer_fd;
    pfd[0].events = (POLLIN | POLLERR | POLLNVAL);
    LOGV("Allocated poll fd");
    bool audioEOSPosted = false;
    int timeout = -1;

    efd = eventfd(0,0);
    pfd[1].fd = efd;
    pfd[1].events = (POLLIN | POLLERR | POLLNVAL);
    while (1) {
        if (killEventThread) {
            eventThreadAlive = false;
            LOGV("Event Thread is dying.");
            return;
        }

        err_poll = poll(pfd, NUM_FDS, timeout);

        if (err_poll == EINTR)
            LOGE("Timer is intrrupted");
        if (pfd[1].revents & POLLIN) {
            uint64_t u;
            read(efd, &u, sizeof(uint64_t));
            LOGE("POLLIN event occured on the event fd, value written to %llu",(unsigned long long)u);
            pfd[1].revents = 0;
            if (u == SIGNAL_EVENT_THREAD) {
                BuffersAllocated tempbuf = *(memBuffersResponseQueue.begin());
                timeout = 1000 * tempbuf.bytesToWrite / (numChannels * PCM_FORMAT * mSampleRate);
                LOGV("Setting timeout due Last buffer seek to %d, mReachedEOS %d, memBuffersRequestQueue.size() %d", timeout, mReachedEOS,memBuffersResponseQueue.size());
                continue;
            }
        }
        if ((pfd[1].revents & POLLERR) || (pfd[1].revents & POLLNVAL))
            LOGE("POLLERR or INVALID POLL");

        LOGV("LPA event");
        if (killEventThread) {
            break;
        }

        if (timeout != -1 && mReachedEOS) {
            LOGV("Timeout %d: Posting EOS event to AwesomePlayer",timeout);
            isPaused = true;
            mPauseTime = mSeekTimeUs + getTimeStamp(A2DP_DISABLED);
            mObserver->postAudioEOS();
            audioEOSPosted = true;
            timeout = -1;
        }
        if (!mReachedEOS) {
            timeout = -1;
        }
        if (err_poll < 0) {
             LOGV("fatal err in poll:%d\n", err_poll);
             eventThreadAlive = false;
             LOGV("Event Thread is dying.");
             break;
        }
        struct snd_timer_tread rbuf[4];
        read(local_handle->timer_fd, rbuf, sizeof(struct snd_timer_tread) * 4 );

        if (!(pfd[0].revents & POLLIN))
             continue;

        pfd[0].revents = 0;
        //pfd[1].revents = 0;

        LOGV("After an event occurs");

        if (killEventThread) {
            break;
        }
        if (memBuffersResponseQueue.empty())
            continue;

        //exit on abrupt event
        Mutex::Autolock autoLock(mLock);
        pthread_mutex_lock(&mem_response_mutex);
        BuffersAllocated buf = *(memBuffersResponseQueue.begin());
        memBuffersResponseQueue.erase(memBuffersResponseQueue.begin());
        /* If the rendering is complete report EOS to the AwesomePlayer */
        if (mObserver && !asyncReset && mReachedEOS && memBuffersResponseQueue.size() == 1) {
            BuffersAllocated tempbuf = *(memBuffersResponseQueue.begin());
            timeout = 1000 * tempbuf.bytesToWrite / (numChannels * PCM_FORMAT * mSampleRate);
            LOGV("Setting timeout to %d,nextbuffer %d, buf.bytesToWrite %d, mReachedEOS %d, memBuffersRequestQueue.size() %d", timeout, tempbuf.bytesToWrite, buf.bytesToWrite, mReachedEOS,memBuffersResponseQueue.size());
        }

        pthread_mutex_unlock(&mem_response_mutex);
        // Post buffer to request Q
        pthread_mutex_lock(&mem_request_mutex);
        memBuffersRequestQueue.push_back(buf);
        pthread_mutex_unlock(&mem_request_mutex);

        pthread_cond_signal(&decoder_cv);

    }
    eventThreadAlive = false;
    if (efd != -1)
        close(efd);
    LOGV("Event Thread is dying.");

}

void *LPAPlayer::A2DPThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->A2DPThreadEntry();
    return NULL;
}

void LPAPlayer::A2DPThreadEntry() {
    pid_t tid  = gettid();
    androidSetThreadPriority(tid,ANDROID_PRIORITY_URGENT_AUDIO);
    prctl(PR_SET_NAME, (unsigned long)"LPA A2DPThread", 0, 0, 0);

    while (1) {
        /* If exitPending break here */
        if (killA2DPThread) {
            break;
        }

        //TODO: Remove this
        pthread_mutex_lock(&mem_response_mutex);
        if (memBuffersResponseQueue.empty() || !mAudioSinkOpen || isPaused || !bIsA2DPEnabled) {
            LOGV("A2DPThreadEntry:: responseQ empty %d mAudioSinkOpen %d isPaused %d bIsA2DPEnabled %d",
                 memBuffersResponseQueue.empty(), mAudioSinkOpen, isPaused, bIsA2DPEnabled);
            LOGV("A2DPThreadEntry:: Waiting on a2dp_cv");
            pthread_cond_wait(&a2dp_cv, &mem_response_mutex);
            LOGV("A2DPThreadEntry:: received signal to wake up");
            pthread_mutex_unlock(&mem_response_mutex);
            continue;
        }
        // A2DP got disabled -- Queue up everything back to Request Queue
        if (!bIsA2DPEnabled) {
            pthread_mutex_lock(&mem_request_mutex);
            memBuffersResponseQueue.clear();
            memBuffersRequestQueue.clear();

            List<BuffersAllocated>::iterator it = bufPool.begin();
            for(;it!=bufPool.end();++it) {
                 memBuffersRequestQueue.push_back(*it);
            }
            pthread_mutex_unlock(&mem_response_mutex);
            pthread_mutex_unlock(&mem_request_mutex);
        }
        //A2DP is enabled -- Continue normal Playback
        else {
            List<BuffersAllocated>::iterator it = memBuffersResponseQueue.begin();
            BuffersAllocated buf = *it;
            memBuffersResponseQueue.erase(it);
            pthread_mutex_unlock(&mem_response_mutex);
            bytesToWrite = buf.bytesToWrite;
            LOGV("bytes To write:%d",bytesToWrite);

            uint32_t bytesWritten = 0;
            uint32_t numBytesRemaining = 0;
            uint32_t bytesAvailInBuffer = 0;
            void* data = buf.localBuf;

            while (bytesToWrite) {
                /* If exitPending break here */
                if (killA2DPThread || !bIsA2DPEnabled) {
                    LOGV("A2DPThreadEntry: A2DPThread set to be killed");
                    break;
                }

                bytesAvailInBuffer = mAudioSink->bufferSize();

                uint32_t writeLen = bytesAvailInBuffer > bytesToWrite ? bytesToWrite : bytesAvailInBuffer;
                LOGV("Writing %d bytes to A2DP ", writeLen);
                bytesWritten = mAudioSink->write(data, writeLen);
                if ( bytesWritten != writeLen ) {
                    //Paused - Wait till resume
                    if (isPaused && bIsA2DPEnabled) {
                        LOGV("Pausing A2DP playback");
                        pthread_mutex_lock(&a2dp_mutex);
                        pthread_cond_wait(&a2dp_cv, &a2dp_mutex);
                        pthread_mutex_unlock(&a2dp_mutex);
                    }


                    //Seeked: break out of loop, flush old buffers and write new buffers
                    LOGV("@_@bytes To write1:%d",bytesToWrite);
                }
                if (mSeeked) {
                    LOGV("Seeking A2DP Playback");
                    break;
                }
                data = (char *) data + bytesWritten;
                mNumA2DPBytesPlayed += bytesWritten;
                bytesToWrite -= bytesWritten;
                LOGV("@_@bytes To write2:%d",bytesToWrite);
            }
            if (mObserver && !asyncReset && mReachedEOS && memBuffersResponseQueue.empty()) {
                LOGV("Posting EOS event to AwesomePlayer");
                mObserver->postAudioEOS();
            }
            pthread_mutex_lock(&mem_request_mutex);
            memBuffersRequestQueue.push_back(buf);
            if (killA2DPThread) {
                pthread_mutex_unlock(&mem_request_mutex);
                break;
            }
            //flush out old buffer
            if (mSeeked || !bIsA2DPEnabled) {
                mSeeked = false;
                LOGV("A2DPThread: Putting buffers back to requestQ from responseQ");
                pthread_mutex_lock(&mem_response_mutex);
                memBuffersResponseQueue.clear();
                memBuffersRequestQueue.clear();

                List<BuffersAllocated>::iterator it = bufPool.begin();
                for(;it!=bufPool.end();++it) {
                     memBuffersRequestQueue.push_back(*it);
                }
                pthread_mutex_unlock(&mem_response_mutex);
            }
            pthread_mutex_unlock(&mem_request_mutex);
            // Signal decoder thread when a buffer is put back to request Q
            pthread_cond_signal(&decoder_cv);
        }
    }
    a2dpThreadAlive = false;

    LOGV("AudioSink stop");
    if(mAudioSinkOpen) {
        mAudioSinkOpen = false;
        mAudioSink->stop();
    }

    LOGV("A2DP Thread is dying.");
}

void *LPAPlayer::EffectsThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->EffectsThreadEntry();
    return NULL;
}

void LPAPlayer::EffectsThreadEntry() {
    while(1) {
        if(killEffectsThread) {
            break;
        }
        pthread_mutex_lock(&effect_mutex);

        if(bEffectConfigChanged && !isPaused) {
            bEffectConfigChanged = false;

            // 1. Clear current effectQ
            LOGV("Clearing EffectQ: size %d", effectsQueue.size());
            while (!effectsQueue.empty())  {
                List<BuffersAllocated>::iterator it = effectsQueue.begin();
                effectsQueue.erase(it);
            }

            // 2. Lock the responseQ mutex
            pthread_mutex_lock(&mem_response_mutex);

            // 3. Copy responseQ to effectQ
            LOGV("Copying responseQ to effectQ: responseQ size %d", memBuffersResponseQueue.size());
            for (List<BuffersAllocated>::iterator it = memBuffersResponseQueue.begin();
                it != memBuffersResponseQueue.end(); ++it) {
                BuffersAllocated buf = *it;
                effectsQueue.push_back(buf);
            }

            // 4. Unlock the responseQ mutex
            pthread_mutex_unlock(&mem_response_mutex);
        }
        // If effectQ is empty just wait for a signal
        // Else dequeue a buffer, apply effects and delete it from effectQ
        if(effectsQueue.empty() || asyncReset || bIsA2DPEnabled || isPaused) {
            LOGV("EffectQ is empty or Reset called or A2DP enabled, waiting for signal");
            pthread_cond_wait(&effect_cv, &effect_mutex);
            LOGV("effectsThread: received signal to wake up");
            pthread_mutex_unlock(&effect_mutex);
        } else {
            pthread_mutex_unlock(&effect_mutex);

            List<BuffersAllocated>::iterator it = effectsQueue.begin();
            BuffersAllocated buf = *it;

            pthread_mutex_lock(&apply_effect_mutex);
            LOGV("effectsThread: applying effects on %p fd %d", buf.memBuf, (int)buf.memFd);
            mAudioFlinger->applyEffectsOn((int16_t*)buf.localBuf,
                                          (int16_t*)buf.memBuf,
                                          (int)buf.bytesToWrite);
            pthread_mutex_unlock(&apply_effect_mutex);
            effectsQueue.erase(it);
        }
    }
    LOGV("Effects thread is dead");
    effectsThreadAlive = false;
}

void *LPAPlayer::A2DPNotificationThreadWrapper(void *me) {
    static_cast<LPAPlayer *>(me)->A2DPNotificationThreadEntry();
    return NULL;
}


void LPAPlayer::A2DPNotificationThreadEntry() {
    while (1) {
        pthread_mutex_lock(&a2dp_notification_mutex);
        pthread_cond_wait(&a2dp_notification_cv, &a2dp_notification_mutex);
        pthread_mutex_unlock(&a2dp_notification_mutex);
        if (killA2DPNotificationThread) {
            break;
        }

        LOGV("A2DP notification has come bIsA2DPEnabled: %d", bIsA2DPEnabled);

        if (bIsA2DPEnabled) {
            struct pcm * local_handle = (struct pcm *)handle;
            LOGV("Flushing all the buffers");
            pthread_mutex_lock(&mem_response_mutex);
            pthread_mutex_lock(&mem_request_mutex);
            memBuffersResponseQueue.clear();
            memBuffersRequestQueue.clear();

            List<BuffersAllocated>::iterator it = bufPool.begin();
            for(;it!=bufPool.end();++it) {
                 memBuffersRequestQueue.push_back(*it);
            }
            pthread_mutex_unlock(&mem_request_mutex);
            pthread_mutex_unlock(&mem_response_mutex);
            LOGV("All the buffers flushed, Now flushing the driver");
            if (ioctl(local_handle->fd, SNDRV_PCM_IOCTL_RESET))
                LOGE("Reset failed!");
            LOGV("Driver flushed and opening mAudioSink");
            if (!mAudioSinkOpen) {
                LOGV("Close Session");
                if (mAudioSink.get() != NULL) {
                    mAudioSink->closeSession();
                    releaseWakeLock();
                    LOGV("mAudioSink close session");
                    mIsAudioRouted = false;
                } else {
                    LOGE("close session NULL");
                }
                sp<MetaData> format = mSource->getFormat();
                const char *mime;
                bool success = format->findCString(kKeyMIMEType, &mime);
                CHECK(success);
                CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));
                success = format->findInt32(kKeySampleRate, &mSampleRate);
                CHECK(success);
                success = format->findInt32(kKeyChannelCount, &numChannels);
                CHECK(success);
                LOGV("Before Audio Sink Open");
                status_t ret = mAudioSink->open(mSampleRate, numChannels,AUDIO_FORMAT_PCM_16_BIT, DEFAULT_AUDIOSINK_BUFFERCOUNT);
                if (!isPaused) {
                    mAudioSink->start();
                }
                LOGV("After Audio Sink Open");
                mAudioSinkOpen = true;
            }
            LOGV("Signalling to decoder cv");
            pthread_cond_signal(&decoder_cv);
        }
        else {
            mInternalSeeking = true;
            mReachedEOS = false;
            mSeekTimeUs += getTimeStamp(A2DP_DISCONNECT);
            mNumA2DPBytesPlayed = 0;
            pthread_cond_signal(&a2dp_cv);
        }
    }
    a2dpNotificationThreadAlive = false;
    LOGV("A2DPNotificationThread is dying");

}

void *LPAPlayer::memBufferAlloc(int32_t nSize, int32_t *mem_fd){
    int32_t memfd = -1;
    void  *mem_buf = NULL;
    void  *local_buf = NULL;
    int i = 0;
    struct pcm * local_handle = (struct pcm *)handle;

    for (i = 0; i < MEM_BUFFER_COUNT; i++) {
        mem_buf = (int32_t *)local_handle->addr + (nSize * i/sizeof(int));
        local_buf = malloc(nSize);
        if (NULL == local_buf) {
            return NULL;
        }

        // 3. Store this information for internal mapping / maintanence
        BuffersAllocated buf(local_buf, mem_buf, nSize, memfd);
        memBuffersRequestQueue.push_back(buf);
        bufPool.push_back(buf);

        // 4. Send the mem fd information
        LOGV("memBufferAlloc calling with required size %d", nSize);
        LOGV("The MEM that is allocated is %d and buffer is %x", memfd, (unsigned int)mem_buf);
    }
    *mem_fd = memfd;
    return NULL;
}

void LPAPlayer::memBufferDeAlloc()
{
    //Remove all the buffers from bufpool 
    while (!bufPool.empty())  {
        List<BuffersAllocated>::iterator it = bufPool.begin();
        BuffersAllocated &memBuffer = *it;
        // free the local buffer corresponding to mem buffer
        free(memBuffer.localBuf);
        LOGV("Removing from bufpool");
        bufPool.erase(it);
    }

}

void LPAPlayer::createThreads() {

    //Initialize all the Mutexes and Condition Variables
    pthread_mutex_init(&mem_request_mutex, NULL);
    pthread_mutex_init(&mem_response_mutex, NULL);
    pthread_mutex_init(&decoder_mutex, NULL);
    pthread_mutex_init(&event_mutex, NULL);
    pthread_mutex_init(&a2dp_mutex, NULL);
    pthread_mutex_init(&effect_mutex, NULL);
    pthread_mutex_init(&apply_effect_mutex, NULL);
    pthread_mutex_init(&a2dp_notification_mutex, NULL);
    pthread_mutex_init(&pause_mutex,NULL);

    pthread_cond_init (&event_cv, NULL);
    pthread_cond_init (&decoder_cv, NULL);
    pthread_cond_init (&a2dp_cv, NULL);
    pthread_cond_init (&a2dp_notification_cv, NULL);
    pthread_cond_init (&pause_cv, NULL);

    // Create 4 threads Effect, decoder, event and A2dp
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);

    killDecoderThread = false;
    killEventThread = false;
    killA2DPThread = false;
    killEffectsThread = false;
    killA2DPNotificationThread = false;

    decoderThreadAlive = true;
    eventThreadAlive = true;
    a2dpThreadAlive = true;
    effectsThreadAlive = true;
    a2dpNotificationThreadAlive = true;

    LOGV("Creating Event Thread");
    pthread_create(&eventThread, &attr, eventThreadWrapper, this);

    LOGV("Creating decoder Thread");
    pthread_create(&decoderThread, &attr, decoderThreadWrapper, this);

    LOGV("Creating A2DP Thread");
    pthread_create(&A2DPThread, &attr, A2DPThreadWrapper, this);

    LOGV("Creating Effects Thread");
    pthread_create(&EffectsThread, &attr, EffectsThreadWrapper, this);

    LOGV("Creating A2DP Notification Thread");
    pthread_create(&A2DPNotificationThread, &attr, A2DPNotificationThreadWrapper, this);

    pthread_attr_destroy(&attr);
}


size_t LPAPlayer::fillBuffer(void *data, size_t size) {
    LOGE("fillBuffer");
    if (mNumFramesPlayed == 0) {
        LOGV("AudioCallback");
    }

    LOGV("Number of Frames Played: %lld", mNumFramesPlayed);
    if (mReachedEOS) {
        return 0;
    }

    size_t size_done = 0;
    size_t size_remaining = size;
    while (size_remaining > 0) {
        MediaSource::ReadOptions options;
        {
            Mutex::Autolock autoLock(mLock);

            if (mSeeking) {
                mInternalSeeking = false;
            }
            if (mSeeking || mInternalSeeking) {
                if (mIsFirstBuffer) {
                    if (mFirstBuffer != NULL) {
                        mFirstBuffer->release();
                        mFirstBuffer = NULL;
                    }
                    mIsFirstBuffer = false;
                }

                options.setSeekTo(mSeekTimeUs);

                if (mInputBuffer != NULL) {
                    mInputBuffer->release();
                    mInputBuffer = NULL;
                }

                // This is to ignore the data already filled in the output buffer
                size_done = 0;
                size_remaining = size;

                mSeeking = false;
                if (mObserver && !asyncReset && !mInternalSeeking) {
                    LOGV("fillBuffer: Posting audio seek complete event");
                    mObserver->postAudioSeekComplete();
                }
                mInternalSeeking = false;
            }
        }

        if (mInputBuffer == NULL) {
            status_t err;

            if (mIsFirstBuffer) {
                mInputBuffer = mFirstBuffer;
                mFirstBuffer = NULL;
                err = mFirstBufferResult;

                mIsFirstBuffer = false;
            } else {
                err = mSource->read(&mInputBuffer, &options);
            }

            CHECK((err == OK && mInputBuffer != NULL)
                  || (err != OK && mInputBuffer == NULL));

            Mutex::Autolock autoLock(mLock);

            if (err != OK) {
                if (err == INFO_FORMAT_CHANGED) {
                    sp<MetaData> format = mSource->getFormat();
                    const char *mime;
                    bool success = format->findCString(kKeyMIMEType, &mime);
                    CHECK(success);
                    CHECK(!strcasecmp(mime, MEDIA_MIMETYPE_AUDIO_RAW));

                    success = format->findInt32(kKeySampleRate, &mSampleRate);
                    CHECK(success);

                    int32_t numChannels;
                    success = format->findInt32(kKeyChannelCount, &numChannels);
                    CHECK(success);

                    if(bIsA2DPEnabled) {
                        mAudioSink->stop();
                        mAudioSink->close();
                        mAudioSinkOpen = false;
                        status_t err = mAudioSink->open(
                                mSampleRate, numChannels, AUDIO_FORMAT_PCM_16_BIT,
                                DEFAULT_AUDIOSINK_BUFFERCOUNT);
                        if (err != OK) {
                            mSource->stop();
                            return err;
                        }
                        mAudioSinkOpen = true;
                        mLatencyUs = (int64_t)mAudioSink->latency() * 1000;
                        mFrameSize = mAudioSink->frameSize();
                        mAudioSink->start();
                    } else {
                        /* TODO: LPA driver needs to be reconfigured
                           For MP3 we might not come here but for AAC we need this */
                        mAudioSink->stop();
                        mAudioSink->closeSession();
                        LOGV("Opening a routing session in fillBuffer: sessionId = %d mSampleRate %d numChannels %d",
                             sessionId, mSampleRate, numChannels);
                        status_t err = mAudioSink->openSession(AUDIO_FORMAT_PCM_16_BIT, sessionId, mSampleRate, numChannels);
                        if (err != OK) {
                            mSource->stop();
                            return err;
                        }
                    }
                    break;
                } else {
                    mReachedEOS = true;
                    mFinalStatus = err;
                    break;
                }
            }

            CHECK(mInputBuffer->meta_data()->findInt64(
                                                      kKeyTime, &mPositionTimeMediaUs));
            mFrameSize = mAudioSink->frameSize();
            mPositionTimeRealUs =
            ((mNumFramesPlayed + size_done / mFrameSize) * 1000000)
            / mSampleRate;

        }

        if (mInputBuffer->range_length() == 0) {
            mInputBuffer->release();
            mInputBuffer = NULL;
            continue;
        }

        size_t copy = size_remaining;
        if (copy > mInputBuffer->range_length()) {
            copy = mInputBuffer->range_length();
        }

        memcpy((char *)data + size_done,
               (const char *)mInputBuffer->data() + mInputBuffer->range_offset(),
               copy);

        mInputBuffer->set_range(mInputBuffer->range_offset() + copy,
                                mInputBuffer->range_length() - copy);

        size_done += copy;
        size_remaining -= copy;
    }
    return size_done;
}

int64_t LPAPlayer::getRealTimeUs() {
    Mutex::Autolock autoLock(mLock);
    return getRealTimeUsLocked();
}


int64_t LPAPlayer::getRealTimeUsLocked(){
    //Used for AV sync: irrelevant API for LPA.
    return 0;
}

int64_t LPAPlayer::getTimeStamp(A2DPState state) {
    int64_t timestamp = 0;
    switch (state) {
    case A2DP_ENABLED:
    case A2DP_DISCONNECT:
        timestamp = (mNumA2DPBytesPlayed * 1000000)
                    /(2 * numChannels * mSampleRate);
        break;
    case A2DP_DISABLED:
    case A2DP_CONNECT: {
        struct pcm * local_handle = (struct pcm *)handle;
        struct snd_compr_tstamp tstamp;
        if (ioctl(local_handle->fd, SNDRV_COMPRESS_TSTAMP, &tstamp)) {
            LOGE("Tunnel Player: failed SNDRV_COMPRESS_TSTAMP\n");
        }
        else {
            LOGV("timestamp = %lld\n", tstamp.timestamp);
            timestamp = tstamp.timestamp;
        }
        break;
    }
    default:
        break;
    }
    return timestamp;
}

int64_t LPAPlayer::getMediaTimeUs() {
    Mutex::Autolock autoLock(mLock);
    LOGV("getMediaTimeUs() isPaused %d mSeekTimeUs %lld mPauseTime %lld", isPaused, mSeekTimeUs, mPauseTime);
    if (isPaused) {
        return mPauseTime;
    } else {
        A2DPState state = bIsA2DPEnabled ? A2DP_ENABLED : A2DP_DISABLED;
        return (mSeekTimeUs + getTimeStamp(state));
    }
}

bool LPAPlayer::getMediaTimeMapping(
                                   int64_t *realtime_us, int64_t *mediatime_us) {
    Mutex::Autolock autoLock(mLock);

    *realtime_us = mPositionTimeRealUs;
    *mediatime_us = mPositionTimeMediaUs;

    return mPositionTimeRealUs != -1 && mPositionTimeMediaUs != -1;
}

void LPAPlayer::requestAndWaitForDecoderThreadExit() {

    if (!decoderThreadAlive)
        return;
    killDecoderThread = true;
    pthread_cond_signal(&decoder_cv);
    pthread_join(decoderThread,NULL);
    LOGV("decoder thread killed");

}

void LPAPlayer::requestAndWaitForEventThreadExit() {
    if (!eventThreadAlive)
        return;
    killEventThread = true;
    uint64_t writeValue = KILL_EVENT_THREAD;
    LOGE("Writing to efd %d",efd);
    write(efd, &writeValue, sizeof(uint64_t));
    if(!bIsA2DPEnabled) {
    }
    pthread_cond_signal(&event_cv);
    pthread_join(eventThread,NULL);
    LOGV("event thread killed");
}

void LPAPlayer::requestAndWaitForA2DPThreadExit() {
    if (!a2dpThreadAlive)
        return;
    killA2DPThread = true;
    pthread_cond_signal(&a2dp_cv);
    pthread_join(A2DPThread,NULL);
    LOGV("a2dp thread killed");
}

void LPAPlayer::requestAndWaitForEffectsThreadExit() {
    if (!effectsThreadAlive)
        return;
    killEffectsThread = true;
    pthread_cond_signal(&effect_cv);
    pthread_join(EffectsThread,NULL);
    LOGV("effects thread killed");
}

void LPAPlayer::requestAndWaitForA2DPNotificationThreadExit() {
    if (!a2dpNotificationThreadAlive)
        return;
    killA2DPNotificationThread = true;
    pthread_cond_signal(&a2dp_notification_cv);
    pthread_join(A2DPNotificationThread,NULL);
    LOGV("a2dp notification thread killed");
}

void LPAPlayer::onPauseTimeOut() {
    Mutex::Autolock autoLock(resumeLock);
    struct msm_audio_stats stats;
    int nBytesConsumed = 0;
    LOGV("onPauseTimeOut");
    if (!mPauseEventPending) {
        return;
    }
    mPauseEventPending = false;
    if(!bIsA2DPEnabled) {
        // 1.) Set seek flags
        mInternalSeeking = true;
        mReachedEOS = false;
        mSeekTimeUs += getTimeStamp(A2DP_DISABLED);

        // 2.) Flush the buffers and transfer everything to request queue
        pthread_mutex_lock(&mem_response_mutex);
        pthread_mutex_lock(&mem_request_mutex);
        memBuffersResponseQueue.clear();
        memBuffersRequestQueue.clear();
        List<BuffersAllocated>::iterator it = bufPool.begin();
        for(;it!=bufPool.end();++it) {
             memBuffersRequestQueue.push_back(*it);
        }
        pthread_mutex_unlock(&mem_request_mutex);
        pthread_mutex_unlock(&mem_response_mutex);
        LOGV("onPauseTimeOut after memBuffersRequestQueue.size() = %d, memBuffersResponseQueue.size() = %d ",memBuffersRequestQueue.size(),memBuffersResponseQueue.size());

        // 3.) Close routing Session
        mAudioSink->closeSession();
        mIsAudioRouted = false;

        // 4.) Release Wake Lock
        releaseWakeLock();
    }

}

} //namespace android
