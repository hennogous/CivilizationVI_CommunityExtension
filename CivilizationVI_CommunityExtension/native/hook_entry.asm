; XP2's plot-analysis caller leaves its row in R12. Pass that row and the
; original return address after NetValue's seven Win64 arguments.
; These offsets use RSP after frame allocation. Leave RCX/RDX/R8/R9 intact
; and copy incoming arguments 5-7 after the outgoing shadow space.
NetFrameSize EQU 58h
NetCallerReturn EQU NetFrameSize
NetIncomingArg5 EQU NetFrameSize + 28h
NetIncomingArg6 EQU NetFrameSize + 30h
NetIncomingArg7 EQU NetFrameSize + 38h
EXTERN AIDistrictScoreNet:PROC
PUBLIC AIDistrictScoreNetEntry
.code
AIDistrictScoreNetEntry PROC FRAME
    sub rsp, NetFrameSize
    .allocstack NetFrameSize
    .endprolog
    mov rax, [rsp + NetIncomingArg5]
    mov [rsp + 20h], rax
    mov rax, [rsp + NetIncomingArg6]
    mov [rsp + 28h], rax
    mov rax, [rsp + NetIncomingArg7]
    mov [rsp + 30h], rax
    mov [rsp + 38h], r12
    mov rax, [rsp + NetCallerReturn]
    mov [rsp + 40h], rax
    call AIDistrictScoreNet
    add rsp, NetFrameSize
    ret
AIDistrictScoreNetEntry ENDP

; This entry is reached by a real CALL replacing the five-byte native MOVZX
; after both prerequisite checks in GetBestBuilding. RBP remains the native
; frame pointer. Replay the MOVZX on both paths and RET to the original CMP.
EXTERN AIBuildingNominationScore:PROC
PUBLIC AIBuildingNominationEntry

NominationCallerReturn EQU 28h
NominationFirstFlag EQU 40h
NominationSecondFlag EQU 48h

AIBuildingNominationEntry PROC FRAME
    sub rsp, 0B8h
    .allocstack 0B8h
    .endprolog
    ; The ordinary nomination uses flags 0/0. QueueNextBuild uses 1/0
    ; or 1/1. Admit only these modes to the adapter. C++ checks the exact
    ; caller return and the independently published preference.
    cmp byte ptr [rbp + NominationFirstFlag], 0
    jne check_queue_mode
    cmp byte ptr [rbp + NominationSecondFlag], 0
    jne replay_flag
    jmp save_comparison_context

check_queue_mode:
    cmp byte ptr [rbp + NominationFirstFlag], 1
    jne replay_flag
    cmp byte ptr [rbp + NominationSecondFlag], 1
    ja replay_flag

save_comparison_context:
    ; The interrupted caller did not expect a function call here. Keep its
    ; volatile registers, reserving the first 32 bytes for Win64 shadow space.
    mov [rsp + 20h], rax
    mov [rsp + 28h], rcx
    mov [rsp + 30h], rdx
    mov [rsp + 38h], r8
    mov [rsp + 40h], r9
    mov [rsp + 48h], r10
    mov [rsp + 50h], r11
    movdqu [rsp + 58h], xmm0
    movdqu [rsp + 68h], xmm1
    movdqu [rsp + 78h], xmm2
    movdqu [rsp + 88h], xmm3
    movdqu [rsp + 98h], xmm4
    movdqu [rsp + 0A8h], xmm5
    mov rcx, r15
    mov edx, r12d
    mov r8d, r14d
    mov r9, [rbp + NominationCallerReturn]
    call AIBuildingNominationScore
    mov r14d, eax
    movdqu xmm5, [rsp + 0A8h]
    movdqu xmm4, [rsp + 98h]
    movdqu xmm3, [rsp + 88h]
    movdqu xmm2, [rsp + 78h]
    movdqu xmm1, [rsp + 68h]
    movdqu xmm0, [rsp + 58h]
    mov r11, [rsp + 50h]
    mov r10, [rsp + 48h]
    mov r9, [rsp + 40h]
    mov r8, [rsp + 38h]
    mov rdx, [rsp + 30h]
    mov rcx, [rsp + 28h]
    mov rax, [rsp + 20h]
replay_flag:
    ; The next native instruction sets flags from the effective score.
    movzx r13d, byte ptr [rbp + NominationSecondFlag]
    add rsp, 0B8h
    ret
AIBuildingNominationEntry ENDP
END
