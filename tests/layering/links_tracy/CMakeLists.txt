# A lint fixture for the tracy_confined check (ADR-0007): a target outside foundation that links
# Tracy. It is never added to the build.
target_link_libraries(ith_fixture PRIVATE ith::tracy)
