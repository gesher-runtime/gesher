#!/usr/bin/env python3
"""RenderScript's API generator (frameworks/rs rsg_generator.c and spec.l), in Python.

  rsg_generator.py <template> <out> <spec>...

AOSP builds rsg-generator, a host C program with a flex lexer, and runs
`cat rs.spec rs_compat.spec | rsg-generator <x>_rsg <out>` for the four files
libRS is built with (frameworks/rs Android.bp, support.bp). Gesher's build has
no flex and cross-compiles from Linux and Windows hosts alike, so this is the
same generator in Python: the lexer's rules (spec.l) and the printers
(rsg_generator.c) are transcribed one for one, and the output is meant to be
the C generator's byte for byte. The template's first character chooses the
output, as in rsg_generator.c: 0 rsgApiStructs.h, 1 rsgApiFuncDecl.h,
2 rsgApi.cpp, 3 rsgApiReplay.cpp.
"""
import re
import sys

LOCAL_FIFO_PREFIX = 'LF_'
RS_PLAYBACK_PREFIX = 'rsp_'
RS_INTERNAL_PREFIX = 'rsi_'

RSG_API_CPP_DOC = (
    "/*\n"
    " * rsgApi.cpp\n"
    " * This file implements the functions responsible for sending messages\n"
    " * to the RS driver layer. The messages are sent through a FIFO that is\n"
    " * shared between the process's caller threads and driver thread.\n"
    " */\n\n")

RSG_API_REPLAY_CPP_DOC = (
    "/*\n"
    " * rsgApiReplay.cpp\n"
    " * This file implements the functions responsible for reading messages\n"
    " * sent to the RS driver layer.\n"
    " */\n\n")


class VarType:
    def __init__(self):
        self.isConst = 0
        self.type = 0
        self.bits = 0
        self.ptrLevel = 0
        self.name = ''
        self.typeName = ''


class ApiEntry:
    def __init__(self, name):
        self.name = name
        self.sync = 0
        self.handcodeApi = 0
        self.direct = 0
        self.nocontext = 0
        self.ret = VarType()
        self.params = []


# --- spec.l -------------------------------------------------------------------
ID = re.compile(r'[a-zA-Z_][a-zA-Z0-9_]*')
BUILTIN_TYPES = {
    'i8': (1, 8), 'i16': (1, 16), 'i32': (1, 32), 'i64': (1, 64),
    'u8': (2, 8), 'u16': (2, 16), 'u32': (2, 32), 'u64': (2, 64),
    'f': (3, 32), 'd': (3, 64),
}


def check_pointer_type(api, base):
    """spec.l's checkPointerType: a <name>_length parameter for each level of pointer."""
    cur = 0
    while cur < base.ptrLevel:
        t = VarType()
        t.type = 4
        t.ptrLevel = cur
        if t.ptrLevel > 0:
            t.isConst = 1
        t.typeName = 'size_t'
        depth = base.ptrLevel - cur
        if depth == 1:
            t.name = base.name + '_length'
        elif depth == 2:
            t.name = base.name + '_length_length'
        api.params.append(t)
        cur += 1


def lex(text):
    """The rules of spec.l, in its start conditions; returns the API entries."""
    apis = []
    state = 'INITIAL'
    next_state = None
    cur = None  # currType
    api = None
    line = 0
    i = 0
    n = len(text)

    def error(c):
        sys.exit(f"error: unexpected character '{c}' at line {line + 1}")

    while i < n:
        c = text[i]
        if state == 'comment':
            # "*"+"/" ends it; anything else (counting lines) is eaten.
            m = re.compile(r'\*+/').match(text, i)
            if m:
                state = 'INITIAL'
                i = m.end()
                continue
            if c == '\n':
                line += 1
            i += 1
            continue
        if c in ' \t':
            i += 1
            continue
        if c == '\n':
            line += 1
            i += 1
            continue
        if state == 'INITIAL':
            if text.startswith('/*', i):
                state = 'comment'
                i += 2
                continue
            m = ID.match(text, i)
            if not m:
                error(c)
            api = ApiEntry(m.group())
            state = 'api_entry'
            i = m.end()
            continue
        if state == 'api_entry':
            if c != '{':
                error(c)
            state = 'api_entry2'
            i += 1
            continue
        if state == 'var_type':
            m = ID.match(text, i)
            if not m:
                error(c)
            word = m.group()
            i = m.end()
            # flex takes the longest match, the first rule on a tie: a keyword
            # only when the whole identifier is the keyword.
            if word == 'const':
                cur.isConst = 1
                continue
            if word in BUILTIN_TYPES:
                cur.type, cur.bits = BUILTIN_TYPES[word]
            else:
                cur.type = 4
                cur.bits = 32
                cur.typeName = word
            state = next_state
            continue
        if state == 'api_entry_param':
            if c == '*':
                cur.ptrLevel += 1
                i += 1
                continue
            m = ID.match(text, i)
            if not m:
                error(c)
            cur.name = m.group()
            i = m.end()
            check_pointer_type(api, cur)
            state = 'api_entry2'
            continue
        if state == 'api_entry2':
            if c == '*':
                cur.ptrLevel += 1
                i += 1
                continue
            if c == '}':
                apis.append(api)
                state = 'INITIAL'
                i += 1
                continue
            # The keywords are matched as literal strings (not whole words):
            # the longest one that prefixes the input.
            for kw in ('handcodeApi', 'nocontext', 'direct', 'param', 'sync', 'ret'):
                if text.startswith(kw, i):
                    break
            else:
                error(c)
            i += len(kw)
            if kw in ('sync', 'handcodeApi', 'direct', 'nocontext'):
                setattr(api, kw, 1)
            elif kw == 'ret':
                cur = api.ret
                next_state = 'api_entry2'
                state = 'var_type'
            else:  # param
                cur = VarType()
                api.params.append(cur)
                next_state = 'api_entry_param'
                state = 'var_type'
            continue
        error(c)
    return apis


