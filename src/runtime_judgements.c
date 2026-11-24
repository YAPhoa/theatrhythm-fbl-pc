/* Timing records: every note's judgement and how early or late it was hit, for the song just played,
 * and the best-scoring run of each song, difficulty and play style in <user>/judgements.dat. After the
 * judgement recorder of limen123's theatrhythm-fbl-pc fork (runtime_judgements.c there); the game
 * hooks that feed it are in runtime_judgement_hooks.c, and the overlay shows the result (F6). Nothing
 * here reads or writes the game's own saves.
 *
 * judgements.dat, little-endian: a 16-byte header (magic "JUDG", version 2, entry count, 0), then per
 * entry 16 bytes (music ID, difficulty u16, play style u16, score, note count) and its notes, 12 bytes
 * each (target ms, offset ms, judge u8, lane u8, flags u16). It is replaced through a temporary file,
 * so a crash while saving keeps the previous one. */
#include "runtime.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define JUDGEMENTS_MAGIC 0x4744554au /* "JUDG" */
#define JUDGEMENTS_VERSION 2u
enum { HEADER_SIZE=16, ENTRY_SIZE=16, NOTE_SIZE=12, MAX_ENTRIES=4096 };

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static char path[600];
static JudgementNote run[JUDGEMENT_NOTES];
static size_t run_count;
static uint32_t run_music;
static int run_difficulty, run_style, run_open;
static int32_t run_last; /* the latest timed note's target */

static uint32_t get32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static uint16_t get16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }
static void put32(unsigned char *p, uint32_t v) { memcpy(p,&v,4); }
static void put16(unsigned char *p, uint16_t v) { memcpy(p,&v,2); }
static void note_out(unsigned char *p, const JudgementNote *n) {
    put32(p,(uint32_t)n->target_ms); put32(p+4,(uint32_t)n->offset_ms);
    p[8]=n->judge; p[9]=n->lane; put16(p+10,n->flags);
}
static void note_in(JudgementNote *n, const unsigned char *p) {
    n->target_ms=(int32_t)get32(p); n->offset_ms=(int32_t)get32(p+4);
    n->judge=p[8]; n->lane=p[9]; n->flags=get16(p+10);
}

void runtime_judgements_configure(const char *file) {
    pthread_mutex_lock(&lock);
    snprintf(path,sizeof(path),"%s",file ? file : "");
    pthread_mutex_unlock(&lock);
}

void runtime_judgements_begin(uint32_t music, int difficulty, int style) {
    pthread_mutex_lock(&lock);
    run_music=music; run_difficulty=difficulty; run_style=style;
    run_count=0; run_open=music!=0; run_last=INT32_MIN;
    pthread_mutex_unlock(&lock);
}

void runtime_judgements_record(int32_t target_ms, int32_t offset_ms, int judge, int lane, unsigned flags) {
    pthread_mutex_lock(&lock);
    if (run_open && run_count<JUDGEMENT_NOTES && judge>=JUDGE_CRITICAL && judge<=JUDGE_MISS) {
        JudgementNote *n=&run[run_count++];
        n->target_ms=target_ms;
        n->offset_ms=judge==JUDGE_MISS || (flags&JUDGE_UNTIMED) ? 0 : offset_ms;
        n->judge=(uint8_t)judge; n->lane=(uint8_t)lane; n->flags=(uint16_t)flags;
        if (!(flags&JUDGE_UNTIMED) && target_ms>run_last) run_last=target_ms;
    }
    pthread_mutex_unlock(&lock);
}

int runtime_judgements_recording(void) {
    pthread_mutex_lock(&lock);
    const int open=run_open;
    pthread_mutex_unlock(&lock);
    return open;
}

int32_t runtime_judgements_last_target(void) {
    pthread_mutex_lock(&lock);
    const int32_t last=run_last;
    pthread_mutex_unlock(&lock);
    return last;
}

void runtime_judgements_summary(const JudgementNote *notes, size_t count, JudgementSummary *out) {
    memset(out,0,sizeof(*out));
    double sum=0;
    for (size_t i=0;i<count;++i) {
        const JudgementNote *n=&notes[i];
        if (n->judge<=JUDGE_MISS) ++out->judges[n->judge];
        if (n->judge<JUDGE_CRITICAL || n->judge>=JUDGE_MISS || (n->flags&JUDGE_UNTIMED)) continue;
        ++out->timed;
        sum+=n->offset_ms;
        if (n->offset_ms<0) ++out->early;
        else if (n->offset_ms>0) ++out->late;
    }
    out->notes=(uint32_t)count;
    if (!out->timed) return;
    const double mean=sum/out->timed;
    double spread=0;
    for (size_t i=0;i<count;++i) {
        const JudgementNote *n=&notes[i];
        if (n->judge<JUDGE_CRITICAL || n->judge>=JUDGE_MISS || (n->flags&JUDGE_UNTIMED)) continue;
        spread+=(n->offset_ms-mean)*(n->offset_ms-mean);
    }
    out->mean_ms=(float)mean;
    out->deviation_ms=out->timed>1 ? (float)sqrt(spread/(out->timed-1)) : 0.0f;
}

/* The whole file (caller frees), or NULL when it is missing, unreadable or not version 2. */
static unsigned char *read_file(size_t *size) {
    FILE *f=fopen(path,"rb");
    if (!f) return NULL;
    unsigned char *data=NULL;
    if (!fseek(f,0,SEEK_END)) {
        const long n=ftell(f);
        if (n>=HEADER_SIZE && n<(64L<<20) && !fseek(f,0,SEEK_SET) && (data=malloc((size_t)n)) &&
            fread(data,1,(size_t)n,f)==(size_t)n) *size=(size_t)n;
        else { free(data); data=NULL; }
    }
    fclose(f);
    if (data && (get32(data)!=JUDGEMENTS_MAGIC || get32(data+4)!=JUDGEMENTS_VERSION)) { free(data); data=NULL; }
    return data;
}

