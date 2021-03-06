// 包含头文件
#include <jni.h>
#include <pthread.h>
#include <semaphore.h>
#include "adev.h"

using namespace android;

// for jni
extern    JavaVM* g_jvm;
JNIEXPORT JNIEnv* get_jni_env(void);

// 内部常量定义
#define DEF_ADEV_BUF_NUM  5
#define DEF_ADEV_BUF_LEN  8192

#define SW_VOLUME_MINDB  -30
#define SW_VOLUME_MAXDB  +12

// 内部类型定义
typedef struct
{
    int64_t   *ppts;
    AUDIOBUF  *pWaveHdr;
    uint8_t   *pWaveBuf;
    int        bufnum;
    int        buflen;
    int        head;
    int        tail;
    sem_t      semr;
    sem_t      semw;
    #define ADEV_CLOSE (1 << 0)
    #define ADEV_PAUSE (1 << 1)
    int        status;
    pthread_t  thread;
    int64_t   *apts;

    // software volume
    int        vol_scaler[256];
    int        vol_zerodb;
    int        vol_curvol;

    // for jni
    jobject    jobj_at;
    jmethodID  jmid_at_init ;
    jmethodID  jmid_at_close;
    jmethodID  jmid_at_play ;
    jmethodID  jmid_at_pause;
    jmethodID  jmid_at_write;
    jbyteArray audio_buffer;
} ADEV_CONTEXT;

static void* audio_render_thread_proc(void *param)
{
    JNIEnv     *env = get_jni_env();
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)param;

    while (!(c->status & ADEV_CLOSE))
    {
        if (c->status & ADEV_PAUSE) {
            usleep(10*1000);
            continue;
        }

        sem_wait(&c->semr);
        if (c->pWaveHdr[c->head].size) {
            env->CallVoidMethod(c->jobj_at, c->jmid_at_write, c->audio_buffer, c->head * c->buflen, c->pWaveHdr[c->head].size);
            c->pWaveHdr[c->head].size = 0;
        }
        if (c->apts) *c->apts = c->ppts[c->head];
        if (++c->head == c->bufnum) c->head = 0;
        sem_post(&c->semw);
    }

    // close audiotrack
    env->CallVoidMethod(c->jobj_at, c->jmid_at_close);

    // need call DetachCurrentThread
    g_jvm->DetachCurrentThread();
    return NULL;
}

static int init_software_volmue_scaler(int *scaler, int mindb, int maxdb)
{
    double a[256];
    double b[256];
    int    z, i;

    for (i=0; i<256; i++) {
        a[i]      = mindb + (maxdb - mindb) * i / 256.0;
        b[i]      = pow(10.0, a[i] / 20.0);
        scaler[i] = (int)(0x10000 * b[i]);
    }

    z = -mindb * 256 / (maxdb - mindb);
    z = z > 0   ? z : 0  ;
    z = z < 255 ? z : 255;
    scaler[0] = 0;        // mute
    scaler[z] = 0x10000;  // 0db
    return z;
}

// 接口函数实现
void* adev_create(int type, int bufnum, int buflen)
{
    JNIEnv       *env  = get_jni_env();
    ADEV_CONTEXT *ctxt = NULL;
    int           i;

    // allocate adev context
    ctxt = (ADEV_CONTEXT*)calloc(1, sizeof(ADEV_CONTEXT));
    if (!ctxt) {
        av_log(NULL, AV_LOG_ERROR, "failed to allocate adev context !\n");
        exit(0);
    }

    bufnum         = bufnum ? bufnum : DEF_ADEV_BUF_NUM;
    buflen         = buflen ? buflen : DEF_ADEV_BUF_LEN;
    ctxt->bufnum   = bufnum;
    ctxt->buflen   = buflen;
    ctxt->head     = 0;
    ctxt->tail     = 0;
    ctxt->ppts     = (int64_t *)calloc(bufnum, sizeof(int64_t));
    ctxt->pWaveHdr = (AUDIOBUF*)calloc(bufnum, sizeof(AUDIOBUF));

    // new buffer
    jbyteArray local_audio_buffer = env->NewByteArray(bufnum * buflen);
    ctxt->audio_buffer = (jbyteArray)env->NewGlobalRef(local_audio_buffer);
    ctxt->pWaveBuf     = (uint8_t  *)env->GetByteArrayElements(ctxt->audio_buffer, 0);
    env->DeleteLocalRef(local_audio_buffer);

    // init wavebuf
    for (i=0; i<bufnum; i++) {
        ctxt->pWaveHdr[i].data = (int16_t*)(ctxt->pWaveBuf + i * buflen);
        ctxt->pWaveHdr[i].size = buflen;
    }

    jclass jcls         = env->FindClass("android/media/AudioTrack");
    ctxt->jmid_at_init  = env->GetMethodID(jcls, "<init>" , "(IIIIII)V");
    ctxt->jmid_at_close = env->GetMethodID(jcls, "release", "()V");
    ctxt->jmid_at_play  = env->GetMethodID(jcls, "play"   , "()V");
    ctxt->jmid_at_pause = env->GetMethodID(jcls, "pause"  , "()V");
    ctxt->jmid_at_write = env->GetMethodID(jcls, "write"  , "([BII)I");

    // new AudioRecord
    #define STREAM_MUSIC        3
    #define ENCODING_PCM_16BIT  2
    #define CHANNEL_STEREO      3
    #define MODE_STREAM         1
    jobject at_obj = env->NewObject(jcls, ctxt->jmid_at_init,
        STREAM_MUSIC, 44100, CHANNEL_STEREO, ENCODING_PCM_16BIT, ctxt->buflen * 2, MODE_STREAM);
    ctxt->jobj_at  = env->NewGlobalRef(at_obj);
    env->DeleteLocalRef(at_obj);

    // start audiotrack
    env->CallVoidMethod(ctxt->jobj_at, ctxt->jmid_at_play);

    // init software volume scaler
    ctxt->vol_zerodb = init_software_volmue_scaler(ctxt->vol_scaler, SW_VOLUME_MINDB, SW_VOLUME_MAXDB);
    ctxt->vol_curvol = ctxt->vol_zerodb;

    // create semaphore
    sem_init(&ctxt->semr, 0, 0     );
    sem_init(&ctxt->semw, 0, bufnum);

    // create audio rendering thread
    pthread_create(&ctxt->thread, NULL, audio_render_thread_proc, ctxt);

    return ctxt;
}

