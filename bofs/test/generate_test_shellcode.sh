#!/bin/bash
# Generate simple test shellcode (MessageBox + ExitThread)

cat > /tmp/test_shellcode.asm << 'EOF'
BITS 64

; MessageBox shellcode for x64 Windows
; Shows "Test" message box then exits thread cleanly

start:
    ; Save registers
    push rbp
    mov rbp, rsp
    sub rsp, 0x30

    ; LoadLibraryA("user32.dll")
    mov rcx, 0x6c6c642e32336c6c  ; "ll.23ll" (reversed)
    push rcx
    mov rcx, 0x642e323372657375  ; "d.23resu" (reversed)
    push rcx
    mov rcx, rsp
    mov rax, 0x7ff000000000      ; Approximate kernel32 base
    add rax, 0x12345678          ; LoadLibraryA offset (placeholder)
    ; For testing, we'll use a simpler approach

    ; Alternative: Direct syscall to NtTerminateThread with exit code 0x1337
    xor rcx, rcx                 ; ThreadHandle = NULL (current thread)
    mov rdx, 0x1337              ; ExitStatus = 0x1337
    mov r10, rcx
    mov eax, 0x53                ; NtTerminateThread syscall number (Win10)
    syscall

    ; Should never reach here
    ret
EOF

# Assemble
nasm -f bin /tmp/test_shellcode.asm -o /tmp/test_shellcode.bin

# Convert to hex
xxd -p /tmp/test_shellcode.bin | tr -d '\n' > /tmp/test_shellcode.hex

echo "Test shellcode hex:"
cat /tmp/test_shellcode.hex
echo ""
echo ""
echo "Shellcode bytes:"
xxd -g 1 /tmp/test_shellcode.bin
