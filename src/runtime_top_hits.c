/* Today's Top Hits as an entry of Music Stages' sort list, next to All and Favorites. The game marks
 * each day's songs with MusicRecord.PickUp and shows only that mark, so the entry is one more
 * SortDataSpecial, of a SortType the game does not have (3, its enum's Max). Most code compares types
 * or handles only 0 and 2; what would take 3 for a category changes:
 *  - the list's creation adds the entry after All (MusicSelect's list: Multi Battle shares it, the
 *    Music Player has its own item class);
 *  - filterItem shows the marked songs;
 *  - filterList shows the entry when there are any, outside Multi Battle (no daily bonus there);
 *  - the separators go: a heading for each of a few songs says little;
 *  - RefreshBadges (both) shows the mark and no count of new songs;
 *  - the entry's label and the list's title take All's route with the entry's own term;
 *  - the title's count is the marked songs'.
 * Every site's bytes are checked first; another game version keeps its list. */
#include "runtime.h"
#include <stdio.h>
#include <string.h>

#define TOP_HITS 3 /* SortType */

#define OBJECT_NEW 0x4545a0
#define LIST_ITEM_SORT_CLASS 0x609b0d0
#define LIST_ITEM_SORT_CTOR 0x15f3690 /* ListItemSort(SortType) */
#define UI_LIST_ADD_ITEM 0x2938ba0
#define UI_LIST_ITEM_REFRESH_VIEW 0x293fd50
#define SET_TERM 0x392f1d0            /* LocalizedTextBase<T>::SetTerm */
#define FILTER_SEPARATOR 0x160f630

#define CREATE_ADD_ALL 0xfd9c7b       /* InitializeCreation's MoveNext: call AddListItem, All's */
#define FILTER_ITEM_OTHER 0x160fedd   /* filterItem: cmp r12d,2 was just made; jne false */
#define FILTER_ITEM_FALSE 0x160ffba
#define FILTER_ITEM_RETURN 0x160ffc0  /* with al */
#define FILTER_LIST_ALL 0xfde521      /* filterList: cmp eax,1; sete al */
#define FILTER_MAIN_SEPARATOR 0x160f605 /* filterMain: pop rbx; pop r14; pop rbp; jmp filterSeparator */
#define BADGES_FAVORITE 0xfdb371      /* RefreshBadges(int, int): cmp eax,2; jne categories */
#define BADGES_FAVORITE_THEN 0xfdb376
#define BADGES_CATEGORIES 0xfdb3c3
#define BADGES_NEXT 0xfdb2d0
#define NEW_BADGE_FAVORITE 0xfdb8fb   /* RefreshBadges(): cmp eax,2; jne categories */
#define NEW_BADGE_FAVORITE_THEN 0xfdb900
#define NEW_BADGE_CATEGORIES 0xfdb96a
#define NEW_BADGE_NEXT 0xfdb870
#define VIEW_ALL 0x15f53ad            /* ListItemSortView::OnSetView: cmp eax,1; jne */
#define VIEW_ALL_TERM 0x15f5428       /* mov rdi,rbx; mov rsi,[rax]; then the tail other branches share */
#define TITLE_ALL 0x1604156           /* SetSortCategoryTitle: cmp eax,1; jne */
#define TITLE_SET_TERM 0x16043d4      /* call SetTerm */
#define TITLE_DIFFICULTY 0x16043d9    /* cmp r12d,3; jne all's count */
#define TITLE_DIFFICULTY_SUM 0x16043e3
#define TITLE_ALL_COUNT 0x16044bd
#define TITLE_COUNT_DONE 0x1604324    /* with r13d */

/* Field offsets: MusicSelectSortListPartsMain._listData, CountDataList.AllCountData, CountData.PickUp,
 * ListItemSort.SortViewParameter and its pick-up and new marks, the music list's current sort data. */
#define SORT_LIST_COUNTS 0xd8
#define COUNTS_ALL 0x60
#define COUNT_PICK_UP 0x18
#define ITEM_VIEW_PARAMETER 0x20
#define VIEW_PICK_UP 0x20
#define VIEW_NEW_COUNT 0x24
#define MUSIC_LIST_SORT_DATA 0xc8

static unsigned char *module;

static void *read_ptr(const void *at) { void *v; memcpy(&v,at,sizeof v); return v; }

/* A sort data's SortType, by its virtual getter: a category's has no field for it. */
static int sort_type(const void *data) {
    const unsigned char *klass=read_ptr(data);
    return ((int (ABI *)(const void *, void *))read_ptr(klass+0x178))(data,read_ptr(klass+0x180));
}

