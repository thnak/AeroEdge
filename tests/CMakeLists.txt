# AeroEdge tests. Deterministic, exit-code-gated (like Quark samples): 0 = pass.
add_executable(walking_skeleton walking_skeleton.cpp)
target_link_libraries(walking_skeleton PRIVATE aero::core aero::nodes quark::quark)
add_test(NAME walking_skeleton COMMAND walking_skeleton)
set_tests_properties(walking_skeleton PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")

# Invariant gate: the flow-execute path is 0-alloc on the steady path (I3). Not a benchmark.
add_executable(flow_zero_alloc flow_zero_alloc.cpp)
target_link_libraries(flow_zero_alloc PRIVATE aero::core aero::nodes)
add_test(NAME flow_zero_alloc COMMAND flow_zero_alloc)
set_tests_properties(flow_zero_alloc PROPERTIES FAIL_REGULAR_EXPRESSION "FAIL")
