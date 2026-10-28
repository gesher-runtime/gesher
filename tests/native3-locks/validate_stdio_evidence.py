"""Validate four actual stderr witness groups; this never accepts missing WCT."""
import argparse
import json
import os
from pathlib import Path
import re


def fields(line):
    return dict(re.findall(r'(\S+)=(\S+)', line))


def hexvalue(value):
    return int(value, 16)


def exactly(lines, prefix):
    selected = [line for line in lines if line.startswith(prefix)]
    assert len(selected) == 1, (prefix, len(selected))
    return fields(selected[0])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--stderr', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    assert os.environ.get('GITHUB_ACTIONS') == 'true', 'CI-only actual witness validation'
    lines = args.stderr.read_text(encoding='utf-8-sig').splitlines()
    layouts = [fields(line) for line in lines if line.startswith('ACTUAL_CRT_LOCK_LAYOUT ')]
    assert len(layouts) == 2 and all(row['decoded'] == 'true' for row in layouts), 'Both actual workers must decode CRT'
    offsets = {int(row['offset']) for row in layouts}
    assert len(offsets) == 1 and next(iter(offsets)) > 0
    offset = next(iter(offsets))
    groups = []
    for line in lines:
        if line.startswith('ACTUAL_STDIO_CLOCK ') and fields(line)['phase'] == 'start':
            groups.append([])
        if groups:
            groups[-1].append(line)
    assert len(groups) == 4, ('missing required real contention cycles', len(groups))
    expected_phases = ['start', 'held-and-entered', 'sample-and-negatives', 'independent-close',
                       'unlock', 'actual-thread-exit-and-join', 'control-cleanup']
    summaries = []
    for index, group in enumerate(groups, 1):
        clocks = [fields(line) for line in group if line.startswith('ACTUAL_STDIO_CLOCK ')]
        assert [row['phase'] for row in clocks] == expected_phases
        assert all(int(row['cycle']) == index for row in clocks)
        assert len({row['deadline'] for row in clocks}) == 1, 'Absolute deadline reset'
        ticks = [int(row['tick']) for row in clocks]
        deadline = int(clocks[0]['deadline'])
        assert ticks == sorted(ticks) and 0 < deadline - ticks[0] <= 5000 and ticks[-1] < deadline
        records = [fields(line) for line in group if line.startswith('ACTUAL_CRT_FILE_OWNER ')]
        assert {row['phase'] for row in records} == {'before', 'held', 'waiting', 'negative-sibling'} and len(records) == 4
        by_phase = {row['phase']: row for row in records}
        before, held, waiting, other = (by_phase[name] for name in ('before', 'held', 'waiting', 'negative-sibling'))
        assert len({(row['FILE'], row['cs'], row['expected-owner'], row['owner-birth']) for row in (before, held, waiting)}) == 1
        assert hexvalue(before['cs']) == hexvalue(before['FILE']) + offset
        assert before['owner'] == before['recursion'] == '0'
        assert held['owner'] == waiting['owner'] == held['expected-owner'] and held['recursion'] == waiting['recursion'] == '1'
        assert int(held['owner-birth']) > 0
        assert hexvalue(other['FILE']) != hexvalue(held['FILE']) and hexvalue(other['cs']) != hexvalue(held['cs'])
        assert hexvalue(other['cs']) == hexvalue(other['FILE']) + offset
        assert other['owner'] == held['owner'] and other['recursion'] == '1'
        stack = exactly(group, 'ACTUAL_CLOSER_STACK ')
        assert stack['captured'] == stack['closed'] == '1' and stack['error'] == '0'
        assert stack['suspend-before'] == '0' and stack['resume-before'] == '1' and int(stack['birth']) > 0
        call = exactly(group, 'ACTUAL_CLOSER_FILE_CALL index=')
        assert hexvalue(call['unwound-rsi']) == hexvalue(call['FILE']) == hexvalue(held['FILE']) == hexvalue(stack['FILE'])
        assert call['exact-mov-rsi-rcx-and-lock-file-iat'] == 'true'
        enter = exactly(group, 'ACTUAL_CLOSER_ENTER_FRAME ')
        begin, end = hexvalue(enter['unwind-begin']), hexvalue(enter['unwind-end'])
        assert begin <= hexvalue(enter['pc']) < end and begin <= hexvalue(enter['named-entry']) < end
        frame_rows = [fields(line) for line in group if line.startswith('ACTUAL_CLOSER_FRAME ')]
        assert len(frame_rows) == int(stack['frames']) <= 32
        assert {int(row['index']) for row in frame_rows} == set(range(len(frame_rows)))
        frames = {row['index']: row for row in frame_rows}
        assert frames[enter['index']]['pc'] == enter['pc'] and frames[call['index']]['pc'] == call['pc']
        summary = exactly(group, 'ACTUAL_CLOSER_FILE_CALL_SUMMARY ')
        assert summary['matches'] == summary['required'] == summary['enter-frames'] == '1' and summary['other-matches'] == '0'
        assert hexvalue(summary['other-FILE']) == hexvalue(other['FILE'])
        trace = exactly(group, 'ACTUAL_WCT samples=')
        assert trace['success'] == '1' and trace['error'] == trace['cycle'] == trace['completed-before-unlock'] == '0'
        assert trace['closer'] == stack['closer'] and trace['owner'] == held['owner'] and hexvalue(trace['FILE']) == hexvalue(held['FILE'])
        nodes = [fields(line) for line in group if line.startswith('ACTUAL_WCT_NODE ')]
        assert len(nodes) == int(trace['count']) and nodes[0]['type'] == '8' and nodes[0]['status'] == '3'
        assert nodes[0]['tid'] == stack['closer'] and nodes[0]['pid'] == stack['pid']
        if len(nodes) > 1:
            assert len(nodes) >= 3 and nodes[1]['type'] == '1' and nodes[2]['type'] == '8'
            assert nodes[2]['tid'] == held['owner'] and nodes[2]['pid'] == stack['pid']
        assert group.count('ACTUAL_STDIO_PHASE independent-fd-closed result=0 closer-completed-before-unlock=0') == 1
        assert group.count('ACTUAL_STDIO_PHASE stream-unlocked') == 1
        negative = exactly(group, 'ACTUAL_CRT_NEGATIVE_CONTROLS ')
        expected_negatives = {'wrong-live-FILE-CS', 'wrong-live-owner', 'actual-other-birth',
                              'completed-real-closer', 'unsupported-code-copy', 'call-other-FILE-argument'}
        assert set(negative) == expected_negatives and set(negative.values()) == {'reject'}
        values = exactly(group, 'ACTUAL_CRT_NEGATIVE_VALUES ')
        assert int(values['cycle']) == index and values['closer'] == values['wrong-owner'] == stack['closer']
        assert values['closer-birth'] == stack['birth'] and values['wrong-birth'] == held['owner-birth']
        assert values['wrong-birth'] != values['closer-birth'] and values['wrong-owner'] != held['owner']
        assert values['completed-control-close'] == '0' and values['completed-control-signaled'] == 'true'
        assert int(values['completed-control-tid']) > 0 and int(values['completed-control-birth']) > 0
        copies = exactly(group, 'WCT_NEGATIVE_METADATA_COPIES ')
        assert copies == dict(error='reject', cycle='reject', **{'wrong-live-thread': 'reject', 'not-actual-OS-cycle': 'true'})
        summaries.append(dict(cycle=index, file=held['FILE'], cs=held['cs'], owner=held['owner'],
                              owner_birth=held['owner-birth'], closer=stack['closer'], closer_birth=stack['birth'],
                              elapsed_ms=ticks[-1] - ticks[0], wct_count=len(nodes), actual_enter_frame=enter,
                              real_negative_cases=sorted(expected_negatives), runtime_acceptance=False))
    args.output.write_text(json.dumps(dict(source=os.environ['GITHUB_SHA'], run=os.environ['GITHUB_RUN_ID'],
                                           attempt=os.environ['GITHUB_RUN_ATTEMPT'], cycles=summaries), indent=2) + '\n')
    print('STDIO_DIAGNOSTIC_EVIDENCE_COMPLETE cycles=4 runtime_acceptance=false', flush=True)


if __name__ == '__main__':
    main()
