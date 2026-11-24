/* The game code that feeds the timing records (runtime_judgements.c), after the hooks of limen123's
 * theatrhythm-fbl-pc fork. Every site is found by its C# name and checked by its bytes
 * (runtime_game_code.c), so 1.00 and 1.04 both work and another version keeps no records.
 * - MusicStagePlaySetting::setConfigInfo(this, mode): the play setting of the song about to start;
 *   read (through its getters) once the song's first note is judged.
 * - ContextPlaydata::FixedTriggerJudge(this, index, judge, time, ...): a note judged, at `time` ms
 *   against the trigger's own time. The other two overloads call this one, and it calls itself for
 *   notes linked to the one judged (a hold's start judged with its end): those count as untimed. A
 *   run starts at the first note after a setting, or when the notes go back in time (a retry from
 *   the pause menu). Practice, auto play, replays, tutorials and timing adjustment record nothing.
 * - MusicResultTitlePartsMain::get_needUpdateBestScore(this): the results screen asks whether to
 *   show New Record. The hook calls the game's own (a copy of its moved entry), then ends the run:
 *   saved as the best when the score beats the one before the song (BeforePlayData.HighScore) and
 *   the game saves the play, and handed to the overlay (F6) with a notice. */
#include "runtime.h"
#include "gpu/theatrhythm_gpu.h"
#include <stdio.h>
#include <string.h>

_Static_assert(sizeof(JudgementNote)==sizeof(TheatrhythmTimingNote),"the overlay reads the notes as they are");

typedef struct { uint64_t rdi, rsi, rdx, rcx, r8, r9, rax, rdx_out; } HookRegs;
typedef int (ABI *HookFn)(HookRegs *regs);

enum { H_CONFIG, H_JUDGE, H_RESULT, C_JUDGE_LIST, C_JUDGE_TRIGGER, C_MUSIC_ID, C_DIFFICULTY, C_STYLE,
       C_PLAY_TYPE, C_REPLAY, C_TUTORIAL, C_TIMING_ADJUST, C_TITLE, C_ID_VALUE, C_HIGH_SCORE, SITES };