/* UIList::AddListItem for All's entry, then the same for this one. */
static void ABI add_sort_item(void *list, void *item, void *method) {
    ((void (ABI *)(void *, void *, void *))(module+UI_LIST_ADD_ITEM))(list,item,method);
    void *klass=read_ptr(module+LIST_ITEM_SORT_CLASS);
    if (read_ptr(item)!=klass) return; /* the Music Player's */
    void *entry=((void *(ABI *)(void *))(module+OBJECT_NEW))(klass);
    ((void (ABI *)(void *, int32_t, void *))(module+LIST_ITEM_SORT_CTOR))(entry,TOP_HITS,NULL);
    ((void (ABI *)(void *, void *, void *))(module+UI_LIST_ADD_ITEM))(list,entry,method);
}

/* filterSeparator(this, separator): none in this entry's list. */
static unsigned char ABI filter_separator(unsigned char *self, void *separator, void *method) {
    if (sort_type(read_ptr(self+MUSIC_LIST_SORT_DATA))==TOP_HITS) return 0;
    return ((unsigned char (ABI *)(void *, void *, void *))(module+FILTER_SEPARATOR))(self,separator,method);
}

/* What RefreshBadges does for All, with the marked songs and no count of new ones. */
static void ABI refresh_badge(unsigned char *item, const unsigned char *sort_list) {
    unsigned char *view=read_ptr(item+ITEM_VIEW_PARAMETER);
    const unsigned char *count=read_ptr((const unsigned char *)read_ptr(sort_list+SORT_LIST_COUNTS)+COUNTS_ALL);
    int32_t marked, new_count;
    memcpy(&marked,count+COUNT_PICK_UP,4);
    memcpy(&new_count,view+VIEW_NEW_COUNT,4);
    const unsigned char pick_up=marked>0;
    if (view[VIEW_PICK_UP]==pick_up && !new_count) return;
    view[VIEW_PICK_UP]=pick_up;
    memset(view+VIEW_NEW_COUNT,0,4);
    ((void (ABI *)(void *, void *))(module+UI_LIST_ITEM_REFRESH_VIEW))(item,NULL);
}

/* The entry's label: a guest System.String (length at +0x10, UTF-16 from +0x14), of the class of All's
 * term `like`, in low memory outside the game's heap, so it is never freed. */
static void *ABI term_of_entry(const unsigned char *like) {
    static const char term[]=TOP_HITS_TERM;
    static unsigned char *string;
    if (string) return string;
    const int32_t length=(int32_t)sizeof term-1;
    unsigned char *s=runtime_low_alloc(0x16+2*(size_t)length);
    if (!s) return (void *)like;
    memset(s,0,0x16+2*(size_t)length);
    memcpy(s,like,8);
    memcpy(s+0x10,&length,4);
    for (int32_t i=0;i<length;++i) { const uint16_t c=(uint8_t)term[i]; memcpy(s+0x14+2*i,&c,2); }
    return string=s;
}

static unsigned char *emit(unsigned char *p, const void *bytes, size_t size) { memcpy(p,bytes,size); return p+size; }
#define EMIT(p,...) emit(p,(const unsigned char[]){__VA_ARGS__},sizeof((const unsigned char[]){__VA_ARGS__}))
static unsigned char *emit_address(unsigned char *p, const void *function) { /* mov rax,function */
    const uint64_t address=(uint64_t)(uintptr_t)function;
    p=EMIT(p,0x48,0xb8);
    return emit(p,&address,8);
}
static unsigned char *emit_call(unsigned char *p, const void *function) { /* mov rax,function; call rax */
    p=emit_address(p,function);
    return EMIT(p,0xff,0xd0);
}
static unsigned char *emit_jump(unsigned char *p, const void *target) { /* jmp [rip] */
    const uint64_t address=(uint64_t)(uintptr_t)target;
    p=EMIT(p,0xff,0x25,0,0,0,0);
    return emit(p,&address,8);
}
#define JUMP_SIZE 14
/* `length` bytes at `site` become a jmp or call (opcode) to `target`; the rest are never run. */
static void write_branch(unsigned char *site, size_t length, unsigned char opcode, const unsigned char *target) {
    const int32_t rel=(int32_t)(target-(site+5));
    site[0]=opcode; memcpy(site+1,&rel,4);
    memset(site+5,0x90,length-5);
}
static int calls(uint64_t site, uint64_t target) { /* call rel32 */
    int32_t rel;
    memcpy(&rel,module+site+1,4);
    return module[site]==0xe8 && site+5+(int64_t)rel==target;
}
#define SITE_IS(offset,...) (!memcmp(module+(offset),(const unsigned char[]){__VA_ARGS__},sizeof((const unsigned char[]){__VA_ARGS__})))

