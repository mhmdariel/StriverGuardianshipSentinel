; ledger.asm
; Windows x64 MASM
;
; RCX = inbound bytes delta
; RDX = outbound bytes delta
; R8  = inbound packet delta
; R9  = outbound packet delta

OPTION CASEMAP:NONE

EXTERN g_inboundBytes:QWORD
EXTERN g_outboundBytes:QWORD
EXTERN g_inboundPackets:QWORD
EXTERN g_outboundPackets:QWORD

PUBLIC UpdateTrafficCounters

.code

UpdateTrafficCounters PROC

    lock add QWORD PTR [g_inboundBytes], rcx
    lock add QWORD PTR [g_outboundBytes], rdx
    lock add QWORD PTR [g_inboundPackets], r8
    lock add QWORD PTR [g_outboundPackets], r9

    ret

UpdateTrafficCounters ENDP

END
