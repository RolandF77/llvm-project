# REQUIRES: x86
# RUN: llvm-mc --triple=x86_64-pc-linux --filetype=obj -o %t.o %s
# RUN: not ld.lld -z pac-plt -z force-bti -z bti-report=error   \
# RUN:     -z pauth-report=error   %t.o -o /dev/null 2>&1 | FileCheck %s
# RUN: not ld.lld -z pac-plt -z force-bti -z bti-report=warning \
# RUN:     -z pauth-report=warning %t.o -o /dev/null 2>&1 | FileCheck %s
#
## Check that we error if -z pac-plt, -z force-bti are present and
## -z bti-report and -z pauth-report are not none when target is not aarch64

# CHECK: error: -z pac-plt only supported on AArch64
# CHECK-NEXT: error: -z force-bti only supported on AArch64
# CHECK-NEXT: error: -z bti-report only supported on AArch64
# CHECK-NEXT: error: -z pauth-report only supported on AArch64

# RUN: not ld.lld -z bti-report=something -z pauth-report=something \
# RUN:     %t.o -o /dev/null 2>&1 | FileCheck --check-prefix=REPORT_INVALID %s
# REPORT_INVALID: error: -z bti-report= parameter something is not recognized
# REPORT_INVALID: error: -z pauth-report= parameter something is not recognized
# REPORT_INVALID-EMPTY:

        .globl start
start:  ret
