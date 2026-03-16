add_executable(example_ml_pipeline
    main.cpp
)

target_link_libraries(example_ml_pipeline
    PRIVATE fre::pipeline
)
