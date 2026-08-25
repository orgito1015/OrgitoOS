#ifndef VM_H
#define VM_H

#include "codegen.h"

int vm_execute(const Bytecode *bc);

/* Frees vm_execute()'s locals/frames buffers if a run is mid-flight when
 * OC_FAIL longjmps out of it (normal completion already frees them itself).
 * oc_run.c's catch point calls this unconditionally after every failed
 * compile/run so repeated failures don't leak vm_execute's ~268 KiB of
 * heap. Safe to call when no run is in progress (no-op). */
void oc_vm_cleanup_after_error(void);

#endif /* VM_H */
