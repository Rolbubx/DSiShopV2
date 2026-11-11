/* Certificats racines (bundle Mozilla publié par curl, data/cacert.pem),
 * utilisés pour vérifier les connexions HTTPS. Terminé par un octet nul. */

    .section .rodata
    .global cacert_pem
    .global cacert_pem_size

    .balign 4
cacert_pem:
    .incbin "data/cacert.pem"
    .byte 0
cacert_pem_end:

    .balign 4
cacert_pem_size:
    .word cacert_pem_end - cacert_pem
