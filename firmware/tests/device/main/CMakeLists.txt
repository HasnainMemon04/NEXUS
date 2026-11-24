idf_component_register(SRCS "selftest.c" REQUIRES nexus_core esp_timer)
set_source_files_properties(selftest.c PROPERTIES COMPILE_OPTIONS "-ffp-contract=off;-fno-fast-math")
