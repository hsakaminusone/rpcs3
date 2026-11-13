#!/usr/bin/env python3
"""Static basic-block and region-length analysis over llvm-objdump PPC64 output.

Two passes per file: pass 1 collects every direct branch target, pass 2 rebuilds
contiguous chunks and computes blocks / chains. Memory stays bounded because
chunks are processed as they end.
"""
import re, sys, statistics
from collections import Counter, defaultdict

LINE = re.compile(r'^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2}\s+)+(\S+)\s*(.*)$')
TARGET = re.compile(r'\b(0x[0-9a-f]+)')

UNCOND = {'b', 'ba'}
COND = {'bt', 'bf', 'bt+', 'bt-', 'bf+', 'bf-', 'bdnz', 'bdnz+', 'bdnz-',
        'bdz', 'bdz+', 'bdz-', 'bdnzt', 'bdnzf', 'bdnzt+', 'bdnzt-',
        'bdnzf+', 'bdnzf-', 'bc', 'bca'}
CALL = {'bl', 'bla', 'bctrl', 'blrl'}
INDIRECT = {'blr', 'bctr', 'bctrl', 'bclr', 'blrl'}
SYSCALL = {'sc'}
BRANCH = UNCOND | COND | CALL | INDIRECT | SYSCALL

def is_branch(m):
    if m in BRANCH:
        return True
    if m.startswith('b') and m not in ('b',) and m[1:2] not in ('t', 'f', 'd', 'c', 'l', 'a'):
        return False
    return m.startswith('b')

def parse(fn):
    """Yield (addr, mnem, ops) in file order."""
    with open(fn, 'r', errors='replace') as f:
        for line in f:
            m = LINE.match(line)
            if m:
                yield int(m.group(1), 16), m.group(2), m.group(3).strip()

def pass1(fn):
    targets = set()
    n = 0
    unknown_b = Counter()
    for addr, mn, ops in parse(fn):
        n += 1
        if is_branch(mn):
            if mn not in BRANCH and mn.startswith('b') and mn not in unknown_b:
                unknown_b[mn] += 1
            if mn in UNCOND or mn in COND or mn in CALL:
                for t in TARGET.findall(ops):
                    targets.add(int(t, 16))
    return targets, n, unknown_b

def chunk_blocks(instrs, targets):
    """instrs: list of (addr, mn, ops) contiguous.  Return list of blocks.

    Each block: dict(start, end, n, term, target, fallthrough_addr, succs)
    """
    leaders = {instrs[0][0]}
    for i, (a, mn, ops) in enumerate(instrs):
        if is_branch(mn):
            if i + 1 < len(instrs):
                leaders.add(instrs[i + 1][0])
            if mn in UNCOND or mn in COND:
                for t in TARGET.findall(ops):
                    leaders.add(int(t, 16))
    blocks = []
    cur = None
    for i, (a, mn, ops) in enumerate(instrs):
        if cur is None or a in leaders:
            if cur:
                blocks.append(cur)
            cur = {'start': a, 'end': a, 'n': 0, 'term': None, 'target': None,
                   'fall': None, 'succs': [], 'addr': a, 'mn': mn}
        cur['n'] += 1
        cur['end'] = a
        if is_branch(mn):
            cur['term'] = mn
            cur['mn'] = mn
            if mn in UNCOND or mn in COND:
                ts = [int(t, 16) for t in TARGET.findall(ops)]
                cur['target'] = ts[0] if ts else None
            if i + 1 < len(instrs):
                cur['fall'] = instrs[i + 1][0]
    if cur:
        blocks.append(cur)
    return blocks

def pct(vals, p):
    if not vals:
        return 0
    v = sorted(vals)
    k = min(len(v) - 1, max(0, int(round((p / 100.0) * (len(v) - 1)))))
    return v[k]

def stats(name, vals, unit=''):
    if not vals:
        print(f'  {name}: none')
        return
    print(f'  {name}: n={len(vals)} mean={statistics.fmean(vals):.2f}{unit} '
          f'median={pct(vals,50)}{unit} p90={pct(vals,90)}{unit} '
          f'p99={pct(vals,99)}{unit} max={max(vals)}{unit}')

def buckets(vals, edges):
    out = []
    lo = 0
    for e in edges:
        out.append(sum(1 for v in vals if lo <= v < e))
        lo = e
    out.append(sum(1 for v in vals if v >= lo))
    return out