void adev_destroy(void *ctxt)
{
    JNIEnv *env = get_jni_env();
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;

    // make audio rendering thread safely exit
    c->status = ADEV_CLOSE;
    sem_post(&c->semr);
    pthread_join(c->thread, NULL);

    // close semaphore
    sem_destroy(&c->semr);
    sem_destroy(&c->semw);

    // free buffers
    free(c->ppts);
    free(c->pWaveHdr);

    // for jni
    env->ReleaseByteArrayElements(c->audio_buffer, (jbyte*)c->pWaveBuf, 0);
    env->DeleteGlobalRef(c->audio_buffer);
    env->DeleteGlobalRef(c->jobj_at     );

    // free adev
    free(c);
}

void adev_request(void *ctxt, AUDIOBUF **ppab)
{
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    sem_wait(&c->semw);
    *ppab = (AUDIOBUF*)&c->pWaveHdr[c->tail];
    (*ppab)->size = c->buflen;
}

void adev_post(void *ctxt, int64_t pts)
{
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    c->ppts[c->tail] = pts;

    //++ software volume scale
    int      multiplier = c->vol_scaler[c->vol_curvol];
    int16_t *buf        = c->pWaveHdr[c->tail].data;
    int      n          = c->pWaveHdr[c->tail].size / sizeof(int16_t);
    if (multiplier > 0x10000) {
        int64_t v;
        while (n--) {
            v = ((int64_t)*buf * multiplier) >> 16;
            v = v < 0x7fff ? v : 0x7fff;
            v = v >-0x7fff ? v :-0x7fff;
            *buf++ = (int16_t)v;
        }
    }
    else if (multiplier < 0x10000) {
        while (n--) {
            *buf = ((int32_t)*buf * multiplier) >> 16; buf++;
        }
    }
    //-- software volume scale

    if (++c->tail == c->bufnum) c->tail = 0;
    sem_post(&c->semr);
}

void adev_pause(void *ctxt, int pause)
{
    JNIEnv *env = get_jni_env();
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    if (pause) {
        c->status |=  ADEV_PAUSE;
        env->CallVoidMethod(c->jobj_at, c->jmid_at_pause);
    }
    else {
        c->status &= ~ADEV_PAUSE;
        env->CallVoidMethod(c->jobj_at, c->jmid_at_play );
    }
}

void adev_reset(void *ctxt)
{
    JNIEnv *env = get_jni_env();
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    while (0 == sem_trywait(&c->semr)) {
        sem_post(&c->semw);
    }
    c->head   = 0;
    c->tail   = 0;
    c->status = 0;
}

void adev_syncapts(void *ctxt, int64_t *apts)
{
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    c->apts = apts;
    if (c->apts) {
        *c->apts = -1;
    }
}

void adev_curdata(void *ctxt, void **buf, int *len)
{
    if (!ctxt) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;
    if (buf) *buf = NULL;
    if (len) *len = 0;
}

void adev_setparam(void *ctxt, int id, void *param)
{
    if (!ctxt || !param) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;

    switch (id) {
    case PARAM_AUDIO_VOLUME:
        {
            int vol = *(int*)param;
            vol += c->vol_zerodb;
            vol  = vol > 0   ? vol : 0  ;
            vol  = vol < 255 ? vol : 255;
            c->vol_curvol = vol;
        }
        break;
    }
}

void adev_getparam(void *ctxt, int id, void *param)
{
    if (!ctxt || !param) return;
    ADEV_CONTEXT *c = (ADEV_CONTEXT*)ctxt;

    switch (id) {
    case PARAM_AUDIO_VOLUME:
        *(int*)param = c->vol_curvol - c->vol_zerodb;
        break;
    }
}

void adev_android_initjni(void *ctxt, JNIEnv *env, jobject obj)
{
    // do nothing
}

