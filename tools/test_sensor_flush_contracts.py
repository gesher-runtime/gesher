#!/usr/bin/env python3
"""Bounded sensor queue and sampler-order tests using production code.

Windows sensor reads are replaced by two in-memory Sources. The production
Sampler, subscription locking, EventBuffer and callback order are retained.
--source-ref compiles the old JNI queue's actual Flush/Push bodies as a
negative control for lost or partially admitted completion markers.
"""
from __future__ import annotations
import argparse, hashlib, os, shlex, subprocess, tempfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
def body(source,start):
    a=source.index(start);b=source.index('{',a);d=1;c=b+1
    while d:d+=(source[c]=='{')-(source[c]=='}');c+=1
    return source[a:c]
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source-ref');p.add_argument('--sanitize',action='store_true')
    p.add_argument('--cxx',default=os.environ.get('CXX','c++'));a=p.parse_args()
    def read(path):
        return subprocess.check_output(['git','show',a.source_ref+':'+path],cwd=ROOT,text=True) if a.source_ref else (ROOT/path).read_text()
    source=read('compat/devices/sensors.cc');jni=read('compat/devices/sensors_jni.cc')
    current='hub::EventBuffer events_' in jni
    if current:
        buffer='#include "sensor_event_buffer.h"\nusing TestBuffer = gesher::devices::sensors::EventBuffer;'
    else:
        # Only the eventfd wake is omitted. Actual overflow, ordering and flush
        # insertion bodies are unchanged; the adapter exposes their return status.
        flush=body(jni,'  void Flush(const std::vector<int>& handles)')
        push=body(jni,'  void Push(const Event& e)')
        push=push[:push.index('    uint64_t one = 1;')]+'  }'
        buffer='''#include "sensor_event_buffer.h"
using Event = gesher::devices::sensors::Event;
namespace hub=gesher::devices::sensors;
class TestBuffer {
public:
bool PushData(const Event& e) { Push(e); return true; }
bool FlushRequest(const std::vector<int>& handles) { Flush(handles); return true; }
std::deque<Event> Take() { std::deque<Event> result; result.swap(events_); return result; }
'''+flush+'\n'+push+'''
std::mutex mutex_; std::deque<Event> events_;
};'''
    template=(ROOT/'tests/devices/sensor_flush_host.cc.in').read_text()
    sampler=source[source.index('struct Subscription {'):source.index('\n}  // namespace',source.index('struct Subscription {'))]
    template=template.replace('@BUFFER@',buffer).replace('@SAMPLER@',sampler if current else '')
    template=template.replace('@NEW_SAMPLER@','1' if current else '0')
    if not current:template=template.replace('buffer.Flush(', 'buffer.FlushRequest(')
    helpers = {}
    for name in ('sensor_event_buffer.h', 'sensors.h'):
        path = 'compat/devices/' + name
        probe = subprocess.run(['git','show',a.source_ref+':'+path],cwd=ROOT,text=True,
                               capture_output=True) if a.source_ref else None
        helpers[name] = probe.stdout if probe is not None and probe.returncode == 0 else (ROOT/path).read_text()
    print('sensor source SHA256',hashlib.sha256((source+jni+''.join(helpers.values())).encode()).hexdigest(),flush=True)
    with tempfile.TemporaryDirectory(prefix='gesher-sensor-flush-') as temp:
        src=Path(temp)/'probe.cc';exe=Path(temp)/'probe';src.write_text(template)
        for name, value in helpers.items(): (Path(temp)/name).write_text(value)
        command=shlex.split(a.cxx)+['-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
            '-I'+str(temp),'-I'+str(ROOT/'compat/devices'),str(src),'-o',str(exe)]
        if a.sanitize:command+=['-fsanitize=address,undefined','-fno-sanitize-recover=all','-fno-omit-frame-pointer']
        subprocess.run(command,check=True,timeout=120)
        return subprocess.run([str(exe)],timeout=30).returncode
if __name__=='__main__':raise SystemExit(main())
