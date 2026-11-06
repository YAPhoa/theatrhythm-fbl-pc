/* The game code the runner patches (Il2CppUserAssemblies.prx), found in whichever game version the
 * dump is. A site is a C# method looked up by name in the game's IL2CPP metadata
 * (Media/Metadata/global-metadata.dat, v29) and its code generation tables in the module, then
 * checked against a byte pattern: the code at its entry, or a place inside it. Code without a
 * method of its own (shared generic code) is found by its pattern in the whole code segment, which
 * must match a known number of times. A pattern per game version where the code differs; the
 * first that matches is used. tools/game_code.py checks the tables against dumps.
 *
 * The game's version (param.sfo APP_VER, and the module's SHA-256 that prepare records in
 * link.json) goes to the log first, so a bug report's log tells which version it is. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* Builds of Il2CppUserAssemblies.prx the sites were made and checked with (tools/game_code.py). */
static const struct { const char *sha256, *version; } known_builds[]={
    {"b8592928fb51f8061290fc23127ccd5199a9564ee4c3f1f5bff3e79e212a4be6","01.00"},
    {"f603d21de5e890f675550bfbf318f3337174f99d1437ff5675a7a861f73734b5","01.04"},
};
static char game_version[96]="unknown";

/* The SHA-256 link_modules.py recorded for Il2CppUserAssemblies.prx in link.json beside the image. */
static int module_sha256(const char *image_path, char *out) {
    char path[4096];
    const char *slash=strrchr(image_path,'/'), *back=strrchr(image_path,'\\');
    if (back>slash) slash=back;
    const int dir=slash ? (int)(slash-image_path)+1 : 0;
    snprintf(path,sizeof(path),"%.*slink.json",dir,image_path);
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    fseek(f,0,SEEK_END); const long size=ftell(f); fseek(f,0,SEEK_SET);
    char *json=size>0 && size<(64<<20) ? malloc((size_t)size+1) : NULL;
    const size_t n=json ? fread(json,1,(size_t)size,f) : 0;
    fclose(f);
    if (!json) return 0;
    json[n]=0;
    const char *entry=strstr(json,"\"Il2CppUserAssemblies.prx\""), *end=entry ? strchr(entry,'}') : NULL;
    const char *sha=entry ? strstr(entry,"\"sha256\": \"") : NULL;
    const int ok=sha && (!end || sha<end) && strspn(sha+11,"0123456789abcdef")>=64;
    if (ok) { memcpy(out,sha+11,64); out[64]=0; }
    free(json);
    return ok;
}

void runtime_game_identify(const char *title_id, const char *app_version, const char *app0, const char *image_path) {
    char sha[65]="", path[4096];
    struct stat st;
    snprintf(path,sizeof(path),"%s/Media/Modules/Il2CppUserAssemblies.prx",app0);
    const long long size=stat(path,&st) ? -1 : (long long)st.st_size;
    const char *build=NULL;
    if (module_sha256(image_path,sha))
        for (size_t i=0;i<sizeof(known_builds)/sizeof(*known_builds);++i)
            if (!strcmp(sha,known_builds[i].sha256)) build=known_builds[i].version;
    snprintf(game_version,sizeof(game_version),"%s %s",title_id[0] ? title_id : "?",app_version[0] ? app_version : "?");
    printf("Game: %s version %s, Il2CppUserAssemblies.prx %lld bytes, sha256 %s (%s%s)\n",title_id[0] ? title_id : "?",
           app_version[0] ? app_version : "unknown",size,sha[0] ? sha : "unknown",
           build ? "the code of version " : "a build the patches were not checked with",build ? build : "");
    if (build && app_version[0] && strcmp(build,app_version))
        printf("Game: param.sfo says %s but the game code is %s's: the dump mixes files of two versions\n",app_version,build);
}
const char *runtime_game_version(void) { return game_version; }

