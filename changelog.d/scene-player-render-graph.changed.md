- The `scene_player` sample and its GPU test draw through the render graph: the depth buffer is a
  pooled transient and no barrier is written by hand. `scene_player::depthBarrier()` is gone.