# --- rsg_generator.c ----------------------------------------------------------
class Out:
    def __init__(self):
        self.parts = []

    def __call__(self, s):
        self.parts.append(s)

    def text(self):
        return ''.join(self.parts)


def print_file_header(f):
    f("/*\n")
    f(" * Copyright (C) 2015 The Android Open Source Project\n")
    f(" *\n")
    f(" * Licensed under the Apache License, Version 2.0 (the \"License\");\n")
    f(" * you may not use this file except in compliance with the License.\n")
    f(" * You may obtain a copy of the License at\n")
    f(" *\n")
    f(" *      http://www.apache.org/licenses/LICENSE-2.0\n")
    f(" *\n")
    f(" * Unless required by applicable law or agreed to in writing, software\n")
    f(" * distributed under the License is distributed on an \"AS IS\" BASIS,\n")
    f(" * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.\n")
    f(" * See the License for the specific language governing permissions and\n")
    f(" * limitations under the License.\n")
    f(" */\n\n")


def print_var_type(f, vt):
    if vt.isConst:
        f("const ")
    if vt.type == 0:
        f("void")
    elif vt.type == 1:
        f("int%i_t" % vt.bits)
    elif vt.type == 2:
        f("uint%i_t" % vt.bits)
    elif vt.type == 3:
        f("float" if vt.bits == 32 else "double")
    elif vt.type == 4:
        f(vt.typeName)
    if vt.ptrLevel:
        f(" ")
        f("*" * vt.ptrLevel)


def print_var_type_and_name(f, vt):
    print_var_type(f, vt)
    if vt.name:
        f(" %s" % vt.name)


def print_arg_list(f, api, assume_previous):
    for ct, p in enumerate(api.params):
        if ct or assume_previous:
            f(", ")
        print_var_type_and_name(f, p)


def print_structures(f, apis):
    for api in apis:
        f("typedef struct RS_CMD_%s_rec RS_CMD_%s;\n" % (api.name, api.name))
    f("\n")
    for ct, api in enumerate(apis):
        f("#define RS_CMD_ID_%s %i\n" % (api.name, ct + 1))
        f("struct __attribute__((packed)) RS_CMD_%s_rec {\n" % api.name)
        for p in api.params:
            f("    ")
            print_var_type_and_name(f, p)
            f(";\n")
        f("};\n\n")


def print_func_decl(f, api, prefix, add_context, is_fn_ptr):
    print_var_type_and_name(f, api.ret)
    if is_fn_ptr:
        f(" (* %s%s) (" % (prefix, api.name))
    else:
        f(" %s%s (" % (prefix, api.name))
    if not api.nocontext:
        f("Context *" if add_context else "RsContext rsc")
    print_arg_list(f, api, not api.nocontext)
    f(")")


def print_func_decls(f, apis, prefix, add_context, extern_c):
    for api in apis:
        if extern_c:
            f("extern \"C\" ")
        print_func_decl(f, api, prefix, add_context, 0)
        f(" __DEPRECATED_IN(31, \"RenderScript is deprecated. See "
          "https://developer.android.com/guide/topics/renderscript/"
          "migrate\");\n")
    f("\n\n")


def print_playback_funcs(f, apis, prefix):
    for api in apis:
        if api.direct:
            continue
        f("void %s%s (Context *, const void *);\n" % (prefix, api.name))


def has_inline_data_pointers(api):
    ret = 0
    if api.sync or api.ret.typeName:
        return 0
    for vt in api.params:
        if not vt.isConst and vt.ptrLevel:
            return 0
        if vt.ptrLevel > 1:
            return 0
        if vt.isConst and vt.ptrLevel:
            ret = 1
    return ret


