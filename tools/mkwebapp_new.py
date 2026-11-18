"""Generate webapp_html.h for the NEW board UI from server/web/new.html.

Mirrors the output format of tools/mkwebapp.ps1 (INDEX_HTML rawliteral +
gzip + LITE + SETUP + LEAF_IMG + WEBAPP_ASSETS), but embeds the new design
and only the assets it actually references (no fonts).
"""
import base64, gzip, os, re

ROOT = r'D:\OpenCode_projeck\OVMS\leafapp'
SRC_HTML = os.path.join(ROOT, 'server', 'web', 'new.html')
LITE = os.path.join(ROOT, 'lite.html')
SETUP = os.path.join(ROOT, 'setup.html')
OUT = r'D:\OpenCode_projeck\OVMS\leafapp\firmware\LeafControl\webapp_html_new.h'

html = open(SRC_HTML, encoding='utf-8').read()
# Убираем переключатель «Классический интерфейс»? Нужен: ведёт на / (старую прошивку
# или на эту же, безвредно). Оставляем как есть.
if ')rawliteral' in html:
    raise SystemExit('new.html contains rawliteral terminator')
# как в ps1: убрать ведущие пробелы и пустые строки
html = re.sub(r'(?m)^[ \t]+', '', html)
html = re.sub(r'(?m)^\r?\n', '', html)

def gz_hex(data):
    comp = gzip.compress(data, 9, mtime=0)
    parts = []
    for i, b in enumerate(comp):
        if i % 16 == 0:
            parts.append('\n  ')
        else:
            parts.append(' ')
        parts.append('0x%02x,' % b)
    return ''.join(parts), len(comp)

gz, gz_len = gz_hex(html.encode('utf-8'))

lite = open(LITE, encoding='utf-8').read()
lite = re.sub(r'(?m)^[ \t]+', '', lite)
lite = re.sub(r'(?m)^\r?\n', '', lite)
lite_gz, lite_len = gz_hex(lite.encode('utf-8'))

img = open(os.path.join(ROOT, 'leaf.png'), 'rb').read()
ih = []
for i, b in enumerate(img):
    if i % 16 == 0:
        ih.append('\n  ')
    else:
        ih.append(' ')
    ih.append('0x%02x,' % b)
img_hex = ''.join(ih)

assets = [
    ('car-lights.png', 'image/png'),
    ('car-cable.png', 'image/png'),
    ('car-wheel1.png', 'image/png'),
    ('car-wheel2.png', 'image/png'),
    ('car-wheel3.png', 'image/png'),
    ('car-wheel4.png', 'image/png'),
    ('logo-mark-light.png', 'image/png'),
    ('logo-mark-dark.png', 'image/png'),
]
sb = []
sb.append('// GENERATED FILE - new UI. Source: server/web/new.html')
sb.append('#ifndef WEBAPP_HTML_H')
sb.append('#define WEBAPP_HTML_H')
sb.append('#include <Arduino.h>')
sb.append('')
sb.append('static const char INDEX_HTML[] PROGMEM = R"rawliteral(')
sb.append(html)
sb.append(')rawliteral";')
sb.append('static const unsigned char INDEX_HTML_GZ[] PROGMEM = {' + gz + '};')
sb.append('static const size_t INDEX_HTML_GZ_LEN = %d;' % gz_len)
sb.append('static const char LITE_HTML[] PROGMEM = R"rawliteral(')
sb.append(lite)
sb.append(')rawliteral";')
sb.append('static const unsigned char LITE_HTML_GZ[] PROGMEM = {' + lite_gz + '};')
sb.append('static const size_t LITE_HTML_GZ_LEN = %d;' % lite_len)
sb.append('')
sb.append('static const unsigned char LEAF_IMG[] PROGMEM = {' + img_hex + '};')
sb.append('static const size_t LEAF_IMG_LEN = %d;' % len(img))
sb.append('static const char LEAF_IMG_TYPE[] PROGMEM = "image/png";')
sb.append('')
entries = []
for name, typ in assets:
    p = os.path.join(ROOT, name)
    if not os.path.isfile(p):
        print('WARN missing asset', name)
        continue
    data = open(p, 'rb').read()
    var = re.sub(r'[^A-Za-z0-9]', '_', name).upper()
    h = []
    for i, b in enumerate(data):
        if i % 16 == 0:
            h.append('\n  ')
        else:
            h.append(' ')
        h.append('0x%02x,' % b)
    sb.append('static const unsigned char %s[] PROGMEM = {%s};' % (var, ''.join(h)))
    entries.append('  {"/%s", %s, sizeof(%s), "%s"},' % (name, var, var, typ))
    print('asset', name, len(data))
sb.append('struct WebAppAsset { const char* path; const unsigned char* data; size_t len; const char* type; };')
sb.append('static const WebAppAsset WEBAPP_ASSETS[] = {')
sb.extend(entries)
sb.append('};')
sb.append('static const size_t WEBAPP_ASSET_COUNT = %d;' % len(entries))
sb.append('')
setup = open(SETUP, encoding='utf-8').read()
setup = re.sub(r'(?m)^[ \t]+', '', setup)
setup = re.sub(r'(?m)^\r?\n', '', setup)
sb.append('static const char SETUP_HTML[] PROGMEM = R"setup(')
sb.append(setup)
sb.append(')setup";')
sb.append('#endif')

open(OUT, 'w', encoding='utf-8', newline='\n').write('\n'.join(sb) + '\n')
print('WROTE', OUT, os.path.getsize(OUT), 'gz_html', gz_len)
