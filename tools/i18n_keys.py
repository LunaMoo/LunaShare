#!/usr/bin/env python3
"""Lists the strings the UI translates (T/TF/TItems arguments plus a few known extras) and, with a language file,
which of them are missing there or no longer used.

  tools/i18n_keys.py                 all keys, one per line (C escapes)
  tools/i18n_keys.py src/app/lang/pl.inc   missing / unused entries for that file
"""
import glob, os, re, sys
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lit = r'"(?:[^"\\]|\\.)*"(?:\s*"(?:[^"\\]|\\.)*")*'
call = re.compile(r'\b(T|TF|TItems)\(\s*(' + lit + r')')
def join(group):
    return ''.join(re.findall(r'"((?:[^"\\]|\\.)*)"', group))
keys = []
def add(k):
    if k not in keys: keys.append(k)
files = sorted(glob.glob(os.path.join(root, 'src/app/*.cpp')))
for f in files:
    s = open(f, encoding='utf-8').read()
    for m in call.finditer(s):
        k = join(m.group(2))
        if m.group(1) == 'TItems':
            for item in k.split('\\0'):
                if item: add(item)
        else:
            add(k.split('##')[0])
    # strings marked for translation without a call (tables): // i18n: "..."
    for m in re.finditer(r'i18n-keys:\s*((?:' + lit + r'\s*,?\s*)+)', s):
        for x in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)): add(x)
# the core's sentences (patterns live in i18n.cpp's kPatterns)
s = open(os.path.join(root, 'src/app/i18n.cpp'), encoding='utf-8').read()
pat = s[s.index('kPatterns[] = {'):]
pat = pat[:pat.index('};')]
for x in re.findall(r'"((?:[^"\\]|\\.)*)"', pat): add(x)
keys = [k for k in keys if re.search(r'[A-Za-z]', k)]
if len(sys.argv) < 2:
    for k in keys: print(k)
    sys.exit(0)
have = re.findall(r'\{\s*"((?:[^"\\]|\\.)*)"\s*,', open(sys.argv[1], encoding='utf-8').read())
missing = [k for k in keys if k not in have]
unused = [k for k in have if k not in keys]
for k in missing: print('missing:', k)
for k in unused: print('unused: ', k)
print(f'{len(keys)} keys, {len(missing)} missing, {len(unused)} unused', file=sys.stderr)
sys.exit(1 if missing else 0)