def call_args(f, api, first):
    """`<first>, a, b` or `a, b` (nocontext), as the generator writes a call's arguments."""
    if not api.nocontext:
        f(first)
    for ct2, vt in enumerate(api.params):
        if ct2 > 0 or not api.nocontext:
            f(", ")
        f("%s" % vt.name)


def print_api_cpp(f, apis):
    f(RSG_API_CPP_DOC)
    f("#include \"rsDevice.h\"\n")
    f("#include \"rsContext.h\"\n")
    f("#include \"rsThreadIO.h\"\n")
    f("#include \"rsgApiStructs.h\"\n")
    f("#include \"rsgApiFuncDecl.h\"\n")
    f("#include \"rsFifo.h\"\n")
    f("\n")
    f("using namespace android;  // NOLINT\n")
    f("using namespace android::renderscript;  // NOLINT\n")
    f("\n")

    for api in apis:
        need_flush = 0
        f("static ")
        print_func_decl(f, api, LOCAL_FIFO_PREFIX, 0, 0)
        f("\n{\n")
        inline = has_inline_data_pointers(api)
        if api.direct:
            f("    ")
            if api.ret.typeName:
                f("return ")
            f(RS_INTERNAL_PREFIX + "%s(" % api.name)
            call_args(f, api, "(Context *)rsc")
            f(");\n")
        elif api.handcodeApi:
            f("    " + LOCAL_FIFO_PREFIX + "%s_handcode(" % api.name)
            call_args(f, api, "(Context *)rsc")
            f(");\n")
        else:
            f("    if (((Context *)rsc)->isSynchronous()) {\n")
            f("        ")
            if api.ret.typeName:
                f("return ")
            f(RS_INTERNAL_PREFIX + "%s(" % api.name)
            call_args(f, api, "(Context *)rsc")
            f(");\n")
            if not api.ret.typeName:
                f("    return;")
            f("    }\n\n")

            f("    ThreadIO *io = &((Context *)rsc)->mIO;\n")
            f("    const size_t size = sizeof(RS_CMD_%s);\n" % api.name)
            if inline:
                f("    size_t dataSize = 0;\n")
                for vt in api.params:
                    if vt.isConst and vt.ptrLevel:
                        f("    dataSize += %s_length;\n" % vt.name)

            if inline:
                f("    RS_CMD_%s *cmd = NULL;\n" % api.name)
                f("    if (dataSize < io->getMaxInlineSize()) {;\n")
                f("        cmd = static_cast<RS_CMD_%s *>(io->coreHeader(RS_CMD_ID_%s, dataSize + size));\n"
                  % (api.name, api.name))
                f("    } else {\n")
                f("        cmd = static_cast<RS_CMD_%s *>(io->coreHeader(RS_CMD_ID_%s, size));\n"
                  % (api.name, api.name))
                f("    }\n")
                f("    uint8_t *payload = (uint8_t *)&cmd[1];\n")
            else:
                f("    RS_CMD_%s *cmd = static_cast<RS_CMD_%s *>(io->coreHeader(RS_CMD_ID_%s, size));\n"
                  % (api.name, api.name, api.name))

            for vt in api.params:
                need_flush += vt.ptrLevel
                if vt.ptrLevel and inline:
                    f("    if (%s_length == 0) {\n" % vt.name)
                    f("        cmd->%s = NULL;\n" % vt.name)
                    f("    } else if (dataSize < io->getMaxInlineSize()) {\n")
                    f("        memcpy(payload, %s, %s_length);\n" % (vt.name, vt.name))
                    f("        cmd->%s = (" % vt.name)
                    print_var_type(f, vt)
                    f(")(payload - ((uint8_t *)&cmd[1]));\n")
                    f("        payload += %s_length;\n" % vt.name)
                    f("    } else {\n")
                    f("        cmd->%s = %s;\n" % (vt.name, vt.name))
                    f("    }\n")
                else:
                    f("    cmd->%s = %s;\n" % (vt.name, vt.name))
            if api.ret.typeName or api.sync:
                need_flush = 1

            f("    io->coreCommit();\n")
            if inline:
                f("    if (dataSize >= io->getMaxInlineSize()) {\n")
                f("        io->coreGetReturn(NULL, 0);\n")
                f("    }\n")
            elif api.ret.typeName:
                f("\n    ")
                print_var_type(f, api.ret)
                f(" ret;\n")
                f("    io->coreGetReturn(&ret, sizeof(ret));\n")
                f("    return ret;\n")
            elif need_flush:
                f("    io->coreGetReturn(NULL, 0);\n")
        f("};\n\n")

    f("\n")

    for api in apis:
        f("extern \"C\" ")
        print_func_decl(f, api, "rs", 0, 0)
        f("\n{\n")
        f("    ")
        if api.ret.typeName:
            f("return ")
        f(LOCAL_FIFO_PREFIX + "%s(" % api.name)
        call_args(f, api, "(Context *)rsc")
        f(");\n")
        f("}\n\n")


