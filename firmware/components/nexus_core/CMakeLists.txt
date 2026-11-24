idf_component_register(
    SRCS "job_queue.c" "exec_pool.c" "kernels.c" "logger.c" "monitor.c" "proto.c" "oslab.c"
    INCLUDE_DIRS "include"
    REQUIRES lwip esp_timer
)

# Equal rounding for Mandelbrot on LX6 and LX7, including boundary pixels.
set_source_files_properties(kernels.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
