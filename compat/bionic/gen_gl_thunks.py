#!/usr/bin/env python3
"""Generates the System V -> Microsoft x64 thunks for EGL and OpenGL ES.

  gen_gl_thunks.py <out.inc> <opengl include dir> <handwritten names...>

An app's .so imports eglXxx/glXxx from libEGL.so, libGLESv1_CM.so,
libGLESv2.so and libGLESv3.so and calls them with the System V ABI; Gesher's
EGL/GLES is ANGLE's libEGL.dll/libGLESv2.dll, whose exports are Microsoft
x64. For every function the Khronos headers (AOSP's
frameworks/native/opengl/include, the headers the NDK ships) declare, this
writes a `sysv_abi` function with the same prototype that calls ANGLE's entry
point through a pointer of the Microsoft-ABI type; the compiler does the
argument shuffle (integer and floating-point registers, the stack, the
callee-saved xmm6-15). Pointers are resolved on first call
(compat/bionic/egl_gles.cc: ResolveGl).

The output (included by egl_gles.cc) has, inside namespace
gesher::bionic::gl:
  - enum I_<name>: the index of every function, handwritten ones included;
  - kGlNames[]: {name, library} for each index;
  - T_<name>(): the thunks (not for handwritten names);
  - kGlThunks[]: {name, thunk} for every generated thunk.

Functions taking or returning a callback type (GLDEBUGPROC, EGLDEBUGPROCKHR,
the blob cache functions, GLVULKANPROCNV, eglGetProcAddress) cannot be
forwarded as they are: the callback is System V code that ANGLE would call
with the Microsoft ABI. They must be handwritten (named on the command line)
or are left out (listed in the output); a new one fails the build so it is
not missed.
"""
import os
import re
import sys

# Headers, in the order that decides which declaration of a name is used
# (ES 3.2 first: it redeclares everything of ES 2.0/3.0/3.1).
HEADERS = [
    'EGL/egl.h', 'EGL/eglext.h',
    'GLES3/gl32.h', 'GLES3/gl31.h', 'GLES3/gl3.h', 'GLES2/gl2.h', 'GLES2/gl2ext.h',
    'GLES/gl.h', 'GLES/glext.h',
]

# Extensions ANGLE advertises that AOSP's headers do not declare (ANGLE's
# include/GLES2/gl2ext_angle.h). A client that sees one in GL_EXTENSIONS takes
# its functions as required: Skia's GrGLInterface fails validation, and
# Flutter's Skia renderer draws nothing, when glBindUniformLocationCHROMIUM is
# null.
EXTRA = 'ANGLE gl2ext_angle.h'
EXTRA_PROTOTYPES = [
    # GL_CHROMIUM_bind_uniform_location
    'GL_APICALL void GL_APIENTRY glBindUniformLocationCHROMIUM (GLuint program, GLint location, '
    'const GLchar *name);',
    # GL_CHROMIUM_copy_texture
    'GL_APICALL void GL_APIENTRY glCopyTextureCHROMIUM (GLuint sourceId, GLint sourceLevel, '
    'GLenum destTarget, GLuint destId, GLint destLevel, GLint internalFormat, GLenum destType, '
    'GLboolean unpackFlipY, GLboolean unpackPremultiplyAlpha, GLboolean unpackUnmultiplyAlpha);',
    'GL_APICALL void GL_APIENTRY glCopySubTextureCHROMIUM (GLuint sourceId, GLint sourceLevel, '
    'GLenum destTarget, GLuint destId, GLint destLevel, GLint xoffset, GLint yoffset, GLint x, '
    'GLint y, GLint width, GLint height, GLboolean unpackFlipY, GLboolean unpackPremultiplyAlpha, '
    'GLboolean unpackUnmultiplyAlpha);',
    # GL_CHROMIUM_framebuffer_mixed_samples
    'GL_APICALL void GL_APIENTRY glCoverageModulationCHROMIUM (GLenum components);',
]

CALLBACK_TYPES = ('GLDEBUGPROC', 'GLDEBUGPROCKHR', 'EGLDEBUGPROCKHR', 'EGLSetBlobFuncANDROID',
                  'EGLGetBlobFuncANDROID', 'GLVULKANPROCNV', '__eglMustCastToProperFunctionPointerType')
# Taking a callback, not forwarded and not handwritten: not provided (the
# loader's stub names them if an app calls one).
LEFT_OUT = {'glGetVkProcAddrNV'}

# Types the headers spell with `long` (khrplatform.h's khronos_intptr_t and
# khronos_ssize_t are `signed long int`: AOSP's copy is for LP64 Android). The
# .so passes them as 64 bits and ANGLE's own headers make them 64 bits on Win64,
# but the shim is compiled with mingw, where `long` is 32 bits: as the header
# spells them, a thunk would drop the upper half and, for an argument on the
# Microsoft stack (the fifth and later), store only 4 of the slot's 8 bytes,
# leaving garbage above (glBindBufferRange's size: Flutter's Impeller then drew
# nothing but its clear colour). The thunks spell them with a fixed width.
LP64_TYPES = {
    'GLintptr': 'int64_t',
    'GLsizeiptr': 'int64_t',
    'EGLsizeiANDROID': 'int64_t',
    'khronos_intptr_t': 'int64_t',
    'khronos_ssize_t': 'int64_t',
    'khronos_uintptr_t': 'uint64_t',
    'khronos_usize_t': 'uint64_t',
}
LP64_TYPE = re.compile(r'\b(' + '|'.join(LP64_TYPES) + r')\b')


