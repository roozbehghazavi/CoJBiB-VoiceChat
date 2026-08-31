// relay_music.c  -- see relay_music.h. MP3 -> 48k mono -> Opus 20ms -> callback.
#include "relay_music.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <opus/opus.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_ONLY_MP3
#include "minimp3.h"          // single-header MP3 decoder (vendored)

#define SR        48000       // must match client SAMPLE_RATE
#define CH        1           // mono
#define FRAME     960         // 20ms @ 48k  (must match client FRAME_SAMPLES)
#define FRAME_MS  20

// ---- playback state (guarded by mtx) ----
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static music_send_fn   g_send = 0;
static char  g_dir[512]  = "/data/music";
static char  g_cmd[512]  = "/data/music_cmd.txt";
static char  g_cur[512]  = "";      // currently-playing file path
static int   g_play      = 0;       // 1 = playing
static int   g_loopOne   = 0;       // repeat current file
static int   g_folder    = 0;       // folder playlist mode
static int   g_shuffle   = 0;       // random order within folder mode
static int   g_skip      = 0;       // request skip
static int   g_reload    = 0;       // request (re)open g_cur

static OpusEncoder* g_enc = 0;
static uint16_t g_seq = 0;
static uint32_t g_ts  = 0;

// ---- decoded-PCM ring for the current file ----
// We decode the whole file to memory (songs are a few MB of PCM; fine) then
// stream 960-sample frames from it on the 20ms clock.
static int16_t* g_pcm = 0;      // 48k mono samples
static size_t   g_pcmLen = 0;   // total samples
static size_t   g_pcmPos = 0;   // playback cursor

__attribute__((unused)) static void freePcm(void){ free(g_pcm); g_pcm=0; g_pcmLen=0; g_pcmPos=0; }

