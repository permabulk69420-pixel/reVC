# Quest stereo and Touch checkpoint

Quest hardware confirmed commit `233cff6fd7029e6ae94d755ecbe2df2cf250d65e` reaches `FOCUSED`, submits continuous stereo frames, and receives Quest Touch input through OpenXR actions. Menu navigation and selection work.

The raw-FBO mono checkpoint restored the complete game render. Native stereo at `f1ee8bf8d3878ccce88169d3d56a4dbb6e9a4bb4` then confirmed both eyes can independently cull, render at 1680x1760, submit continuously, and shut down cleanly. Its remaining hardware failure is projection geometry: the view is strongly zoomed/angled even though the world and stereoscopic difference are present.

Build `8ce40a78fb3d62d38f64972d274b4decf1c46564` preserves that per-eye render timing and corrects only the RenderWare `viewOffset` sign used to translate OpenXR's asymmetric frustum centre. Preserve the immersive host, EGL ownership, session lifecycle, Touch action bridge, pre-culling left-eye start, right-eye render-list rebuild, and native per-eye target while validating this projection correction.
