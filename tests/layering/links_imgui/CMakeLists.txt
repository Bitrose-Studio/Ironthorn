# A lint fixture for the imgui_confined check (ADR-0029 rule 2): a target outside editor_ui that
# links ImGui. It is never added to the build.
target_link_libraries(ith_fixture PRIVATE ith::imgui)
