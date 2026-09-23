# Vendored ESP-WHO components

Upstream: https://github.com/espressif/esp-who  (commit 1abda05, "Merge branch 'object_track' into 'master'")
License: Apache-2.0 (see LICENSE in this folder). Only the components this firmware links are included; the directory
layout is unchanged so the relative `path:` entries in their idf_component.yml files keep working.

## Local modifications (everything else is byte-identical to upstream)

```
modified: components/who_detect/CMakeLists.txt
added:    components/who_detect/who_bench.hpp
modified: components/who_detect/who_detect.cpp
modified: components/who_detect/who_detect.hpp
added:    components/who_frame_cap/who_cam_counter.hpp
modified: components/who_frame_cap/who_frame_cap_node.cpp
modified: components/who_recognition/who_recognition.cpp
```

What they do:
- who_detect/who_bench.hpp (new)      timing helpers, BENCH result lines, heartbeat, and the frame tap (`dump_frame`)
                                      that prints the exact image handed to the detector plus its detections.
- who_detect/who_detect.{hpp,cpp}     time the detector run, count frames, and call the frame tap.
- who_recognition/who_recognition.cpp time the recognizer and print one BENCH line per recognition attempt.
- who_frame_cap/who_cam_counter.hpp (new), who_frame_cap_node.cpp   count frames delivered by the camera driver.

To re-sync with a newer ESP-WHO, diff these files against upstream and re-apply the changes above.
