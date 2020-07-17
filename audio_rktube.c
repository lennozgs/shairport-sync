/*
 * rokid audio bridge
 */

#if defined(__ANDROID__)

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "common.h"
#include "audio.h"

#define LOG_TAG "shairport_rktube"
#include "cutils/log.h"

static int fd = -1;

char *pipename = NULL;

/**
 * OPENSLES
 */
#include "SLES/OpenSLES.h"
#include "SLES/OpenSLES_Android.h"
#include <string.h>
#include <assert.h>

// engine interfaces
static SLObjectItf engineObject = NULL;
static SLEngineItf engineEngine;

// output mix interfaces
static SLObjectItf outputMixObject = NULL;
static SLEnvironmentalReverbItf outputMixEnvironmentalReverb = NULL;

// buffer queue player interfaces
static SLObjectItf bqPlayerObject = NULL;
static SLPlayItf bqPlayerPlay;
static SLAndroidSimpleBufferQueueItf bqPlayerBufferQueue;
static SLEffectSendItf bqPlayerEffectSend;
static SLMuteSoloItf bqPlayerMuteSolo;
static SLVolumeItf bqPlayerVolume;
static SLmilliHertz bqPlayerSampleRate = 0;
static int bqPlayerBufSize = 0;
static short *resampleBuf = NULL;

static pthread_mutex_t  audioEngineLock = PTHREAD_MUTEX_INITIALIZER;

static const SLEnvironmentalReverbSettings reverbSettings =
    SL_I3DL2_ENVIRONMENT_PRESET_STONECORRIDOR;

// file descriptor player interfaces
static SLObjectItf fdPlayerObject = NULL;
static SLPlayItf fdPlayerPlay;
static SLSeekItf fdPlayerSeek;
static SLMuteSoloItf fdPlayerMuteSolo;
static SLVolumeItf fdPlayerVolume;

// pointer and size of the next player buffer to enqueue, and number of remaining buffers
static short *nextBuffer;
static unsigned nextSize;
static int nextCount;



// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
    //debug(1, "callback");
    //int ignore = non_blocking_write(fd, nextBuffer, nextSize);

    /*
    assert(bq == bqPlayerBufferQueue);
    SLresult result;
    if (--nextCount > 0 && NULL != nextBuffer && 0 != nextSize) {
        result = (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, nextBuffer, nextSize);
        (void)result;
    }
    */
}

void createEngine()
{
    SLresult result;

    // create engine
    result = slCreateEngine(&engineObject, 0, NULL, 0, NULL, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the engine
    result = (*engineObject)->Realize(engineObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the engine interface, which is needed in order to create other objects
    result = (*engineObject)->GetInterface(engineObject, SL_IID_ENGINE, &engineEngine);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // create output mix, with environmental reverb specified as a non-required interface
    const SLInterfaceID ids[1] = {SL_IID_ENVIRONMENTALREVERB};
    const SLboolean req[1] = {SL_BOOLEAN_FALSE};
    result = (*engineEngine)->CreateOutputMix(engineEngine, &outputMixObject, 1, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the output mix
    result = (*outputMixObject)->Realize(outputMixObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the environmental reverb interface
    // this could fail if the environmental reverb effect is not available,
    // either because the feature is not present, excessive CPU load, or
    // the required MODIFY_AUDIO_SETTINGS permission was not requested and granted
    result = (*outputMixObject)->GetInterface(outputMixObject, SL_IID_ENVIRONMENTALREVERB,
            &outputMixEnvironmentalReverb);
    if (SL_RESULT_SUCCESS == result) {
        result = (*outputMixEnvironmentalReverb)->SetEnvironmentalReverbProperties(
                outputMixEnvironmentalReverb, &reverbSettings);
        (void)result;
    }
    // ignore unsuccessful result codes for environmental reverb, as it is optional for this example

}

void createBufferQueueAudioPlayer(int sampleRate, int bufSize)
{
    SLresult result;
    //if (sampleRate >= 0 && bufSize >= 0 ) {
        bqPlayerSampleRate = sampleRate * 1000;
        /*
         * device native buffer size is another factor to minimize audio latency, not used in this
         * sample: we only play one giant buffer here
         */
        bqPlayerBufSize = bufSize;
    //}

    // configure audio source
    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 4};
    SLDataFormat_PCM format_pcm = {
        SL_DATAFORMAT_PCM, 
        2, 
        SL_SAMPLINGRATE_44_1,
        SL_PCMSAMPLEFORMAT_FIXED_16, 
        SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, 
        SL_BYTEORDER_LITTLEENDIAN
    };
    /*
     * Enable Fast Audio when possible:  once we set the same rate to be the native, fast audio path
     * will be triggered
     */
    if(bqPlayerSampleRate) {
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
            bqPlayerSampleRate? 2 : 3, ids, req);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // realize the player
    result = (*bqPlayerObject)->Realize(bqPlayerObject, SL_BOOLEAN_FALSE);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the play interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_PLAY, &bqPlayerPlay);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // get the buffer queue interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_BUFFERQUEUE,
            &bqPlayerBufferQueue);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

    // register callback on the buffer queue
    result = (*bqPlayerBufferQueue)->RegisterCallback(bqPlayerBufferQueue, bqPlayerCallback, NULL);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

#if 0
    // get the effect send interface
    bqPlayerEffectSend = NULL;
    if( 0 == bqPlayerSampleRate) {
        result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_EFFECTSEND,
                                                 &bqPlayerEffectSend);
        assert(SL_RESULT_SUCCESS == result);
        (void)result;
    }
#endif

#if 0   // mute/solo is not supported for sources that are known to be mono, as this is
    // get the mute/solo interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_MUTESOLO, &bqPlayerMuteSolo);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

#if 1
    // get the volume interface
    result = (*bqPlayerObject)->GetInterface(bqPlayerObject, SL_IID_VOLUME, &bqPlayerVolume);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;
#endif

    // set the player's state to playing
    result = (*bqPlayerPlay)->SetPlayState(bqPlayerPlay, SL_PLAYSTATE_PLAYING);
    assert(SL_RESULT_SUCCESS == result);
    (void)result;

}

