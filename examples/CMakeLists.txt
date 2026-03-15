# examples/CMakeLists.txt
#
# C++ example templates for secondary development.
# Build with: cmake -S . -B build -DPYLABHUB_BUILD_EXAMPLES=ON
#
# These examples link against pylabhub::utils (shared library) and demonstrate
# direct use of the C++ API without Python scripting.

# All examples link against pylabhub::utils.
set(EXAMPLE_LINK_LIBS pylabhub::utils)

# ── Existing examples ──────────────────────────────────────────────────────────

add_executable(example_hub_active_service hub_active_service_example.cpp)
target_link_libraries(example_hub_active_service PRIVATE ${EXAMPLE_LINK_LIBS})

add_executable(example_hub_health hub_health_example.cpp)
target_link_libraries(example_hub_health PRIVATE ${EXAMPLE_LINK_LIBS})

add_executable(example_datahub_producer datahub_producer_example.cpp)
target_link_libraries(example_datahub_producer PRIVATE ${EXAMPLE_LINK_LIBS})

add_executable(example_datahub_consumer datahub_consumer_example.cpp)
target_link_libraries(example_datahub_consumer PRIVATE ${EXAMPLE_LINK_LIBS})

add_executable(example_raii_layer raii_layer_example.cpp)
target_link_libraries(example_raii_layer PRIVATE ${EXAMPLE_LINK_LIBS})

add_executable(example_schema schema_example.cpp)
target_link_libraries(example_schema PRIVATE ${EXAMPLE_LINK_LIBS})

# ── New template: Processor pipeline ───────────────────────────────────────────

add_executable(example_processor_pipeline cpp_processor_template.cpp)
target_link_libraries(example_processor_pipeline PRIVATE ${EXAMPLE_LINK_LIBS})
