#include <vx_spawn2.h>
#include "common.h"

__kernel void kernel_main(kernel_arg_t* __UNIFORM__ arg) {
  auto dst_ptr = reinterpret_cast<uint32_t*>(arg->dst_addr);
  uint32_t value;

  asm volatile(
    "addi t0, zero, 1\n\t"
    "add  t1, t0, t0\n\t"
    "addi t2, zero, 7\n\t"
    "add  t3, t2, t2\n\t"
    "add  %0, t1, t3\n\t"
    : "=r"(value)
    :
    : "t0", "t1", "t2", "t3"
  );

  dst_ptr[threadIdx.x] = value;
}
