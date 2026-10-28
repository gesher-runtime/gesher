# Requirement matrix: bounded integration survey, 2026-10-29

This is a navigation and evidence matrix, not an exhaustive inventory. It covers the explicitly inspected roadmap entries, latest accepted handoff and current source branches. The prior conversation surveyed only 18 units; remaining roadmap, ADRs, status sections and historical handoffs have not all been independently resurveyed. Open or historical checkboxes are not evidence of aggregate completion. The authoritative detailed requirements remain in their referenced sources.

| Area | Evidence and status | Remaining boundary / source |
| --- | --- | --- |
| Integration | full40731915058/source81fd, 38 jobs, 32 success/6 intentional skips; f155 merge non-doc equal; 1157 inventory unchanged | New code union requires fresh full; [acceptance](tracks/coherent-aot-native-40731915058-accepted.md) |
| TSF | component40769699534/source8ad, both Windows 74 fresh checks and seven controller controls audited in C:/work/gesher-ci-review/tsf-8ad-40769699534-integrator-actual-audit.md | Coherent full; rich formats, insertion and hardware remain open; [Windows](tracks/windows4-final.md) |
| Services | controlled worker40751467438/source d9e6, detailed C:/work/gesher-ci-review/services-40751467438-integrator-actual-audit.json | Later supervisor correction lacks exact-source proof; real provider and positive geocoding; [brief](tracks/services3.md) |
| Binder authority | 3c1c source-only; peer C:/work/gesher-ci-review/binder-private-admission-lifetime-peer-high-3c1c066d.md | Private authority/runtime, failure injection including Publish WAIT_FAILED, secure CLI/cohost/harness and full regressions; C:/work/gesher-binder-peer-identity-current/docs/binder-private-admission-lifetime-source.md |
| Secondary AOT | d4d source-only; previous a5ab peer blocked authority, receipt and budget; corrections require fresh peer | Authentic Binder trust root, exact component rebuild and full; general loaders, profiles, filesystem races; C:/work/gesher-aot3-resource-current/docs/handoff/tracks/aot3-secondary-current.md, [roadmap](../roadmap.md) |
| Native | accepted selected scope in full407319; blocking4711 source component pending | Record-lock blocking/signals, raw CLOEXEC and remaining public NDK/POSIX/inotify; [brief](tracks/native3.md), [roadmap](../roadmap.md) |
| Exec | previously bounded full verified | Shell permissions, broader conformance, console interruption, property policy, fork/static; [roadmap](../roadmap.md), [brief](tracks/exec4.md) |
| ARM | bounded environment/FPCR/FPSR/varargs evidence; aggregate open | Guest JIT, signals/ucontext, LSE, HFA/binary128, cache/stack and CRT/libm; [roadmap](../roadmap.md), [brief](tracks/arm64-2.md) |
| Media/hardware | bounded media slices accepted | DXVA, physical audio, device routes, remaining exports, container/protected media; [roadmap](../roadmap.md), [brief](tracks/media4.md). Owner explicitly confirms no interactive hardware Windows CI exists; physical proof is external blocker. |
| Web4 | host-object407153 red: missing frame event on 2022, opaque-frame oracle failed on 2025 | Lifecycle and production qualification open; [brief](tracks/web4.md) |
| Main/release | no final PR, tag or release | All known requirements and actual full/installer/provenance proof before owner-preauthorized merge and release; [checkpoint](session-2026-10-28-aot-native-full.md) |

Further survey required: all other roadmap and ADR entries and every relevant track Status and next steps, including cross-cutting platform and device obligations. Preserve their evidence and do not infer closure from this bounded table.
