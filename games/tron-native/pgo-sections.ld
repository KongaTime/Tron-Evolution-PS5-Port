/* TRON_PGO=gen: place clang's profile sections inside the PS5 layout (they
 * would be orphans after .bss, which the native converter rejects). */
SECTIONS {
    __llvm_prf_names : { KEEP(*(__llvm_prf_names)) }
} INSERT AFTER .rodata;
SECTIONS {
    __llvm_prf_cnts : { KEEP(*(__llvm_prf_cnts)) }
    __llvm_prf_data : { KEEP(*(__llvm_prf_data)) }
    __llvm_prf_bits : { KEEP(*(__llvm_prf_bits)) }
    __llvm_prf_vnds : { KEEP(*(__llvm_prf_vnds)) }
    __llvm_prf_vns : { KEEP(*(__llvm_prf_vns)) }
    __llvm_prf_vtab : { KEEP(*(__llvm_prf_vtab)) }
    __llvm_covfun : { KEEP(*(__llvm_covfun)) }
    __llvm_covmap : { KEEP(*(__llvm_covmap)) }
} INSERT AFTER .data;
