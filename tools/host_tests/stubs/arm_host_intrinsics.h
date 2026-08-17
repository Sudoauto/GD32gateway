#ifndef GW_HOST_ARM_INTRINSICS_H
#define GW_HOST_ARM_INTRINSICS_H
/* Host-only parser shims. They are never part of the embedded build. */
#define __builtin_arm_nop() ((void)0)
#define __builtin_arm_dsb(x) ((void)(x))
#define __builtin_arm_isb(x) ((void)(x))
#define __builtin_arm_dmb(x) ((void)(x))
#endif
