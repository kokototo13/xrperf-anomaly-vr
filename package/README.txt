xrperf - performance layer for S.T.A.L.K.E.R. Anomaly VR (VR AoE mod, OpenXR)
================================================================================
(Russian version: README.ru.txt)

What it does
  - The game renders at 85% resolution; xrperf upscales it back to the headset
    resolution with FSR (AMD) or NIS (NVIDIA).
  Up to ~30% more performance when the GPU is the bottleneck.
  Game and mod files are NOT modified.

Install
  1. Extract this folder anywhere outside the game folder (e.g. C:\xrperf).
     Don't move it afterwards.
  2. Run install.bat.
  3. Start the game through the launcher as usual.
  Remove: run uninstall.bat, then delete the folder.

Switch while playing (applies within a second)
  FSR.bat     - FSR upscaling (default)
  NIS.bat     - NIS upscaling
  OFF.bat     - upscaling off (native resolution from the next game start)

Settings - xrperf.ini
  render_scale  0.85 by default. Lower = faster but softer. Needs a game restart.
  sharpness     0..1.

Game graphics settings matter even more - see the project page:
  https://github.com/kokototo13/xrperf-anomaly-vr

Problems? See logs\xrperf.log (it also shows FPS and GPU time per frame).

Third-party licenses (AMD FSR, NVIDIA NIS, OpenXR) are in the licenses folder.
