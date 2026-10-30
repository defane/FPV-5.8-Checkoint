# Pure C: host-tested in test/test_race_timing.c. The C6 fleet hub feeds it
# gate crossings and reads per-channel lap/sector timing.
idf_component_register(SRCS "race_timing.c" INCLUDE_DIRS "include")
