#!/usr/bin/env python3
"""스테이지 블록 덤프 diff (FORMAT.md v1 blocks) — 진단 전용.

바닐라 덤프의 팔레트 프로퍼티 순서는 상태정의 순서, C 덤프는 캐노니컬
(알파벳) 순서라 이름을 정규화해 비교한다. 사용:
    diff_stage_dump.py <A.blocks.txt> <B.blocks.txt> [--limit N]
출력: 불일치 셀 (world x,y,z) A-상태 B-상태, 클래스 요약.
"""
import sys
from collections import Counter


def canon(state):
    if '[' not in state:
        return state
    name, props = state.split('[', 1)
    props = props.rstrip(']').split(',')
    return name + '[' + ','.join(sorted(props)) + ']'


def read_dump(path):
    pal = {}
    data = []
    in_data = False
    cx = cz = 0
    min_y = -64
    for line in open(path):
        if in_data:
            data.append([int(t) for t in line.split()])
        elif line.startswith('palette '):
            _, idx, state = line.split(None, 2)
            pal[int(idx)] = canon(state.strip())
        elif line.strip() == 'data':
            in_data = True
        elif line.startswith('# chunk '):
            cx, cz = map(int, line.split()[2:4])
        elif line.startswith('# minY '):
            min_y = int(line.split()[2])
    return pal, data, cx, cz, min_y


def main():
    a_path, b_path = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[sys.argv.index('--limit') + 1]) if '--limit' in sys.argv else 40
    pa, da, cx, cz, min_y = read_dump(a_path)
    pb, db, _, _, _ = read_dump(b_path)
    assert len(da) == len(db), (len(da), len(db))
    classes = Counter()
    shown = n = 0
    for row in range(len(da)):
        y = min_y + row // 16
        lz = row % 16
        for lx in range(16):
            sa, sb = pa[da[row][lx]], pb[db[row][lx]]
            if sa == sb:
                continue
            n += 1
            classes[(sa, sb)] += 1
            if shown < limit:
                print(f'({cx*16+lx},{y},{cz*16+lz}) A={sa} B={sb}')
                shown += 1
    print(f'== {n} diff cells, {len(classes)} classes')
    for (sa, sb), c in classes.most_common(20):
        print(f'  {c:6d}  A={sa}  B={sb}')
    sys.exit(1 if n else 0)


if __name__ == '__main__':
    main()
