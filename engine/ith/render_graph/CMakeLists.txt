# The render graph (ADR-0028): passes declare the resources they use; the graph culls, derives
# barriers, pools transient resources and records the passes into command lists, in parallel on
# the job system. A client module in the engine layer, used by the renderers and the editor
# viewport; a dedicated server doesn't render (ADR-0017).
ith_add_module(render_graph
    LAYER engine
    KIND client
    SOURCES
        Compile.cpp
        Dump.cpp
        Execute.cpp
        GraphData.h
        RenderGraph.h
        RenderGraph.cpp
        ResourcePool.h
        ResourcePool.cpp
    DEPENDS
        foundation
        jobs
        rhi)
