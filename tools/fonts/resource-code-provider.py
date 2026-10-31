"""Pure SIGNED_SYSTEM32_UCRT_CODE byte admission; never opens a path.

CI parent owns the signature, native System32 path and retained Read/ShareRead
handle. This module authenticates its full framed bytes and caches only111
bytes. Disk equality is not loaded-image trust, alignment or lock ownership.
"""
import hashlib
import re
import struct

MAX_BYTES = 128 * 1024 * 1024
MAX_SECTIONS = 96
ROLE = 'SIGNED_SYSTEM32_UCRT_CODE'
PROFILES = {
 'd06d0a421398fb663c6c49586e09774143460fdb1dce489aa9844846b9398555':
     ('10.0.20348.5756', 0xb84b4, 0x110000),
 '9601850e2ede2cd86b441223557cae0fa7f5e33b1dd9d481c58c8cc2d688bdc3':
     ('10.0.26100.33648', 0x3c1d8, 0x14b000),
}

def require(value, reason):
    if not value:
        raise ValueError(reason)

def window(data, start):
    """Synthetic-memory-testable PE mapper; no import decoder changes."""
    require(type(data) is bytes and 64 <= len(data) <= MAX_BYTES, 'provider size')
    require(type(start) is int and 0 <= start <= 0xffffffff-111, 'window overflow')
    def span(at, count):
        require(type(at) is int and 0 <= at <= len(data) and 0 <= count <= len(data)-at,
                'truncated provider')
    def unpack(fmt, at):
        span(at, struct.calcsize(fmt)); return struct.unpack_from(fmt, data, at)
    require(data[:2] == b'MZ', 'DOS signature')
    pe, = unpack('<I', 60); require(pe >= 64, 'DOS overlap')
    span(pe, 24); require(data[pe:pe+4] == b'PE\0\0', 'PE signature')
    machine, count = unpack('<HH', pe+4)
    require(machine == 0x8664 and 1 <= count <= MAX_SECTIONS, 'AMD64 section count')
    opt_size, = unpack('<H', pe+20); opt = pe+24
    span(opt, opt_size); require(opt_size >= 112, 'optional header size')
    require(unpack('<H', opt)[0] == 0x20b, 'PE32+')
    image_size, headers = unpack('<II', opt+56)
    table = opt+opt_size; span(table, count*40)
    require(table+count*40 <= headers <= len(data) and headers <= image_size,
            'headers/image bounds')
    require(start+111 <= image_size, 'window image bound')
    virtual = [(0, headers)]; raw_ranges = [(0, headers)]; selected = []
    for index in range(count):
        at = table+index*40
        vs, rva, size, raw = unpack('<IIII', at+8)
        flags, = unpack('<I', at+36); extent = max(vs, size)
        require(extent > 0 and rva > 0 and rva+extent <= min(1 << 32, image_size),
                'section virtual bounds')
        require(all(rva+extent <= lo or rva >= hi for lo, hi in virtual), 'virtual overlap')
        virtual.append((rva, rva+extent))
        if size:
            span(raw, size)
            require(all(raw+size <= lo or raw >= hi for lo, hi in raw_ranges), 'raw overlap')
            raw_ranges.append((raw, raw+size))
        if rva <= start and start+111 <= rva+size:
            require(flags & 0x20000000, 'window not executable')
            selected.append((raw+start-rva, index))
    require(len(selected) == 1, 'window not uniquely file-backed')
    offset, section = selected[0]; span(offset, 111)
    cache = data[offset:offset+111]
    return {'start_rva':start, 'end_rva':start+95, 'cache_length':111,
            'comparison_last_rva':start+109, 'file_offset':offset, 'section':section,
            'size_of_image':image_size, 'cache_hex':cache.hex(),
            'cache_sha256':hashlib.sha256(cache).hexdigest()}

def admit(data, entry, context):
    require(type(entry) is dict and set(entry) == {'role','name','bytes','sha256','version'},
            'provider frame schema')
    require(entry['role'] == ROLE and entry['name'] == 'ucrtbase.dll', 'provider role/name')
    require(type(entry['bytes']) is int and 64 <= entry['bytes'] <= MAX_BYTES, 'provider bytes')
    require(type(data) is bytes and len(data) == entry['bytes'], 'provider byte count')
    digest = hashlib.sha256(data).hexdigest()
    require(entry['sha256'] == digest and digest in PROFILES, 'provider exact profile hash')
    version, start, extent = PROFILES[digest]
    require(entry['version'] == version, 'provider version')
    require(type(context) is dict and set(context) == {'source','run','attempt'}, 'context shape')
    require(type(context['source']) is str and re.fullmatch('[0-9a-f]{40}',context['source']), 'source')
    require(all(type(context[k]) is str and re.fullmatch('[1-9][0-9]{0,19}',context[k])
                for k in ('run','attempt')), 'run/attempt')
    result = window(data, start); require(result['size_of_image'] == extent, 'profile image extent')
    import json
    record_hash = hashlib.sha256(json.dumps(entry,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    key = '\n'.join((context['source'],context['run'],context['attempt'],ROLE,digest,
                     format(start,'x'),'111',record_hash))
    return {'record':'PROVIDER_CACHE','role':ROLE,'profile':entry,'context':context,
            'input_record_sha256':record_hash,
            'cache_key_sha256':hashlib.sha256(key.encode('ascii')).hexdigest(), **result,
            'instruction_boundary_proved':False,'owner_qualified':False}
