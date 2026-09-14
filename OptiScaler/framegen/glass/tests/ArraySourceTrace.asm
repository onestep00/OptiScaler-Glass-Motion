option casemap:none
extern DiagnosticWrapperTarget:qword
public DiagnosticArrayReturn
public DiagnosticDynamicArrayReturn
public DiagnosticSplitArrayReturn
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
SplitArraySourceTraceFixture proc frame
    push rbx
    .pushreg rbx
    push r14
    .pushreg r14
    push r15
    .pushreg r15
    sub rsp, 20h
    .allocstack 20h
    .endprolog
    mov r14, rcx
    mov r15, qword ptr [rsp + 60h]
    mov rbx, qword ptr [rsp + 68h]
    mov rcx, rdx
    mov rdx, r8
    mov r8, r9
    call qword ptr [DiagnosticWrapperTarget]
DiagnosticSplitArrayReturn::
    add rsp, 20h
    pop r15
    pop r14
    pop rbx
    ret
SplitArraySourceTraceFixture endp
end
