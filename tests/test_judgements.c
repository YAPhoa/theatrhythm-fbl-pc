/* Timing records (runtime_judgements.c): a run's notes and summary, the best run kept per song,
 * difficulty and style through judgements.dat, and a damaged or foreign file read as no records. */
#undef NDEBUG /* the checks are asserts; release builds define NDEBUG */
#include "../src/runtime_judgements.c"
#include <assert.h>
#include <unistd.h>

static char file[300];

static size_t play(uint32_t music, int difficulty, int style, int32_t base, uint32_t score, int best) {
    runtime_judgements_begin(music,difficulty,style);
    runtime_judgements_record(1000,base-10,JUDGE_CRITICAL,0,JUDGE_RAINBOW);
    runtime_judgements_record(2000,base+10,JUDGE_GREAT,1,0);
    runtime_judgements_record(3000,base,JUDGE_GOOD,2,0);
    runtime_judgements_record(4000,123,JUDGE_MISS,3,0);    /* a Miss keeps no offset */
    runtime_judgements_record(500,250,JUDGE_BAD,0,JUDGE_UNTIMED); /* judged with another note */
    JudgementNote notes[8];
    return runtime_judgements_finish(score,best,notes,8);
}

int main(void) {
    const char *tmp=getenv("TMP");
    if (!tmp) tmp=getenv("TEMP");
    if (!tmp) tmp="/tmp";
    snprintf(file,sizeof(file),"%s/theatrhythm-judgements-test-%d.dat",tmp,(int)getpid());
    remove(file);
    runtime_judgements_configure(file);

    /* nothing recorded outside a run, and music 0 opens none */
    runtime_judgements_record(1,1,JUDGE_GREAT,0,0);
    assert(!runtime_judgements_recording());
    runtime_judgements_begin(0,0,0);
    assert(!runtime_judgements_recording());
    assert(runtime_judgements_finish(1,1,NULL,0)==0);

    /* a run: its notes, summary and latest target; finishing twice ends it once */
    runtime_judgements_begin(160000700,2,0);
    assert(runtime_judgements_recording() && runtime_judgements_last_target()==INT32_MIN);
    runtime_judgements_record(1000,-20,JUDGE_CRITICAL,0,JUDGE_RAINBOW);
    runtime_judgements_record(2000,10,JUDGE_GREAT,1,0);
    runtime_judgements_record(3000,40,JUDGE_GOOD,2,0);
    runtime_judgements_record(4000,99,JUDGE_MISS,3,0);
    runtime_judgements_record(9000,300,JUDGE_BAD,0,JUDGE_UNTIMED);
    runtime_judgements_record(5000,1,7,0,0); /* not a judge */
    assert(runtime_judgements_last_target()==4000); /* the untimed note's 9000 does not count */
    JudgementNote notes[8];
    assert(runtime_judgements_finish(900000,0,notes,8)==5);
    assert(runtime_judgements_finish(900000,0,notes,8)==0);
    assert(notes[3].offset_ms==0 && notes[4].offset_ms==0 && notes[0].flags==JUDGE_RAINBOW);
    JudgementSummary s;
    runtime_judgements_summary(notes,5,&s);
    assert(s.notes==5 && s.timed==3 && s.early==1 && s.late==2);
    assert(s.judges[JUDGE_CRITICAL]==1 && s.judges[JUDGE_MISS]==1 && s.judges[JUDGE_BAD]==1);
    assert(fabsf(s.mean_ms-10.0f)<0.01f && fabsf(s.deviation_ms-30.0f)<0.01f);
    /* not a best: no file */
    FILE *f=fopen(file,"rb");
    assert(!f);

    /* bests: kept per song, difficulty and style; a later best replaces only its own */
    assert(play(160000700,2,0,5,800000,1)==5);
    assert(play(160000700,2,1,-5,700000,1)==5);
    assert(play(160000701,3,0,0,600000,1)==5);
    assert(play(160000700,2,0,20,850000,1)==5);
    assert(play(160000700,2,0,99,1,0)==5); /* not a best: changes nothing */
    JudgementNote best[8];
    uint32_t score=0;
    assert(runtime_judgements_best(160000700,2,0,best,8,&score)==5 && score==850000 && best[1].offset_ms==30);
    assert(runtime_judgements_best(160000700,2,1,best,8,&score)==5 && score==700000 && best[1].offset_ms==5);
    assert(runtime_judgements_best(160000701,3,0,best,8,&score)==5 && score==600000);
    assert(runtime_judgements_best(160000700,1,0,best,8,&score)==0 && score==0);
    assert(runtime_judgements_best(160000700,2,0,best,2,&score)==2); /* at most cap */
    f=fopen(file,"rb");
    assert(f);
    unsigned char header[16];
    assert(fread(header,1,16,f)==16);
    assert(!memcmp(header,"JUDG",4) && get32(header+4)==2 && get32(header+8)==3);
    fclose(f);
    char tmpfile[320];
    snprintf(tmpfile,sizeof(tmpfile),"%s.tmp",file);
    assert(!fopen(tmpfile,"rb")); /* replaced, not left behind */

    /* a cut-off file keeps its whole entries; a foreign one reads as none and is replaced */
    f=fopen(file,"rb");
    unsigned char data[4096];
    const size_t size=fread(data,1,sizeof(data),f);
    fclose(f);
    f=fopen(file,"wb"); fwrite(data,1,size-7,f); fclose(f);
    assert(runtime_judgements_best(160000700,2,0,best,8,&score)==0); /* the last entry, cut */
    assert(runtime_judgements_best(160000701,3,0,best,8,&score)==5);
    f=fopen(file,"wb"); fputs("not a records file at all",f); fclose(f);
    assert(runtime_judgements_best(160000700,2,0,best,8,&score)==0);
    assert(play(160000702,0,2,0,100,1)==5);
    assert(runtime_judgements_best(160000702,0,2,best,8,&score)==5 && score==100);

    /* the same song in several entries (one with no notes): a best replaces them all with one, and
     * the buffer holds the run once whatever the file repeats */
    {
        unsigned char dup[16+4*16+3*12]={0}, *p=dup+16;
        memcpy(dup,"JUDG",4); put32(dup+4,2); put32(dup+8,4);
        for (int i=0;i<4;++i) {
            put32(p,160000704); put16(p+4,1); put16(p+6,0); put32(p+8,(uint32_t)(10+i));
            const uint32_t n=i==1 ? 3 : 0;
            put32(p+12,n); p+=16+n*12;
        }
        f=fopen(file,"wb"); fwrite(dup,1,(size_t)(p-dup),f); fclose(f);
        assert(runtime_judgements_best(160000704,1,0,best,8,&score)==0 && score==10); /* the first, empty */
        runtime_judgements_begin(160000704,1,0);
        for (int i=0;i<JUDGEMENT_NOTES;++i) runtime_judgements_record(i,1,JUDGE_GREAT,0,0);
        assert(runtime_judgements_finish(5000,1,NULL,0)==JUDGEMENT_NOTES);
        static JudgementNote many[JUDGEMENT_NOTES];
        assert(runtime_judgements_best(160000704,1,0,many,JUDGEMENT_NOTES,&score)==JUDGEMENT_NOTES && score==5000);
        f=fopen(file,"rb"); assert(fread(header,1,16,f)==16); fclose(f);
        assert(get32(header+8)==1);
        /* an entry claiming more notes than the file holds ends the read there */
        put32(dup+16+12,1000);
        f=fopen(file,"wb"); fwrite(dup,1,(size_t)(p-dup),f); fclose(f);
        assert(runtime_judgements_best(160000704,1,0,best,8,&score)==0);
    }

    /* no path: records nothing to disk */
    runtime_judgements_configure("");
    assert(play(160000703,0,0,0,100,1)==5);
    assert(runtime_judgements_best(160000703,0,0,best,8,&score)==0);
    remove(file);
    puts("judgements: ok");
    return 0;
}
