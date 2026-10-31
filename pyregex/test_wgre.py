import re
import time

import wgre

cases = [
    ('abc', 'xxabcxx', True), ('abc', 'ab', False), ('a.c', 'abc', True),
    ('a.c', 'a\nc', False), ('colou?r', 'color', True), ('colou?r', 'colour', True),
    ('ab*c', 'ac', True), ('ab*c', 'abbbc', True), ('ab+c', 'ac', False),
    ('ab+c', 'abbc', True), ('cat|dog', 'hotdog', True), ('cat|dog', 'bird', False),
    ('(ab)+c', 'ababc', True), ('(a|b)*c', 'abbac', True), ('(a*)*b', 'aaab', True),
    ('(a*)*b', 'aaa', False), ('', 'anything', True), ('', '', True),
    (r'a\.c', 'abc', False), (r'a\.c', 'a.c', True), (r'\*', '2*3', True),
    ('(a|)b', 'b', True), ('a||b', 'zzz', True), ('世界', 'hello 世界', True),
]

failed = 0
for pattern, text, expected in cases:
    got = wgre.Pattern(pattern).search(text)
    agrees = bool(re.search(pattern, text)) == expected
    if got != expected or not agrees:
        failed += 1
        print('FAIL', repr(pattern), repr(text), 'got', got, 'expected', expected)
print(f'{len(cases) - failed} of {len(cases)} passed')

for bad in ['(', ')', '*', 'a\\', '(a']:
    try:
        wgre.Pattern(bad)
        print('no error for', repr(bad))
    except ValueError as e:
        print('rejected', repr(bad), '-', e)

text = 'a' * 26
start = time.perf_counter()
wgre.Pattern('(a+)+b').search(text)
ours = time.perf_counter() - start
start = time.perf_counter()
re.search('(a+)+b', text)
theirs = time.perf_counter() - start
print(f'(a+)+b on {len(text)} a\'s: wgre {ours * 1000:.2f} ms, re {theirs * 1000:.0f} ms')
