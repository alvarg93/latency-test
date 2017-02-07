#include <assert.h>
#include <jni.h>
#include <string.h>
#include <pthread.h>

// for native audio
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include <tgmath.h>
#include <math.h>
#include <android/log.h>
#include <time.h>

#define  LOG_TAG    "LatencyTest"

#define  LOGI(...)  __android_log_print(ANDROID_LOG_INFO,LOG_TAG,__VA_ARGS__)

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static short *resampleBuf = NULL;
// a mutext to guard against re-entrance to record & playback
// as well as make recording and playing back to be mutually exclusive
// this is to avoid crash at situations like:
//    recording is in session [not finished]
//    user presses record button and another recording coming in
// The action: when recording/playing back is not finished, ignore the new request
static pthread_mutex_t audioEngineLock = PTHREAD_MUTEX_INITIALIZER;

// aux effect on the output mix, used by the buffer queue player
static const SLEnvironmentalReverbSettings reverbSettings =
        SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

// recorder interfaces
static SLObjectItf recorderObject = NULL;
static SLRecordItf recorderRecord;
static SLAndroidSimpleBufferQueueItf recorderBufferQueue;


// pointer and size of the next player buffer to enqueue, and number of remaining buffers
static short *nextBuffer;
static unsigned nextSize;

/*
 * ------------------------- AUDIO LOOP LATENCY TEST --------------------------
 */
// 1 millisecond of recorded audio at 16 kHz mono, 16-bit signed little endian
// synthesized beep clip
#define BEEP_FRAMES 4000
static short beepBuffer[BEEP_FRAMES];
#define RECORDER_FRAMES (16 * 1)
#define TEST_DURATION 5000
#define PLAY_MESSAGE_FORMAT "play beep at %ld ns"
#define RECEIVED_MESSAGE_FORMAT "received at %ld ns, difference = %ld"
#define RESULT_BUFFER_SIZE 10
#define TIME_VALUE_TYPE long
static short recorderBuffer[RECORDER_FRAMES];
static short recorderBuffer2[RECORDER_FRAMES];
static TIME_VALUE_TYPE results[RESULT_BUFFER_SIZE];
short recordBufferCount;
static int toneFreq = 4000;
static double piValue = 3.14159;

static int isBeeping = 0;
static int once = 1;
static int resultsCount;

static TIME_VALUE_TYPE now_ms(void);

TIME_VALUE_TYPE lastDelay;


// synthesize a mono sawtooth wave and place it into a buffer (called automatically on load)
__attribute__((constructor)) static void onDlOpen(void) {
    unsigned i;
    for (i = 0; i < BEEP_FRAMES; ++i) {
        beepBuffer[i] =
                2 * i < BEEP_FRAMES ? sin(2.0 * piValue * i / (44100 / toneFreq)) * 32768 : 0;
    }
}

void releaseResampleBuf(void) {
    if (0 == bqPlayerSampleRate) {
        //we are not using fast path, so we were not creating buffers, nothing to do
        return;
    }

    free(resampleBuf);
    resampleBuf = NULL;
}

/*
 * Only support up-sampling
 */
short *createResampledBuf(uint32_t idx, uint32_t srcRate, unsigned *size) {
    short *src = NULL;
    short *workBuf;
    int upSampleRate;
    int32_t srcSampleCount = 0;

    if (0 == bqPlayerSampleRate) {
        return NULL;
    }
    if (bqPlayerSampleRate % srcRate) {
        /*
         * simple up-sampling, must be divisible
         */
        return NULL;
    }
    upSampleRate = bqPlayerSampleRate / srcRate;

    switch (idx) {
        case 0:
            return NULL;
        case 3: // BEEP_CLIP
            srcSampleCount = BEEP_FRAMES;
            src = beepBuffer;
            break;
        default:
            assert(0);
            return NULL;
    }

    resampleBuf = (short *) malloc((srcSampleCount * upSampleRate) << 1);
    if (resampleBuf == NULL) {
        return resampleBuf;
    }
    workBuf = resampleBuf;
    for (int sample = 0; sample < srcSampleCount; sample++) {
        for (int dup = 0; dup < upSampleRate; dup++) {
            *workBuf++ = src[sample];
        }
    }

    *size = (srcSampleCount * upSampleRate) << 1;     // sample format is 16 bit
    return resampleBuf;
}

// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    assert(bq == bqPlayerBufferQueue);
    assert(NULL == context);
    if (recordBufferCount > 0) {
        SLresult result;
        once = 1;
        lastDelay = now_ms();
        LOGI(PLAY_MESSAGE_FORMAT, lastDelay);
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
        assert (SL_RESULT_SUCCESS == result);
        (void) result;
    } else {
        releaseResampleBuf();
        pthread_mutex_unlock(&audioEngineLock);
    }
}


// this callback handler is called every time a buffer finishes recording
void bqRecorderCallback(SLAndroidSimpleBufferQueueItf bq, void *context) {
    TIME_VALUE_TYPE curTime = now_ms();
    recordBufferCount--;
    assert(bq == recorderBufferQueue);
    assert(NULL == context);
    // for streaming recording, here we would call Enqueue to give recorder the next buffer to fill
    // but instead, this is a one-time buffer so we stop recording
    short *curBuffer = recordBufferCount % 2 ? recorderBuffer : recorderBuffer2;
    int max = 0;
    for (int i = 0; i < RECORDER_FRAMES; i++) {
        max = abs(curBuffer[i]) > max ? abs(curBuffer[i]) : max;
    }

    if (max > 1000 && once) {
        once = 0;
        if (!isBeeping) {
            long latency = curTime - lastDelay;
            if (resultsCount < RESULT_BUFFER_SIZE)
                results[resultsCount] = latency;
            resultsCount++;
            LOGI(RECEIVED_MESSAGE_FORMAT, curTime, latency);
        }
        isBeeping = 1;
    } else {
        isBeeping = 0;
    }
    SLresult result;
    if (recordBufferCount > 0) {
        if (recordBufferCount % 2)
            result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer2,
                                                     RECORDER_FRAMES * sizeof(short));
        else
            result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer,
                                                     RECORDER_FRAMES * sizeof(short));
        assert(SL_RESULT_SUCCESS == result);
    } else {
        result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    }
}


// create the engine and output mix objects
void Java_com_janyga_latencytest_LatencyTest_createEngine(JNIEnv *env, jclass clazz) {
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

}


// create buffer queue audio player
void Java_com_janyga_latencytest_LatencyTest_createBufferQueueAudioPlayer(JNIEnv *env,
                                                                          jclass clazz,
                                                                          jint sampleRate,
                                                                          jint bufSize) {
    SLresult result;
    if (sampleRate >= 0 && bufSize >= 0) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
    }

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_8,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if (bqPlayerSampleRate) {
        format_pcm.samplesPerSec = bqPlayerSampleRate;       //sample rate in mili second
    }
    SLDataSource audioSrc = {&loc_bufq, &format_pcm};

    // configure audio sink
    SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
    SLDataSink audioSnk = {&loc_outmix, NULL};

    /*
     * create audio player:
     *     fast audio does not support when SL_IID_EFFECTSEND is required, skip it
     *     for fast audio case
     */
    const SLInterfaceID ids[3] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME, SL_IID_EFFECTSEND,
            /*SL_IID_MUTESOLO,*/};
    const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE,
            /*SL_BOOLEAN_TRUE,*/ };

    result = (*engineEngine)->CreateAudioPlayer(engineEngine, &bqPlayerObject, &audioSrc, &audioSnk,
                                                bqPlayerSampleRate ? 2 : 3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
                                             &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the effect send interface
    bqPlayerEffectSend = NULL;
    if (0 == bqPlayerSampleRate) {
        result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
                                                 &bqPlayerEffectSend);
        assert(SL_RESULT_SUCCESS == result);
        (void) result;
    }

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
}