def print_playback_cpp(f, apis):
    f(RSG_API_REPLAY_CPP_DOC)
    f("#include \"rsDevice.h\"\n")
    f("#include \"rsContext.h\"\n")
    f("#include \"rsThreadIO.h\"\n")
    f("#include \"rsgApiStructs.h\"\n")
    f("#include \"rsgApiFuncDecl.h\"\n")
    f("\n")
    f("namespace android {\n")
    f("namespace renderscript {\n")
    f("\n")

    for api in apis:
        need_flush = 0
        if api.direct:
            continue
        inline = has_inline_data_pointers(api)
        f("void " + RS_PLAYBACK_PREFIX + "%s(Context *con, const void *vp, size_t cmdSizeBytes) {\n"
          % api.name)
        f("    const RS_CMD_%s *cmd = static_cast<const RS_CMD_%s *>(vp);\n" % (api.name, api.name))

        if inline:
            f("    const uint8_t *baseData = 0;\n")
            f("    if (cmdSizeBytes != sizeof(RS_CMD_%s)) {\n" % api.name)
            f("        baseData = &((const uint8_t *)vp)[sizeof(*cmd)];\n")
            f("    }\n")

        f("    ")
        if api.ret.typeName:
            f("\n    ")
            print_var_type(f, api.ret)
            f(" ret = ")
        f(RS_INTERNAL_PREFIX + "%s(con" % api.name)
        for vt in api.params:
            need_flush += vt.ptrLevel
            if inline and vt.ptrLevel:
                f(",\n           cmd->%s_length == 0 ? NULL : (const %s *)&baseData[(intptr_t)cmd->%s]"
                  % (vt.name, vt.typeName, vt.name))
            else:
                f(",\n           cmd->%s" % vt.name)
        f(");\n")

        if inline:
            f("    size_t totalSize = 0;\n")
            for p in api.params:
                if p.ptrLevel:
                    f("    totalSize += cmd->%s_length;\n" % p.name)
            f("    if ((totalSize != 0) && (cmdSizeBytes == sizeof(RS_CMD_%s))) {\n" % api.name)
            f("        con->mIO.coreSetReturn(NULL, 0);\n")
            f("    }\n")
        elif api.ret.typeName:
            f("    con->mIO.coreSetReturn(&ret, sizeof(ret));\n")
        elif api.sync or need_flush:
            f("    con->mIO.coreSetReturn(NULL, 0);\n")

        f("};\n\n")

    f("RsPlaybackLocalFunc gPlaybackFuncs[%i] = {\n" % (len(apis) + 1))
    f("    NULL,\n")
    for api in apis:
        if api.direct:
            f("    NULL,\n")
        else:
            f("    %s%s,\n" % (RS_PLAYBACK_PREFIX, api.name))
    f("};\n")

    f("};\n")
    f("};\n")


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    template, out_path, specs = sys.argv[1], sys.argv[2], sys.argv[3:]
    with open(template, 'rb') as fh:
        choice = fh.read(1).decode('ascii', 'replace')
    if choice < '0' or choice > '3':
        sys.exit("Uknown command: '%s'" % choice)
    text = ''
    for s in specs:
        with open(s, encoding='utf-8', newline='') as fh:
            text += fh.read()
    apis = lex(text)

    f = Out()
    print_file_header(f)
    if choice == '0':
        f("\n")
        f("#include \"rsContext.h\"\n")
        f("#include \"rsFifo.h\"\n")
        f("\n")
        f("namespace android {\n")
        f("namespace renderscript {\n")
        print_structures(f, apis)
        print_func_decls(f, apis, RS_INTERNAL_PREFIX, 1, 0)
        print_playback_funcs(f, apis, RS_PLAYBACK_PREFIX)
        f("typedef void (*RsPlaybackLocalFunc)(Context *, const void *, size_t sizeBytes);\n")
        f("extern RsPlaybackLocalFunc gPlaybackFuncs[%i];\n" % (len(apis) + 1))
        f("}\n")
        f("}\n")
    elif choice == '1':
        print_func_decls(f, apis, "rs", 0, 1)
    elif choice == '2':
        print_api_cpp(f, apis)
    else:
        print_playback_cpp(f, apis)

    new = f.text()
    try:
        with open(out_path, encoding='utf-8', newline='') as fh:
            if fh.read() == new:
                return
    except OSError:
        pass
    with open(out_path, 'w', encoding='utf-8', newline='\n') as fh:
        fh.write(new)


if __name__ == '__main__':
    main()
