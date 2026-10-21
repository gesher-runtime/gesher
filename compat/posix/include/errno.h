// gesher-base: <errno.h> with Linux's errno values.
//
// Android's Java libraries hard-code Linux's numbers (android.system.
// OsConstants.EAGAIN == 11, ENOTEMPTY == 39, ...) and compare them with what
// native code reports, so gesher-base speaks Linux's numbering throughout.
// Values 1-34 are the same in the Windows CRT; the others differ, and
// gesher-base translates the CRT's (gesher_errno_from_crt) where it calls
// the CRT. See docs/decisions/0008-linux-abi.md.
#pragma once

#include_next <errno.h>

#undef EDEADLK
#undef ENAMETOOLONG
#undef ENOLCK
#undef ENOSYS
#undef ENOTEMPTY
#undef ELOOP
#undef EWOULDBLOCK
#undef ENOMSG
#undef EIDRM
#undef ENOSTR
#undef ENODATA
#undef ETIME
#undef ENOSR
#undef ENOLINK
#undef EPROTO
#undef EBADMSG
#undef EOVERFLOW
#undef EILSEQ
#undef ENOTSOCK
#undef EDESTADDRREQ
#undef EMSGSIZE
#undef EPROTOTYPE
#undef ENOPROTOOPT
#undef EPROTONOSUPPORT
#undef EOPNOTSUPP
#undef ENOTSUP
#undef EAFNOSUPPORT
#undef EADDRINUSE
#undef EADDRNOTAVAIL
#undef ENETDOWN
#undef ENETUNREACH
#undef ENETRESET
#undef ECONNABORTED
#undef ECONNRESET
#undef ENOBUFS
#undef EISCONN
#undef ENOTCONN
#undef ETIMEDOUT
#undef ECONNREFUSED
#undef EHOSTUNREACH
#undef EALREADY
#undef EINPROGRESS
#undef ECANCELED
#undef EOWNERDEAD
#undef ENOTRECOVERABLE
#undef ETXTBSY

#define ENOTBLK         15
#define ETXTBSY         26
#define EDEADLK         35
#define ENAMETOOLONG    36
#define ENOLCK          37
#define ENOSYS          38
#define ENOTEMPTY       39
#define ELOOP           40
#define EWOULDBLOCK     EAGAIN
#define ENOMSG          42
#define EIDRM           43
#define ECHRNG          44
#define EL2NSYNC        45
#define EL3HLT          46
#define EL3RST          47
#define ELNRNG          48
#define EUNATCH         49
#define ENOCSI          50
#define EL2HLT          51
#define EBADE           52
#define EBADR           53
#define EXFULL          54
#define ENOANO          55
#define EBADRQC         56
#define EBADSLT         57
#define EDEADLOCK       EDEADLK
#define EBFONT          59
#define ENOSTR          60
#define ENODATA         61
#define ETIME           62
#define ENOSR           63
#define ENONET          64
#define ENOPKG          65
#define EREMOTE         66
#define ENOLINK         67
#define EADV            68
#define ESRMNT          69
#define ECOMM           70
#define EPROTO          71
#define EMULTIHOP       72
#define EDOTDOT         73
#define EBADMSG         74
#define EOVERFLOW       75
#define ENOTUNIQ        76
#define EBADFD          77
#define EREMCHG         78
#define ELIBACC         79
#define ELIBBAD         80
#define ELIBSCN         81
#define ELIBMAX         82
#define ELIBEXEC        83
#define EILSEQ          84
#define ERESTART        85
#define ESTRPIPE        86
#define EUSERS          87
#define ENOTSOCK        88
#define EDESTADDRREQ    89
#define EMSGSIZE        90
#define EPROTOTYPE      91
#define ENOPROTOOPT     92
#define EPROTONOSUPPORT 93
#define ESOCKTNOSUPPORT 94
#define EOPNOTSUPP      95
#define ENOTSUP         EOPNOTSUPP
#define EPFNOSUPPORT    96
#define EAFNOSUPPORT    97
#define EADDRINUSE      98
#define EADDRNOTAVAIL   99
#define ENETDOWN        100
#define ENETUNREACH     101
#define ENETRESET       102
#define ECONNABORTED    103
#define ECONNRESET      104
#define ENOBUFS         105
#define EISCONN         106
#define ENOTCONN        107
#define ESHUTDOWN       108
#define ETOOMANYREFS    109
#define ETIMEDOUT       110
#define ECONNREFUSED    111
#define EHOSTDOWN       112
#define EHOSTUNREACH    113
#define EALREADY        114
#define EINPROGRESS     115
#define ESTALE          116
#define EUCLEAN         117
#define ENOTNAM         118
#define ENAVAIL         119
#define EISNAM          120
#define EREMOTEIO       121
#define EDQUOT          122
#define ENOMEDIUM       123
#define EMEDIUMTYPE     124
#define ECANCELED       125
#define ENOKEY          126
#define EKEYEXPIRED     127
#define EKEYREVOKED     128
#define EKEYREJECTED    129
#define EOWNERDEAD      130
#define ENOTRECOVERABLE 131
#define ERFKILL         132
#define EHWPOISON       133

#ifdef __cplusplus
extern "C" {
#endif

// The Linux errno value for an errno value set by the Windows CRT (or by
// winpthreads, which uses the CRT's numbering).
int gesher_errno_from_crt(int crt_errno);

#ifdef __cplusplus
}
#endif
