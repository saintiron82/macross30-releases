/* Locale-neutral UTF-8 UI tables. Data only; loaded once for process lifetime. */
#ifndef MACROSS_UI_EXTERNAL_H
#define MACROSS_UI_EXTERNAL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif
typedef struct { char key[96]; wchar_t* text; } MacrossExternalUiRow;
static MacrossExternalUiRow* macross_external_rows;
static size_t macross_external_count;
static int macross_external_state; /* 0 uninitialized, 1 absent, 2 valid, -1 invalid */
static wchar_t* macross_ui_decode(const unsigned char* s) {
    size_t n=strlen((const char*)s),at=0;
    const unsigned char* end=s+n;
    wchar_t* out=(wchar_t*)calloc(n+1,sizeof(wchar_t));
    if(!out)return NULL;
    while(*s) {
        unsigned c=*s++,cp=c;int rest=0;
        if(c=='\\') {
            c=*s++;if(!c)goto bad;
            if(c=='n')cp='\n';else if(c=='t')cp='\t';else if(c=='\\')cp='\\';else goto bad;
        } else if(c<32||c==127)goto bad;
        else if(c>=128) {
            if(c>=0xC2&&c<=0xDF){rest=1;cp=c&31;}
            else if(c>=0xE0&&c<=0xEF){rest=2;cp=c&15;}
            else if(c>=0xF0&&c<=0xF4){rest=3;cp=c&7;}
            else goto bad;
            if((size_t)(end-s)<(size_t)rest)goto bad;
            for(int j=0;j<rest;j++){c=*s++;if((c&0xC0)!=0x80)goto bad;cp=(cp<<6)|(c&63);}
            if((rest==1&&cp<0x80)||(rest==2&&cp<0x800)||(rest==3&&cp<0x10000)||
               cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF)||(cp>=0x80&&cp<0xA0))goto bad;
        }
#if WCHAR_MAX <= 0xFFFF
        if(cp>0xFFFF){cp-=0x10000;out[at++]=(wchar_t)(0xD800+(cp>>10));out[at++]=(wchar_t)(0xDC00+(cp&1023));}
        else
#endif
        out[at++]=(wchar_t)cp;
    }
    return out;
bad:free(out);return NULL;
}
/* Read bounded physical lines; never let NUL or an overlong prefix hide bytes. */
static int macross_ui_read_line(FILE* f,char* line,size_t capacity,size_t* bytes) {
    size_t length=0;int c;
    while((c=fgetc(f))!=EOF) {
        if(++*bytes>4u*1024u*1024u||c==0)return -1;
        if(c=='\n') {
            if(length&&line[length-1]=='\r')--length;
            line[length]=0;
            if(memchr(line,'\r',length))return -1;
            return 1;
        }
        if(length+1>=capacity)return -1;
        line[length++]=(char)c;
    }
    return ferror(f)||length ? -1 : 0;
}
static void macross_ui_external_load(void) {
    const char* path=getenv("MACROSS_UI_CATALOG_FILE");
    const char* locale=getenv("MACROSS_GAME_LANGUAGE");
    FILE* f=NULL;char line[16384];size_t bytes=0;int read_status;
    const char prefix[]="MACROSS_UI_STRINGS_V1\t";
    macross_external_state=1;
    if(!path||!*path)return;
    macross_external_state=-1;
    if(!locale||!*locale)return;
#ifdef _WIN32
    {
        wchar_t wide[32768];
        if(!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path,-1,wide,32768))return;
        f=_wfopen(wide,L"rb");
    }
#else
    f=fopen(path,"rb");
#endif
    if(!f)return;
    if(macross_ui_read_line(f,line,sizeof line,&bytes)!=1)goto end;
    if(strncmp(line,prefix,sizeof(prefix)-1)||strcmp(line+sizeof(prefix)-1,locale))goto end;
    macross_external_rows=(MacrossExternalUiRow*)calloc(4096,sizeof(MacrossExternalUiRow));
    if(!macross_external_rows)goto end;
    while((read_status=macross_ui_read_line(f,line,sizeof line,&bytes))==1) {
        char* tab;
        if(macross_external_count>=4096)goto end;
        tab=strchr(line,'\t');
        if(!tab||tab==line||tab-line>=96||strchr(tab+1,'\t')||!tab[1])goto end;
        *tab++=0;
        for(const char* k=line;*k;k++)if(!((*k>='a'&&*k<='z')||(*k>='A'&&*k<='Z')||(*k>='0'&&*k<='9')||*k=='_'||*k=='.'||*k=='-'))goto end;
        for(size_t i=0;i<macross_external_count;i++)if(!strcmp(macross_external_rows[i].key,line))goto end;
        strcpy(macross_external_rows[macross_external_count].key,line);
        macross_external_rows[macross_external_count].text=macross_ui_decode((const unsigned char*)tab);
        if(!macross_external_rows[macross_external_count].text)goto end;
        macross_external_count++;
    }
    if(read_status==0&&macross_external_count)macross_external_state=2;
end:
    fclose(f);
    if(macross_external_state!=2) {
        for(size_t i=0;i<macross_external_count;i++)free(macross_external_rows[i].text);
        free(macross_external_rows);macross_external_rows=NULL;macross_external_count=0;
    }
}
#ifdef _WIN32
static INIT_ONCE macross_external_once=INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK macross_ui_external_init(PINIT_ONCE once,PVOID parameter,PVOID* context) {
    (void)once;(void)parameter;(void)context;
    macross_ui_external_load();return TRUE;
}
#else
static pthread_once_t macross_external_once=PTHREAD_ONCE_INIT;
#endif
static const wchar_t* macross_ui_external_string(const char* key) {
#ifdef _WIN32
    InitOnceExecuteOnce(&macross_external_once,macross_ui_external_init,NULL,NULL);
#else
    pthread_once(&macross_external_once,macross_ui_external_load);
#endif
    if(macross_external_state<0)return L"[invalid UI catalog]";
    if(macross_external_state==2&&key)
        for(size_t i=0;i<macross_external_count;i++)if(!strcmp(key,macross_external_rows[i].key))return macross_external_rows[i].text;
    return NULL;
}
#endif
