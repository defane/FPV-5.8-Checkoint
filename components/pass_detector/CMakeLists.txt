# Pure C: host-tested in test/test_pass_detector.c, test/test_scan_detector.c and test/test_pass_calib.c.
idf_component_register(SRCS "pass_detector.c" "scan_detector.c" "pass_calib.c" INCLUDE_DIRS "include")
# Fed ~4000 readings a second on the detector: -O2 whatever the app's optimisation.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
