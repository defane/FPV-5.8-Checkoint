# Pure C: host-tested in test/test_pass_detector.c and test/test_scan_detector.c.
idf_component_register(SRCS "pass_detector.c" "scan_detector.c" INCLUDE_DIRS "include")
