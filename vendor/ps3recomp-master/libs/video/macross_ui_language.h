/* Native game UI follows the selected game pack. Launcher UI is independent. */
#ifndef MACROSS_UI_LANGUAGE_H
#define MACROSS_UI_LANGUAGE_H
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "macross_ui_external.h"
static int macross_ui_locale(void) {
    const char* value=getenv("MACROSS_GAME_LANGUAGE");
    if(!value || !*value)value=getenv("MACROSS_UI_LANGUAGE");
    if(value && (!strcmp(value,"en") || !strncmp(value,"en-",3) || !strcmp(value,"ru") || !strncmp(value,"ru-",3)))return 1;
    if(value && (!strcmp(value,"ja") || !strncmp(value,"ja-",3)))return 2;
    return value && strcmp(value,"ko") && strncmp(value,"ko-",3) ? 1 : 0;
}
static const wchar_t* macross_ui_string(const char* key) {
    const wchar_t* external=macross_ui_external_string(key);
    if(external)return external;
    const char* locale=getenv("MACROSS_GAME_LANGUAGE");
    if(!locale || !*locale)locale=getenv("MACROSS_UI_LANGUAGE");
    if(locale && (!strcmp(locale,"ru") || !strncmp(locale,"ru-",3))) {
        static const struct {const char* key;const wchar_t* text;} russian[]={
#define MACROSS_RU_STRING(key,text) {key,text},
#include "macross_ui_ru.def"
#undef MACROSS_RU_STRING
            {NULL,NULL}
        };
        if(key)for(size_t i=0;russian[i].key;i++)if(!strcmp(key,russian[i].key))return russian[i].text;
    }
    static const struct { const char* key; const wchar_t *text[3]; } rows[]={
#define MACROSS_UI_STRING(key,ko,en,ja) {key,{ko,en,ja}},
#include "macross_ui_strings.def"
#include "../system/savedlg_strings.def"
#undef MACROSS_UI_STRING
    };
    if(key)for(size_t i=0;i<sizeof(rows)/sizeof(rows[0]);i++)
        if(!strcmp(key,rows[i].key))return rows[i].text[macross_ui_locale()];
    return L"[missing UI string]";
}
#endif