/* The module as the probe mapped and relocated it, and its code segment (offset 0). */
static unsigned char *module;
static uint64_t module_size, code_size;
static const char *app0_dir;
/* global-metadata.dat: header pairs (offset, size) per section. */
static unsigned char *meta;
static size_t meta_size;
static int meta_tried;
enum { SEC_STRINGS=2, SEC_METHODS=5, SEC_NESTED=15, SEC_TYPES=19, SEC_IMAGES=20 };
static uint32_t section_at[32], section_size[32];
/* Il2CppCodeGenModule.methodPointers and its count per image, found once each. */
static struct { int image; const uint64_t *pointers; uint32_t count; } cgms[16];
static int cgm_count;

void runtime_game_code_open(unsigned char *base, uint64_t size, uint64_t text_size, const char *app0) {
    module=base; module_size=size; code_size=text_size && text_size<=size ? text_size : size; app0_dir=app0;
}
void runtime_game_code_close(void) {
    free(meta); meta=NULL; meta_size=0; meta_tried=0; cgm_count=0;
}

static uint32_t u32(const unsigned char *p) { uint32_t v; memcpy(&v,p,4); return v; }
static int load_metadata(void) {
    if (meta_tried) return meta!=NULL;
    meta_tried=1;
    char path[4096];
    snprintf(path,sizeof(path),"%s/Media/Metadata/global-metadata.dat",app0_dir ? app0_dir : ".");
    FILE *f=fopen(path,"rb");
    if (!f) return 0;
    fseek(f,0,SEEK_END); const long size=ftell(f); fseek(f,0,SEEK_SET);
    meta=size>8+256 ? malloc((size_t)size) : NULL;
    if (!meta || fread(meta,1,(size_t)size,f)!=(size_t)size || u32(meta)!=0xfab11bafu || u32(meta+4)!=29) {
        fclose(f); free(meta); meta=NULL;
        return 0;
    }
    fclose(f);
    meta_size=(size_t)size;
    for (int i=0;i<32;++i) {
        section_at[i]=u32(meta+8+8*i); section_size[i]=u32(meta+12+8*i);
        if ((uint64_t)section_at[i]+section_size[i]>meta_size) { free(meta); meta=NULL; return 0; }
    }
    return 1;
}
static const char *mstring(uint32_t index) {
    return index<section_size[SEC_STRINGS] ? (const char *)meta+section_at[SEC_STRINGS]+index : "";
}
/* Records: Il2CppTypeDefinition 88 bytes (name, namespace; methodStart at 36, nestedTypesStart at 48,
 * method_count at 64, nested_type_count at 72), Il2CppMethodDefinition 32 (name; token at 20),
 * Il2CppImageDefinition 40 (name; typeStart at 8, typeCount at 12). */
static const unsigned char *type_def(uint32_t i) { return meta+section_at[SEC_TYPES]+88*(size_t)i; }
static uint32_t type_count(void) { return section_size[SEC_TYPES]/88; }
static uint16_t u16(const unsigned char *p) { uint16_t v; memcpy(&v,p,2); return v; }

/* The type definition named "Namespace.Type/Nested/...", or -1 (none, or several). */
static int64_t find_type(const char *path, size_t length) {
    const char *slash=memchr(path,'/',length);
    const size_t outer=slash ? (size_t)(slash-path) : length;
    const char *dot=NULL;
    for (const char *p=path;p<path+outer;++p) if (*p=='.') dot=p;
    const size_t ns=dot ? (size_t)(dot-path) : 0;
    const char *name=dot ? dot+1 : path;
    const size_t name_length=(size_t)(path+outer-name);
    int64_t found=-1;
    for (uint32_t i=0;i<type_count();++i) {
        const unsigned char *t=type_def(i);
        const char *n=mstring(u32(t)), *s=mstring(u32(t+4));
        if (strlen(n)==name_length && !memcmp(n,name,name_length) && strlen(s)==ns && !memcmp(s,path,ns)) {
            if (found>=0) return -1;
            found=i;
        }
    }
    for (const char *part=slash ? slash+1 : NULL;found>=0 && part;) {
        const char *next=memchr(part,'/',(size_t)(path+length-part));
        const size_t n=next ? (size_t)(next-part) : (size_t)(path+length-part);
        const unsigned char *t=type_def((uint32_t)found);
        const uint32_t start=u32(t+48), count=u16(t+72);
        int64_t inner=-1;
        for (uint32_t k=start;k<start+count && 4*(uint64_t)(k+1)<=section_size[SEC_NESTED];++k) {
            const uint32_t index=u32(meta+section_at[SEC_NESTED]+4*(size_t)k);
            if (index>=type_count()) continue;
            const char *name_k=mstring(u32(type_def(index)));
            if (strlen(name_k)==n && !memcmp(name_k,part,n)) { if (inner>=0) return -1; inner=index; }
        }
        found=inner;
        part=next ? next+1 : NULL;
    }
    return found;
}

