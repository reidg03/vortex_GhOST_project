#include <vx_spawn2.h>
#include "common.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto src_ptr = reinterpret_cast<const uint32_t*>(arg->src_addr);
  auto dst_ptr = reinterpret_cast<uint32_t*>(arg->dst_addr);
  uint32_t value;

  asm volatile(
    "lw   t0, 0(%1)\n\t"
    "addi t2, t0, 5\n\t"
    "addi t1, zero, 7\n\t"
    "addi t0, zero, 9\n\t"
    "add  %0, t2, t1\n\t"
    "addi t2, zero, 88\n\t"
    : "=r"(value)
    : "r"(src_ptr + threadIdx.x)
    : "t0", "t1", "t2", "memory"
  );

  dst_ptr[threadIdx.x] = value;
}
