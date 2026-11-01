; Windows x64: ECX contains a strictly positive integer n.
; Returns 1 + 2 + ... + n in EAX (modulo 2^32).
entry:
    xor eax, eax
again:
    add eax, ecx
    dec ecx
    jnz again
    ret
