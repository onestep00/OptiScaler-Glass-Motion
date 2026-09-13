option casemap:none
extern DiagnosticWrapperTarget:qword
public DiagnosticArrayReturn
public DiagnosticDynamicArrayReturn
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
DynamicArraySourceTraceFixture proc frame
    push rbx
    .pushreg rbx
    push rsi
    .pushreg rsi
    sub rsp, 28h
    .allocstack 28h
    .endprolog
    mov rbx, rcx
    mov rsi, qword ptr [rsp + 60h]
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call qword ptr [DiagnosticWrapperTarget]
DiagnosticDynamicArrayReturn::
    add rsp, 28h
    pop rsi
    pop rbx
    ret
DynamicArraySourceTraceFixture endp
end
