/* Native string resolution and GDI text metrics, with no visible window. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "macross_ui_language.h"
static const char* keys[] = {
#define MACROSS_UI_STRING(key,ko,en,ja) key,
#include "macross_ui_strings.def"
#include "../system/savedlg_strings.def"
#undef MACROSS_UI_STRING
};
int main(void) {
    HDC dc=CreateCompatibleDC(NULL);
    HFONT font=CreateFontW(-16,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Malgun Gothic");
    if(!dc||!font)return 2;
    HGDIOBJ old=SelectObject(dc,font);
    for(size_t i=0;i<sizeof(keys)/sizeof(keys[0]);++i) {
        const wchar_t* value=macross_ui_string(keys[i]);
        char utf8[16384];
        int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value,-1,utf8,sizeof utf8,NULL,NULL);
        if(n<=0||!wcscmp(value,L"[missing UI string]"))return 3;
        SIZE size={0,0};
        if(!GetTextExtentPoint32W(dc,value,(int)wcslen(value),&size))return 4;
        RECT rect={0,0,420,0};
        if(!DrawTextW(dc,value,-1,&rect,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX))return 5;
        printf("%s\t%ld\t%ld\t",keys[i],size.cx,rect.bottom-rect.top);
        for(int j=0;j<n-1;++j)printf("%02x",(unsigned char)utf8[j]);
        putchar('\n');
    }
    SelectObject(dc,old);DeleteObject(font);DeleteDC(dc);
    return 0;
}
