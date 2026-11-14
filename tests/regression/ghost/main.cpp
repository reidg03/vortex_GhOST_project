#include <vortex2.h>
#include "common.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(expr) do { \
    vx_result_t _r = (expr); \
    if (_r != VX_SUCCESS) { \
        std::fprintf(stderr, "FAIL %s:%d: '%s' returned %s\n", \
                     __FILE__, __LINE__, #expr, vx_result_string(_r)); \
        std::exit(1); \
    } \
} while (0)

int main() {
  vx_device_h device = nullptr;
  vx_queue_h queue = nullptr;
  vx_buffer_h dst_buffer = nullptr;
  vx_module_h module = nullptr;
  vx_kernel_h kernel = nullptr;
  vx_event_h launch_event = nullptr;
  vx_event_h read_event = nullptr;

  CHECK(vx_device_open(0, &device));

  vx_queue_info_t queue_info = { sizeof(queue_info), nullptr, VX_QUEUE_PRIORITY_NORMAL, 0 };
  CHECK(vx_queue_create(device, &queue_info, &queue));

  uint64_t num_threads = 0;
  CHECK(vx_device_query(device, VX_CAPS_NUM_THREADS, &num_threads));
  const uint64_t buffer_size = num_threads * sizeof(uint32_t);
  std::vector<uint32_t> results(num_threads, 0);

  CHECK(vx_buffer_create(device, buffer_size, VX_MEM_WRITE, &dst_buffer));
  kernel_arg_t kernel_arg{};
  CHECK(vx_buffer_address(dst_buffer, &kernel_arg.dst_addr));

  CHECK(vx_module_load_file(device, "kernel.vxbin", &module));
  CHECK(vx_module_get_kernel(module, "main", &kernel));

  vx_launch_info_t launch_info{};
  launch_info.struct_size = sizeof(launch_info);
  launch_info.kernel = kernel;
  launch_info.args_host = &kernel_arg;
  launch_info.args_size = sizeof(kernel_arg);
  launch_info.ndim = 1;
  launch_info.grid_dim[0] = 1;
  launch_info.block_dim[0] = static_cast<uint32_t>(num_threads);
  CHECK(vx_enqueue_launch(queue, &launch_info, 0, nullptr, &launch_event));

  CHECK(vx_enqueue_read(queue, results.data(), dst_buffer, 0, buffer_size,
                        1, &launch_event, &read_event));
  CHECK(vx_event_wait_value(read_event, 1, VX_TIMEOUT_INFINITE));

  int errors = 0;
  for (uint32_t tid = 0; tid < num_threads; ++tid) {
    if (results[tid] != 16) {
      std::fprintf(stderr, "thread %u: expected 16, got %u\n", tid, results[tid]);
      ++errors;
    }
  }

  vx_event_release(read_event);
  vx_event_release(launch_event);
  vx_kernel_release(kernel);
  vx_module_release(module);
  vx_buffer_release(dst_buffer);
  vx_queue_release(queue);
  vx_device_release(device);

  if (errors != 0) {
    std::printf("FAILED: %d mismatches\n", errors);
    return 1;
  }
  std::printf("PASSED!\n");
  return 0;
}
