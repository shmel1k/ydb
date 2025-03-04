# Generated by devtools/yamaker.

LIBRARY()

SUBSCRIBER(g:cpp-contrib)

VERSION(14.0.6)

LICENSE(Apache-2.0 WITH LLVM-exception)

LICENSE_TEXTS(.yandex_meta/licenses.list.txt)

PEERDIR(
    contrib/libs/llvm14
    contrib/libs/llvm14/include
    contrib/libs/llvm14/lib/CodeGen
    contrib/libs/llvm14/lib/IR
    contrib/libs/llvm14/lib/Support
    contrib/libs/llvm14/lib/Target/X86
    contrib/libs/llvm14/lib/Target/X86/AsmParser
    contrib/libs/llvm14/lib/Target/X86/Disassembler
    contrib/libs/llvm14/lib/Target/X86/MCTargetDesc
    contrib/libs/llvm14/lib/Target/X86/TargetInfo
    contrib/libs/llvm14/tools/llvm-exegesis/lib
)

ADDINCL(
    ${ARCADIA_BUILD_ROOT}/contrib/libs/llvm14/lib/Target/X86
    contrib/libs/llvm14/lib/Target/X86
    contrib/libs/llvm14/tools/llvm-exegesis/lib/X86
)

NO_COMPILER_WARNINGS()

NO_UTIL()

SRCS(
    Target.cpp
    X86Counter.cpp
)

END()