// Linear resampler: srcRate -> 48000, srcCh -> mono. Appends to g_pcm.
static int loadMp3(const char* path, int16_t** outPcm, size_t* outLen_){
    FILE* f=fopen(path,"rb"); if(!f){ printf("[music] cannot open %s\n",path); return 0; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    if(sz<=0){ fclose(f); return 0; }
    unsigned char* mp3=(unsigned char*)malloc(sz);
    if(!mp3){ fclose(f); return 0; }
    if(fread(mp3,1,sz,f)!=(size_t)sz){ fclose(f); free(mp3); return 0; }
    fclose(f);

    static mp3dec_t dec; mp3dec_init(&dec);
    // First pass: decode fully into a temp growable buffer at native rate/ch,
    // resampling per-frame to 48k mono.
    int16_t* out=0; size_t outLen=0, outCap=0;
    int off=0, srcRate=0, srcCh=0;
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    double phase=0.0;   // resample phase accumulator (src samples per out sample)
    // We resample incrementally: keep last sample for linear interp across frames.
    double ratio=1.0; int haveRatio=0; int16_t prev=0; int havePrev=0;

    while(off<sz){
        int n=mp3dec_decode_frame(&dec, mp3+off, sz-off, pcm, &info);
        off+=info.frame_bytes;
        if(info.frame_bytes==0) break;   // no more sync
        if(n==0) continue;               // header only
        srcRate=info.hz; srcCh=info.channels;
        if(!haveRatio){ ratio=(double)srcRate/SR; haveRatio=1; }

        // Downmix to mono into a small temp
        // (n = samples per channel)
        for(int i=0;i<n;i++){
            int s = (srcCh==2) ? ((pcm[2*i]+pcm[2*i+1])/2) : pcm[i];
            // linear resample from srcRate to 48000
            // emit output samples while phase < 1 step
            if(!havePrev){ prev=(int16_t)s; havePrev=1; phase=0.0; continue; }
            // advance: for each source sample, we may emit >=0 output samples
            phase += 1.0/ratio;         // 1/ratio output samples per src sample
            while(phase>=1.0){
                phase-=1.0;
                double frac=1.0-phase;  // interp position between prev and s
                int outS=(int)(prev+(s-prev)*frac);
                if(outLen>=outCap){ outCap=outCap?outCap*2:SR*8; out=(int16_t*)realloc(out,outCap*sizeof(int16_t)); if(!out){ free(mp3); return 0; } }
                out[outLen++]=(int16_t)outS;
            }
            prev=(int16_t)s;
        }
    }
    free(mp3);
    if(outLen==0){ free(out); return 0; }
    *outPcm=out; *outLen_=outLen;
    printf("[music] loaded %s : %zu samples (%.1fs)\n", path, outLen, (double)outLen/SR);
    return 1;
}

// Load a file and atomically swap it in as the current stream (under mtx).
static int loadAndSwap(const char* path){
    int16_t* np=0; size_t nl=0;
    if(!loadMp3(path,&np,&nl)) return 0;
    pthread_mutex_lock(&mtx);
    free(g_pcm); g_pcm=np; g_pcmLen=nl; g_pcmPos=0;
    pthread_mutex_unlock(&mtx);
    return 1;
}

// pick next *.mp3 in folder (alphabetical), after g_cur; wraps.
static int nextInFolder(char* out, int cap){
    DIR* d=opendir(g_dir); if(!d) return 0;
    char names[256][256]; int cnt=0;
    struct dirent* e;
    while((e=readdir(d)) && cnt<256){
        const char* nm=e->d_name; size_t L=strlen(nm);
        if(L>4 && (strcasecmp(nm+L-4,".mp3")==0)) { strncpy(names[cnt],nm,255); names[cnt][255]=0; cnt++; }
    }
    closedir(d);
    if(cnt==0) return 0;
    // sort (simple)
    for(int i=0;i<cnt;i++)for(int j=i+1;j<cnt;j++) if(strcmp(names[i],names[j])>0){ char t[256]; strcpy(t,names[i]); strcpy(names[i],names[j]); strcpy(names[j],t); }
    // find current basename
    const char* curb=strrchr(g_cur,'/'); curb=curb?curb+1:g_cur;
    int idx=-1; for(int i=0;i<cnt;i++) if(strcmp(names[i],curb)==0){ idx=i; break; }
    int ni;
    if(g_shuffle && cnt>1){
        // pick a random track different from the current one
        do { ni = rand()%cnt; } while(ni==idx);
    } else {
        ni=(idx+1)%cnt;                 // sequential (alphabetical) next, wraps
    }
    snprintf(out,cap,"%s/%s",g_dir,names[ni]);
    return 1;
}

static void resolvePath(const char* arg, char* out, int cap){
    if(arg[0]=='/' ) { snprintf(out,cap,"%s",arg); }
    else snprintf(out,cap,"%s/%s",g_dir,arg);
}

// ---- command file watcher ----
static time_t g_cmdMtime=0; static off_t g_cmdSize=-1;
static void pollCmd(void){
    struct stat st; if(stat(g_cmd,&st)!=0) return;
    // Detect changes by (mtime, size). mtime alone has 1s resolution and can
    // miss a second command written in the same second; size catches those.
    if(st.st_mtime==g_cmdMtime && st.st_size==g_cmdSize) return;
    g_cmdMtime=st.st_mtime; g_cmdSize=st.st_size;
    FILE* f=fopen(g_cmd,"r"); if(!f) return;
    char line[600]; if(!fgets(line,sizeof(line),f)){ fclose(f); return; } fclose(f);
    char* p=strchr(line,'\n'); if(p)*p=0; p=strchr(line,'\r'); if(p)*p=0;
    char verb[32]={0}, arg[512]={0};
    sscanf(line," %31s %511[^\n]", verb, arg);
    pthread_mutex_lock(&mtx);
    if(strcasecmp(verb,"stop")==0){ g_play=0; }
    else if(strcasecmp(verb,"skip")==0){ g_skip=1; }
    else if(strcasecmp(verb,"folder")==0){ g_folder=1; g_shuffle=0; g_loopOne=0; g_play=1; g_cur[0]=0; g_skip=1; }
    else if(strcasecmp(verb,"shuffle")==0){ g_folder=1; g_shuffle=1; g_loopOne=0; g_play=1; g_cur[0]=0; g_skip=1; }
    else if(strcasecmp(verb,"loop")==0){
        // "loop <file>" loops that file; bare "loop" loops whatever is current.
        if(arg[0]) resolvePath(arg,g_cur,sizeof(g_cur));
        g_loopOne=1; g_folder=0; g_play=1;
        if(arg[0]) g_reload=1;                 // only reload if a new file was named
    }
    else if(strcasecmp(verb,"play")==0 && arg[0]){ resolvePath(arg,g_cur,sizeof(g_cur)); g_loopOne=0; g_folder=0; g_play=1; g_reload=1; }
    printf("[music] cmd: %s %s\n", verb, arg);
    pthread_mutex_unlock(&mtx);
}

static void* musicThread(void* _){
    (void)_;
    int err=0; g_enc=opus_encoder_create(SR,CH,OPUS_APPLICATION_AUDIO,&err);
    if(err!=OPUS_OK){ printf("[music] opus encoder create failed\n"); return 0; }
    opus_encoder_ctl(g_enc, OPUS_SET_BITRATE(64000));      // music: a bit richer than voice
    opus_encoder_ctl(g_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(g_enc, OPUS_SET_VBR(1));

    struct timespec next; clock_gettime(CLOCK_MONOTONIC,&next);
    time_t lastPoll=0;
    int16_t frame[FRAME];

    for(;;){
        // pace to 20ms
        next.tv_nsec += FRAME_MS*1000000L;
        if(next.tv_nsec>=1000000000L){ next.tv_nsec-=1000000000L; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, 0);

        time_t nowt=time(0);
        if(nowt!=lastPoll){ pollCmd(); lastPoll=nowt; }

        pthread_mutex_lock(&mtx);
        int play=g_play, reload=g_reload, skip=g_skip, folder=g_folder, loopOne=g_loopOne;
        g_reload=0; g_skip=0;
        char curCopy[512]; strncpy(curCopy,g_cur,sizeof(curCopy)); curCopy[511]=0;
        pthread_mutex_unlock(&mtx);

        if(!play){ continue; }

        // (re)load if requested, or advance folder on skip / end
        if(reload || skip){
            if(folder){ char path[512]; if(nextInFolder(path,sizeof(path))){ pthread_mutex_lock(&mtx); strncpy(g_cur,path,sizeof(g_cur)); g_cur[511]=0; pthread_mutex_unlock(&mtx); loadAndSwap(path); } }
            else if(curCopy[0]){ loadAndSwap(curCopy); }
        }
        // Safety: we're supposed to play but nothing is loaded (e.g. bare "loop"
        // or "play" after a prior track ended and freed the buffer). Load g_cur.
        int needLoad=0;
        pthread_mutex_lock(&mtx); needLoad = (g_pcm==0 && g_cur[0]!=0); pthread_mutex_unlock(&mtx);
        if(needLoad && !folder && curCopy[0]){ loadAndSwap(curCopy); }

        // ---- pull one 960-sample frame under the mutex (never race a reload) ----
        int haveFrame=0;
        pthread_mutex_lock(&mtx);
        if(g_pcm && g_pcmLen>0){
            if(g_pcmPos>=g_pcmLen){
                // reached end
                if(loopOne){ g_pcmPos=0; }
                else if(folder){ g_pcmPos=g_pcmLen; /* trigger advance below */ }
                else { g_play=0; free(g_pcm); g_pcm=0; g_pcmLen=0; g_pcmPos=0; }
            }
            if(g_pcm && g_pcmPos<g_pcmLen){
                size_t remain=g_pcmLen-g_pcmPos;
                int nCopy = remain>=FRAME ? FRAME : (int)remain;
                memcpy(frame, g_pcm+g_pcmPos, nCopy*sizeof(int16_t));
                if(nCopy<FRAME) memset(frame+nCopy,0,(FRAME-nCopy)*sizeof(int16_t));
                g_pcmPos += nCopy;
                haveFrame=1;
            }
        }
        int needAdvance = (folder && g_pcm && g_pcmPos>=g_pcmLen && !haveFrame);
        pthread_mutex_unlock(&mtx);

        // folder mode: at end of a track, advance to the next (outside the lock)
        if(needAdvance){
            char path[512];
            if(nextInFolder(path,sizeof(path))){ pthread_mutex_lock(&mtx); strncpy(g_cur,path,sizeof(g_cur)); g_cur[511]=0; pthread_mutex_unlock(&mtx); loadAndSwap(path); }
            continue;
        }
        if(!haveFrame) continue;

        // encode + emit
        unsigned char opus[1024];
        int len=opus_encode(g_enc, frame, FRAME, opus, sizeof(opus));
        if(len>0 && g_send){ g_send(opus, len, g_seq, g_ts); }
        g_seq++; g_ts += FRAME;
    }
    return 0;
}

void music_start(music_send_fn send_cb, const char* musicDir, const char* cmdFile){
    srand((unsigned)time(0));     // seed shuffle RNG
    g_send=send_cb;
    const char* e;
    if(musicDir && *musicDir) { strncpy(g_dir,musicDir,sizeof(g_dir)-1); }
    else if((e=getenv("MUSIC_DIR")) && *e) { strncpy(g_dir,e,sizeof(g_dir)-1); }
    if(cmdFile && *cmdFile) { strncpy(g_cmd,cmdFile,sizeof(g_cmd)-1); }
    else if((e=getenv("MUSIC_CMD")) && *e) { strncpy(g_cmd,e,sizeof(g_cmd)-1); }
    printf("[music] dir=%s cmd=%s\n", g_dir, g_cmd);
    pthread_t th; pthread_create(&th,0,musicThread,0); pthread_detach(th);
}