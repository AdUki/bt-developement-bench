# mpg123 decodes btbenchd's MP3 streams to stdout (-s); it never plays anything itself. oe-core
# picks PulseAudio output when the distro has the pulseaudio feature, which would pull in libpulse
# for a sound server the image does not run. ALSA output is enough for using it by hand.
PACKAGECONFIG = "alsa"