def analyze(fn):
    targets, ninstr, unknown_b = pass1(fn)
    print(f'\n=== {fn} ===')
    print(f'instructions: {ninstr:,}   direct branch targets: {len(targets):,}')
    if unknown_b:
        print('unknown branch-ish mnemonics:', dict(unknown_b))

    blocks = []
    chunk = []
    prev = None
    total_chunks = 0
    for a, mn, ops in parse(fn):
        if prev is not None and a != prev + 4:
            if chunk:
                blocks.extend(chunk_blocks(chunk, targets))
                total_chunks += 1
            chunk = []
        chunk.append((a, mn, ops))
        prev = a
    if chunk:
        blocks.extend(chunk_blocks(chunk, targets))
        total_chunks += 1

    # index and predecessor edges
    by_addr = {}
    for bi, b in enumerate(blocks):
        b['id'] = bi
        by_addr[b['start']] = bi
    preds = defaultdict(set)
    for bi, b in enumerate(blocks):
        t = b['term']
        if t is None:
            continue
        if t in UNCOND:
            if b['target'] in by_addr:
                preds[by_addr[b['target']]].add(bi)
        elif t in COND:
            if b['target'] in by_addr:
                preds[by_addr[b['target']]].add(bi)
            if b['fall'] in by_addr:
                preds[by_addr[b['fall']]].add(bi)
    # block that is the target of an unconditional/conditional branch anywhere
    branch_target_blocks = {by_addr[t] for t in targets if t in by_addr}

    blens = [b['n'] for b in blocks]
    print(f'contiguous chunks: {total_chunks:,}   basic blocks: {len(blocks):,}   '
          f'blocks/chunk={len(blocks)/max(1,total_chunks):.2f}')
    print(f'branch density: {sum(1 for b in blocks if b["term"]) / max(1,ninstr)*100:.1f}% of instructions end a block')
    stats('block length (guest insns)', blens)
    edges = [1, 3, 6, 11, 21, 41, 81, 161]
    print('  block-length histogram:', dict(zip(
        ['1-2', '3-5', '6-10', '11-20', '21-40', '41-80', '81-160', '161+'],
        buckets(blens, edges))))
    print(f'  blocks with >1 predecessor: {sum(1 for b in blocks if len(preds[b["id"]])>1)/len(blocks)*100:.1f}%'
          f'   with 0 predecessors: {sum(1 for b in blocks if not preds[b["id"]])/len(blocks)*100:.1f}%'
          f'   branch-targeted: {len(branch_target_blocks)/len(blocks)*100:.1f}%')

    def chain(b0, conservative):
        members = {b0}
        cur = b0
        length = 0
        steps = 0
        while True:
            b = blocks[cur]
            length += b['n']
            steps += 1
            if length >= 256:
                return length, 'budget'
            t = b['term']
            if t is None:
                nxt = by_addr.get(b['end'] + 4)
                reason = 'end-of-chunk'
            elif t in INDIRECT:
                return length, 'indirect/return'
            elif t in SYSCALL:
                return length, 'syscall'
            elif t in CALL:
                nxt = by_addr.get(b['fall'])
                reason = 'after-call'
            elif t in UNCOND:
                nxt = by_addr.get(b['target'])
                reason = 'uncond'
            elif t in COND:
                nxt = by_addr.get(b['fall'])
                reason = 'cond-fallthrough'
            else:
                return length, 'other'
            if nxt is None:
                return length, 'out-of-chunk'
            if nxt in members:
                return length, 'loop'
            outside = preds[nxt] - members
            if outside:
                return length, 'external-pred'
            if conservative and nxt in branch_target_blocks:
                return length, 'entry-block'
            members.add(nxt)
            cur = nxt
            if steps > 4096:
                return length, 'runaway'

    # What the current translator actually inlines in one compiled function:
    # CallFunction follows unconditional direct b chains; a conditional branch
    # ends the function (UseCondition emits the fallthrough call, BC the taken call).
    len_now = []
    for b in blocks:
        cur = b['id']
        L = 0
        steps = 0
        seen = set()
        while True:
            bb = blocks[cur]
            L += bb['n']
            steps += 1
            t = bb['term']
            if t in UNCOND:
                nxt = by_addr.get(bb['target'])
                if nxt is None or nxt in seen:
                    break
                seen.add(nxt)
                cur = nxt
                if steps > 4096:
                    break
            else:
                break
        len_now.append(L)
    print('\n  current inline chain (unconditional b only; a conditional branch ends it):')
    stats('  length', len_now)
    print('  length histogram:', dict(zip(
        ['1-2', '3-5', '6-10', '11-20', '21-40', '41-80', '81+'],
        buckets(len_now, [3, 6, 11, 21, 41, 81]))))

    # Natural-loop bodies: a direct branch whose target is earlier in the address
    # space.  Body size approximated as the instruction span target..branch.
    loops = []
    back = 0
    fwd = 0
    for b in blocks:
        t = b['term']
        if t not in (UNCOND | COND) or b['target'] is None:
            continue
        if b['target'] < b['start']:
            back += 1
            span = (b['end'] - b['target']) // 4 + 1
            if 2 <= span <= 8192:
                loops.append(span)
        else:
            fwd += 1
    print(f'\n  back-edge branches: {back:,}  forward branches: {fwd:,}  '
          f'back/fwd={back/max(1,fwd+fwd*0+back)*100:.1f}%')
    stats('loop body span (guest insns)', loops)
    print('  loop-body histogram:', dict(zip(
        ['2-9', '10-19', '20-39', '40-79', '80-159', '160-319', '320+'],
        buckets(loops, [10, 20, 40, 80, 160, 320]))))

    # Loop-header region: BFS from each back-edge target, restricted to the span
    # [header, back-edge].  Both successors are followed (this is the doc's
    # worklist), so it measures what a loop-first region can actually absorb.
    headers = {}
    for b in blocks:
        t = b['term']
        if t in (UNCOND | COND) and b['target'] is not None and b['target'] < b['start']:
            h = b['target']
            headers[h] = max(headers.get(h, 0), b['end'])
    loop_regions = []
    covered = set()
    for h, hi in headers.items():
        if h not in by_addr:
            continue
        start_id = by_addr[h]
        seen = {start_id}
        work = [start_id]
        L = 0
        while work:
            cur = work.pop()
            bb = blocks[cur]
            if not (h <= bb['start'] <= hi):
                continue
            L += bb['n']
            covered.add(cur)
            t = bb['term']
            succ = []
            if t in UNCOND:
                succ = [by_addr.get(bb['target'])]
            elif t in COND:
                succ = [by_addr.get(bb['target']), by_addr.get(bb['fall'])]
            elif t is None:
                succ = [by_addr.get(bb['end'] + 4)]
            for s in succ:
                if s is not None and s not in seen and h <= blocks[s]['start'] <= hi:
                    seen.add(s)
                    work.append(s)
        loop_regions.append(L)
    print(f'\n  loop-header regions: {len(loop_regions):,} headers, '
          f'{sum(loop_regions):,} insns absorbed '
          f'({sum(loop_regions)/max(1,ninstr)*100:.1f}% of the section)')
    stats('  loop-region length', loop_regions)
    print('  loop-region histogram:', dict(zip(
        ['2-9', '10-19', '20-39', '40-79', '80-159', '160-319', '320+'],
        buckets(loop_regions, [10, 20, 40, 80, 160, 320]))))
    ge40 = sum(1 for x in loop_regions if x >= 40)
    print(f'  loop regions >=40 insns: {ge40/max(1,len(loop_regions))*100:.1f}%   '
          f'>=100: {sum(1 for x in loop_regions if x>=100)/max(1,len(loop_regions))*100:.1f}%')

    for conservative in (False, True):
        seen = set()
        lens = []
        reasons = Counter()
        for b in blocks:
            if b['id'] in seen:
                continue
            L, r = chain(b['id'], conservative)
            # mark all blocks along the chain as consumed
            cur = b['id']
            while cur is not None and cur not in seen:
                seen.add(cur)
                bb = blocks[cur]
                t = bb['term']
                nxt = None
                if t in UNCOND:
                    nxt = by_addr.get(bb['target'])
                elif t in COND or t in CALL:
                    nxt = by_addr.get(bb['fall'])
                elif t is None:
                    nxt = by_addr.get(bb['end'] + 4)
                if nxt is None or nxt in seen:
                    break
                if (preds[nxt] - set()) and not (preds[nxt] <= seen):
                    break
                if conservative and nxt in branch_target_blocks:
                    break
                cur = nxt
            lens.append(L)
            reasons[r] += 1
        print(f'\n  region chain (conservative={conservative}):')
        stats('  length', lens)
        print('  length histogram:', dict(zip(
            ['1-9', '10-19', '20-39', '40-79', '80-159', '160-255', '256+'],
            buckets(lens, [10, 20, 40, 80, 160, 256]))))
        print('  stop reasons:', dict(reasons.most_common()))
        tail = [x for x in lens if x >= 40]
        print(f'  regions >=40 insns: {len(tail)/len(lens)*100:.1f}%   '
              f'>=100: {sum(1 for x in lens if x>=100)/len(lens)*100:.1f}%')
    return blocks, blens

for fn in sys.argv[1:]:
    analyze(fn)