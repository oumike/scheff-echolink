// The web config page, embedded as ESP-IDF's EMBED_TXTFILES does: the bytes
// and a NUL between _binary_index_html_start and _binary_index_html_end.
    .section .rodata
    .global _binary_index_html_start
    .global _binary_index_html_end
    .balign 4
_binary_index_html_start:
    .incbin INDEX_HTML
    .byte 0
_binary_index_html_end:
    .section .note.GNU-stack,"",%progbits
