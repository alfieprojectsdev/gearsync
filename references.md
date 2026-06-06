1. Low-Latency Audio & DSP (The C++ Core)
The hardest part of this project is routing raw microphone data through Android natively without stuttering.

google/oboe: This is the holy grail for your audio layer. Google’s official repo contains an extensive samples folder. Specifically, look at their OboeTester and RhythmGame source code. They demonstrate exactly how to set up an AudioStreamDataCallback in C++ and measure the exact tap-to-tone latency you need for your shift blips.

papergray/MicUp: A recent, highly relevant project that captures real-time Android microphone input, runs it through a C++ DSP effects chain, and routes the audio. It is a fantastic reference for structuring your JNI bridge and managing raw PCM audio streams natively.