static const GameSite sites[SITES]={
    {"Game.Scene.MusicStage.MusicStagePlaySetting::setConfigInfo", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 53 50 80 3d ?? ?? ?? ?? 00 41 89 f6 49 89 ff 75 13"}},
    {"Game.Rhythm.ContextPlaydata::FixedTriggerJudge(5)", 0, 0, 0,
     {"55 48 89 e5 41 57 41 56 | 41 55 41 54 53 48 83 ec 28 80 3d ?? ?? ?? ?? 00"}},
    /* up to `mov rdi,[rip+X]`, X the play data's class (its statics' first field: the play, whose
     * score is at +0x50 and "not saved" flag at +0xbc) */
    {"Game.Scene.MusicResult.MusicResultTitlePartsMain::get_needUpdateBestScore", 0, 0, 0,
     {"55 48 89 e5 41 56 53 | 80 3d ?? ?? ?? ?? 00 49 89 fe 75 1f 48 8d 3d ?? ?? ?? ?? e8 ?? ?? ?? ?? 48 8d 3d "
      "?? ?? ?? ?? e8 ?? ?? ?? ?? c6 05 ?? ?? ?? ?? 01 48 8b 3d ?? ?? ?? ?? 83 bf e0 00 00 00 00"}},
    /* FixedTriggerJudge's own reads, in 1.00's registers and in 1.04's: the triggers list (+0x10:
     * count +0x18, items +0x10 from +0x20), and a trigger's judged flag (+0x4c) and time (+0x14) */
    {"Game.Rhythm.ContextPlaydata::FixedTriggerJudge(5)", 1, 0, 0,
     {"49 8b 46 10 39 58 18 0f 86 ?? ?? ?? ?? 45 84 e4 74 ?? 48 8b 40 10 48 63 cb 41 0f b6 dc 4c 8b 7c c8 20",
      "49 8b 45 10 39 58 18 0f 86 ?? ?? ?? ?? 45 84 ff 74 ?? 48 8b 40 10 48 63 cb 4c 8b 74 c8 20"}},
    {"Game.Rhythm.ContextPlaydata::FixedTriggerJudge(5)", 1, 0, 0,
     {"41 89 47 48 45 88 67 4c 4c 89 e8 8b 4d d4 48 8b 55 c8 45 8b 6f 14",
      "41 89 46 48 45 88 7e 4c 44 89 7d d0 45 8b 7e 14"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_MusicID", 0, 0, 0, {"48 8b 47 18 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_MSDifficulty", 0, 0, 0, {"8b 47 50 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_MSPlayStyle", 0, 0, 0, {"8b 47 54 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_MSPlayType", 0, 0, 0, {"8b 87 08 01 00 00 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_ReplayMode", 0, 0, 0, {"8a 87 0c 01 00 00 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_TutorialType", 0, 0, 0, {"8b 87 10 01 00 00 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_TimingAdjustmentMode", 0, 0, 0, {"8a 87 15 01 00 00 c3"}},
    {"Game.Scene.MusicStage.MusicStagePlaySetting::get_MusicTitle", 0, 0, 0, {"48 8b 47 30 c3"}},
    {"Gummi.Foundation.ID::get_Value", 0, 0, 0, {"8b 47 10 c3"}},
    {"Game.PlayData.BeforePlayData::get_HighScore", 0, 0, 0, {"55 48 89 e5 80 3d ?? ?? ?? ?? 00"}},
};
enum { PLAY_CLASS_LOAD=50 }; /* get_needUpdateBestScore: offset of `mov rdi,[rip+X]` (7 bytes) */
static unsigned char *module;
static GameCode code_at[SITES];
static unsigned char *result_original; /* get_needUpdateBestScore's moved entry, then a jump back */
#define CODE(site) ((void *)(module+code_at[site].offset))
#define GETTER(type, site, object) (((type (ABI *)(void *, void *))CODE(site))(object,NULL))

static void *setting;       /* the latest MusicStagePlaySetting configured (the game keeps it) */
static uint32_t music;      /* the song of the run being recorded (0: none) */
static int difficulty, style;
static char title[128];
static JudgementNote shown[JUDGEMENT_NOTES], best[JUDGEMENT_NOTES];

static uint64_t load64(const void *p) { uint64_t v; memcpy(&v,p,8); return v; }
static int32_t load32(const void *p) { int32_t v; memcpy(&v,p,4); return v; }
static const char *class_name(const void *object) { /* Il2CppClass: image, gc_desc, name */
    const char *klass=(const char *)load64(object);
    return klass ? (const char *)load64(klass+0x10) : "";
}
/* A System.String (length +0x10, UTF-16 +0x14) as UTF-8 into `out`. */
static void string_utf8(const unsigned char *s, char *out, size_t size) {
    size_t n=0;
    const int32_t length=s ? load32(s+0x10) : 0;
    for (int32_t i=0;i<length && i<512;++i) {
        uint16_t c; memcpy(&c,s+0x14+2*i,2);
        unsigned char b[3]; size_t k;
        if (c<0x80) { b[0]=(unsigned char)c; k=1; }
        else if (c<0x800) { b[0]=(unsigned char)(0xc0|c>>6); b[1]=(unsigned char)(0x80|(c&0x3f)); k=2; }
        else if (c>=0xd800 && c<0xe000) { b[0]='?'; k=1; } /* surrogates: not in the fonts anyway */
        else { b[0]=(unsigned char)(0xe0|c>>12); b[1]=(unsigned char)(0x80|((c>>6)&0x3f)); b[2]=(unsigned char)(0x80|(c&0x3f)); k=3; }
        if (n+k>=size) break;
        memcpy(out+n,b,k); n+=k;
    }
    out[n]=0;
}

/* The run starts: the setting's song, unless it is not a scored play (practice 1, auto 2, a replay,
 * a tutorial or the timing adjustment). */
static void begin_run(void) {
    music=0;
    if (setting && !strcmp(class_name(setting),"MusicStagePlaySetting")) {
        const int scored=GETTER(int32_t,C_PLAY_TYPE,setting)==0 && !(GETTER(uint8_t,C_REPLAY,setting)&1) &&
                         GETTER(int32_t,C_TUTORIAL,setting)==-1 && !(GETTER(uint8_t,C_TIMING_ADJUST,setting)&1);
        void *id=GETTER(void *,C_MUSIC_ID,setting);
        if (scored && id) {
            music=(uint32_t)GETTER(int32_t,C_ID_VALUE,id);
            difficulty=GETTER(int32_t,C_DIFFICULTY,setting);
            style=GETTER(int32_t,C_STYLE,setting);
            string_utf8(GETTER(const unsigned char *,C_TITLE,setting),title,sizeof(title));
        }
    }
    runtime_judgements_begin(music,difficulty,style);
    if (music) printf("Timing records: recording song %u (difficulty %d, style %d)\n",music,difficulty,style);
}

/* setConfigInfo(this, mode): the next song plays with this setting. */
static ABI int config(HookRegs *r) {
    setting=(void *)r->rdi;
    runtime_judgements_begin(0,0,0); /* the run before is over without results */
    music=0;
    return 0;
}

/* FixedTriggerJudge(this, index, judge, time, ...): records a note the game has not judged yet. The
 * game's judges: 1 Rainbow Critical, 2 Critical, 3 Great, 4 Good, 5 Bad, 6 Miss. */
static ABI int judge(HookRegs *r) {
    const unsigned char *context=(const unsigned char *)r->rdi;
    const uint32_t index=(uint32_t)r->rsi, game_judge=(uint32_t)r->rdx&0xff;
    if (!context || game_judge<1 || game_judge>6) return 0;
    const unsigned char *list=(const unsigned char *)load64(context+0x10);
    if (!list || index>=(uint32_t)load32(list+0x18)) return 0;
    const unsigned char *items=(const unsigned char *)load64(list+0x10);
    const unsigned char *trigger=items ? (const unsigned char *)load64(items+0x20+8*(uint64_t)index) : NULL;
    if (!trigger || trigger[0x4c]) return 0;
    const int32_t target=load32(trigger+0x14), lane=load32(trigger+0x1c);
    /* the return address, above the stub's HookRegs: inside this function for a linked note */
    const uint64_t back=load64((const unsigned char *)r+72);
    const unsigned char *function=CODE(H_JUDGE);
    const int linked=back>(uint64_t)(uintptr_t)function && back<(uint64_t)(uintptr_t)function+0x1000;
    if (!linked && (!runtime_judgements_recording() || (int64_t)target<(int64_t)runtime_judgements_last_target()-5000)) begin_run();
    if (!music) return 0;
    const int judges[7]={0,JUDGE_CRITICAL,JUDGE_CRITICAL,JUDGE_GREAT,JUDGE_GOOD,JUDGE_BAD,JUDGE_MISS};
    int32_t offset=(int32_t)r->rcx-target;
    if (offset>300) offset=300;
    if (offset<-300) offset=-300;
    runtime_judgements_record(target,offset,judges[game_judge],lane,(game_judge==1 ? JUDGE_RAINBOW : 0)|(linked ? JUDGE_UNTIMED : 0));
    return 0;
}

/* get_needUpdateBestScore(this): the game's answer, after which the run ends (once). */
static ABI int result(HookRegs *r) {
    const uint8_t answer=((uint8_t (ABI *)(void *))result_original)((void *)r->rdi);
    r->rax=answer;
    if (!runtime_judgements_recording() || !music) return 1;
    /* the play: [statics of the class get_needUpdateBestScore loads] */
    const unsigned char *load=(const unsigned char *)CODE(H_RESULT)+PLAY_CLASS_LOAD;
    const unsigned char *slot=load+7+load32(load+3);
    const unsigned char *klass=(const unsigned char *)load64(slot);
    const unsigned char *statics=klass ? (const unsigned char *)load64(klass+0xb8) : NULL;
    const unsigned char *play=statics ? (const unsigned char *)load64(statics) : NULL;
    const int32_t score=play ? load32(play+0x50) : 0, before=((int32_t (ABI *)(void))CODE(C_HIGH_SCORE))();
    const int saved=play && !play[0xbc], new_best=saved && (answer&1) && score>0 && score>before;
    const size_t count=runtime_judgements_finish(score>0 ? (uint32_t)score : 0,new_best,shown,JUDGEMENT_NOTES);
    uint32_t best_score=0;
    const size_t best_count=runtime_judgements_best(music,difficulty,style,best,JUDGEMENT_NOTES,&best_score);
    const TheatrhythmTiming timing={title,music,difficulty,style,score>0 ? (uint32_t)score : 0,new_best,
                                    (const TheatrhythmTimingNote *)shown,(uint32_t)count,
                                    best_score,(const TheatrhythmTimingNote *)best,(uint32_t)best_count};
    theatrhythm_gpu_timing(&timing);
    JudgementSummary s;
    runtime_judgements_summary(shown,count,&s);
    char notice[160];
    if (s.timed) {
        snprintf(notice,sizeof(notice),"Timing: %+.1f ms on average, %.1f ms spread (F6: graph)",s.mean_ms,s.deviation_ms);
        theatrhythm_gpu_notice(notice,6000);
    }
    printf("Timing records: song %u ended, %zu notes, mean %+.1f ms, spread %.1f ms%s\n",music,count,s.mean_ms,
           s.deviation_ms,new_best ? ", new best" : "");
    music=0;
    return 1;
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
/* mov [rsp+disp], reg (store) or mov reg, [rsp+disp] (load); reg in x86 numbering. */
static unsigned char *rsp_slot(unsigned char *p, int store, int reg, uint8_t disp) {
    const unsigned char bytes[]={(unsigned char)(0x48|(reg>=8 ? 4 : 0)),store ? 0x89 : 0x8b,(unsigned char)(0x44|(reg&7)<<3),0x24,disp};
    return emit(p,bytes,sizeof bytes);
}
#define STUB_SIZE 144 /* the most emit_stub writes (16 moved bytes at most) */
#define ORIGINAL_SIZE 30 /* emit_original: 16 moved bytes at most, then a jump */
/* The stub for `fn` at `p`, as the exit prompt's: the argument registers saved in a HookRegs on the
 * stack (the call 16-byte aligned: rsp is 8 mod 16 at the entry), fn called, then its result
 * returned, or the registers restored, the moved prologue run and the function entered after it. */
static unsigned char *emit_stub(unsigned char *p, HookFn fn, unsigned char *function, unsigned moved) {
    static const int args[6]={7,6,2,1,8,9}; /* rdi rsi rdx rcx r8 r9 */
    const int32_t frame=72;
    p=emit(p,(const unsigned char[]){0x48,0x81,0xec},3); p=emit(p,&frame,4); /* sub rsp,frame */
    for (int i=0;i<6;++i) p=rsp_slot(p,1,args[i],(uint8_t)(8*i));
    p=emit(p,(const unsigned char[]){0x48,0x89,0xe7},3);                       /* mov rdi,rsp */
    const uint64_t call=(uint64_t)(uintptr_t)fn;
    p=emit(p,(const unsigned char[]){0x48,0xb8},2); p=emit(p,&call,8);        /* mov rax,fn */
    p=emit(p,(const unsigned char[]){0xff,0xd0,0x85,0xc0},4);                 /* call rax; test eax,eax */
    unsigned char *jnz=p; p=emit(p,(const unsigned char[]){0x75,0},2);        /* jnz ret */
    for (int i=0;i<6;++i) p=rsp_slot(p,0,args[i],(uint8_t)(8*i));
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    p=emit(p,function,moved);                                                  /* the moved bytes */
    const uint64_t back=(uint64_t)(uintptr_t)(function+moved);
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6); p=emit(p,&back,8); /* jmp [rip]: back */
    jnz[1]=(unsigned char)(p-(jnz+2));
    p=rsp_slot(p,0,0,48); p=rsp_slot(p,0,2,56);                               /* rax, rdx = results */
    p=emit(p,(const unsigned char[]){0x48,0x81,0xc4},3); p=emit(p,&frame,4); /* add rsp,frame */
    return emit(p,(const unsigned char[]){0xc3},1);                           /* ret */
}
/* The game function as it was, for host code to call: its moved bytes, then a jump past them. */
static unsigned char *emit_original(unsigned char *p, unsigned char *function, unsigned moved) {
    p=emit(p,function,moved);
    const uint64_t back=(uint64_t)(uintptr_t)(function+moved);
    p=emit(p,(const unsigned char[]){0xff,0x25,0,0,0,0},6);
    return emit(p,&back,8);
}

int runtime_judgement_hooks_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    module=image+module_base;
    char path[600];
    snprintf(path,sizeof(path),"%s/judgements.dat",runtime_file_user_dir());
    runtime_judgements_configure(path);
    if (runtime_game_code_find(sites,SITES,code_at,"Timing records") || size<3*STUB_SIZE+ORIGINAL_SIZE) {
        puts("Timing records: off in this game version");
        return -1;
    }
    static const struct { int site; HookFn fn; } hooks[]={{H_CONFIG,config},{H_JUDGE,judge},{H_RESULT,result}};
    unsigned char *p=stubs;
    result_original=p;
    p=emit_original(p,CODE(H_RESULT),code_at[H_RESULT].moved);
    for (size_t i=0;i<sizeof(hooks)/sizeof(*hooks);++i) {
        unsigned char *function=CODE(hooks[i].site);
        const unsigned moved=code_at[hooks[i].site].moved;
        const int64_t rel=(int64_t)(p-(function+5));
        if (moved<5 || moved>16 || rel!=(int32_t)rel) { puts("Timing records: off (hook out of reach, runner bug)"); return -1; }
        unsigned char *stub=p;
        p=emit_stub(p,hooks[i].fn,function,moved);
        const int32_t rel32=(int32_t)(stub-(function+5));
        function[0]=0xe9; memcpy(function+1,&rel32,4);
        memset(function+5,0x90,moved-5u); /* never run: the stub jumps past them */
    }
    puts("Timing records: on (F6 shows the last song's timing)");
    return 0;
}
