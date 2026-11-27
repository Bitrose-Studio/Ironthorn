- The editor viewport camera has a 90-degree horizontal field of view (`EditorCamera::fovX`,
  replacing the vertical `fovY`); the vertical angle follows from the image's aspect
  (`ith::verticalFov`). Wide viewports no longer stretch at their edges, and Focus fits the
  selection in both directions. Viewport cameras saved with `fovY` keep their position and
  direction and get the default field of view.