/* The image's Il2CppCodeGenModule: the structure whose moduleName points at the image's file name
 * (relocated: an absolute pointer into the module), methodPointerCount at +8, methodPointers at +16. */
static int code_gen_module(int image, const uint64_t **pointers, uint32_t *count) {
    for (int i=0;i<cgm_count;++i) if (cgms[i].image==image) { *pointers=cgms[i].pointers; *count=cgms[i].count; return *pointers!=NULL; }
    const unsigned char *im=meta+section_at[SEC_IMAGES]+40*(size_t)image;
    const char *name=mstring(u32(im));
    const size_t n=strlen(name)+1;
    const uint64_t base=(uint64_t)(uintptr_t)module;
    *pointers=NULL; *count=0;
    for (uint64_t at=0;at+n<=module_size && !*pointers;++at) {
        const unsigned char *hit=memchr(module+at,name[0],module_size-n+1-at);
        if (!hit) break;
        at=(uint64_t)(hit-module);
        if (memcmp(hit,name,n)) continue;
        for (uint64_t c=code_size&~7ull;c+24<=module_size;c+=8) {
            uint64_t value, table; memcpy(&value,module+c,8);
            if (value!=base+at) continue;
            memcpy(&table,module+c+16,8);
            if (table>=base && table<base+module_size && !(table&7)) {
                *pointers=(const uint64_t *)(uintptr_t)table; *count=u32(module+c+8);
                break;
            }
        }
    }
    if (cgm_count<(int)(sizeof(cgms)/sizeof(*cgms))) { cgms[cgm_count].image=image; cgms[cgm_count].pointers=*pointers; cgms[cgm_count++].count=*count; }
    return *pointers!=NULL;
}

/* The module offset of a method's code ("Namespace.Type::Method", or "...::Method(N)" for the
 * overload with N parameters), or 0 with `why` set. Overloads without code of their own (generic
 * methods) do not count. */
static uint64_t find_method(const char *name, const char **why) {
    const char *colons=strstr(name,"::");
    if (!colons) { *why="not a method name"; return 0; }
    const int64_t type=find_type(name,(size_t)(colons-name));
    if (type<0) { *why="its class is not in global-metadata.dat"; return 0; }
    const char *method_name=colons+2, *paren=strchr(method_name,'(');
    const size_t name_length=paren ? (size_t)(paren-method_name) : strlen(method_name);
    const int parameters=paren ? atoi(paren+1) : -1;
    int image=-1;
    for (uint32_t i=0;i<section_size[SEC_IMAGES]/40;++i) {
        const unsigned char *im=meta+section_at[SEC_IMAGES]+40*(size_t)i;
        if ((uint32_t)type>=u32(im+8) && (uint32_t)type-u32(im+8)<u32(im+12)) image=(int)i;
    }
    const uint64_t *pointers; uint32_t pointer_count;
    if (image<0 || !code_gen_module(image,&pointers,&pointer_count)) { *why="no code table for its assembly"; return 0; }
    const unsigned char *t=type_def((uint32_t)type);
    const uint32_t start=u32(t+36), count=u16(t+64);
    const uint64_t base=(uint64_t)(uintptr_t)module;
    uint64_t found=0;
    int named=0;
    for (uint32_t i=start;i<start+count && 32*(uint64_t)(i+1)<=section_size[SEC_METHODS];++i) {
        const unsigned char *m=meta+section_at[SEC_METHODS]+32*(size_t)i; /* parameterCount at 30 */
        const char *n=mstring(u32(m));
        if (strlen(n)!=name_length || memcmp(n,method_name,name_length) || (parameters>=0 && u16(m+30)!=parameters)) continue;
        ++named;
        const uint32_t rid=u32(m+20)&0xffffff;
        uint64_t code=0;
        if (rid && rid<=pointer_count) memcpy(&code,&pointers[rid-1],8);
        if (code<=base || code-base>=code_size) continue;
        if (found) { *why="it has overloads"; return 0; }
        found=code-base;
    }
    if (!found) *why=named ? "it has no code" : "no such method in global-metadata.dat";
    return found;
}

