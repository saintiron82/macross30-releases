"""Complete native F9 catalogs for the 0.4.0 frame/rumble/mouse controls."""
import json
from pathlib import Path
import re
import zipfile

ROOT = Path(__file__).resolve().parents[2]
VIDEO = ROOT / 'vendor/ps3recomp-master/libs/video'
OUT = ROOT / 'localization/native-ui/0.4.0'
BASE = ROOT / 'build/releases/0.3.12/field-display-20261126/candidate/assets'
ROW = re.compile(r'MACROSS_UI_STRING\("([^"\n]+)", L"((?:\\.|[^"\\])*)", L"((?:\\.|[^"\\])*)", L"((?:\\.|[^"\\])*)"\)')


def decode(raw):
    return json.loads('"' + raw + '"')


def quoted(text):
    return json.dumps(text, ensure_ascii=False)


def parse_def(path):
    return {m[0]: dict(zip(('ko-KR', 'en-US', 'ja-JP'), map(decode, m[1:])))
            for m in ROW.findall(path.read_text('utf8'))}


def export(locale, rows):
    escape = lambda s: s.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')
    return ('MACROSS_UI_STRINGS_V1\t' + locale + '\n' +
            ''.join(k + '\t' + escape(v) + '\n' for k, v in sorted(rows.items()))).encode('utf8')


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    core = parse_def(VIDEO / 'macross_ui_strings.def')
    core.update({
        'options.fps_limit': dict(zip(('ko-KR','en-US','ja-JP'), ('최대 프레임','Maximum FPS','最大フレームレート'))),
        'options.fps_limit.unlimited': dict(zip(('ko-KR','en-US','ja-JP'), ('무제한','Unlimited','無制限'))),
        'options.rumble': dict(zip(('ko-KR','en-US','ja-JP'), ('진동','Vibration','振動'))),
        'options.rumble_strength': dict(zip(('ko-KR','en-US','ja-JP'), ('진동 세기','Vibration strength','振動の強さ'))),
        'options.uncap': dict(zip(('ko-KR','en-US','ja-JP'), ('30 FPS 제한 해제','Unlock 30 FPS cap','30 FPS制限解除'))),
        'options.restart_help': dict(zip(('ko-KR','en-US','ja-JP'), (
            '전체화면·VSync·자막·프레임 제한은 즉시 적용됩니다.\n해상도와 HD 텍스처는 재실행 후 적용됩니다.\n프레임 제한을 바꿔도 게임 속도 보정은 유지됩니다.',
            'Fullscreen, VSync, subtitles and FPS: live.\nResolution and HD textures: restart required.\nFPS changes retain game-speed correction.',
            '全画面・VSync・字幕・FPS制限は即時反映されます。\n解像度とHDテクスチャは再起動後に反映されます。\n制限を切り替えてもゲーム速度の補正を維持します。')))
    })
    extra = {
        'ru-RU': {
            'options.fps_limit':'Макс. частота кадров', 'options.fps_limit.unlimited':'Без ограничения',
            'options.rumble':'Вибрация', 'options.rumble_strength':'Сила вибрации',
            'options.uncap':'Снять ограничение 30 FPS',
            'options.restart_help':'Экран, VSync, субтитры и FPS: сразу.\nРазрешение и HD-текстуры: перезапуск.\nКоррекция скорости игры всегда активна.',
            'mouse.enable':'Мышь: правый стик', 'mouse.invert':'Инверсия Y',
            'mouse.sensitivity_x':'Чувствит. мыши X', 'mouse.sensitivity_y':'Чувствит. мыши Y',
            'mouse.capture_help':'Курсор закреплён; F9/Alt+Tab — отпустить',
            'mouse.numeric_help':'Чувствительность: 0.01–8.00 (шаг 0.01)',
            'input.mouse.left':'Мышь: левая', 'input.mouse.right':'Мышь: правая',
            'input.mouse.middle':'Мышь: средняя', 'input.mouse.x1':'Мышь: боковая 1', 'input.mouse.x2':'Мышь: боковая 2',
            'input.keyboard.title':'Клавиатура и мышь', 'input.keyboard.open':'Клавиши и мышь...',
            'input.keyboard.help':'Выберите действие и нажмите клавишу или кнопку мыши. Esc — отмена, Del — очистка.\nСтрелки также управляют левым стиком, если ему не назначены клавиши.'
        },
        'es-ES': {
            'options.fps_limit':'FPS máximos', 'options.fps_limit.unlimited':'Sin límite',
            'options.rumble':'Vibración', 'options.rumble_strength':'Fuerza de vibración',
            'options.uncap':'Desactivar el límite de 30 FPS',
            'options.restart_help':'Pantalla, VSync, subtítulos y FPS: al instante.\nResolución y texturas HD: reiniciar.\nSe mantiene la corrección de velocidad.',
            'mouse.enable':'Ratón como stick derecho', 'mouse.invert':'Invertir eje Y',
            'mouse.sensitivity_x':'Sensibilidad del ratón X', 'mouse.sensitivity_y':'Sensibilidad del ratón Y',
            'mouse.capture_help':'Cursor fijo en juego; F9/Alt+Tab lo libera',
            'mouse.numeric_help':'Sensibilidad: 0.01–8.00 (pasos de 0.01)',
            'input.mouse.left':'Ratón: izquierdo', 'input.mouse.right':'Ratón: derecho',
            'input.mouse.middle':'Ratón: central', 'input.mouse.x1':'Ratón: lateral 1', 'input.mouse.x2':'Ratón: lateral 2',
            'input.keyboard.title':'Teclado y ratón', 'input.keyboard.open':'Teclas y ratón...',
            'input.keyboard.help':'Elige una acción y pulsa una tecla o botón del ratón. Esc cancela; Supr borra.\nLas flechas también controlan el stick izquierdo si no tiene teclas asignadas.'
        }
    }
    catalogs = {locale: {k: v[locale] for k, v in core.items()} for locale in ('ko-KR','en-US','ja-JP')}
    saved = parse_def(ROOT / 'vendor/ps3recomp-master/libs/system/savedlg_strings.def')
    for locale in catalogs:
        catalogs[locale].update({k: v[locale] for k, v in saved.items()})
    gaps = {}
    for locale in ('ru-RU', 'es-ES', 'en-US'):
        with zipfile.ZipFile(BASE / ('Macross30-text-' + locale + '-0.3.12.zip')) as z:
            rows = dict(line.split('\t', 1) for line in z.read('native-ui.tsv').decode('utf8').splitlines()[1:] if line)
            rows = {k: v.replace('\\n','\n').replace('\\t','\t').replace('\\\\','\\') for k, v in rows.items()}
        gaps[locale] = sorted(set(core) - set(rows))
        if locale in extra:
            rows.update(extra[locale])
            if set(core) - set(rows):
                raise ValueError('Missing core UI translations: ' + locale)
            catalogs[locale] = rows
    for locale, rows in catalogs.items():
        (OUT / (locale + '.tsv')).write_bytes(export(locale, rows))
    (OUT / 'en-US-fix.tsv').write_bytes(export('en-US-fix', catalogs['en-US']))
    # Keep KO/EN/JA literals in the existing three-column ABI.
    (VIDEO / 'macross_ui_strings.def').write_text(
        '/* Native UI strings; 0.4.0 FPS, vibration and mouse controls. */\n' +
        ''.join('MACROSS_UI_STRING(' + quoted(k) + ', ' + ', '.join('L' + quoted(v[loc]) for loc in ('ko-KR','en-US','ja-JP')) + ')\n'
                for k, v in core.items()), encoding='utf8')
    for locale, macro, filename in [('ru-RU','MACROSS_RU_STRING','macross_ui_ru.def'),
                                    ('es-ES','MACROSS_ES_STRING','macross_ui_es.def')]:
        (VIDEO / filename).write_text('/* Native UI fallback; generated from the reviewed locale catalog. */\n' +
            ''.join(macro + '(' + quoted(k) + ', L' + quoted(v) + ')\n' for k, v in sorted(catalogs[locale].items())), encoding='utf8')
    header = VIDEO / 'macross_ui_language.h'
    text = header.read_text('utf8')
    if '#include "macross_ui_es.def"' not in text:
        marker = '    static const struct { const char* key; const wchar_t *text[3]; } rows[]={'
        block = '''    if(locale && (!strcmp(locale,"es") || !strncmp(locale,"es-",3))) {
        static const struct {const char* key;const wchar_t* text;} spanish[]={
#define MACROSS_ES_STRING(key,text) {key,text},
#include "macross_ui_es.def"
#undef MACROSS_ES_STRING
            {NULL,NULL}
        };
        if(key)for(size_t i=0;spanish[i].key;i++)if(!strcmp(key,spanish[i].key))return spanish[i].text;
    }
'''
        if text.count(marker) != 1:
            raise ValueError('Unexpected native UI header layout')
        header.write_text(text.replace(marker, block + marker), encoding='utf8')
    report = dict(locales=sorted(catalogs), core_keys=len(core), keys={k:len(v) for k,v in catalogs.items()},
                  previous_external_catalog_missing_core_keys=gaps,
                  updated_keys={k:sorted(v) for k,v in extra.items()},
                  all_core_keys_present=True, game_started=False, visual_check='pending')
    (OUT / 'COVERAGE.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf8')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
