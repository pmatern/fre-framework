add_executable(example_multi_decision_pipeline
    main.cpp
)

target_link_libraries(example_multi_decision_pipeline
    PRIVATE fre::pipeline
)