static void parameters(audio_parameters *info) {
    SLmillibel maxVolume = (*bqPlayerVolume)->GetMaxVolumeLevel(bqPlayerVolume, &maxVolume);
    info->maximum_volume_dB = maxVolume;
    info->minimum_volume_dB = SL_MILLIBEL_MIN;
}

static void volume(double vol){
    debug(1, "volume(%f)", vol);
    (*bqPlayerVolume)->SetVolumeLevel(bqPlayerVolume, (SLmillibel)vol);
}

static SLVolumeItf getVolume()
{
    return bqPlayerVolume;
}

/**
 * \OPENSLES
 */

static void start(int sample_rate) {
  // this will leave fd as -1 if a reader hasn't been attached
#if 0
  fd = open(pipename, O_WRONLY | O_NONBLOCK);
  ALOGI("rktube audio output started at Fs=%d Hz\n", sample_rate);
#endif

#if 0
  createBufferQueueAudioPlayer(sample_rate, 99225);
#endif
}

static prevSamples = 0;
static void play(short buf[], int samples) {

//#if 1
//    nextBuffer = (short*)buf;
//    nextSize  = samples * 4;

    /*
    if(samples != prevSamples){
        ALOGI("SAMPLES %d\n", samples);
        prevSamples = samples;
    }
    */
    (*bqPlayerBufferQueue)->Enqueue(bqPlayerBufferQueue, (short *)buf, samples*4);
    //nextCount = 2;
//#endif

#if 0
  // if the file is not open, try to open it.
  if (fd == -1) {
     fd = open(pipename, O_WRONLY | O_NONBLOCK); 
  }
  // if it's got a reader, write to it.
  if (fd != -1) {
    int ignore = non_blocking_write(fd, buf, samples * 4);
  } 
#endif

}

static void stop(void) { 
  ALOGI("rktube audio stopped\n"); 
}

static int init(int argc, char **argv) {
  debug(1, "rktube init");
  const char *str;
  int value;
  
  config.audio_backend_buffer_desired_length = 44100; // one second.
  //config.audio_backend_latency_offset = 44100;
  config.audio_backend_latency_offset = 40000;

  if (config.cfg != NULL) {
    /* Get the Output Pipename. */
    const char *str;
    if (config_lookup_string(config.cfg, "rktube.name", &str)) {
      pipename = (char *)str;
    }
 
    if ((pipename) && (strcasecmp(pipename, "STDOUT") == 0))
      die("Can't use \"pipe\" backend for STDOUT. Use the \"stdout\" backend instead.");

    /* Get the desired buffer size setting. */
    if (config_lookup_int(config.cfg, "rktube.audio_backend_buffer_desired_length", &value)) {
      if ((value < 0) || (value > 132300))
        die("Invalid rktube pipe audio backend buffer desired length \"%d\". It should be between 0 and 132300, default is 44100",
            value);
      else
        config.audio_backend_buffer_desired_length = value;
    }

    /* Get the latency offset. */
    if (config_lookup_int(config.cfg, "rktube.audio_backend_latency_offset", &value)) {
      if ((value < -66150) || (value > 66150))
        die("Invalid rktube pipe audio backend buffer latency offset \"%d\". It should be between -66150 and +66150, default is 0",
            value);
      else
        config.audio_backend_latency_offset = value;
    }
  }

/*  
  if ((pipename == NULL) && (argc != 1))
    die("bad or missing argument(s) to rktube pipe");

  if (argc == 1)
    pipename = strdup(argv[0]);
  
  // here, create the pipe
  if (mkfifo(pipename, 0644) && errno != EEXIST)
    die("Could not create output pipe \"%s\"", pipename);

  debug(1, "Pipename is \"%s\"", pipename);
  */

  createEngine();

  createBufferQueueAudioPlayer(44100, 4100);

  return 0;
}

static void deinit(void) {
   if (fd > 0)
    close(fd);
}

static void help(void) { 
  printf("    rktube module using pipe,so takes 1 argument: the name of the FIFO to write to.\n");
}

audio_output audio_rktube = {.name = "rktube",
                            .help = &help,
                            .init = &init,
                            .deinit = &deinit,
                            .start = &start,
                            .stop = &stop,
                            .flush = NULL,
                            .delay = NULL,
                            .play = &play,
                            .volume = &volume,
                            .parameters = &parameters,
                            .mute = NULL};

#endif // defined(__ANDROID)__
