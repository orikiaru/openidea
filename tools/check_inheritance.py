"""Reject multiple direct bases in owned C++ declarations (including generated files)."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOKEN = re.compile(r'//[^\n]*|/\*[\s\S]*?\*/|R"([^ ()\\\t\r\n]{0,16})\([\s\S]*?\)\1"|"(?:\\.|[^"\\])*"|\x27(?:\\.|[^\x27\\])*\x27|::|[A-Za-z_]\w*|[^\s]')


def violations(source):
    tokens = [(match.group(), match.start()) for match in TOKEN.finditer(source)
              if not match.group().startswith(('//', '/*', '"', "'", 'R"'))]
    found = []
    for index, (word, offset) in enumerate(tokens):
        if word not in ('class', 'struct') or (index and tokens[index - 1][0] == 'enum'):
            continue
        depth = 0
        base = False
        for token, _ in tokens[index + 1:]:
            if token in (';', '{', '}', '=') and depth == 0:
                break
            if token in ('<', '(', '['):
                depth += 1
            elif token in ('>', ')', ']'):
                if depth == 0:
                    break
                depth -= 1
            elif depth == 0 and token == ':':
                base = True
            elif depth == 0 and token == ',':
                if base:
                    found.append(source.count('\n', 0, offset) + 1)
                break
    return sorted(set(found))


def main():
    count = 0
    failures = []
    for directory in ('cbaselib', 'modules'):
        for path in (ROOT / directory).rglob('*'):
            if path.suffix not in ('.h', '.hpp', '.cpp', '.cc', '.cxx') or '3rd' in path.relative_to(ROOT).parts:
                continue
            count += 1
            for line in violations(path.read_text(encoding='utf-8-sig')):
                failures.append(f'{path.relative_to(ROOT)}:{line}: multiple inheritance is forbidden')
    print('\n'.join(failures) if failures else f'Single inheritance checked: {count} owned C++ files')
    return bool(failures)


if __name__ == '__main__':
    raise SystemExit(main())
