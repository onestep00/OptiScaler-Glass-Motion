option casemap:none
extern DiagnosticWrapperTarget:qword
public DiagnosticArrayReturn
.code
ArraySourceTraceFixture proc frame
    push rdi
    .pushreg rdi
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov rdi, rcx
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call qword ptr [DiagnosticWrapperTarget]
DiagnosticArrayReturn::
    add rsp, 20h
    pop rdi
    ret
ArraySourceTraceFixture endp
end
