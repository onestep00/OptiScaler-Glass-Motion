option casemap:none
extern DiagnosticCallerObserve:proc
.code
DiagnosticCallerFixture proc frame
    push rdi
    .pushreg rdi
    push rbx
    .pushreg rbx
    push r14
    .pushreg r14
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rdi, rcx
    mov rbx, rdx
    mov r14, r8
    lea rcx, DiagnosticCallerReturn
    mov rdx, r9
    call DiagnosticCallerObserve
DiagnosticCallerReturn:
    add rsp, 20h
    pop r14
    pop rbx
    pop rdi
    ret
DiagnosticCallerFixture endp
end