/* Calls `visit` for each well-formed entry of `data` (stopping at the first that runs past the end):
 * its header and its notes. */
static void each_entry(const unsigned char *data, size_t size,
                       void (*visit)(const unsigned char *entry, const unsigned char *notes, void *context), void *context) {
    const uint32_t count=get32(data+8);
    size_t at=HEADER_SIZE;
    for (uint32_t i=0;i<count && i<MAX_ENTRIES && at+ENTRY_SIZE<=size;++i) {
        const uint32_t notes=get32(data+at+12);
        if (notes>JUDGEMENT_NOTES || at+ENTRY_SIZE+(size_t)notes*NOTE_SIZE>size) break;
        visit(data+at,data+at+ENTRY_SIZE,context);
        at+=ENTRY_SIZE+(size_t)notes*NOTE_SIZE;
    }
}
static int same_key(const unsigned char *entry, uint32_t music, int difficulty, int style) {
    return get32(entry)==music && get16(entry+4)==(uint16_t)difficulty && get16(entry+6)==(uint16_t)style;
}

typedef struct { unsigned char *out; size_t used; uint32_t count; uint32_t music; int difficulty, style; } Rewrite;
static void copy_other(const unsigned char *entry, const unsigned char *notes, void *context) {
    Rewrite *w=context;
    if (same_key(entry,w->music,w->difficulty,w->style)) return;
    const size_t bytes=ENTRY_SIZE+(size_t)get32(entry+12)*NOTE_SIZE;
    memcpy(w->out+w->used,entry,ENTRY_SIZE);
    memcpy(w->out+w->used+ENTRY_SIZE,notes,bytes-ENTRY_SIZE);
    w->used+=bytes; ++w->count;
}
typedef struct { uint32_t music; int difficulty, style; uint32_t score; JudgementNote *notes; size_t cap, count; int found; } Find;
static void find_entry(const unsigned char *entry, const unsigned char *notes, void *context) {
    Find *f=context;
    if (f->found || !same_key(entry,f->music,f->difficulty,f->style)) return;
    f->found=1; f->score=get32(entry+8);
    f->count=get32(entry+12);
    for (size_t i=0;i<f->count && i<f->cap;++i) note_in(&f->notes[i],notes+i*NOTE_SIZE);
}

/* Replaces `path` with `data`: written to path.tmp first, then moved over it. */
static int write_file(const unsigned char *data, size_t size) {
    char tmp[620];
    snprintf(tmp,sizeof(tmp),"%s.tmp",path);
    FILE *f=fopen(tmp,"wb");
    if (!f) return -1;
    const int ok=fwrite(data,1,size,f)==size;
    if (fclose(f) || !ok) { remove(tmp); return -1; }
#ifdef _WIN32
    if (!MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) { remove(tmp); return -1; }
#else
    if (rename(tmp,path)) { remove(tmp); return -1; }
#endif
    return 0;
}

/* Saves the run as the best of its song, difficulty and style, over any earlier one (lock held). */
static int save_best(uint32_t score) {
    size_t size=0;
    unsigned char *old=read_file(&size);
    Rewrite w={malloc(HEADER_SIZE+(old ? size : 0)+ENTRY_SIZE+run_count*NOTE_SIZE),HEADER_SIZE,0,
               run_music,run_difficulty,run_style};
    if (!w.out) { free(old); return -1; }
    if (old) each_entry(old,size,copy_other,&w);
    free(old);
    unsigned char *entry=w.out+w.used;
    put32(entry,run_music); put16(entry+4,(uint16_t)run_difficulty); put16(entry+6,(uint16_t)run_style);
    put32(entry+8,score); put32(entry+12,(uint32_t)run_count);
    for (size_t i=0;i<run_count;++i) note_out(entry+ENTRY_SIZE+i*NOTE_SIZE,&run[i]);
    w.used+=ENTRY_SIZE+run_count*NOTE_SIZE; ++w.count;
    put32(w.out,JUDGEMENTS_MAGIC); put32(w.out+4,JUDGEMENTS_VERSION); put32(w.out+8,w.count); put32(w.out+12,0);
    const int r=write_file(w.out,w.used);
    free(w.out);
    return r;
}

size_t runtime_judgements_finish(uint32_t score, int new_best, JudgementNote *notes, size_t cap) {
    pthread_mutex_lock(&lock);
    if (!run_open) { pthread_mutex_unlock(&lock); return 0; }
    run_open=0;
    const size_t count=run_count;
    if (notes) memcpy(notes,run,(count<cap ? count : cap)*sizeof(*run));
    if (new_best && score && count && path[0]) {
        if (save_best(score)) printf("Timing records: could not save %s\n",path);
        else printf("Timing records: best run of song %u saved (%zu notes)\n",run_music,count);
    }
    pthread_mutex_unlock(&lock);
    return count;
}

size_t runtime_judgements_best(uint32_t music, int difficulty, int style, JudgementNote *notes, size_t cap, uint32_t *score) {
    pthread_mutex_lock(&lock);
    size_t size=0;
    unsigned char *data=path[0] ? read_file(&size) : NULL;
    Find f={music,difficulty,style,0,notes,cap,0,0};
    if (data) each_entry(data,size,find_entry,&f);
    free(data);
    pthread_mutex_unlock(&lock);
    if (score) *score=f.found ? f.score : 0;
    return f.found ? (f.count<cap ? f.count : cap) : 0;
}
