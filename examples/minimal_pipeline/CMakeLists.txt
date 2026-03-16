add_executable(example_minimal_pipeline
    main.cpp
)

target_link_libraries(example_minimal_pipeline
    PRIVATE fre::pipeline
)