def fixed_width(t):
    return LP64_TYPE.sub(lambda m: LP64_TYPES[m.group(1)], t)


PROTO = re.compile(r'^(?:GL_APICALL|GL_API|EGLAPI)\s+(?P<ret>.*?)\s*\b(?:GL_APIENTRY|EGLAPIENTRY)\s+'
                   r'(?P<name>\w+)\s*\((?P<args>.*)\)\s*;\s*$')
PARAM = re.compile(r'^(?P<type>.*?)(?P<name>\b\w+)\s*(?P<array>\[[^\]]*\])?\s*$')


def parse(include_dir):
    funcs = {}
    order = []
    for header in HEADERS + [EXTRA]:
        if header == EXTRA:
            lines = EXTRA_PROTOTYPES
        else:
            with open(os.path.join(include_dir, header), encoding='utf-8') as f:
                lines = f.readlines()
        for line in lines:
            m = PROTO.match(line.strip())
            if not m:
                continue
            name = m.group('name')
            if name in funcs:
                continue
            ret = m.group('ret').strip()
            args = m.group('args').strip()
            params = []
            if args and args != 'void':
                for i, p in enumerate(a.strip() for a in args.split(',')):
                    pm = PARAM.match(p)
                    if not pm or not pm.group('type').strip() or pm.group('type').strip() == 'const':
                        raise SystemExit(f'{header}: cannot parse parameter "{p}" of {name}')
                    params.append((pm.group('type').strip(), pm.group('array') or ''))
            funcs[name] = (ret, params, header)
            order.append(name)
    return funcs, order


def main():
    out, include_dir, handwritten = sys.argv[1], sys.argv[2], set(sys.argv[3:])
    funcs, order = parse(include_dir)
    missing = handwritten - set(funcs)
    if missing:
        raise SystemExit(f'handwritten names not in the headers: {sorted(missing)}')
    lines = [
        '// Generated by compat/bionic/gen_gl_thunks.py from frameworks/native/opengl/include',
        '// (' + ', '.join(HEADERS) + '). Do not edit.',
        '',
        'namespace gesher::bionic::gl {',
        '',
        'enum : int {',
    ]
    for i, name in enumerate(order):
        lines.append(f'  I_{name} = {i},')
    lines += ['  kGlCount = %d,' % len(order), '};', '',
              'const GlName kGlNames[kGlCount] = {']
    for name in order:
        lib = 'kLibEgl' if name.startswith('egl') else 'kLibGles'
        lines.append(f'    {{"{name}", {lib}}},')
    lines += ['};', '']
    thunks = []
    left_out = []
    for name in order:
        ret, params, header = funcs[name]
        text = ret + ' ' + ' '.join(t for t, _ in params)
        if name in handwritten:
            continue
        if name in LEFT_OUT:
            left_out.append(name)
            continue
        if any(re.search(r'\b' + t + r'\b', text) for t in CALLBACK_TYPES):
            raise SystemExit(f'{header}: {name} takes or returns a callback; handwrite it '
                             f'(egl_gles.cc) or add it to LEFT_OUT')
        ret = fixed_width(ret)
        params = [(fixed_width(t), arr) for t, arr in params]
        decl = ', '.join(f'{t} a{i}{arr}' for i, (t, arr) in enumerate(params)) or 'void'
        types = ', '.join(f'{t}{"*" if arr else ""}' for t, arr in params) or 'void'
        call = ', '.join(f'a{i}' for i in range(len(params)))
        ret_kw = '' if ret == 'void' else 'return '
        lines.append(f'{ret} BIONIC_SYSV T_{name}({decl}) {{')
        lines.append(f'  {ret_kw}reinterpret_cast<{ret} (KHRONOS_APIENTRY*)({types})>(Fn(I_{name}))({call});')
        lines.append('}')
        thunks.append(name)
    lines += ['', 'const GlThunk kGlThunks[] = {']
    for name in thunks:
        lines.append(f'    {{"{name}", reinterpret_cast<void*>(&T_{name})}},')
    lines += ['};', '']
    lines.append('// Not provided (a callback type the thunks cannot forward): ' +
                 (', '.join(left_out) or 'none') + '.')
    lines += ['', '}  // namespace gesher::bionic::gl', '']
    tmp = out + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    os.replace(tmp, out)
    print(f'gen_gl_thunks: {len(order)} functions, {len(thunks)} generated thunks, '
          f'{len(handwritten)} handwritten, {len(left_out)} left out')


if __name__ == '__main__':
    main()