int runtime_top_hits_install(unsigned char *image, uint64_t module_base, unsigned char *stubs, size_t size) {
    module=image+module_base;
    const int same=size>=640 && calls(CREATE_ADD_ALL,UI_LIST_ADD_ITEM) && calls(TITLE_SET_TERM,SET_TERM) &&
        SITE_IS(FILTER_ITEM_OTHER,0x0f,0x85,0xd7,0,0,0) &&
        SITE_IS(FILTER_LIST_ALL,0x83,0xf8,0x01,0x0f,0x94,0xc0) &&
        SITE_IS(FILTER_MAIN_SEPARATOR,0x5b,0x41,0x5e,0x5d,0xeb,0x25) &&
        SITE_IS(BADGES_FAVORITE,0x83,0xf8,0x02,0x75,0x4d) &&
        SITE_IS(NEW_BADGE_FAVORITE,0x83,0xf8,0x02,0x75,0x6a) &&
        SITE_IS(VIEW_ALL,0x83,0xf8,0x01,0x0f,0x85,0x8b,0,0,0) &&
        SITE_IS(VIEW_ALL_TERM,0x48,0x89,0xdf,0x48,0x8b,0x30) &&
        SITE_IS(TITLE_ALL,0x83,0xf8,0x01,0x75,0x16) &&
        SITE_IS(TITLE_DIFFICULTY,0x41,0x83,0xfc,0x03,0x0f,0x85,0xda,0,0,0);
    if (!same) {
        printf("Top Hits: game code differs; the sort list stays as it is\n");
        return -1;
    }
    unsigned char *p=stubs, *stub;

    /* The list's creation. */
    stub=p;
    p=emit_address(p,(const void *)add_sort_item);
    p=EMIT(p,0xff,0xe0);                                        /* jmp rax */
    write_branch(module+CREATE_ADD_ALL,5,0xe8,stub);

    /* cmp r12d,3; jne false; mov rax,[rbx+0x48] (BackupMusicRecord); mov al,[rax+0x13] (PickUp); return */
    stub=p;
    p=EMIT(p,0x41,0x83,0xfc,TOP_HITS, 0x75,4+3+JUMP_SIZE, 0x48,0x8b,0x43,0x48, 0x8a,0x40,0x13);
    p=emit_jump(p,module+FILTER_ITEM_RETURN);
    p=emit_jump(p,module+FILTER_ITEM_FALSE);
    int32_t rel=(int32_t)(stub-(module+FILTER_ITEM_OTHER+6));
    memcpy(module+FILTER_ITEM_OTHER+2,&rel,4);

    /* cmp eax,3; je entry; cmp eax,1; sete al; back
     * entry: al=_sceneType!=MultiBattle && AllCountData.PickUp>0; back */
    stub=p;
    p=EMIT(p,0x83,0xf8,TOP_HITS, 0x74,6+JUMP_SIZE, 0x83,0xf8,0x01,0x0f,0x94,0xc0);
    p=emit_jump(p,module+FILTER_LIST_ALL+6);
    p=EMIT(p,0x31,0xc0,                                         /* xor eax,eax */
             0x41,0x83,0xbe,0x20,0x01,0,0,0x01, 0x74,7+4+4+3,   /* cmp dword [r14+0x120],1; je back */
             0x49,0x8b,0x86,SORT_LIST_COUNTS,0,0,0,             /* mov rax,[r14+_listData] */
             0x48,0x8b,0x40,COUNTS_ALL,                         /* mov rax,[rax+AllCountData] */
             0x83,0x78,COUNT_PICK_UP,0,                         /* cmp dword [rax+PickUp],0 */
             0x0f,0x9f,0xc0);                                   /* setg al */
    p=emit_jump(p,module+FILTER_LIST_ALL+6);
    write_branch(module+FILTER_LIST_ALL,6,0xe9,stub);

    /* filterMain's tail call to filterSeparator. */
    stub=p;
    p=EMIT(p,0x5b,0x41,0x5e,0x5d);
    p=emit_address(p,(const void *)filter_separator);
    p=EMIT(p,0xff,0xe0);                                        /* jmp rax */
    write_branch(module+FILTER_MAIN_SEPARATOR,6,0xe9,stub);

    /* The two RefreshBadges: rbx is the item in both, r12 or r14 the sort list. */
    stub=p;
    p=EMIT(p,0x83,0xf8,0x02, 0x75,JUMP_SIZE);                   /* cmp eax,2; jne next */
    p=emit_jump(p,module+BADGES_FAVORITE_THEN);
    p=EMIT(p,0x83,0xf8,TOP_HITS, 0x75,3+3+12+JUMP_SIZE);        /* next: cmp eax,3; jne other */
    p=EMIT(p,0x48,0x89,0xdf, 0x4c,0x89,0xe6);                   /* mov rdi,rbx; mov rsi,r12 */
    p=emit_call(p,(const void *)refresh_badge);
    p=emit_jump(p,module+BADGES_NEXT);
    p=emit_jump(p,module+BADGES_CATEGORIES);                    /* other */
    write_branch(module+BADGES_FAVORITE,5,0xe9,stub);

    stub=p;
    p=EMIT(p,0x83,0xf8,0x02, 0x75,JUMP_SIZE);                   /* cmp eax,2; jne next */
    p=emit_jump(p,module+NEW_BADGE_FAVORITE_THEN);
    p=EMIT(p,0x83,0xf8,TOP_HITS, 0x75,3+3+12+JUMP_SIZE);        /* next: cmp eax,3; jne other */
    p=EMIT(p,0x48,0x89,0xdf, 0x4c,0x89,0xf6);                   /* mov rdi,rbx; mov rsi,r14 */
    p=emit_call(p,(const void *)refresh_badge);
    p=emit_jump(p,module+NEW_BADGE_NEXT);
    p=emit_jump(p,module+NEW_BADGE_CATEGORIES);                 /* other */
    write_branch(module+NEW_BADGE_FAVORITE,5,0xe9,stub);

    /* The label: test al,1; nop; je. All (1) and this (3) are the odd types. */
    memcpy(module+VIEW_ALL,(const unsigned char[]){0xa8,0x01,0x90,0x0f,0x84},5);
    stub=p;
    p=EMIT(p,0x48,0x89,0xdf, 0x48,0x8b,0x30);                   /* mov rdi,rbx; mov rsi,[rax]: All's term */
    p=EMIT(p,0x49,0x8b,0x4f,0x18, 0x83,0x79,0x40,TOP_HITS);     /* mov rcx,[r15+0x18]; cmp dword [rcx+0x40],3 */
    p=EMIT(p,0x75,2+3+12+3+2);                                  /* jne keep */
    p=EMIT(p,0x57,0x52, 0x48,0x89,0xf7);                        /* push rdi; push rdx; mov rdi,rsi */
    p=emit_call(p,(const void *)term_of_entry);
    p=EMIT(p,0x48,0x89,0xc6, 0x5a,0x5f);                        /* mov rsi,rax; pop rdx; pop rdi */
    p=emit_jump(p,module+VIEW_ALL_TERM+6);                      /* keep */
    write_branch(module+VIEW_ALL_TERM,6,0xe9,stub);

    /* The title: the same test, then SetTerm with this entry's term. */
    memcpy(module+TITLE_ALL,(const unsigned char[]){0xa8,0x01,0x90,0x74},4);
    stub=p;
    p=EMIT(p,0x49,0x8b,0x8f,MUSIC_LIST_SORT_DATA,0,0,0, 0x83,0x79,0x40,TOP_HITS); /* mov rcx,[r15+0xc8]; cmp dword [rcx+0x40],3 */
    p=EMIT(p,0x75,3+3+12+3+3);                                  /* jne keep */
    p=EMIT(p,0x57,0x52,0x52, 0x48,0x89,0xf7);                   /* push rdi; push rdx twice (alignment); mov rdi,rsi */
    p=emit_call(p,(const void *)term_of_entry);
    p=EMIT(p,0x48,0x89,0xc6, 0x5a,0x5a,0x5f);                   /* mov rsi,rax; pop rdx twice; pop rdi */
    p=emit_jump(p,module+SET_TERM);                             /* keep */
    write_branch(module+TITLE_SET_TERM,5,0xe8,stub);

    /* The title's count: this entry's is the marked songs', in any sort category. */
    stub=p;
    p=EMIT(p,0x49,0x8b,0x87,MUSIC_LIST_SORT_DATA,0,0,0, 0x83,0x78,0x40,TOP_HITS); /* mov rax,[r15+0xc8]; cmp dword [rax+0x40],3 */
    p=EMIT(p,0x74,4+2+2*JUMP_SIZE);                             /* je entry */
    p=EMIT(p,0x41,0x83,0xfc,0x03, 0x75,JUMP_SIZE);              /* cmp r12d,3; jne all */
    p=emit_jump(p,module+TITLE_DIFFICULTY_SUM);
    p=emit_jump(p,module+TITLE_ALL_COUNT);                      /* all */
    p=EMIT(p,0x49,0x8b,0x46,COUNTS_ALL, 0x44,0x8b,0x68,COUNT_PICK_UP); /* entry: mov rax,[r14+AllCountData]; mov r13d,[rax+PickUp] */
    p=emit_jump(p,module+TITLE_COUNT_DONE);
    write_branch(module+TITLE_DIFFICULTY,10,0xe9,stub);

    if ((size_t)(p-stubs)>size) return -1; /* the size check above covers this */
    printf("Top Hits: a sort list entry for today's songs\n");
    return 0;
}