typedef struct { unsigned char bytes[512], mask[512]; size_t length; int moved; } Pattern;
static int parse(const char *text, Pattern *p) {
    p->length=0; p->moved=-1;
    for (const char *s=text;*s;) {
        while (*s==' ') ++s;
        if (!*s) break;
        if (*s=='|') { p->moved=(int)p->length; ++s; continue; }
        if (p->length==sizeof(p->bytes)) return 0;
        if (s[0]=='?' && s[1]=='?') { p->bytes[p->length]=0; p->mask[p->length++]=0; s+=2; continue; }
        char hex[3]={s[0],s[0] ? s[1] : 0,0}, *end;
        const long v=strtol(hex,&end,16);
        if (end!=hex+2) return 0;
        p->bytes[p->length]=(unsigned char)v; p->mask[p->length++]=1; s+=2;
    }
    for (int i=0;i<p->moved;++i) if (!p->mask[i]) return 0; /* moved bytes are always fixed */
    return 1;
}
static int match_at(const Pattern *p, uint64_t at) {
    if (at+p->length>code_size) return 0;
    for (size_t i=0;i<p->length;++i) if (p->mask[i] && module[at+i]!=p->bytes[i]) return 0;
    return 1;
}
/* Matches of p in [start, end) (at 16-byte aligned offsets when `aligned`), up to `limit` in out. */
static int scan(const Pattern *p, uint64_t start, uint64_t end, int aligned, uint64_t *out, int limit) {
    size_t first=0;
    while (first<p->length && !p->mask[first]) ++first;
    if (first==p->length) return 0;
    int count=0;
    if (aligned) start=(start+15)&~15ull;
    for (uint64_t at=start;at+p->length<=end;at+=aligned ? 16 : 1) {
        if (!aligned) { /* to the next place its first fixed byte is */
            const unsigned char *hit=memchr(module+at+first,p->bytes[first],end-at-first);
            if (!hit) break;
            at=(uint64_t)(hit-module)-first;
            if (at+p->length>end) break;
        }
        if (module[at+first]!=p->bytes[first] || !match_at(p,at)) continue;
        if (count<limit) out[count]=at;
        ++count;
    }
    return count;
}

int runtime_game_code_find(const GameSite *sites, size_t count, GameCode *found, const char *who) {
    for (size_t i=0;i<count;++i) {
        const GameSite *s=&sites[i];
        const char *why="its code differs";
        uint64_t at=0;
        if (s->method && (!load_metadata() || !(at=find_method(s->method,&why)))) {
            if (!meta) why="global-metadata.dat is missing or not IL2CPP metadata v29";
        } else {
            found[i].offset=0;
            for (int k=0;k<3 && s->patterns[k] && !found[i].offset;++k) {
                Pattern p;
                if (!parse(s->patterns[k],&p)) { why="bad pattern (runner bug)"; break; }
                uint64_t hits[8];
                int n=-1;
                if (s->method && !s->inside) n=match_at(&p,at) ? (hits[0]=at,1) : 0;
                else if (s->method) n=scan(&p,at,at+0x10000<code_size ? at+0x10000 : code_size,0,hits,8);
                else n=scan(&p,0,code_size,!s->inside,hits,8);
                const int want=s->method ? 1 : s->instances;
                if (n==want) {
                    found[i].offset=hits[s->method ? 0 : s->instance];
                    found[i].moved=p.moved<0 ? 0 : (unsigned)p.moved;
                    found[i].pattern=k;
                } else if (n>0) {
                    why=n>want ? "its pattern matches too often" : "its pattern matches too seldom";
                    break;
                }
            }
            if (found[i].offset) continue;
        }
        printf("%s: %s not found in game %s (%s)\n",who,s->method ? s->method : s->patterns[0],game_version,why);
        return -1;
    }
    return 0;
}
