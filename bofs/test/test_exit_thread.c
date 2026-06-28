/*
 * Minimal test shellcode - just exits thread with code 0x1337
 *
 * Compile:
 *   x86_64-w64-mingw32-gcc -c test_exit_thread.c -o test_exit_thread.o
 *   x86_64-w64-mingw32-objcopy -O binary test_exit_thread.o test_exit_thread.bin
 *   xxd -p test_exit_thread.bin | tr -d '\n'
 */

void __attribute__((naked)) shellcode_entry() {
    __asm__(
        "xor %rcx, %rcx\n"           // ThreadHandle = NULL (current thread)
        "mov $0x1337, %rdx\n"        // ExitStatus = 0x1337
        "mov %rcx, %r10\n"
        "mov $0x53, %eax\n"          // NtTerminateThread syscall
        "syscall\n"
        "ret\n"
    );
}