// create audio recorder: recorder is not in fast path
//    like to avoid excessive re-sampling while playing back from Hello & Android clip
jboolean Java_com_janyga_latencytest_LatencyTest_createAudioRecorder(JNIEnv *env, jclass clazz) {
    SLresult result;

    // configure audio source
    SLDataLocator_IODevice loc_dev = {SL_DATALOCATOR_IODEVICE, SL_IODEVICE_AUDIOINPUT,
                                      SL_DEFAULTDEVICEID_AUDIOINPUT, NULL};
    SLDataSource audioSrc = {&loc_dev, NULL};

    // configure audio sink
    SLDataLocator_AndroidSimpleBufferQueue loc_bq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format_pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_16,
                                   SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                                   SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSink audioSnk = {&loc_bq, &format_pcm};

    // create audio recorder
    // (requires the RECORD_AUDIO permission)
    const SLInterfaceID id[1] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
    const SLboolean req[1] = {SL_BOOLEAN_TRUE};
    result = (*engineEngine)->CreateAudioRecorder(engineEngine, &recorderObject, &audioSrc,
                                                  &audioSnk, 1, id, req);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // realize the audio recorder
    result = (*recorderObject)->Realize(recorderObject, SL_BOOLEAN_FALSE);
    if (SL_RESULT_SUCCESS != result) {
        return JNI_FALSE;
    }

    // get the record interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_RECORD, &recorderRecord);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // get the buffer queue interface
    result = (*recorderObject)->GetInterface(recorderObject, SL_IID_ANDROIDSIMPLEBUFFERQUEUE,
                                             &recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    // register callback on the buffer queue
    result = (*recorderBufferQueue)->RegisterCallback(recorderBufferQueue, bqRecorderCallback,
                                                      NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    return JNI_TRUE;
}

// set the recording state for the audio recorder
void Java_com_janyga_latencytest_LatencyTest_startRecording(JNIEnv *env, jobject obj) {

    resultsCount = 0;
    for (int i = 0; i < RESULT_BUFFER_SIZE; i++)
        results[i] = 0L;

    SLresult result;

    if (pthread_mutex_trylock(&audioEngineLock)) {
        return;
    }
    // in case already recording, stop recording and clear buffer queue
    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_STOPPED);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
    result = (*recorderBufferQueue)->Clear(recorderBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;

    recordBufferCount = TEST_DURATION;
    result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer,
                                             RECORDER_FRAMES * sizeof(short));
    assert(SL_RESULT_SUCCESS == result);

    result = (*recorderBufferQueue)->Enqueue(recorderBufferQueue, recorderBuffer2,
                                             RECORDER_FRAMES * sizeof(short));
    (void) result;

    result = (*recorderRecord)->SetRecordState(recorderRecord, SL_RECORDSTATE_RECORDING);
    assert(SL_RESULT_SUCCESS == result);

    nextBuffer = createResampledBuf(3, SL_SAMPLINGRATE_8, &nextSize);
    if (!nextBuffer) {
        nextBuffer = (short *) beepBuffer;
        nextSize = sizeof(beepBuffer);
    }
    lastDelay = now_ms();
    LOGI(PLAY_MESSAGE_FORMAT, lastDelay);
    result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
    assert(SL_RESULT_SUCCESS == result);
    (void) result;
}

JNIEXPORT jlongArray Java_com_janyga_latencytest_LatencyTest_checkResultBuffer(JNIEnv *env,
                                                                               jobject obj) {
    jlongArray result;
    result = (*env)->NewLongArray(env, RESULT_BUFFER_SIZE);
    if (result == NULL) {
        return NULL; /* out of memory error thrown */
    }
    int i;
    // fill a temp structure to use to populate the java int array
    jlong fill[256];
    for (i = 0; i < RESULT_BUFFER_SIZE; i++) {
        fill[i] = results[i]; // put whatever logic you want to populate the values here.
    }
    // move from the temp structure to the java structure
    (*env)->SetLongArrayRegion(env, result, 0, RESULT_BUFFER_SIZE, fill);
    return result;
}


// shut down the native audio system
void Java_com_janyga_latencytest_LatencyTest_shutdown(JNIEnv *env, jclass clazz) {

    // destroy buffer queue audio player object, and invalidate all associated interfaces
    if (bqPlayerObject != NULL) {
        (*bqPlayerObject)->Destroy(bqPlayerObject);
        bqPlayerObject = NULL;
        bqPlayerPlay = NULL;
        bqPlayerBufferQueue = NULL;
        bqPlayerEffectSend = NULL;
        bqPlayerMuteSolo = NULL;
        bqPlayerVolume = NULL;
    }

    // destroy audio recorder object, and invalidate all associated interfaces
    if (recorderObject != NULL) {
        (*recorderObject)->Destroy(recorderObject);
        recorderObject = NULL;
        recorderRecord = NULL;
        recorderBufferQueue = NULL;
    }

    // destroy output mix object, and invalidate all associated interfaces
    if (outputMixObject != NULL) {
        (*outputMixObject)->Destroy(outputMixObject);
        outputMixObject = NULL;
    }

    // destroy engine object, and invalidate all associated interfaces
    if (engineObject != NULL) {
        (*engineObject)->Destroy(engineObject);
        engineObject = NULL;
        engineEngine = NULL;
    }
    pthread_mutex_unlock(&audioEngineLock);
}

static TIME_VALUE_TYPE now_ms(void) {
    struct timespec res;
    clock_gettime(CLOCK_MONOTONIC, &res);
    return res.tv_sec * 1000 + res.tv_nsec / 1000000;
}