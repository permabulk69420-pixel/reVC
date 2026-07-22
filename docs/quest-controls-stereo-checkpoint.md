# Quest stereo and Touch checkpoint

Quest hardware confirmed commit `233cff6fd7029e6ae94d755ecbe2df2cf250d65e` reaches `FOCUSED`, submits continuous stereo frames, and receives Quest Touch input through OpenXR actions. Menu navigation and selection work.

Known visual limitation at this checkpoint: the replay-based second-eye path can present black during direct loading/cutscene drawing and can lose the 3D world while HUD/minimap overlays remain. Preserve the immersive host, EGL ownership, OpenXR session, stereo frame lifecycle, and Touch action bridge while correcting render-pass classification/culling.
