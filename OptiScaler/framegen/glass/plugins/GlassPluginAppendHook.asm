; Register-preserving trampoline for the array append hook.
;
; The original function's full argument set is not known, so every volatile
; argument register is saved across the C++ helper and restored before the tail
; jump. A plain C++ hook could clobber r8/r9 and crash the engine.
extern glassArrayMapAppendHook:proc
extern glassArrayMapAppendTarget:qword
extern glassArrayMapGroupedTarget:qword

.code
; Bare pass-throughs for the isolation run: patch the function but do no work,
; so a crash there is caused by the patch itself and not by the hook body.
glassArrayMapAppendPassTrampoline proc
    jmp     qword ptr [glassArrayMapAppendTarget]
glassArrayMapAppendPassTrampoline endp

glassArrayMapGroupedPassTrampoline proc
    jmp     qword ptr [glassArrayMapGroupedTarget]
glassArrayMapGroupedPassTrampoline endp

glassArrayMapAppendTrampoline proc
    push    rcx
    push    rdx
    push    r8
    push    r9
    push    r10
    push    r11
    sub     rsp, 28h
    call    glassArrayMapAppendHook
    add     rsp, 28h
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdx
    pop     rcx
    jmp     qword ptr [glassArrayMapAppendTarget]
glassArrayMapAppendTrampoline endp
end
