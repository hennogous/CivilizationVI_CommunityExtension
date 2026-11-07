; MSVC/MASM Win64 entry. Preserve caller R12 and the seven original arguments.
; The stack allocation and unwind metadata match the qualified native shim.
EXTERN AIDistrictScoreNet:PROC
PUBLIC AIDistrictScoreNetEntry
.code
AIDistrictScoreNetEntry PROC FRAME
    sub rsp, 58h
    .allocstack 58h
    .endprolog
    mov rax, [rsp + 80h]
    mov [rsp + 20h], rax
    mov rax, [rsp + 88h]
    mov [rsp + 28h], rax
    mov rax, [rsp + 90h]
    mov [rsp + 30h], rax
    mov [rsp + 38h], r12
    mov rax, [rsp + 58h]
    mov [rsp + 40h], rax
    call AIDistrictScoreNet
    add rsp, 58h
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
    cmp byte ptr [rbp + NominationFirstFlag], 0
    jne replay_flag
    cmp byte ptr [rbp + NominationSecondFlag], 0
    jne replay_flag

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
